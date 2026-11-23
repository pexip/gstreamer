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

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  for (i = 0; i < 5; i++) {
    fail_unless_equals_int (GST_FLOW_OK,
        gst_base_idle_src_alloc_buffer (base_src, 100, &buf));
    gst_base_idle_src_submit_buffer (base_src, buf);
    gst_buffer_unref (gst_harness_pull (h));
  }

  gst_harness_teardown (h);
}

GST_END_TEST;

/* A source with fixed caps, so that a CAPS event is sent, whose start() lets
 * another thread submit a buffer while the source is still starting. That is
 * what a thread answering a request from the peer does. */
typedef GstBaseIdleSrc TestCapsIdleSrc;
typedef GstBaseIdleSrcClass TestCapsIdleSrcClass;

static GstStaticPadTemplate caps_src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("foo/bar"));

static GType test_caps_idle_src_get_type (void);

G_DEFINE_TYPE (TestCapsIdleSrc, test_caps_idle_src, GST_TYPE_BASE_IDLE_SRC);

static gpointer
submit_buffer_thread (gpointer data)
{
  gst_base_idle_src_submit_buffer (GST_BASE_IDLE_SRC (data),
      gst_buffer_new_allocate (NULL, 100, NULL));
  return NULL;
}

static gboolean
test_caps_idle_src_start (GstBaseIdleSrc * base)
{
  if (g_object_get_data (G_OBJECT (base), "submit-in-start")) {
    GThread *thread = g_thread_new ("submitter", submit_buffer_thread, base);

    g_thread_join (thread);
    /* give the streaming thread it kicked off time to push */
    g_usleep (50 * 1000);
  }
  return TRUE;
}

static void
test_caps_idle_src_init (TestCapsIdleSrc * src)
{
}

static void
test_caps_idle_src_class_init (TestCapsIdleSrcClass * klass)
{
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &caps_src_template);
  GST_BASE_IDLE_SRC_CLASS (klass)->start = test_caps_idle_src_start;
}

static GMutex seen_lock;
static GString *seen;

static GstPadProbeReturn
record_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  const gchar *what = NULL;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER)
    what = "buffer";
  else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
    what = GST_EVENT_TYPE_NAME (GST_PAD_PROBE_INFO_EVENT (info));

  if (what) {
    g_mutex_lock (&seen_lock);
    g_string_append_printf (seen, "%s ", what);
    g_mutex_unlock (&seen_lock);
  }
  return GST_PAD_PROBE_OK;
}

static GstHarness *
caps_idle_src_harness_new (GstElement ** src)
{
  GstHarness *h;

  *src = g_object_new (test_caps_idle_src_get_type (), NULL);
  h = gst_harness_new_with_element (*src, NULL, "src");
  gst_harness_set_sink_caps_str (h, "foo/bar");

  seen = g_string_new (NULL);
  gst_pad_add_probe (h->sinkpad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, record_probe, NULL, NULL);
  return h;
}

/* A buffer submitted while the source is still starting must not overtake
 * stream-start, caps and segment. */
GST_START_TEST (baseidlesrc_submit_during_start_keeps_event_order)
{
  GstElement *src;
  GstHarness *h = caps_idle_src_harness_new (&src);

  g_object_set_data (G_OBJECT (src), "submit-in-start", GINT_TO_POINTER (TRUE));
  gst_harness_play (h);
  gst_buffer_unref (gst_harness_pull (h));

  g_mutex_lock (&seen_lock);
  fail_unless_equals_string (seen->str, "stream-start caps segment buffer ");
  g_mutex_unlock (&seen_lock);

  gst_harness_teardown (h);
  g_object_unref (src);
  g_string_free (seen, TRUE);
}

GST_END_TEST;

typedef struct
{
  GstBaseIdleSrc *src;
  gint done;
} Producers;

static gpointer
producer_thread (gpointer data)
{
  Producers *p = data;

  while (!g_atomic_int_get (&p->done)) {
    /* Submitting to a stopped source is an error that gets logged, so only
     * submit once it runs. That still races with both start() and stop(). */
    if (g_atomic_int_get (&p->src->running))
      gst_base_idle_src_submit_buffer (p->src,
          gst_buffer_new_allocate (NULL, 100, NULL));
    else
      g_thread_yield ();
  }
  return NULL;
}

/* Producers that keep submitting while the source is started and stopped over
 * and over must never get data out ahead of stream-start, caps and segment. */
GST_START_TEST (baseidlesrc_restart_with_busy_producers_keeps_order)
{
  GstElement *src;
  GstHarness *h = caps_idle_src_harness_new (&src);
  Producers p;
  GThread *threads[3];
  guint i, cycle, n_buffers = 0;

  p.src = GST_BASE_IDLE_SRC (src);
  p.done = 0;
  for (i = 0; i < G_N_ELEMENTS (threads); i++)
    threads[i] = g_thread_new ("producer", producer_thread, &p);

  for (cycle = 0; cycle < 30; cycle++) {
    gboolean saw_caps = FALSE, saw_segment = FALSE;
    gchar **tokens;

    g_mutex_lock (&seen_lock);
    g_string_truncate (seen, 0);
    g_mutex_unlock (&seen_lock);

    fail_unless (gst_element_set_state (src, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);
    g_usleep (2 * 1000);
    fail_unless (gst_element_set_state (src, GST_STATE_NULL)
        != GST_STATE_CHANGE_FAILURE);

    g_mutex_lock (&seen_lock);
    fail_unless (g_str_has_prefix (seen->str, "stream-start "),
        "cycle %u: stream-start is not first: %s", cycle, seen->str);
    tokens = g_strsplit (seen->str, " ", -1);
    for (i = 0; tokens[i] && *tokens[i]; i++) {
      if (g_str_equal (tokens[i], "caps")) {
        saw_caps = TRUE;
      } else if (g_str_equal (tokens[i], "segment")) {
        saw_segment = saw_caps;
      } else if (g_str_equal (tokens[i], "buffer")) {
        fail_unless (saw_caps && saw_segment,
            "cycle %u: buffer before caps and segment: %s", cycle, seen->str);
        n_buffers++;
      }
    }
    g_mutex_unlock (&seen_lock);
    g_strfreev (tokens);
  }

  g_atomic_int_set (&p.done, 1);
  for (i = 0; i < G_N_ELEMENTS (threads); i++)
    g_thread_join (threads[i]);
  fail_unless (n_buffers > 0, "no buffer was pushed, nothing got tested");

  gst_harness_teardown (h);
  g_object_unref (src);
  g_string_free (seen, TRUE);
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
  tcase_add_test (tc, baseidlesrc_submit_during_start_keeps_event_order);
  tcase_add_test (tc, baseidlesrc_restart_with_busy_producers_keeps_order);

  return s;
}

GST_CHECK_MAIN (baseidlesrc);
