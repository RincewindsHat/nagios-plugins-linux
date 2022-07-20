// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * License: GPLv3+
 * Copyright (c) 2022 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A library for checking the Linux Pressure Stall Information (PSI)
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "files.h"
#include "logging.h"
#include "messages.h"
#include "string-macros.h"
#include "xalloc.h"
#include "xasprintf.h"

static int deep = 0;

static int
files_filematch (const char *pattern, const char *name)
{
  if (pattern != NULL)
    {
      int status = fnmatch (pattern, name, /* flags = */ 0);
      if (status != 0)
	{
	  dbg ("(i)    name `%s' does not match pattern `%s'\n",
	       name, pattern);
	  return FNM_NOMATCH;
	}
	dbg ("(i)    name `%s' does MATCH pattern `%s'\n",
	     name, pattern);
    }

  return 0;
}

int
files_filecount (const char *dir, unsigned int flags,
		 int64_t age, int64_t size, const char *pattern)
{
  int filecount = 0, status;
  DIR *dirp;
  time_t now;

  errno = 0;
  if ((dirp = opendir (dir)) == NULL)
    {
      dbg ("(e) cannot open %s (%s)\n", dir, strerror (errno));
      return -1;
    }

  now = time (NULL);

  if (age != 0)
    dbg ("(i) looking for files that were touched %s %u seconds ago...\n"
	 , (age < 0) ? "less than" : "more than"
	 , (age < 0) ? (unsigned int)(-age) : (unsigned int)age);
  if (size != 0)
    dbg ("(i) looking for files with size %s than %ld bytes...\n"
	 , (size < 0) ? "less" : "greater"
	 , (size < 0) ? -size : size);
  if (pattern != NULL)
    dbg ("(i) looking for files that math the pattern `%s'...\n", pattern);

  /* Scan entries under the 'dirp' directory */
  for (;;)
    {
      char abs_path[PATH_MAX];
      struct dirent *dp;
      struct stat statbuf;
      errno = 0;

      if ((dp = readdir (dirp)) == NULL)
	{
	  if (errno != 0)
	    plugin_error (STATE_UNKNOWN, errno, "readdir() failure");
	  else
	    break;		/* end-of-directory */
	}

      /* ignore directory entries */
      if (STREQ (dp->d_name, ".") || STREQ (dp->d_name, ".."))
	continue;

      if (!(flags & FILES_INCLUDE_HIDDEN) && (dp->d_name[0] == '.'))
	continue;

      snprintf (abs_path, sizeof (abs_path), "%s/%s", dir, dp->d_name);

      status = lstat (abs_path, &statbuf);
      if (status != 0)
	plugin_error (STATE_UNKNOWN, errno, "lstat (%s) failed", abs_path);

      switch (statbuf.st_mode & S_IFMT)
	{
	default:
	  if (flags & FILES_IGNORE_UNKNOWN)
	    continue;
	  break;
	case S_IFBLK:
	case S_IFCHR:
	case S_IFIFO:
	case S_IFSOCK:
	  dbg ("(%d) %s (special file)\n", deep, abs_path);
	  if (flags & FILES_REGULAR_ONLY)
	    continue;
	  break;
	case S_IFDIR:
	  dbg ("(%d) %s (directory)\n", deep, abs_path);
	  if (flags & FILES_RECURSIVE)
	    {
	      char *subdir = xasprintf ("%s/%s", dir, dp->d_name);
	      if (!(flags & FILES_REGULAR_ONLY))
		{
		  if (0 == files_filematch (pattern, dp->d_name))
		    filecount++;
		  dbg ("(%d)  --> #%d\n", deep, filecount);
		}

	      deep++;
	      dbg ("+ recursive call of files_filecount for %s\n", subdir);
	      int partial = files_filecount (subdir, flags, age, size, pattern);
	      if (partial > 0)
		{
		  filecount += partial;
		  dbg ("(%d)  --> #%d\n", deep, filecount);
		}

	      free (subdir);
	      continue;
	    }
	  if (flags & FILES_REGULAR_ONLY)
	    continue;
	  break;
	case S_IFLNK:
	  dbg ("(%d) %s (symlink)\n", deep, abs_path);
	  if (flags & (FILES_IGNORE_SYMLINKS | FILES_REGULAR_ONLY))
	    continue;
	  break;
	case S_IFREG:
	  dbg ("(%d) %s (regular file)\n", deep, abs_path);
	  break;
	}

      if (age != 0)
	{
	  time_t mtime = now;
	  if (age < 0)
	    mtime += age;
	  else
	    mtime -= age;

	  dbg ("(%d) %s file touched %d seconds ago\n"
	       , deep
	       , abs_path
	       , (int)(now - statbuf.st_mtime));

	  if (((age < 0) && (statbuf.st_mtime < mtime)) ||
	      ((age > 0) && (statbuf.st_mtime > mtime)))
	    continue;
	}
      if (size != 0)
	{
	  off_t abs_size;
	  if (size < 0)
	    abs_size = (off_t) ((-1) * size);
	  else
	    abs_size = (off_t) size;

	  dbg ("(%d) %s file size: %ld bytes\n"
	       , deep
	       , abs_path
	       , statbuf.st_size);

	  if (((size < 0) && (statbuf.st_size > abs_size)) ||
	      ((size > 0) && (statbuf.st_size < abs_size)))
	    continue;
	}

      if (files_filematch (pattern, dp->d_name) != 0)
	continue;

      filecount++;
      dbg ("(%d)  --> #%d\n", deep, filecount);
    }

  dbg ("- return #%d (%s)\n", filecount, dir);
  deep--;
  closedir (dirp);
  return filecount;
}