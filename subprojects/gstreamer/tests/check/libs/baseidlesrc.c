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
  g_object_unref (src);
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
  g_object_unref (src);
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
  // FIXME: Enable check once _pull_list API lands.
  // gst_buffer_list_unref (gst_harness_pull_list (h));

  gst_harness_teardown (h);
  g_object_unref (src);
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
  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_thread_pool_set_and_get)
{
  GstElement *src;
  GstBaseIdleSrc *base_src;
  GstTaskPool *thread_pool;
  GstTaskPool *new_thread_pool;
  GError *err = NULL;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  /* The element must expose a default internal pool. */
  thread_pool = gst_base_idle_src_get_thread_pool (base_src);
  fail_unless (thread_pool != NULL);
  gst_object_unref (thread_pool);

  /* Build a replacement pool and prepare it, checking for failure. */
  new_thread_pool = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (new_thread_pool),
      2);
  gst_task_pool_prepare (new_thread_pool, &err);
  fail_unless (err == NULL, "task pool prepare failed: %s",
      err ? err->message : "(no error)");

  /* set_thread_pool() takes ownership (transfer full), so we ref-up for our
   * own use here. */
  gst_base_idle_src_set_thread_pool (base_src,
      gst_object_ref (new_thread_pool));

  thread_pool = gst_base_idle_src_get_thread_pool (base_src);
  fail_unless (thread_pool == new_thread_pool);
  fail_unless_equals_int (2,
      gst_shared_task_pool_get_max_threads (GST_SHARED_TASK_POOL
          (thread_pool)));

  gst_object_unref (thread_pool);       /* drop the get_thread_pool() ref */
  g_object_unref (src);         /* this releases the element's ref */

  /* Now we own the last ref; we may safely cleanup + unref our local one. */
  gst_task_pool_cleanup (new_thread_pool);
  gst_object_unref (new_thread_pool);
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

  /* yield to cause some havoc */
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

  return NULL;
}

GST_START_TEST (baseidlesrc_thread_pool_submit)
{
  GstHarness *hs[MAX_SRCS];
  GstElement *srcs[MAX_SRCS];
  GThread *threads[MAX_SRCS];
  GstBaseIdleSrc *base_src;
  GError *err = NULL;
  GstTaskPool *pool;
  guint i;

  pool = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (pool),
      MAX_SRCS / 2);
  gst_task_pool_prepare (pool, &err);
  fail_unless (err == NULL, "task pool prepare failed: %s",
      err ? err->message : "(no error)");

  /* create all sources and harnesses in one go */
  for (i = 0; i < MAX_SRCS; i++) {
    srcs[i] = g_object_new (test_idle_src_get_type (), NULL);
    base_src = GST_BASE_IDLE_SRC (srcs[i]);

    /* transfer-full — give each element its own ref */
    gst_base_idle_src_set_thread_pool (base_src, gst_object_ref (pool));

    hs[i] = gst_harness_new_with_element (GST_ELEMENT (srcs[i]), NULL, "src");
    gst_harness_set_sink_caps_str (hs[i], "foo/bar");
    gst_harness_play (hs[i]);
  }

  for (i = 0; i < MAX_SRCS; i++) {
    char *thread_name = g_strdup_printf ("pusher-%d", i);
    threads[i] = g_thread_new (thread_name, _push_func, hs[i]);
    g_free (thread_name);
  }

  for (i = 0; i < MAX_SRCS; i++)
    g_thread_join (threads[i]);

  for (i = 0; i < MAX_SRCS; i++) {
    gst_harness_teardown (hs[i]);
    g_object_unref (srcs[i]);
  }

  /* All elements have released their refs; we own the last one — safe to
   * cleanup + unref. */
  gst_task_pool_cleanup (pool);
  gst_object_unref (pool);
}

GST_END_TEST;

static void
_yield_task (G_GNUC_UNUSED void *user_data)
{
  g_thread_yield ();
}

GST_START_TEST (baseidlesrc_default_thread_pool_is_prepared)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstTaskPool *pool = gst_base_idle_src_get_thread_pool (base_src);
  GError *err = NULL;
  gpointer handle;

  fail_unless (pool != NULL);
  fail_unless (GST_IS_SHARED_TASK_POOL (pool));
  fail_unless_equals_int (1,
      gst_shared_task_pool_get_max_threads (GST_SHARED_TASK_POOL (pool)));

  /* Already prepared → push must succeed without prepare(). */
  handle = gst_task_pool_push (pool, _yield_task, NULL, &err);
  fail_unless (err == NULL, "default pool push failed: %s",
      err ? err->message : "");
  if (handle)
    gst_task_pool_join (pool, handle);

  gst_object_unref (pool);
  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_replace_thread_pool_preserves_shared_pool)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GError *err = NULL;
  GstTaskPool *shared, *other;
  gpointer h;

  shared = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (shared), 2);
  gst_task_pool_prepare (shared, &err);
  fail_unless (err == NULL);

  gst_base_idle_src_set_thread_pool (base_src, gst_object_ref (shared));

  other = gst_shared_task_pool_new ();
  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (other), 1);
  gst_task_pool_prepare (other, &err);
  fail_unless (err == NULL);
  gst_base_idle_src_set_thread_pool (base_src, gst_object_ref (other));

  /* Would fail/UAF if shared had been cleaned up. */
  h = gst_task_pool_push (shared, _yield_task, NULL, &err);
  fail_unless (err == NULL);
  if (h)
    gst_task_pool_join (shared, h);

  g_object_unref (src);

  gst_task_pool_cleanup (other);
  gst_object_unref (other);

  gst_task_pool_cleanup (shared);
  gst_object_unref (shared);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_finalize_one_keeps_shared_pool_alive)
{
  GError *err = NULL;
  GstTaskPool *pool = gst_shared_task_pool_new ();
  TestIdleSrc *a, *b;
  GstHarness *hb;
  GstBuffer *buf;

  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (pool), 2);
  gst_task_pool_prepare (pool, &err);
  fail_unless (err == NULL);

  a = g_object_new (test_idle_src_get_type (), NULL);
  b = g_object_new (test_idle_src_get_type (), NULL);
  gst_base_idle_src_set_thread_pool (GST_BASE_IDLE_SRC (a),
      gst_object_ref (pool));
  gst_base_idle_src_set_thread_pool (GST_BASE_IDLE_SRC (b),
      gst_object_ref (pool));

  hb = gst_harness_new_with_element (GST_ELEMENT (b), NULL, "src");
  gst_harness_set_sink_caps_str (hb, "foo/bar");
  gst_harness_play (hb);

  g_object_unref (a);           /* must not break b */

  fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (b, &buf));
  gst_base_idle_src_submit_buffer (GST_BASE_IDLE_SRC (b), buf);
  gst_buffer_unref (gst_harness_pull (hb));

  gst_harness_teardown (hb);
  g_object_unref (b);

  gst_task_pool_cleanup (pool);
  gst_object_unref (pool);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submission_order_is_preserved)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstHarness *h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  const guint N = 200;
  guint i;

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  for (i = 0; i < N; i++) {
    GstBuffer *buf;
    fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
    GST_BUFFER_OFFSET (buf) = i;
    gst_base_idle_src_submit_buffer (base_src, buf);
  }
  for (i = 0; i < N; i++) {
    GstBuffer *buf = gst_harness_pull (h);
    fail_unless_equals_uint64 (GST_BUFFER_OFFSET (buf), i);
    gst_buffer_unref (buf);
  }

  gst_harness_teardown (h);
  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_do_timestamp_on_buffer_list)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstHarness *h;
  GstBufferList *list;
  guint i;

  gst_base_idle_src_set_do_timestamp (base_src, TRUE);
  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  list = gst_buffer_list_new_sized (4);
  for (i = 0; i < 4; i++) {
    GstBuffer *buf;
    fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
    gst_buffer_list_insert (list, -1, buf);
  }
  gst_base_idle_src_submit_buffer_list (base_src, list);

  for (i = 0; i < 4; i++) {
    GstBuffer *buf = gst_harness_pull (h);
    fail_unless (buf != NULL);
    fail_unless (GST_BUFFER_PTS_IS_VALID (buf));
    gst_buffer_unref (buf);
  }

  gst_harness_teardown (h);
  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_set_format_rejects_invalid)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);

  ASSERT_CRITICAL (gst_base_idle_src_set_format (base_src, GST_FORMAT_PERCENT));
  ASSERT_CRITICAL (gst_base_idle_src_set_format (base_src, GST_FORMAT_DEFAULT));

  gst_base_idle_src_set_format (base_src, GST_FORMAT_BYTES);
  gst_base_idle_src_set_format (base_src, GST_FORMAT_TIME);
  gst_base_idle_src_set_format (base_src, GST_FORMAT_BUFFERS);

  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submit_pull_loop_no_deadlock)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstHarness *h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  gint64 deadline;
  guint i;

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);
  deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  for (i = 0; i < 100; i++) {
    GstBuffer *buf;
    fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
    gst_base_idle_src_submit_buffer (base_src, buf);
    gst_buffer_unref (gst_harness_pull (h));
    fail_unless (g_get_monotonic_time () < deadline,
        "submit/pull loop deadlocked or stalled");
  }

  gst_harness_teardown (h);
  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submit_after_stop_is_safe)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstHarness *h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  GstBuffer *buf;

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);
  gst_harness_teardown (h);

  fail_unless_equals_int (GST_FLOW_OK, test_idle_src_alloc (src, &buf));
  gst_base_idle_src_submit_buffer (base_src, buf);      /* must drop, not crash */

  g_object_unref (src);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_default_pool_is_cleaned_up_on_swap)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GError *err = NULL;
  GstTaskPool *replacement = gst_shared_task_pool_new ();

  gst_shared_task_pool_set_max_threads (GST_SHARED_TASK_POOL (replacement), 1);
  gst_task_pool_prepare (replacement, &err);
  fail_unless (err == NULL);

  /* Swap out the default pool — the previous (default) one is owned by us
   * and must be cleaned up + unreffed inside set_thread_pool(). Valgrind
   * will catch the leak if cleanup() is skipped. */
  gst_base_idle_src_set_thread_pool (GST_BASE_IDLE_SRC (src),
      gst_object_ref (replacement));

  gst_task_pool_cleanup (replacement);
  gst_object_unref (replacement);

  g_object_unref (src);
}

GST_END_TEST;

typedef struct
{
  GstBaseIdleSrc *src;
  gint stop;                    /* atomic */
} StopRaceCtx;

static gpointer
_stop_race_producer (gpointer data)
{
  StopRaceCtx *c = data;
  while (!g_atomic_int_get (&c->stop)) {
    GstBuffer *buf = gst_buffer_new_allocate (NULL, 64, NULL);
    gst_base_idle_src_submit_buffer (c->src, buf);
  }
  return NULL;
}

GST_START_TEST (baseidlesrc_stop_races_with_submit)
{
  TestIdleSrc *src = g_object_new (test_idle_src_get_type (), NULL);
  GstBaseIdleSrc *base_src = GST_BASE_IDLE_SRC (src);
  GstHarness *h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");
  StopRaceCtx ctx = { base_src, 0 };
  GThread *producer;

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  producer = g_thread_new ("producer", _stop_race_producer, &ctx);
  g_usleep (50 * 1000);         /* let it churn */
  gst_harness_teardown (h);     /* triggers stop() */
  g_atomic_int_set (&ctx.stop, 1);
  g_thread_join (producer);

  g_object_unref (src);
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
  tcase_add_test (tc, baseidlesrc_default_thread_pool_is_prepared);
  tcase_add_test (tc, baseidlesrc_replace_thread_pool_preserves_shared_pool);
  tcase_add_test (tc, baseidlesrc_finalize_one_keeps_shared_pool_alive);
  tcase_add_test (tc, baseidlesrc_submission_order_is_preserved);
  tcase_add_test (tc, baseidlesrc_do_timestamp_on_buffer_list);
  tcase_add_test (tc, baseidlesrc_set_format_rejects_invalid);
  tcase_add_test (tc, baseidlesrc_submit_pull_loop_no_deadlock);
  tcase_add_test (tc, baseidlesrc_submit_after_stop_is_safe);
  tcase_add_test (tc, baseidlesrc_default_pool_is_cleaned_up_on_swap);
  tcase_add_test (tc, baseidlesrc_stop_races_with_submit);

  return s;
}

GST_CHECK_MAIN (baseidlesrc);
