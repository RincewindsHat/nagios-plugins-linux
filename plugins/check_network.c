/*
 * License: GPLv3+
 * Copyright (c) 2014,2015,2020 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A Nagios plugin that displays some network interfaces.statistics.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#ifdef HAVE_LINUX_IF_LINK_H
# include <linux/if_link.h>
#else
# include <linux/rtnetlink.h>
#endif
#include <string.h>

#include "common.h"
#include "messages.h"
#include "netinfo.h"
#include "progname.h"
#include "progversion.h"
#include "string-macros.h"
#include "system.h"
#include "xalloc.h"
#include "xasprintf.h"
#include "xstrton.h"

#define MAX_PRINTED_INTERFACES		5

static const char *program_copyright =
  "Copyright (C) 2014,2015,2020 Davide Madrisan <" PACKAGE_BUGREPORT ">\n";

static struct option const longopts[] = {
  {(char *) "check-link", no_argument, NULL, 'k'},
  {(char *) "ifname", required_argument, NULL, 'i'},
  {(char *) "ifname-debug", no_argument, NULL, 0},
  {(char *) "no-bytes", no_argument, NULL, 'b'},
  {(char *) "no-collisions", no_argument, NULL, 'C'},
  {(char *) "no-drops", no_argument, NULL, 'd'},
  {(char *) "no-errors", no_argument, NULL, 'e'},
  {(char *) "no-loopback", no_argument, NULL, 'l'},
  {(char *) "no-multicast", no_argument, NULL, 'm'},
  {(char *) "no-packets", no_argument, NULL, 'p'},
  {(char *) "no-wireless", no_argument, NULL, 'W'},
  {(char *) "perc", no_argument, NULL, '%'},
  {(char *) "help", no_argument, NULL, GETOPT_HELP_CHAR},
  {(char *) "version", no_argument, NULL, GETOPT_VERSION_CHAR},
  {NULL, 0, NULL, 0}
};

static _Noreturn void
usage (FILE * out)
{
  fprintf (out, "%s (" PACKAGE_NAME ") v%s\n", program_name, program_version);
  fputs ("This plugin displays some network interfaces.statistics.\n", out);
  fputs (program_copyright, out);
  fputs (USAGE_HEADER, out);
  fprintf (out, "  %s [-klW] [-bCdemp] [-i <ifname-regex>] [delay]\n",
	   program_name);
  fprintf (out, "  %s [-klW] [-bCdemp] [-i <ifname-regex>] --ifname-debug\n",
	   program_name);
  fputs (USAGE_OPTIONS, out);
  fputs ("  -i, --ifname         only display interfaces matching a regular "
	 "expression\n", out);
  fputs ("      --ifname-debug   display the list of metric keys and exit\n",
	 out);
  fputs ("  -k, --check-link     report an error if at least a link is down\n",
	 out);
  fputs ("  -l, --no-loopback    skip the loopback interface\n", out);
  fputs ("  -W, --no-wireless    skip the wireless interfaces\n", out);
  fputs (USAGE_HELP, out);
  fputs (USAGE_VERSION, out);
  fputs ("  By default all the counter are reported but it's possible to "
	 "select a subset:\n", out);
  fputs ("  -b  --no-bytes       omit the rx/tx bytes counter from perfdata\n",
	 out);
  fputs ("  -C, --no-collisions  omit the collisions counter from perfdata\n",
	 out);
  fputs ("  -d  --no-drops       omit the rx/tx drop counters from perfdata\n",
	 out);
  fputs ("  -e  --no-errors      omit the rx/tx errors counters "
	 "from perfdata\n", out);
  fputs ("  -m, --no-multicast   omit the multicast counter from perfdata\n",
	 out);
  fputs ("  -p, --no-packets     omit the rx/tx packets counter from "
	 "perfdata\n", out);
  fputs ("  -%, --perc           return percentage metrics if possible\n",
	 out);
  fprintf (out, "  delay is the delay between the two network snapshots "
	   "in seconds (default: %dsec)\n", DELAY_DEFAULT);
  fputs (USAGE_NOTE, out);
  fputs ("  The option --ifname supports POSIX Extended Regular Expression "
	 "syntax.\n", out);
  fputs ("  See: https://man7.org/linux/man-pages/man7/regex.7.html\n", out);
  fputs (USAGE_EXAMPLES, out);
  fprintf (out, "  %s\n", program_name);
  fprintf (out, "  %s --check-link --ifname \"^(enp|eth)\" 5\n", program_name);
  fprintf (out, "  %s --ifname \"^(enp|wlp)\" --ifname-debug -Cdm\n",
	   program_name);
  fprintf (out, "  %s --perc --ifname \"^(enp|eth)\" 5\n", program_name);
  fprintf (out, "  %s --no-loopback --no-wireless 3\n", program_name);
  fprintf (out, "  %s --no-loopback -bCdmp 3   "
	   "# only report tx/rx errors in the perfdata\n", program_name);

  exit (out == stderr ? STATE_UNKNOWN : STATE_OK);
}

static _Noreturn void
print_version (void)
{
  printf ("%s (" PACKAGE_NAME ") v%s\n", program_name, program_version);
  fputs (program_copyright, stdout);
  fputs (GPLv3_DISCLAIMER, stdout);

  exit (STATE_OK);
}

static inline char *
fmt_perfdata_bytes (const char *ifname, const char* label,
		    unsigned int counter, unsigned long long speed, bool perc)
{
  char *perfdata;

  if (perc && (speed > 0))
    {
      float counter_perc = (100.0 / speed) * counter;
      perfdata =
	xasprintf ("%s_%s/s=%.2f%%;;;0;100.0", ifname, label, counter_perc);
    }
  else if (!perc && (speed > 0))
    perfdata = xasprintf("%s_%s/s=%u;;;0;%llu", ifname, label, counter, speed);
  else
    perfdata = xasprintf ("%s_%s/s=%u", ifname, label, counter);

  return perfdata;
}

int
main (int argc, char **argv)
{
  int c, i, option_index = 0;
  nagstatus status = STATE_OK;
  bool ifname_debug = false,
       pd_bytes = true,
       pd_collisions = true,
       pd_drops = true,
       pd_errors = true,
       pd_multicast = true,
       pd_packets = true,
       report_perc = false;
  char *bp, *ifname_regex = NULL;
  size_t size;
  unsigned int options = 0;
  unsigned long delay;
  FILE *perfdata;

  set_program_name (argv[0]);

  while ((c = getopt_long (argc, argv,
			   "Cbdei:klmpW%" GETOPT_HELP_VERSION_STRING,
			   longopts, &option_index)) != -1)
    {
      switch (c)
	{
	default:
	  usage (stderr);
	case 0:
	  if (STREQ (longopts[option_index].name, "ifname-debug"))
	    ifname_debug = true;
	  break;
	case 'b':
	  pd_bytes = false;
	  break;
	case 'C':
	  pd_collisions = false;
	  break;
	case 'd':
	  pd_drops = false;
	  break;
	case 'e':
	  pd_errors = false;
	  break;
	case 'i':
	  ifname_regex = xstrdup (optarg);
	  break;
	case 'k':
	  options |= CHECK_LINK;
	  break;
	case 'l':
	  options |= NO_LOOPBACK;
	  break;
	case 'm':
	  pd_multicast = false;
	  break;
	case 'p':
	  pd_packets = false;
	  break;
	case 'W':
	  options |= NO_WIRELESS;
	  break;
	case '%':
	  report_perc = true;
	  break;
	case_GETOPT_HELP_CHAR
	case_GETOPT_VERSION_CHAR

	}
    }

  delay = ifname_debug ? 0 : DELAY_DEFAULT;
  if (optind < argc)
    {
      if (ifname_debug)
	usage (stderr);

      delay = strtol_or_err (argv[optind++], "failed to parse argument");
      if (delay < 1)
	plugin_error (STATE_UNKNOWN, 0, "delay must be positive integer");
      else if (DELAY_MAX < delay)
	plugin_error (STATE_UNKNOWN, 0,
                      "too large delay value (greater than %d)", DELAY_MAX);
    }

  unsigned int ninterfaces;
  struct iflist *ifl, *iflhead =
    netinfo (options, ifname_regex, delay, &ninterfaces);

  if (ninterfaces < 1)
    status = STATE_UNKNOWN;

#define __printf_tx_rx__(metric) \
  do                                                 \
    {                                                \
      printf (" - %s_tx%s\t ", ifl->ifname, metric); \
      printf ("%s_rx%s\n", ifl->ifname, metric);     \
    }                                                \
  while (0)

  if (ifname_debug)
    {
      for (ifl = iflhead; ifl != NULL; ifl = ifl->next)
	{
	  printf ("%s\n", ifl->ifname);
	  if (pd_bytes)
	    __printf_tx_rx__ ("byte/s");
	  if (pd_errors)
	    __printf_tx_rx__ ("err/s");
	  if (pd_drops)
	    __printf_tx_rx__ ("drop/s");
	  if (pd_packets)
	    __printf_tx_rx__ ("pck/s");
	  if (pd_collisions)
	    printf (" - %s_coll/s\n", ifl->ifname);
	  if (pd_multicast)
	    printf (" - %s_mcast/s\n", ifl->ifname);
	}
      exit (STATE_UNKNOWN);
    }

#undef __printf_tx_rx__

  perfdata = open_memstream (&bp, &size);

  /* performance data format:
   * 'label'=value[UOM];[warn];[crit];[min];[max] */
  for (ifl = iflhead; ifl != NULL; ifl = ifl->next)
    {
      unsigned long long speed =
 	(ifl->speed > 0) ? ifl->speed * 1000*1000/8 : 0;
      if (DUPLEX_HALF == ifl->duplex)
	speed /= 2;

      if (pd_bytes)
	{
	  char *perfdata_txbyte =
	    fmt_perfdata_bytes (ifl->ifname, "txbyte", ifl->tx_bytes,
				speed, report_perc);
	  char *perfdata_rxbyte =
	    fmt_perfdata_bytes (ifl->ifname, "rxbyte", ifl->rx_bytes,
				speed, report_perc);
	  fprintf (perfdata, "%s %s ", perfdata_txbyte, perfdata_rxbyte);
	  free (perfdata_txbyte);
	  free (perfdata_rxbyte);
	}
      if (pd_errors)
	fprintf (perfdata
		, "%s_txerr/s=%u %s_rxerr/s=%u "
		, ifl->ifname, ifl->tx_errors
		, ifl->ifname, ifl->rx_errors);
      if (pd_drops)
	fprintf (perfdata
		, "%s_txdrop/s=%u %s_rxdrop/s=%u "
		, ifl->ifname, ifl->tx_dropped
		, ifl->ifname, ifl->rx_dropped);
      if (pd_packets)
        fprintf (perfdata
		 , "%s_rxpck/s=%u %s_txpck/s=%u "
		 , ifl->ifname, ifl->rx_packets
		 , ifl->ifname, ifl->tx_packets);
      if (pd_collisions)
        fprintf (perfdata, "%s_coll/s=%u "
		 , ifl->ifname, ifl->collisions);
      if (pd_multicast)
        fprintf (perfdata, "%s_mcast/s=%u "
		 , ifl->ifname, ifl->multicast);
    }

  fclose (perfdata);

  printf ("%s %s - found %u interface(s) ("
	  , program_name_short
	  , state_text (status)
	  , ninterfaces);
  for (ifl = iflhead, i=0; ifl != NULL; ifl = ifl->next, i++)
    if (i < MAX_PRINTED_INTERFACES)
      printf ("%s%s", i < 1 ? "" : ",", ifl->ifname);
    else
      {
	printf (",...");
	break;
      }
  printf (") | %s\n", bp);

  freeiflist (iflhead);

  return status;
}
