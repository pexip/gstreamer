/* GstHarness - A test-harness for GStreamer testing
 *
 * Copyright (C) 2013 Pexip <pexip.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_HARNESS_H__
#define __GST_HARNESS_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/check/gsttestclock.h>

G_BEGIN_DECLS

typedef struct _GstHarness GstHarness;
typedef struct _GstHarnessThread GstHarnessThread;

struct _GstHarness {
  GstElement * element;
  GstPad * srcpad;
  GstPad * sinkpad;
  GstCaps * src_caps;
  GstCaps * sink_caps;
  volatile guint recv_buffers;
  GAsyncQueue * buffer_queue;
  GAsyncQueue * src_event_queue;
  GAsyncQueue * sink_event_queue;
  GstPad * sink_forward_pad;
  gchar * element_sinkpad_name;
  gchar * element_srcpad_name;
  GstClockTime latency_min;
  GstClockTime latency_max;
  gsize sine_cont;
  GstHarness * src_harness;
  GstHarness * sink_harness;
  gboolean has_clock_wait;
  gboolean drop_buffers;
  GstClockTime last_push_ts;
  GstAllocator * allocator;
  GstAllocationParams allocation_params;
  GstBufferPool * pool;
  GstAllocator * propose_allocator;
  GstAllocationParams propose_params;

  gboolean pull_mode_active;
  GCond pull_cond;
  GMutex pull_mutex;

  GPtrArray * stress;
};

GstHarness * gst_harness_new (const char * name);
GstHarness * gst_harness_new_parse (const gchar * launchline);
GstHarness * gst_harness_new_with_element (GstElement * element,
    const gchar * sinkpad, const gchar * srcpad);
GstHarness * gst_harness_new_with_templates (const gchar * element_name,
    GstStaticPadTemplate * hsrc, GstStaticPadTemplate * hsink);
GstHarness * gst_harness_new_with_padnames (const gchar * element_name,
    const gchar * sinkpad, const gchar * srcpad);
GstHarness * gst_harness_new_full (GstElement * element,
    GstStaticPadTemplate * hsrc, const gchar * sinkpad,
    GstStaticPadTemplate * hsink, const gchar * srcpad);

void gst_harness_add_element_srcpad (GstHarness * h, GstPad * srcpad);

void gst_harness_teardown (GstHarness * h);

void gst_harness_play (GstHarness * h);
void gst_harness_set_pull_mode (GstHarness * h);

void gst_harness_add_probe (GstHarness * h,
    const gchar * element_name, const gchar * pad_name, GstPadProbeType mask,
    GstPadProbeCallback callback, gpointer user_data,
    GDestroyNotify destroy_data);

void gst_harness_set (GstHarness * h,
    const gchar * element_name, const gchar * first_property_name, ...);

void gst_harness_get (GstHarness * h,
    const gchar * element_name, const gchar * first_property_name, ...);

void gst_harness_signal_connect (GstHarness * h,
    const gchar * element_name, const gchar * signal_name,
    GCallback handler, gpointer data);

void gst_harness_signal (GstHarness * h,
    const gchar * element_name, const gchar * signal_name);

GstElement * gst_harness_find_element (GstHarness * h,
    const gchar * element_name);

void gst_harness_set_sink_caps (GstHarness * h, GstCaps * caps);
void gst_harness_set_src_caps (GstHarness * h, GstCaps * caps);
#define gst_harness_set_caps(h, in, out)  \
  gst_harness_set_sink_caps (h, out), gst_harness_set_src_caps (h, in)

#define gst_harness_set_sinkcaps_str(h,str) \
  gst_harness_set_sink_caps (h, gst_caps_from_string (str))
#define gst_harness_set_srccaps_str(h, str) \
  gst_harness_set_src_caps (h, gst_caps_from_string (str))
#define gst_harness_set_caps_str(h, in, out)  \
  gst_harness_set_sinkcaps_str (h, out), gst_harness_set_srccaps_str (h, in)

GstClockTime gst_harness_query_latency (GstHarness * h);
void gst_harness_set_us_latency (GstHarness * h, GstClockTime latency);

/* buffers */
GstFlowReturn gst_harness_push (GstHarness * h, GstBuffer * buffer);
GstBuffer * gst_harness_push_and_wait (GstHarness * h, GstBuffer * buffer);
GstBuffer * gst_harness_pull (GstHarness * h);
GstBuffer * gst_harness_try_pull (GstHarness * h);
#define gst_harness_buffers_in_queue(h) g_async_queue_length ((h)->buffer_queue)
#define gst_harness_buffers_received(h) (h)->recv_buffers
void gst_harness_set_drop_buffers (GstHarness * h, gboolean drop_buffers);
void gst_harness_dump_to_file (GstHarness * h, const gchar * filename);

GstBuffer * gst_harness_create_buffer (GstHarness * h, gsize size);

/* downstream events */
gboolean gst_harness_push_event (GstHarness * h, GstEvent * ev);
GstEvent * gst_harness_pull_event (GstHarness * h);
GstEvent * gst_harness_try_pull_event (GstHarness * h);
gint gst_harness_events_received (GstHarness * h);

/* upstream events */
gboolean gst_harness_send_upstream_event (GstHarness * h, GstEvent * ev);
GstEvent * gst_harness_pull_upstream_event (GstHarness * h);
GstEvent * gst_harness_try_pull_upstream_event (GstHarness *h);
gint gst_harness_upstream_events_received (GstHarness * h);

/* harness src & sink*/
void gst_harness_add_src (GstHarness * h,
    const gchar * src_element_name, gboolean has_clock_wait);
void gst_harness_add_src_parse (GstHarness * h,
    const gchar * launchline, gboolean has_clock_wait);
GstFlowReturn gst_harness_push_from_src (GstHarness * h);
GstFlowReturn gst_harness_src_crank_and_push_many (GstHarness * h,
    gint cranks, gint pushes);

void gst_harness_add_sink (GstHarness * h, const gchar * sink_element_name);
void gst_harness_add_sink_parse (GstHarness * h, const gchar * launchline);
void gst_harness_push_to_sink (GstHarness * h);
void gst_harness_sink_push_many (GstHarness * h, gint pushes);

void gst_harness_src_push_event (GstHarness * h);

/* Async */
gpointer gst_harness_push_async (GstHarness * h, GstBuffer * buffer,
    GstTaskPool* task_pool);
gpointer gst_harness_push_event_async (GstHarness * h, GstEvent * event,
    GstTaskPool* task_pool);

/* TestClock Functions */
void gst_harness_use_testclock (GstHarness * h);
GstTestClock * gst_harness_get_testclock (GstHarness * h);
gboolean gst_harness_set_time (GstHarness * h, GstClockTime time);
gboolean gst_harness_wait_for_clock_id_waits (GstHarness * h,
           guint waits, guint timeout);
gboolean gst_harness_crank_single_clock_wait (GstHarness * h);
gboolean gst_harness_crank_multiple_clock_waits (GstHarness * h,
           unsigned int waits);
void gst_harness_use_systemclock (GstHarness * h);

/* Stress */
guint gst_harness_stress_thread_stop (GstHarnessThread * t);
GstHarnessThread * gst_harness_stress_custom_start (GstHarness * h,
    GFunc init, GFunc callback, gpointer data, gulong sleep);

#define gst_harness_stress_statechange_start(h)                                \
  gst_harness_stress_statechange_start_full (h, G_USEC_PER_SEC / 100)
GstHarnessThread * gst_harness_stress_statechange_start_full (GstHarness * h,
    gulong sleep);

#define gst_harness_stress_push_buffer_start(h, c, s, b)                       \
  gst_harness_stress_push_buffer_start_full (h, c, s, b, 0)
GstHarnessThread * gst_harness_stress_push_buffer_start_full (GstHarness * h,
    GstCaps * caps, const GstSegment * segment, GstBuffer * buf, gulong sleep);

typedef GstBuffer * (*GstHarnessPrepareBuffer) (GstHarness * h, gpointer data);
#define gst_harness_stress_push_buffer_with_cb_start(h, c, s, f, d, n)         \
  gst_harness_stress_push_buffer_with_cb_start_full (h, c, s, f, d, n, 0)
GstHarnessThread * gst_harness_stress_push_buffer_with_cb_start_full (
    GstHarness * h, GstCaps * caps, const GstSegment * segment,
    GstHarnessPrepareBuffer func, gpointer data, GDestroyNotify notify,
    gulong sleep);

/* Pushing events should generally be OOB events.
 * If you need serialized events, you may use a custom stress thread which
 * both pushes buffers and events! */
#define gst_harness_stress_push_event_start(h, e)                              \
  gst_harness_stress_push_event_start_full (h, e, 0)
GstHarnessThread * gst_harness_stress_push_event_start_full (GstHarness * h,
    GstEvent * event, gulong sleep);

#define gst_harness_stress_send_upstream_event_start(h, e)                     \
  gst_harness_stress_send_upstream_event_start_full (h, e, 0)
GstHarnessThread * gst_harness_stress_send_upstream_event_start_full (
    GstHarness * h, GstEvent * event, gulong sleep);

#define gst_harness_stress_property_start(h, n, v)                             \
  gst_harness_stress_property_start_full (h, n, v, G_USEC_PER_SEC / 1000)
GstHarnessThread * gst_harness_stress_property_start_full (GstHarness * h,
    const gchar * name, const GValue * value, gulong sleep);

#define gst_harness_stress_requestpad_start(h, t, n, c, r)                     \
  gst_harness_stress_requestpad_start_full (h, t, n, c, r, G_USEC_PER_SEC / 100)
GstHarnessThread * gst_harness_stress_requestpad_start_full (GstHarness * h,
    GstPadTemplate * templ, const gchar * name, GstCaps * caps,
    gboolean release, gulong sleep);

G_END_DECLS

#endif /* __GST_HARNESS_H__ */

