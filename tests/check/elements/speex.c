/* GStreamer
 *
 * unit test for speex
 *
 * Copyright (C) <2017> Havard Graff <havard@pexip.com>
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <string.h>

GST_START_TEST (test_encoder_timestamp)
{
  GstHarness * h = gst_harness_new ("speexenc");
  GstBuffer * buf;
  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\"", TRUE);

  /* push 20ms of audio-data */
  gst_harness_push_from_src (h);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (0, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (0, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (0, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (0, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (0, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  /* push another 20ms of audio-data */
  gst_harness_push_from_src (h);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  gst_harness_teardown (h);
}
GST_END_TEST;

GST_START_TEST (test_encoder_to_decoder_timestamp)
{
  GstHarness * h = gst_harness_new_parse ("speexenc ! speexdec");
  GstBuffer * buf;
  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\"", TRUE);

  /* push 20ms of audio-data */
  gst_harness_push_from_src (h);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (0, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  /* push another 20ms of audio-data */
  gst_harness_push_from_src (h);

  buf = gst_harness_pull (h);
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_PTS (buf));
  fail_unless_equals_uint64 (20 * GST_MSECOND, GST_BUFFER_DURATION (buf));
  gst_buffer_unref (buf);

  gst_harness_teardown (h);
}
GST_END_TEST;

static Suite *
speex_suite (void)
{
  Suite *s = suite_create ("speex");
  TCase *tc_chain = tcase_create ("speex");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_encoder_timestamp);
  tcase_add_test (tc_chain, test_encoder_to_decoder_timestamp);

  return s;
}

GST_CHECK_MAIN (speex)
