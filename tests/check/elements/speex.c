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

GST_START_TEST (test_headers_in_caps)
{
  GstHarness * h = gst_harness_new ("speexdec");
  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\" ! "
      "speexenc", TRUE);

  /* the first audio-buffer produces 3 packets:
     streamheader, vorbiscomments and the encoded buffer */
  gst_harness_src_crank_and_push_many (h, 1, 3);

  /* verify the decoder produces exactly one decoded buffer */
  gst_buffer_unref (gst_harness_pull (h));
  fail_unless_equals_int (0, gst_harness_buffers_in_queue (h));

  gst_harness_teardown (h);
}
GST_END_TEST;

GST_START_TEST (test_headers_in_buffers)
{
  GstHarness * h = gst_harness_new ("speexdec");
  gst_harness_set_src_caps_str (h, "audio/x-speex");

  /* turn off forwarding to avoid getting caps from the src-harness,
     and hence not accessing the streamheaders through the caps */
  gst_harness_set_forwarding (h, FALSE);

  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\" ! "
      "speexenc", TRUE);

  /* the first audio-buffer produces 3 packets:
     streamheader, vorbiscomments and the encoded buffer */
  gst_harness_src_crank_and_push_many (h, 1, 3);

  /* verify the decoder produces exactly one decoded buffer */
  gst_buffer_unref (gst_harness_pull (h));
  fail_unless_equals_int (0, gst_harness_buffers_in_queue (h));

  gst_harness_teardown (h);
}
GST_END_TEST;

GST_START_TEST (test_headers_not_in_buffers)
{
  GstHarness * h = gst_harness_new ("speexdec");

  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\" ! "
      "speexenc", TRUE);

  /* the first audio-buffer produces 3 packets:
     streamheader, vorbiscomments and the encoded buffer */
  gst_harness_src_crank_and_push_many (h, 1, 0);

  /* now remove the streamheader and vorbiscomment */
  gst_buffer_unref (gst_harness_pull (h->src_harness));
  gst_buffer_unref (gst_harness_pull (h->src_harness));

  /* and just push the encoded buffer to the decoder */
  gst_harness_src_crank_and_push_many (h, 0, 1);

  /* verify the decoder produces exactly one decoded buffer */
  gst_buffer_unref (gst_harness_pull (h));
  fail_unless_equals_int (0, gst_harness_buffers_in_queue (h));

  gst_harness_teardown (h);
}
GST_END_TEST;

GST_START_TEST (test_headers_from_flv)
{
  GstHarness * h = gst_harness_new ("speexdec");

  /* set caps with the same streamheader flvdemux would add */
  gst_harness_set_src_caps_str (h, "audio/x-speex, "
      "streamheader=(buffer)< 5370656578202020312e312e3132000000000000000000000"
      "00000000100000050000000803e0000010000000400000001000000ffffffff500000000"
      "000000001000000000000000000000000000000, "
      "0b0000004e6f20636f6d6d656e74730000000001 >, "
      "rate=(int)16000, channels=(int)1");

  /* turn off forwarding to avoid getting caps from the src-harness,
     and force it to use the flv-streamheaders we have set in the caps */
  gst_harness_set_forwarding (h, FALSE);

  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=320 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\" ! "
      "speexenc", TRUE);

  /* the first audio-buffer produces 3 packets:
     streamheader, vorbiscomments and the encoded buffer */
  gst_harness_src_crank_and_push_many (h, 1, 3);

  /* verify the decoder produces exactly one decoded buffer */
  gst_buffer_unref (gst_harness_pull (h));
  fail_unless_equals_int (0, gst_harness_buffers_in_queue (h));

  gst_harness_teardown (h);
}
GST_END_TEST

GST_START_TEST (test_new_segment_before_first_output_buffer)
{
  GstHarness *h = gst_harness_new ("speexenc");
  GstBuffer *buf;
  GstSegment segment;

  /* 10 ms buffers */
  gst_harness_add_src_parse (h, "audiotestsrc is-live=1 samplesperbuffer=160 ! "
      "capsfilter caps=\"audio/x-raw,format=S16LE,rate=16000\"", TRUE);

  /* push 10 ms of audio-data */
  gst_harness_push_from_src (h);

  /* send a second segment event */
  gst_segment_init (&segment, GST_FORMAT_TIME);
  fail_unless (gst_harness_push_event (h, gst_event_new_segment (&segment)));

  /* push 10 ms of audio-data */
  gst_harness_push_from_src (h);

  /* pull 20 ms of encoded audio-data with headers */
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

  gst_harness_teardown (h);
}
GST_END_TEST;


static Suite *
speex_suite (void)
{
  Suite *s = suite_create ("speex");
  TCase *tc_chain;

  suite_add_tcase (s, (tc_chain = tcase_create ("timestamping")));
  tcase_add_test (tc_chain, test_encoder_timestamp);
  tcase_add_test (tc_chain, test_encoder_to_decoder_timestamp);

  suite_add_tcase (s, (tc_chain = tcase_create ("headers")));
  tcase_add_test (tc_chain, test_headers_in_caps);
  tcase_add_test (tc_chain, test_headers_in_buffers);
  tcase_add_test (tc_chain, test_headers_not_in_buffers);
  tcase_add_test (tc_chain, test_headers_from_flv);

  suite_add_tcase (s, (tc_chain = tcase_create ("events")));
  tcase_add_test (tc_chain, test_new_segment_before_first_output_buffer);


  return s;
}

GST_CHECK_MAIN (speex)
