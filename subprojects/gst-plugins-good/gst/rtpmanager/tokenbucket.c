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
#include "tokenbucket.h"

void
token_bucket_reset (TokenBucket * tb)
{
  tb->prev_time = GST_CLOCK_TIME_NONE;
  tb->bucket_size = 0;
}

void
token_bucket_set_bps (TokenBucket * tb, gint64 bps)
{
  tb->bps = bps;
}

void
token_bucket_set_max_bucket_size (TokenBucket * tb, gint64 max_bucket_size)
{
  tb->max_bucket_size = max_bucket_size;
}

void
token_bucket_init (TokenBucket * tb, gint64 bps, gint max_bucket_size)
{
  token_bucket_reset (tb);
  token_bucket_set_bps (tb, bps);
  token_bucket_set_max_bucket_size (tb, max_bucket_size);
}

void
token_bucket_add_tokens (TokenBucket * tb, GstClockTime now)
{
  gint64 tokens = 0;
  GstClockTimeDiff elapsed_time = 0;

  if (!GST_CLOCK_TIME_IS_VALID (now))
    return;

  /* get the elapsed time */
  if (GST_CLOCK_TIME_IS_VALID (tb->prev_time)) {
    if (now < tb->prev_time) {
      GST_INFO ("We have already produced tokens for this time "
          "(%" GST_TIME_FORMAT " < %" GST_TIME_FORMAT ")",
          GST_TIME_ARGS (now), GST_TIME_ARGS (tb->prev_time));
    } else {
      elapsed_time = GST_CLOCK_DIFF (tb->prev_time, now);
    }
  }

  tb->prev_time = now;

  /* check for umlimited bps and fill the bucket if that is the case */
  if (tb->bps == -1) {
    if (tb->max_bucket_size != -1)
      tb->bucket_size = tb->max_bucket_size;
    return;
  }

  /* no bitrate, no bits to add */
  if (tb->bps == 0) {
    return;
  }

  /* no extra time to add tokens for */
  if (elapsed_time == 0) {
    return;
  }

  /* calculate number of tokens and add them to the bucket */
  tokens = gst_util_uint64_scale_round (elapsed_time, tb->bps, GST_SECOND);
  tb->bucket_size += tokens;
  if (tb->max_bucket_size != -1 && tb->bucket_size > tb->max_bucket_size) {
    tb->bucket_size = tb->max_bucket_size;
  }
  GST_LOG ("Added %" G_GINT64_FORMAT " tokens to bucket "
      "(contains %" G_GINT64_FORMAT " tokens)", tokens, tb->bucket_size);

  GST_LOG ("Elapsed time: %" GST_TIME_FORMAT " produces %" G_GINT64_FORMAT
      " tokens, new prev_time: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (elapsed_time), tokens, GST_TIME_ARGS (tb->prev_time));
}

GstClockTime
token_bucket_get_missing_tokens_time (TokenBucket * tb, gint tokens)
{
  GstClockTimeDiff ret;
  gint missing_tokens;

  /* unlimited bits per second and unlimited bucket size
     means tokens are always available */
  if (tb->bps == -1 && tb->max_bucket_size == -1)
    return 0;

  /* we we have room in our bucket, no need to wait */
  if (tb->bucket_size >= tokens)
    return 0;

  /* lets not divide by 0 */
  if (tb->bps == 0)
    return 0;

  missing_tokens = tokens - tb->bucket_size;
  ret = gst_util_uint64_scale (GST_SECOND, missing_tokens, tb->bps);

  return ret;
}

gboolean
token_bucket_take_tokens (TokenBucket * tb, gint tokens, gboolean force)
{
  /* unlimited bits per second and unlimited bucket size
     means tokens are always available */
  if (tb->bps == -1 && tb->max_bucket_size == -1)
    return TRUE;

  /* if we force it, or we have enough tokens, remove
     the tokens from the bucket */
  if (force || tb->bucket_size >= tokens) {
    GST_LOG ("Removing %u tokens from bucket (%" G_GINT64_FORMAT ")",
        tokens, tb->bucket_size);
    tb->bucket_size -= tokens;
    return TRUE;
  }

  return FALSE;
}
