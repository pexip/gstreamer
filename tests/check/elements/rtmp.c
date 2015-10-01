/* GStreamer
 *
 * unit test for rtmp
 *
 * Copyright (C) <2016> Havard Graff <havard@pexip.com>
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

#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

static gint
add_listen_fd (int port)
{
  gint fd = socket (AF_INET6, SOCK_STREAM, 0);
  g_assert_cmpint (fd, >=, 0);

  int sock_optval = 1;
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &sock_optval, sizeof (sock_optval));

  struct sockaddr_in6 sin;
  memset (&sin, 0, sizeof (struct sockaddr_in6));
  sin.sin6_family = AF_INET6;
  sin.sin6_port = htons (port);
  sin.sin6_addr = in6addr_any;

  g_assert (bind (fd, (struct sockaddr *) &sin, sizeof (sin)) >= 0);
  listen (fd, 10);

  return fd;
}


GST_START_TEST (rtmpsink_unlock)
{
  gint fd;
  GstHarness *h;
  GTimer *timer;

  fd = add_listen_fd (22000);
  h = gst_harness_new_parse
      ("queue ! rtmpsink location=rtmp://localhost:22000/app/streamname1");
  gst_harness_set_src_caps_str (h, "video/x-flv");

  timer = g_timer_new ();
  gst_harness_push (h, gst_buffer_new ());

  if (__i__ == 1)
    g_usleep (G_USEC_PER_SEC / 10);

  gst_harness_teardown (h);
  fail_unless (g_timer_elapsed (timer, NULL) < 2.0);

  close (fd);
}

GST_END_TEST;

GST_START_TEST (rtmpsink_unlock_race)
{
  GstHarness *h = gst_harness_new_parse ("rtmpsink location=rtmp://a/b/c");
  GstHarnessThread *statechange, *push;
  GstSegment segment;
  GstCaps *caps = gst_caps_from_string ("video/x-flv");
  GstBuffer *buf = gst_buffer_new ();

  gst_segment_init (&segment, GST_FORMAT_TIME);

  statechange = gst_harness_stress_statechange_start_full (h, 1);
  push = gst_harness_stress_push_buffer_start (h, caps, &segment, buf);

  g_usleep (G_USEC_PER_SEC * 1);

  gst_harness_stress_thread_stop (statechange);
  gst_harness_stress_thread_stop (push);

  gst_caps_unref (caps);
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

GST_END_TEST;


static Suite *
rtmp_suite (void)
{
  Suite *s = suite_create ("rtmp");
  TCase *tc_chain = tcase_create ("general");
  suite_add_tcase (s, tc_chain);

  tcase_add_loop_test (tc_chain, rtmpsink_unlock, 0, 2);
  tcase_add_test (tc_chain, rtmpsink_unlock_race);

  return s;
}

GST_CHECK_MAIN (rtmp);
