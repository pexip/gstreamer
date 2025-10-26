/* GStreamer
 *
 * some unit tests for GstBaseIdleSrc
 *
 * Copyright (C) 2023 Havard Graff <hgr@pexip.com>
 *               2023 Camilo Celis Guzman <camilo@pexip.com>
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
#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/check/gstconsistencychecker.h>
#include <gst/base/gstbaseidlesrc.h>

typedef GstBaseIdleSrc TestIdleSrc;
typedef GstBaseIdleSrcClass TestIdleSrcClass;

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GType test_idle_src_get_type (void);

G_DEFINE_TYPE (TestIdleSrc, test_idle_src, GST_TYPE_BASE_IDLE_SRC);

static void
test_idle_src_init (TestIdleSrc * src)
{
}

static void
test_idle_src_class_init (TestIdleSrcClass * klass)
{
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &src_template);
}

GST_START_TEST (baseidlesrc_up_and_down)
{
  GstElement *src;
  GstHarness *h;

  src = g_object_new (test_idle_src_get_type (), NULL);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");

  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submit_buffer)
{
  GstElement *src;
  GstHarness *h;
  GstBaseIdleSrc *base_src;
  GstBuffer *buf;
  guint i;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");

  gst_harness_set_sink_caps_str (h, "video/x-raw,format=RGB,width=1,height=1");
  gst_harness_play (h);

  for (i = 0; i < 5; i++) {
    fail_unless_equals_int (GST_FLOW_OK,
        gst_base_idle_src_alloc_buffer (base_src, 100, &buf));
    GST_BUFFER_PTS (buf) = i * GST_MSECOND;
    gst_base_idle_src_submit_buffer (base_src, buf);

    buf = gst_harness_pull (h);
    fail_unless (buf != NULL);
    fail_unless_equals_uint64 (GST_BUFFER_PTS (buf), i * GST_MSECOND);
    gst_buffer_unref (buf);
  }

  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submit_buffer_list)
{
  GstElement *src;
  GstHarness *h;
  GstBaseIdleSrc *base_src;
  GstBuffer *buf;
  GstBufferList *buf_list;
  guint i;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  buf_list = gst_buffer_list_new_sized (20);

  for (i = 0; i < 5; i++) {
    fail_unless_equals_int (GST_FLOW_OK,
        gst_base_idle_src_alloc_buffer (base_src, 100, &buf));
    gst_buffer_list_insert (buf_list, -1, buf);
  }

  gst_base_idle_src_submit_buffer_list (base_src, buf_list);
  gst_buffer_list_unref (gst_harness_pull_list (h));

  gst_harness_teardown (h);
}

GST_END_TEST;

static void
fail_unless_equals_event_type (const GstEvent * event,
    GstEventType expected_type)
{
  fail_unless (GST_EVENT_TYPE (event) == expected_type,
      "'%s' expected, got '%s'", gst_event_type_get_name (expected_type),
      gst_event_type_get_name (GST_EVENT_TYPE (event)));
}

GST_START_TEST (baseidlesrc_handle_events)
{
  GstElement *src;
  GstHarness *h;
  GstBaseIdleSrc *base_src;
  GstBuffer *buf;
  GstEvent *event;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  h = gst_harness_new_with_element (src, NULL, "src");
  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  fail_unless_equals_int (GST_FLOW_OK,
      gst_base_idle_src_alloc_buffer (base_src, 64, &buf));
  gst_base_idle_src_submit_buffer (base_src, buf);

  event = gst_harness_pull_event (h);
  fail_unless_equals_event_type (event, GST_EVENT_STREAM_START);
  gst_event_unref (event);

  event = gst_harness_pull_event (h);
  fail_unless_equals_event_type (event, GST_EVENT_SEGMENT);
  gst_event_unref (event);

  gst_buffer_unref (gst_harness_pull (h));

  gst_harness_teardown (h);
}

GST_END_TEST;

static Suite *
baseidlesrc_suite (void)
{
  Suite *s = suite_create ("GstBaseIdleSrc");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (s, tc);
  tcase_add_test (tc, baseidlesrc_up_and_down);
  tcase_add_test (tc, baseidlesrc_submit_buffer);
  tcase_add_test (tc, baseidlesrc_submit_buffer_list);
  tcase_add_test (tc, baseidlesrc_handle_events);

  return s;
}

GST_CHECK_MAIN (baseidlesrc);
