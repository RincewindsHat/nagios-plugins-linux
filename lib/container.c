/*
 * License: GPLv3+
 * Copyright (c) 2018 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A library for checking sysfs for Docker exposed metrics.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE		/* activate extra prototypes for glibc */
#endif

#ifndef NPL_TESTING
static const char *docker_socket = DOCKER_SOCKET;
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "common.h"
#include "collection.h"
#include "logging.h"
#include "messages.h"
#include "string-macros.h"
#include "system.h"
#include "url_encode.h"
#include "xalloc.h"
#include "xasprintf.h"

#include "json.h"

#define DOCKER_CONTAINERS_JSON  0x01

typedef struct chunk
{
  char *memory;
  size_t size;
} chunk_t;

static int
json_eq (const char *json, jsmntok_t * tok, const char *s)
{
  if (tok->type == JSMN_STRING && (int) strlen (s) == tok->end - tok->start &&
      strncmp (json + tok->start, s, tok->end - tok->start) == 0)
    return 0;

  return -1;
}

/* parse the json stream returned by Docker and return a pointer to the
   hashtable containing the values of the discovered 'tokens'.
   return NULL if the data cannot be parsed.  */

static hashtable_t *
json_parser (const char *json, const char *token)
{
  int i, r;
  jsmn_parser parser;
  hashtable_t *hashtable = NULL;

  jsmn_init (&parser);
  r = jsmn_parse (&parser, json, strlen (json), NULL, 0);
  if (r < 0)
    return NULL;

  jsmntok_t *buffer = xnmalloc (r, sizeof (jsmntok_t));
  dbg ("number of json tokens: %d\n", r);

  jsmn_init (&parser);
  jsmn_parse (&parser, json, strlen (json), buffer, r);

  hashtable = counter_create ();

  for (i = 1; i < r; i++)
    {
      if (0 == json_eq (json, &buffer[i], token))
	{
	  size_t strsize = buffer[i + 1].end - buffer[i + 1].start;
	  char *value = xmalloc (strsize + 1);
	  memcpy (value, json + buffer[i + 1].start, strsize);

	  dbg ("found token \"%s\" with value \"%s\" at position %d\n",
	       token, value, buffer[i].start);
	  counter_put (hashtable, value);
	}
    }

  return hashtable;
}

#ifndef NPL_TESTING

static size_t
write_memory_callback (void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  chunk_t *mem = (chunk_t *) userp;

  mem->memory = realloc (mem->memory, mem->size + realsize + 1);
  if (NULL == mem->memory)
    plugin_error (STATE_UNKNOWN, errno, "memory exhausted");

  memcpy (&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

static void
docker_init (CURL ** curl_handle, chunk_t * chunk)
{
  chunk->memory = malloc (1);	/* will be grown as needed by the realloc above */
  chunk->size = 0;		/* no data at this point */

  curl_global_init (CURL_GLOBAL_ALL);

  /* init the curl session */
  *curl_handle = curl_easy_init ();
  if (NULL == (*curl_handle))
    plugin_error (STATE_UNKNOWN, errno,
		  "cannot start a libcurl easy session");

  curl_easy_setopt (*curl_handle, CURLOPT_UNIX_SOCKET_PATH, docker_socket);
  dbg ("CURLOPT_UNIX_SOCKET_PATH is set to \"%s\"\n", docker_socket);

  /* send all data to this function */
  curl_easy_setopt (*curl_handle, CURLOPT_WRITEFUNCTION,
		    write_memory_callback);
  curl_easy_setopt (*curl_handle, CURLOPT_WRITEDATA, (void *) chunk);

  /* some servers don't like requests that are made without a user-agent
     field, so we provide one */
  curl_easy_setopt (*curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
}

static CURLcode
docker_get (CURL * curl_handle, const int query)
{
  CURLcode res;
  char *api_version, *class, *url, *filter = NULL;

  switch (query)
    {
      default:
	plugin_error (STATE_UNKNOWN, 0, "unknown docker query");
      case DOCKER_CONTAINERS_JSON:
	api_version = "1.18";
	filter = url_encode ("{\"status\":{\"running\":true}}");
	class = "containers/json";
	url = filter ?
	  xasprintf ("http://v%s/%s?filters=%s", api_version, class, filter) :
	  xasprintf ("http://v%s/%s", api_version, class);
	break;
    }

  dbg ("docker rest url: %s\n", url);

  curl_easy_setopt (curl_handle, CURLOPT_URL, url);
  res = curl_easy_perform (curl_handle);

  free (filter);
  free (url);

  return res;
}

static void
docker_close (CURL * curl_handle, chunk_t * chunk)
{
  /* cleanup curl stuff */
  curl_easy_cleanup (curl_handle);

  free (chunk->memory);

  /* we're done with libcurl, so clean it up */
  curl_global_cleanup ();
}

#else

static void
docker_get (chunk_t * chunk, const int query)
{
  char *filename = NULL;

  switch (query)
    {
      default:
	/* FIXME: do signal something is broken */
	break;
      case DOCKER_CONTAINERS_JSON:
	filename = NPL_TEST_PATH_CONTAINER_JSON;
	break;
    }

  chunk->memory = test_fstringify (filename);
  chunk->size = strlen (chunk->memory);
}

static void
docker_close (chunk_t * chunk)
{
  free (chunk->memory);
}

#endif /* NPL_TESTING */

/* Returns the number of running Docker containers  */

unsigned int
docker_running_containers (const char *image, char **perfdata, bool verbose)
{
  chunk_t chunk;
  hashtable_t *hashtable;
  unsigned int running_containers = 0;

#ifndef NPL_TESTING

  CURL *curl_handle = NULL;
  CURLcode res;

  docker_init (&curl_handle, &chunk);
  res = docker_get (curl_handle, DOCKER_CONTAINERS_JSON);
  if (CURLE_OK != res)
    {
      docker_close (curl_handle, &chunk);
      plugin_error (STATE_UNKNOWN, errno, "%s", curl_easy_strerror (res));
    }

#else

  docker_get (&chunk, DOCKER_CONTAINERS_JSON);

#endif /* NPL_TESTING */

  assert (chunk.memory);
  dbg ("%lu bytes retrieved\n", chunk.size);
  dbg ("json data: %s", chunk.memory);

  hashtable = json_parser (chunk.memory, "Image");
  if (NULL == hashtable)
    plugin_error (STATE_UNKNOWN, 0,
		  "unable to parse the json data for \"Image\"s");
  dbg ("number of docker unique images: %u\n",
       counter_get_unique_elements (hashtable));

  if (image)
    {
      hashable_t *np = counter_lookup (hashtable, image);
      assert (NULL != np);
      running_containers = np ? np->count : 0;
      *perfdata = xasprintf ("containers_%s=%u", image, running_containers);
    }
  else
    {
      running_containers = counter_get_elements (hashtable);
      size_t size;
      FILE *stream = open_memstream (perfdata, &size);
      for (unsigned int j = 0; j < hashtable->uniq; j++)
	{
	  hashable_t *np = counter_lookup (hashtable, hashtable->keys[j]);
	  assert (NULL != np);
	  fprintf (stream, "containers_%s=%u ",
		   hashtable->keys[j], np->count);
	}
      fprintf (stream, "containers_total=%u", hashtable->elements);
      fclose (stream);
    }

  counter_free (hashtable);

  docker_close (
#ifndef NPL_TESTING
		 curl_handle,
#endif
		 &chunk);

  return running_containers;
}
