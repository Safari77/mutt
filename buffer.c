/*
 * Copyright (C) 1996-2002,2010,2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
 * Copyright (C) 2018,2020 Kevin J. McCarthy <kevin@8t8.us>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "buffer.h"


static size_t BufferPoolCount = 0;
static size_t BufferPoolLen  = 0;
static BUFFER **BufferPool = NULL;


/* Creates and initializes a BUFFER */
BUFFER *mutt_buffer_new(void)
{
  BUFFER *b;

  b = safe_malloc(sizeof(BUFFER));

  mutt_buffer_init(b);

  return b;
}

/* Initialize a new BUFFER */
BUFFER *mutt_buffer_init(BUFFER *b)
{
  memset(b, 0, sizeof(BUFFER));
  return b;
}

void mutt_buffer_free(BUFFER **p)
{
  if (!p || !*p)
    return;

  FREE(&(*p)->data);
  /* dptr is just an offset to data and shouldn't be freed */
  FREE(p);             /* __FREE_CHECKED__ */
}

void mutt_buffer_clear(BUFFER *b)
{
  b->dptr = b->data;
  if (b->dptr)
    *(b->dptr) = '\0';
}

/* This is used for cases where the buffer is read from.
 * A value is placed in the buffer, and then b->dptr is set back to the
 * beginning as a read marker instead of write marker.
 */
void mutt_buffer_rewind(BUFFER *b)
{
  b->dptr = b->data;
}

/* Creates and initializes a BUFFER by copying the seed string. */
BUFFER *mutt_buffer_from(const char *seed)
{
  BUFFER *b;

  if (!seed)
    return NULL;

  b = mutt_buffer_new();
  b->data = safe_strdup(seed);
  b->dsize = mutt_strlen(seed) + 1;
  b->dptr = b->data + (b->dsize - 1);
  return b;
}

size_t mutt_buffer_len(BUFFER *buf)
{
  return buf->dptr - buf->data;
}

/* Increases the allocated size of the buffer */
void mutt_buffer_increase_size(BUFFER *buf, size_t new_size)
{
  size_t offset;
  int was_null;

  if (!buf)
    return;

  if (buf->dsize < new_size)
  {
    was_null = (buf->data == NULL);
    offset = buf->data ? (buf->dptr - buf->data) : 0;
    buf->dsize = new_size;
    safe_realloc(&buf->data, buf->dsize);
    buf->dptr = buf->data + offset;
    /* This ensures an initially NULL buf->data is now properly terminated. */
    if (was_null)
      *(buf->dptr) = '\0';
  }
}

/* Ensure buffer->dptr points to the end of the buffer. */
void mutt_buffer_fix_dptr(BUFFER *buf)
{
  if (!buf)
    return;

  buf->dptr = buf->data;

  if (buf->data && buf->dsize > 0)
  {
    buf->data[buf->dsize - 1] = '\0';
    buf->dptr = strchr(buf->data, '\0');
  }
}

static int _mutt_buffer_add_printf(BUFFER *buf, const char *fmt, va_list ap)
{
  va_list ap_retry;
  int len;
  size_t blen, doff;

  if (!buf || !fmt)
    return -1;

  va_copy(ap_retry, ap);

  if (!buf->dptr)
    buf->dptr = buf->data;

  /* Avoid subtracting NULL pointers (UB) and use size_t for offsets */
  doff = buf->data ? (size_t)(buf->dptr - buf->data) : 0;
  blen = (buf->data && buf->dsize > doff) ? (buf->dsize - doff) : 0;

  /* solaris 9 vsnprintf barfs when blen is 0 */
  if (!blen)
  {
    blen = 128;
    mutt_buffer_increase_size(buf, buf->dsize + blen);
    blen = buf->dsize - doff;
  }

  len = vsnprintf(buf->dptr, blen, fmt, ap);
  if (len < 0)
  {
    va_end(ap_retry);
    return len;
  }

  if ((size_t)len >= blen)
  {
    size_t add = ((size_t)len + 1) - blen;
    if (add < 128)
      add = 128;
    mutt_buffer_increase_size(buf, buf->dsize + add);
    len = vsnprintf(buf->dptr, buf->dsize - doff, fmt, ap_retry);
  }

  if (len > 0)
    buf->dptr += len;

  va_end(ap_retry);

  return len;
}

int mutt_buffer_printf(BUFFER *buf, const char *fmt, ...)
{
  va_list ap;
  int rv;

  va_start(ap, fmt);
  mutt_buffer_clear(buf);
  rv = _mutt_buffer_add_printf(buf, fmt, ap);
  va_end(ap);

  return rv;
}

int mutt_buffer_add_printf(BUFFER *buf, const char *fmt, ...)
{
  va_list ap;
  int rv;

  va_start(ap, fmt);
  rv = _mutt_buffer_add_printf(buf, fmt, ap);
  va_end(ap);

  return rv;
}

/* Dynamically grows a BUFFER to accommodate s, in increments of 128 bytes.
 * Always one byte bigger than necessary for the null terminator, and
 * the buffer is always null-terminated */
void mutt_buffer_addstr_n(BUFFER *buf, const char *s, size_t len)
{
  size_t offset = 0;
  size_t dptr_offset;
  int is_overlap;

  if (!buf)
    return;

  if (!s || len == 0)
  {
    if (!buf->data)
      mutt_buffer_increase_size(buf, 128);
    return;
  }

  if (!buf->dptr)
    buf->dptr = buf->data;

  dptr_offset = buf->data ? (size_t)(buf->dptr - buf->data) : 0;
  is_overlap = (buf->data && s >= buf->data && s < buf->data + buf->dsize);
  if (is_overlap)
    offset = (size_t)(s - buf->data);

  if (!buf->data || (dptr_offset + len + 1 > buf->dsize))
  {
    mutt_buffer_increase_size(buf, buf->dsize + (len < 128 ? 128 : len + 1));
    /* If realloc moved buf->data, update s so it doesn't point to freed memory */
    if (is_overlap)
      s = buf->data + offset;
  }

  /* Use memmove instead of memcpy to safely handle overlapping buffers */
  memmove(buf->dptr, s, len);
  buf->dptr += len;
  *(buf->dptr) = '\0';
}

void mutt_buffer_addstr(BUFFER *buf, const char *s)
{
  mutt_buffer_addstr_n(buf, s, mutt_strlen(s));
}

void mutt_buffer_addch(BUFFER *buf, char c)
{
  mutt_buffer_addstr_n(buf, &c, 1);
}

void mutt_buffer_strcpy_n(BUFFER *buf, const char *s, size_t len)
{
  if (!buf)
    return;

  /* Rewind write pointer without truncating buf->data[0] before s is read */
  buf->dptr = buf->data;

  if (!s || len == 0)
  {
    if (buf->dptr)
      *(buf->dptr) = '\0';
    return;
  }

  mutt_buffer_addstr_n(buf, s, len);
}

void mutt_buffer_strcpy(BUFFER *buf, const char *s)
{
  if (!buf)
    return;

  /* Compute length before touching buf so s is not modified if it points inside buf */
  mutt_buffer_strcpy_n(buf, s, mutt_strlen(s));
}

void mutt_buffer_substrcpy(BUFFER *buf, const char *beg, const char *end)
{
  if (!buf)
    return;

  if (beg && end && end > beg)
    mutt_buffer_strcpy_n(buf, beg, (size_t)(end - beg));
  else
    mutt_buffer_clear(buf);
}

static void increase_buffer_pool(void)
{
  BUFFER *newbuf;
  size_t target;

  BufferPoolLen += 5;
  safe_realloc(&BufferPool, BufferPoolLen * sizeof(BUFFER *));
  target = BufferPoolCount + 5;
  while (BufferPoolCount < target)
  {
    newbuf = mutt_buffer_new();
    mutt_buffer_increase_size(newbuf, LONG_STRING);
    mutt_buffer_clear(newbuf);
    BufferPool[BufferPoolCount++] = newbuf;
  }
}

void mutt_buffer_pool_init(void)
{
  increase_buffer_pool();
}

void mutt_buffer_pool_free(void)
{
  muttdbg(1, "%zu of %zu returned to pool",
          BufferPoolCount, BufferPoolLen);

  while (BufferPoolCount)
    mutt_buffer_free(&BufferPool[--BufferPoolCount]);
  FREE(&BufferPool);
  BufferPoolLen = 0;
}

BUFFER *mutt_buffer_pool_get(void)
{
  if (!BufferPoolCount)
    increase_buffer_pool();
  return BufferPool[--BufferPoolCount];
}

void mutt_buffer_pool_release(BUFFER **pbuf)
{
  BUFFER *buf;

  if (!pbuf || !*pbuf)
    return;

  if (BufferPoolCount >= BufferPoolLen)
  {
    muttdbg(1, "Internal buffer pool error");
    mutt_buffer_free(pbuf);
    return;
  }

  buf = *pbuf;
  if ((buf->dsize > LONG_STRING*2) || (buf->dsize < LONG_STRING))
  {
    buf->dsize = LONG_STRING;
    safe_realloc(&buf->data, buf->dsize);
  }
  mutt_buffer_clear(buf);
  BufferPool[BufferPoolCount++] = buf;

  *pbuf = NULL;
}
