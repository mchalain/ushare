/*
 * http.c : GeeXboX uShare Web Server handler.
 * Originally developped for the GeeXboX project.
 * Parts of the code are originated from GMediaServer from Oskar Liljeblad.
 * Copyright (C) 2005-2007 Benjamin Zores <ben@geexbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "metadata.h"
#include "minmax.h"
#include "trace.h"
#include "presentation.h"
#include "osdep.h"
#include "mime.h"

#define PROTOCOL_TYPE_PRE_SZ  11   /* for the str length of "http-get:*:" */
#define PROTOCOL_TYPE_SUFF_SZ 2    /* for the str length of ":*" */

typedef struct web_file_s {
  off_t pos;
  char *contents;
  off_t len;
} web_file_t;

static int
http_read (void *hdl, void *buf, size_t buflen);
static off_t
http_lseek (void *hdl, off_t offset, int origin);
static void
http_cleanup (void *hdl);
static void
http_close (void *hdl);

static dlna_stream_t *
get_file_memory (const char *url, const char *description,
                 const size_t length, const char *content_type)
{
  dlna_stream_t *stream = NULL;

  stream = calloc (1, sizeof (dlna_stream_t));
  stream->fd = 0;
  stream->url = strdup (url);
  stream->read = http_read;
  stream->lseek = http_lseek;
  stream->cleanup = http_cleanup;
  stream->close = http_close;
  strcpy (stream->mime, content_type);
  stream->length = length;

  web_file_t *file;

  file = malloc (sizeof (web_file_t));
  file->pos = 0;
  /* description is stored into a buffer object */
  /* the buffer is allocated for different pages (presentation, cgi) */
  /* the web server run in multi threading */
  /* than it possible that the contain of the buffer changes before than the previous is closed */
  file->contents = strdup (description);
  file->len = length;

  stream->private = file;
  return stream;
}

static dlna_stream_t *
http_open (void *cookie, const char *filename)
{
  ushare_t *data = (ushare_t *)cookie;

  if (!filename)
    return NULL;

  log_verbose ("http_open, filename : %s\n", filename);

  if (data->use_presentation)
  {
        printf ("coucou %s %s\n", filename, USHARE_PRESENTATION_PAGE);
    if (strstr (filename, USHARE_PRESENTATION_PAGE) != NULL)
    {
        printf ("coucou\n");
      if (build_presentation_page (data) < 0)
        return NULL;
        printf ("coucou\n");
      return get_file_memory (USHARE_PRESENTATION_PAGE, data->presentation->buf,
                            data->presentation->len, PRESENTATION_PAGE_CONTENT_TYPE);
    }
    else if (strstr (filename, USHARE_CGI) != NULL)
    {
      if (process_cgi (data, (char *) (filename + strlen (USHARE_CGI) + 1)) < 0)
        return NULL;
      return get_file_memory (USHARE_PRESENTATION_PAGE, data->presentation->buf,
                            data->presentation->len, PRESENTATION_PAGE_CONTENT_TYPE);
    }
  }
  return NULL;
}

static int
http_read (void *hdl, void *buf, size_t buflen)
{
  dlna_stream_t *stream = hdl;
  web_file_t *file = (web_file_t *) stream->private;
  ssize_t len = -1;

  log_verbose ("http_read\n");

  if (!file)
    return -1;

  len = (size_t) MIN (buflen, file->len - file->pos);
  memcpy (buf, file->contents + file->pos, (size_t) len);

  if (len >= 0)
    file->pos += len;

  log_verbose ("Read %zd bytes.\n", len);

  return len;
}

static off_t
http_lseek (void *hdl, off_t offset, int origin)
{
  dlna_stream_t *stream = hdl;
  web_file_t *file = (web_file_t *) stream->private;
  off_t newpos = -1;

  log_verbose ("http_seek\n");

  if (!file)
    return -1;

  switch (origin)
  {
  case SEEK_SET:
    log_verbose ("Attempting to seek to %lld (was at %lld) in %s\n",
                offset, file->pos, stream->url);
    newpos = offset;
    break;
  case SEEK_CUR:
    log_verbose ("Attempting to seek by %lld from %lld in %s\n",
                offset, file->pos, stream->url);
    newpos = file->pos + offset;
    break;
  case SEEK_END:
    log_verbose ("Attempting to seek by %lld from end (was at %lld) in %s\n",
                offset, file->pos, stream->url);

    newpos = file->len + offset;
    break;
  }

  if (newpos < 0 || newpos > file->len)
  {
    log_verbose ("%s: cannot seek: %s\n", stream->url, strerror (EINVAL));
    return -1;
  }

  file->pos = newpos;

  return 0;
}

static void
http_cleanup (void *hdl)
{
  dlna_stream_t *stream = hdl;
  web_file_t *file = (web_file_t *) stream->private;

  file->len = 0;
  file->pos = 0;
}

static void
http_close (void *hdl)
{
  dlna_stream_t *stream = hdl;
  web_file_t *file = (web_file_t *) stream->private;

  log_verbose ("http_close\n");

  if (!file)
    return;

  if (file->contents)
    free (file->contents);
  free (file);
  free (stream);
}

dlna_http_callback_t ushare_http_callbacks = {
  .open = http_open,
  .next = NULL,
};
