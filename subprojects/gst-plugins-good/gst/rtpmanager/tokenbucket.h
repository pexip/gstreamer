/* GStreamer
* Copyright (C) 2021 Pexip (http://pexip.com/)
*   @author: Havard Graff <havard@pexip.com>
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Library General Public
* License as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Library General Public License for more details.
*
* You should have received a copy of the GNU Library General Public
* License along with this library; if not, write to the
* Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
* Boston, MA 02110-1301, USA.
*/
#ifndef __TOKEN_BUCKET_H__
#define __TOKEN_BUCKET_H__

#include <gst/gst.h>

typedef struct _TokenBucket TokenBucket;

struct _TokenBucket
{
  gint64 max_bucket_size;
  gint64 bps;
  GstClockTime prev_time;
  gint64 bucket_size;
};

void token_bucket_init (TokenBucket * tb, gint64 bps, gint max_bucket_size);
void token_bucket_set_bps (TokenBucket * tb, gint64 bps);
void token_bucket_set_max_bucket_size (TokenBucket * tb, gint64 bps);
void token_bucket_reset (TokenBucket * tb);
void token_bucket_add_tokens (TokenBucket * tb, GstClockTime now);
GstClockTime token_bucket_get_missing_tokens_time (TokenBucket * tb, gint tokens);
gboolean token_bucket_take_tokens (TokenBucket * tb, gint tokens, gboolean force);

#endif /* __TOKEN_BUCKET_H__ */
