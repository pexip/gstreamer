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

static GstFlowReturn
test_idle_src_alloc (TestIdleSrc * src, GstBuffer ** buf)
{
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstBaseIdleSrcClass *klass = GST_BASE_IDLE_SRC_GET_CLASS (base_src);
  return klass->alloc (base_src, 100, buf);
}

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
  TestIdleSrc *src;
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
    fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
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
  TestIdleSrc *src;
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
    fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
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
  TestIdleSrc *src;
  GstHarness *h;
  GstBaseIdleSrc *base_src;
  GstBuffer *buf;
  GstEvent *event;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
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

GST_START_TEST (baseidlesrc_thread_pool_set_and_get)
{
  GstElement *src;
  GstBaseIdleSrc *base_src;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  GstTaskPool *thread_pool = gst_base_idle_src_get_thread_pool (base_src);
  fail_unless (thread_pool);
  gst_object_unref (thread_pool);

  GstTaskPool *new_thread_pool = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (new_thread_pool),
      2);

  gst_base_idle_src_set_thread_pool (base_src, new_thread_pool);
  thread_pool = gst_base_idle_src_get_thread_pool (base_src);
  fail_unless (thread_pool == new_thread_pool);
  fail_unless_equals_int (2,
      gst_shared_task_pool_get_max_threads (GST_SHARED_TASK_POOL
          (thread_pool)));

  gst_object_unref (thread_pool);
}

GST_END_TEST;

#define MAX_SRCS 16

static gpointer
_push_func (gpointer data)
{
  GstBuffer *buf;
  GstBufferList *buf_list;
  guint i, j;

  GstHarness *h = data;
  GstElement *e = h->element;
  TestIdleSrc *src = (TestIdleSrc *) e;
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);

  /* push some buffer lists */
  GST_LOG ("Pushing some buffer lists from source %s",
      gst_element_get_name (e));
  for (i = 0; i < 5; i++) {
    buf_list = gst_buffer_list_new_sized (20);
    for (j = 0; j < 5; j++) {
      fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
      gst_buffer_list_insert (buf_list, -1, buf);
    }
    gst_base_idle_src_submit_buffer_list (base_src, buf_list);
    if (g_random_int_range (0, 100) == 3) {
      GST_LOG ("Randomly yielding during buffer list push from source %s",
          gst_element_get_name (e));
      g_thread_yield ();
    }
  }

  /* yield to cause some hadvoc */
  GST_LOG ("Yielding from source %s", gst_element_get_name (e));
  g_thread_yield ();

  /* push some buffers */
  GST_LOG ("Pushing some buffers from source %s", gst_element_get_name (e));
  fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
  for (i = 0; i < 100; i++) {
    gst_base_idle_src_submit_buffer (base_src, gst_buffer_ref (buf));
    if (g_random_int_range (0, 100) == 3) {
      GST_LOG ("Randomly yielding during buffer push from source %s",
          gst_element_get_name (e));
      g_thread_yield ();
    }
  }
  gst_buffer_unref (buf);
}

GST_START_TEST (baseidlesrc_thread_pool_submit)
{
  GstHarness *hs[MAX_SRCS];
  GstElement *srcs[MAX_SRCS];
  GThread *threads[MAX_SRCS];
  GstBaseIdleSrc *base_src;
  guint i;

  GstTaskPool *pool = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (pool),
      MAX_SRCS / 2);

  /* create all sources and harnesses in one go */
  for (i = 0; i < MAX_SRCS; i++) {
    srcs[i] = g_object_new (test_idle_src_get_type (), NULL);
    base_src = GST_BASE_IDLE_SRC (srcs[i]);

    gst_base_idle_src_set_thread_pool (base_src, pool);

    hs[i] = gst_harness_new_with_element (GST_ELEMENT (srcs[i]), NULL, "src");
    gst_harness_set_sink_caps_str (hs[i], "foo/bar");
    gst_harness_play (hs[i]);
  }

  /* start pushing to from all sources */
  for (i = 0; i < MAX_SRCS; i++) {
    char *thread_name = g_strdup_printf ("pusher-%d", i);
    threads[i] = g_thread_new (thread_name, _push_func, hs[i]);
  }

  /* wait for all sources to finish pushing */
  for (i = 0; i < MAX_SRCS; i++) {
    g_thread_join (threads[i]);
  }

  /* teardown */
  for (i = 0; i < MAX_SRCS; i++) {
    gst_harness_teardown (hs[i]);
  }
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
  tcase_add_test (tc, baseidlesrc_thread_pool_set_and_get);
  tcase_add_test (tc, baseidlesrc_thread_pool_submit);

  return s;
}

GST_CHECK_MAIN (baseidlesrc);
