/* GStreamer
 *
 * unit test for vp9dec
 *
 * Copyright (C) 2026 Pexip AS.
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

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>

#define N_FRAMES 7

static GstBuffer *
pull_encoded_frame (GstHarness * enc, gboolean keyframe)
{
  GstBuffer *buf;

  gst_harness_src_crank_and_push_many (enc, 1, 1);
  buf = gst_harness_pull (enc);
  fail_unless_equals_int (keyframe,
      !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT));
  return buf;
}

static gint
pop_key_unit_requests (GstHarness * h)
{
  GstEvent *ev;
  gint count = 0;

  while ((ev = gst_harness_try_pull_upstream_event (h)) != NULL) {
    if (gst_video_event_is_force_key_unit (ev))
      count++;
    gst_event_unref (ev);
  }
  return count;
}

/* Pushes frames[first..last] and returns how many times the decoder asked
 * for a key frame (once per frame libvpx failed to decode) and how many
 * decoded frames came out */
static void
push_frames (GstHarness * h, GstBuffer ** frames, gint first, gint last,
    gint * requests, gint * decoded)
{
  gint i;

  for (i = first; i <= last; i++)
    fail_unless_equals_int (GST_FLOW_OK,
        gst_harness_push (h, gst_buffer_ref (frames[i])));
  *requests = pop_key_unit_requests (h);
  *decoded = gst_harness_buffers_in_queue (h);
  while (gst_harness_buffers_in_queue (h) > 0)
    gst_buffer_unref (gst_harness_pull (h));
}

/* A delta frame that libvpx fails to decode leaves the decoder waiting for a
 * key frame. VP9 needs a sync point, so the delta frames that follow must be
 * discarded before they reach libvpx: only the damaged frame fails and asks
 * for a key frame. */
GST_START_TEST (test_decode_drops_delta_frames_after_error)
{
  GstHarness *h, *enc;
  GstBuffer *frames[N_FRAMES];
  gint requests, decoded, i;

  enc = gst_harness_new ("vp9enc");
  g_object_set (enc->element, "lag-in-frames", 0, "cpu-used", 8, "deadline",
      G_GINT64_CONSTANT (1), "keyframe-max-dist", 9999,
      "min-force-key-unit-interval", G_GUINT64_CONSTANT (0), NULL);
  gst_harness_add_src_parse (enc, "videotestsrc is-live=true ! "
      "capsfilter caps=\"video/x-raw,format=I420,width=320,height=240,"
      "framerate=25/1\"", TRUE);

  h = gst_harness_new ("vp9dec");
  /* Count a key frame request for every frame libvpx fails to decode: no rate
   * limiting, and no automatic requests, which would also be made for each
   * frame the base class discards */
  g_object_set (h->element, "max-errors", -1,
      "min-force-key-unit-interval", G_GUINT64_CONSTANT (0),
      "automatic-request-sync-points", FALSE, NULL);
  gst_harness_set_src_caps_str (h, "video/x-vp9");

  fail_unless (gst_video_decoder_get_needs_sync_point (GST_VIDEO_DECODER
          (h->element)));

  /* K D D D D K D */
  frames[0] = pull_encoded_frame (enc, TRUE);
  for (i = 1; i <= 4; i++)
    frames[i] = pull_encoded_frame (enc, FALSE);
  gst_harness_push_upstream_event (enc,
      gst_video_event_new_upstream_force_key_unit (GST_CLOCK_TIME_NONE, TRUE,
          0));
  frames[5] = pull_encoded_frame (enc, TRUE);
  frames[6] = pull_encoded_frame (enc, FALSE);

  /* damage the first delta frame so that libvpx fails to decode it */
  frames[1] = gst_buffer_make_writable (frames[1]);
  gst_buffer_resize (frames[1], 0, 3);

  push_frames (h, frames, 0, 0, &requests, &decoded);
  fail_unless_equals_int (0, requests);
  fail_unless_equals_int (1, decoded);

  push_frames (h, frames, 1, 1, &requests, &decoded);
  fail_unless_equals_int (1, requests);
  fail_unless_equals_int (0, decoded);

  /* the three delta frames after the damaged one never reach libvpx */
  push_frames (h, frames, 2, 4, &requests, &decoded);
  fail_unless_equals_int (0, requests);
  fail_unless_equals_int (0, decoded);

  /* decoding resumes at the next key frame */
  push_frames (h, frames, 5, 6, &requests, &decoded);
  fail_unless_equals_int (0, requests);
  fail_unless_equals_int (2, decoded);

  for (i = 0; i < N_FRAMES; i++)
    gst_buffer_unref (frames[i]);
  gst_harness_teardown (h);
  gst_harness_teardown (enc);
}

GST_END_TEST;

static Suite *
vp9dec_suite (void)
{
  Suite *s = suite_create ("vp9dec");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, test_decode_drops_delta_frames_after_error);

  return s;
}

GST_CHECK_MAIN (vp9dec);
