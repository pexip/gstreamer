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

 #include "gstharness.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static void gst_harness_stress_free (GstHarnessThread * t);

#define HARNESS_KEY "harness"
#define HARNESS_REF "harness-ref"

static GstStaticPadTemplate hsrctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );
static GstStaticPadTemplate hsinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

static GstFlowReturn
gst_harness_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstHarness * h = g_object_get_data (G_OBJECT (pad), HARNESS_KEY);
  (void) parent;
  g_assert (h != NULL);
  g_mutex_lock (&h->pull_mutex);
  g_atomic_int_inc (&h->recv_buffers);

  if (h->drop_buffers)
    gst_buffer_unref (buffer);
  else
    g_async_queue_push (h->buffer_queue, buffer);

  if (h->pull_mode_active) {
    g_cond_wait (&h->pull_cond, &h->pull_mutex);
  }
  g_mutex_unlock (&h->pull_mutex);

  return GST_FLOW_OK;
}

static gboolean
gst_harness_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstHarness * h = g_object_get_data (G_OBJECT (pad), HARNESS_KEY);
  (void) parent;
  g_assert (h != NULL);
  g_async_queue_push (h->src_event_queue, event);
  return TRUE;
}

static gboolean
gst_harness_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstHarness * h = g_object_get_data (G_OBJECT (pad), HARNESS_KEY);
  gboolean forward;

  g_assert (h != NULL);
  (void) parent;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
    case GST_EVENT_CAPS:
    case GST_EVENT_SEGMENT:
      forward = TRUE;
      break;
    default:
      forward = FALSE;
      break;
  }

  if (forward && h->sink_forward_pad) {
    gst_pad_push_event (h->sink_forward_pad, event);
  } else {
    g_async_queue_push (h->sink_event_queue, event);
  }

  return TRUE;
}

static void
gst_harness_decide_allocation (GstHarness * h, GstCaps * caps)
{
  GstQuery * query;
  GstAllocator * allocator;
  GstAllocationParams params;
  GstBufferPool * pool = NULL;
  guint size, min, max;

  query = gst_query_new_allocation (caps, FALSE);
  gst_pad_peer_query (h->srcpad, query);

  if (gst_query_get_n_allocation_params (query) > 0) {
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
#if 0
    /* Most elements create their own pools if pool == NULL. Not sure if we
     * want to do that in the harness since we may want to test the pool
     * implementation of the elements. Not creating a pool will however ignore
     * the returned size. */
    if (pool == NULL)
      pool = gst_buffer_pool_new ();
#endif
  } else {
    pool = NULL;
    size = min = max = 0;
  }
  gst_query_unref (query);

  if (pool) {
    GstStructure * config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, min, max);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);
    gst_buffer_pool_set_config (pool, config);
  }

  if (pool != h->pool) {
    if (h->pool != NULL)
      gst_buffer_pool_set_active (h->pool, FALSE);
    if (pool)
      gst_buffer_pool_set_active (pool, TRUE);
  }

  h->allocation_params = params;
  if (h->allocator)
    gst_object_unref (h->allocator);
  h->allocator = allocator;
  if (h->pool)
    gst_object_unref (h->pool);
  h->pool = pool;
}

static void
gst_harness_negotiate (GstHarness * h)
{
  GstCaps * caps;

  caps = gst_pad_get_current_caps (h->srcpad);
  if (caps != NULL) {
    gst_harness_decide_allocation (h, caps);
    gst_caps_unref (caps);
  } else {
    GST_FIXME_OBJECT (h, "Cannot negotiate allocation because caps is not set");
  }
}

void
gst_harness_set_src_caps (GstHarness * h, GstCaps * caps)
{
  GstSegment segment;

  g_assert (gst_pad_push_event (h->srcpad, gst_event_new_caps (caps)));
  gst_caps_take (&h->src_caps, caps);

  gst_segment_init (&segment, GST_FORMAT_TIME);
  g_assert (gst_pad_push_event (h->srcpad, gst_event_new_segment (&segment)));
}

void
gst_harness_set_sink_caps (GstHarness * h, GstCaps * caps)
{
  gst_caps_take (&h->sink_caps, caps);
  gst_pad_push_event (h->sinkpad, gst_event_new_reconfigure ());
}

static gboolean
gst_harness_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = TRUE;
  GstHarness * h = g_object_get_data (G_OBJECT (pad), HARNESS_KEY);
  g_assert (h != NULL);

  // FIXME: forward all queries?

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      gst_query_set_latency (query, TRUE, h->latency_min, h->latency_max);
      break;
    case GST_QUERY_CAPS:
    {
      GstCaps * caps, * filter = NULL;

      caps = h->sink_caps ? gst_caps_ref (h->sink_caps) : gst_caps_new_any ();

      gst_query_parse_caps (query, &filter);
      if (filter != NULL) {
        gst_caps_take (&caps,
            gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST));
      }

      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
    }
      break;
    case GST_QUERY_ALLOCATION:
    {
      if (h->sink_forward_pad != NULL) {
        GstPad * peer = gst_pad_get_peer (h->sink_forward_pad);
        g_assert (peer != NULL);
        res = gst_pad_query (peer, query);
        gst_object_unref (peer);
      } else {
        GstCaps * caps;
        gboolean need_pool;

        gst_query_parse_allocation (query, &caps, &need_pool);

        /* FIXME: Can this be removed? */
        g_assert_cmpuint (0, ==, gst_query_get_n_allocation_params (query));
        gst_query_add_allocation_param (query,
            h->propose_allocator, &h->propose_params);

        GST_DEBUG_OBJECT (pad, "proposing allocation %" GST_PTR_FORMAT,
            h->propose_allocator);
      }
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
  }

  return res;
}

static gboolean
gst_harness_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = TRUE;
  GstHarness * h = g_object_get_data (G_OBJECT (pad), HARNESS_KEY);
  g_assert (h != NULL);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      gst_query_set_latency (query, TRUE, h->latency_min, h->latency_max);
      break;
    case GST_QUERY_CAPS:
    {
      GstCaps * caps, * filter = NULL;

      caps = h->src_caps ? gst_caps_ref (h->src_caps) : gst_caps_new_any ();

      gst_query_parse_caps (query, &filter);
      if (filter != NULL) {
        gst_caps_take (&caps,
            gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST));
      }

      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
    }
      break;
    default:
      res = gst_pad_query_default (pad, parent, query);
  }
  return res;
}

GstHarness *
gst_harness_new (const gchar * name)
{
  return gst_harness_new_with_padnames (name, "sink", "src");
}

GstHarness *
gst_harness_new_with_element (GstElement * element,
    const gchar * sinkpad, const gchar * srcpad)
{
  return gst_harness_new_full (element,
      &hsrctemplate, sinkpad, &hsinktemplate, srcpad);
}

GstHarness *
gst_harness_new_with_templates (const gchar * element_name,
    GstStaticPadTemplate * hsrc, GstStaticPadTemplate * hsink)
{
  GstHarness * h;
  GstElement * element = gst_element_factory_make (element_name, NULL);
  g_assert (element != NULL);

  h = gst_harness_new_full (element, hsrc, "sink", hsink, "src");
  gst_object_unref (element);
  return h;
}

GstHarness *
gst_harness_new_with_padnames (const gchar * element_name,
    const gchar * sinkpad, const gchar * srcpad)
{
  GstHarness * h;
  GstElement * element = gst_element_factory_make (element_name, NULL);
  g_assert (element != NULL);

  h = gst_harness_new_with_element (element, sinkpad, srcpad);
  gst_object_unref (element);
  return h;
}

static void
gst_harness_element_ref (GstHarness * h)
{
  guint * data = g_object_get_data (G_OBJECT (h->element), HARNESS_REF);
  if (data == NULL) {
      data = g_new0 (guint, 1);
      *data = 1;
      g_object_set_data_full (G_OBJECT (h->element), HARNESS_REF, data, g_free);
  } else {
      (*data)++;
  }
}

static guint
gst_harness_element_unref (GstHarness * h)
{
  guint * data = g_object_get_data (G_OBJECT (h->element), HARNESS_REF);
  g_assert (data != NULL);
  (*data)--;
  return *data;
}

static void
gst_harness_link_element_srcpad (GstHarness * h,
    const gchar * element_srcpad_name)
{
  GstPad * srcpad = gst_element_get_static_pad (h->element, element_srcpad_name);
  if (srcpad == NULL)
    srcpad = gst_element_get_request_pad (h->element, element_srcpad_name);
  g_assert (srcpad);
  g_assert_cmpint (gst_pad_link (srcpad, h->sinkpad), ==, GST_PAD_LINK_OK);
  g_free (h->element_srcpad_name);
  h->element_srcpad_name = gst_pad_get_name (srcpad);

  gst_object_unref (srcpad);
}

static void
gst_harness_link_element_sinkpad (GstHarness * h,
    const gchar * element_sinkpad_name)
{
  GstPad * sinkpad = gst_element_get_static_pad (h->element, element_sinkpad_name);
  if (sinkpad == NULL)
    sinkpad = gst_element_get_request_pad (h->element, element_sinkpad_name);
  g_assert (sinkpad);
  g_assert_cmpint (gst_pad_link (h->srcpad, sinkpad), ==, GST_PAD_LINK_OK);
  g_free (h->element_sinkpad_name);
  h->element_sinkpad_name = gst_pad_get_name (sinkpad);

  gst_object_unref (sinkpad);
}

static void
gst_harness_setup_src_pad (GstHarness * h,
    GstStaticPadTemplate * src_tmpl, const gchar * element_sinkpad_name)
{
  g_assert (src_tmpl);

  h->src_event_queue = g_async_queue_new_full ((GDestroyNotify)gst_event_unref);

  /* sending pad */
  h->srcpad = gst_pad_new_from_static_template (src_tmpl, "src");
  g_assert (h->srcpad);
  g_object_set_data (G_OBJECT (h->srcpad), HARNESS_KEY, h);

  gst_pad_set_query_function (h->srcpad, gst_harness_src_query);
  gst_pad_set_event_function (h->srcpad, gst_harness_src_event);

  gst_pad_set_active (h->srcpad, TRUE);

  if (element_sinkpad_name)
    gst_harness_link_element_sinkpad (h, element_sinkpad_name);
}

static void
gst_harness_setup_sink_pad (GstHarness * h,
    GstStaticPadTemplate * sink_tmpl, const gchar * element_srcpad_name)
{
  g_assert (sink_tmpl);

  h->buffer_queue = g_async_queue_new_full ((GDestroyNotify)gst_buffer_unref);
  h->sink_event_queue = g_async_queue_new_full ((GDestroyNotify)gst_event_unref);

  /* receiving pad */
  h->sinkpad = gst_pad_new_from_static_template (sink_tmpl, "sink");
  g_assert (h->sinkpad);
  g_object_set_data (G_OBJECT (h->sinkpad), HARNESS_KEY, h);

  gst_pad_set_chain_function (h->sinkpad, gst_harness_chain);
  gst_pad_set_query_function (h->sinkpad, gst_harness_sink_query);
  gst_pad_set_event_function (h->sinkpad, gst_harness_sink_event);

  gst_pad_set_active (h->sinkpad, TRUE);

  if (element_srcpad_name)
    gst_harness_link_element_srcpad (h, element_srcpad_name);
}

void
gst_harness_add_element_srcpad (GstHarness * h, GstPad * srcpad)
{
  gst_harness_setup_sink_pad (h, &hsinktemplate, NULL);
  g_assert_cmpint (gst_pad_link (srcpad, h->sinkpad), ==, GST_PAD_LINK_OK);
  g_free (h->element_srcpad_name);
  h->element_srcpad_name = gst_pad_get_name (srcpad);
}

void
gst_harness_play (GstHarness * h)
{
  GstState state, pending;

  g_assert_cmpint (GST_STATE_CHANGE_SUCCESS, ==,
      gst_element_set_state (h->element, GST_STATE_PLAYING));
  g_assert_cmpint (GST_STATE_CHANGE_SUCCESS, ==,
      gst_element_get_state (h->element, &state, &pending, 0));
  g_assert_cmpint (GST_STATE_PLAYING, ==, state);
}

static void
turn_async_and_sync_off (GstElement * element)
{
  GObjectClass * class = G_OBJECT_GET_CLASS (element);
  if (g_object_class_find_property (class, "async"))
    g_object_set (element, "async", FALSE, NULL);
  if (g_object_class_find_property (class, "sync"))
    g_object_set (element, "sync", FALSE, NULL);
}

GstHarness *
gst_harness_new_full (GstElement * element,
    GstStaticPadTemplate * hsrc, const gchar * sinkpad,
    GstStaticPadTemplate * hsink, const gchar * srcpad)
{
  GstHarness * h;
  gboolean is_sink, is_src;

  g_return_val_if_fail (element != NULL, NULL);

  h = g_new0 (GstHarness, 1);
  g_assert (h != NULL);

  GST_DEBUG_OBJECT (h, "about to create new harness %p", h);
  h->element = gst_object_ref (element);
  h->latency_min = 0;
  h->latency_max = GST_CLOCK_TIME_NONE;
  h->sine_cont = 0;
  h->drop_buffers = FALSE;
  h->last_push_ts = GST_CLOCK_TIME_NONE;

  h->propose_allocator = NULL;
  gst_allocation_params_init (&h->propose_params);

  g_mutex_init (&h->pull_mutex);
  g_cond_init (&h->pull_cond);

  is_src =   GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SOURCE);
  is_sink =  GST_OBJECT_FLAG_IS_SET (element, GST_ELEMENT_FLAG_SINK);

  /* setup the loose srcpad linked to the element sinkpad */
  if (!is_src && hsrc)
    gst_harness_setup_src_pad (h, hsrc, sinkpad);

  /* setup the loose sinkpad linked to the element srcpad */
  if (!is_sink && hsink)
    gst_harness_setup_sink_pad (h, hsink, srcpad);

  /* as a harness sink, we should not need sync and async */
  if (is_sink)
    turn_async_and_sync_off (h->element);

  if (h->srcpad != NULL) {
    gchar * stream_id = g_strdup_printf ("%s-%p",
        GST_OBJECT_NAME (h->element), h);
    g_assert (gst_pad_push_event (h->srcpad,
          gst_event_new_stream_start (stream_id)));
    g_free (stream_id);
  }

  /* don't start sources, they start producing data! */
  if (!is_src)
    gst_harness_play (h);

  gst_harness_element_ref (h);

  GST_DEBUG_OBJECT (h, "created new harness %p with srcpad (%p, %s, %s) and sinkpad (%p, %s, %s)",
        h, h->srcpad, GST_DEBUG_PAD_NAME (h->srcpad), h->sinkpad, GST_DEBUG_PAD_NAME (h->sinkpad));

  h->stress = g_ptr_array_new_with_free_func (
      (GDestroyNotify)gst_harness_stress_free);

  return h;
}

GstHarness *
gst_harness_new_parse (const gchar * launchline)
{
  GstHarness * h;
  GstBin * bin;
  gchar * desc;
  GstPad * pad;
  GstIterator * iter;
  gboolean done = FALSE;

  g_return_val_if_fail (launchline != NULL, NULL);

  desc = g_strdup_printf ("bin.( %s )", launchline);
  bin = (GstBin *)gst_parse_launch_full (desc, NULL, GST_PARSE_FLAG_NONE, NULL);
  g_free (desc);

  if (G_UNLIKELY (bin == NULL))
    return NULL;

  /* find pads and ghost them if necessary */
  if ((pad = gst_bin_find_unlinked_pad (bin, GST_PAD_SRC)) != NULL) {
    gst_element_add_pad (GST_ELEMENT (bin), gst_ghost_pad_new ("src", pad));
    gst_object_unref (pad);
  }
  if ((pad = gst_bin_find_unlinked_pad (bin, GST_PAD_SINK)) != NULL) {
    gst_element_add_pad (GST_ELEMENT (bin), gst_ghost_pad_new ("sink", pad));
    gst_object_unref (pad);
  }

  iter = gst_bin_iterate_sinks (bin);
  while (!done) {
    GValue item = { 0, };

    switch (gst_iterator_next (iter, &item)) {
      case GST_ITERATOR_OK:
        turn_async_and_sync_off (GST_ELEMENT (g_value_get_object (&item)));
        g_value_reset (&item);
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        gst_object_unref (bin);
        gst_iterator_free (iter);
        g_return_val_if_reached (NULL);
        break;
    }
  }
  gst_iterator_free (iter);

  h = gst_harness_new_full (GST_ELEMENT_CAST (bin),
      &hsrctemplate, "sink", &hsinktemplate, "src");
  gst_object_unref (bin);
  return h;
}

void
gst_harness_set_pull_mode (GstHarness * h)
{
  h->pull_mode_active = TRUE;
}

GstClockTime
gst_harness_query_latency (GstHarness * h)
{
  GstQuery * query;
  gboolean is_live;
  GstClockTime min = GST_CLOCK_TIME_NONE;
  GstClockTime max;

  query = gst_query_new_latency ();

  if (gst_pad_peer_query (h->sinkpad, query)) {
    gst_query_parse_latency (query, &is_live, &min, &max);
  }
  gst_query_unref (query);

  return min;
}

void
gst_harness_set_us_latency (GstHarness * h, GstClockTime latency)
{
  h->latency_min = latency;
}

GstBuffer *
gst_harness_create_buffer (GstHarness * h, gsize size)
{
  GstBuffer * ret = NULL;

  if (gst_pad_check_reconfigure (h->srcpad))
    gst_harness_negotiate (h);

  if (h->pool) {
    g_assert_cmpint (gst_buffer_pool_acquire_buffer (h->pool, &ret, NULL), ==,
        GST_FLOW_OK);
    if (gst_buffer_get_size (ret) != size) {
      GST_DEBUG_OBJECT (h,
          "use fallback, pool is configured with a different size (%zu != %zu)",
          size, gst_buffer_get_size (ret));
      gst_buffer_unref (ret);
      ret = NULL;
    }
  }

  if (!ret)
    ret = gst_buffer_new_allocate (h->allocator, size, &h->allocation_params);

  g_assert (ret != NULL);
  return ret;
}

GstFlowReturn
gst_harness_push (GstHarness * h, GstBuffer * buffer)
{
  g_assert (buffer != NULL);
  h->last_push_ts = GST_BUFFER_TIMESTAMP (buffer);
  return gst_pad_push (h->srcpad, buffer);
}

GstBuffer *
gst_harness_push_and_wait (GstHarness * h, GstBuffer * buffer)
{
  gst_harness_push (h, buffer);
  return gst_harness_pull (h);
}

gboolean
gst_harness_push_event (GstHarness * h, GstEvent * ev)
{
  return gst_pad_push_event (h->srcpad, ev);
}

gboolean
gst_harness_send_upstream_event (GstHarness * h, GstEvent * event)
{
  g_return_val_if_fail (event != NULL, FALSE);
  g_return_val_if_fail (GST_EVENT_IS_UPSTREAM (event), FALSE);

  return gst_pad_push_event (h->sinkpad, event);
}

GstBuffer *
gst_harness_pull (GstHarness * h)
{
  GstBuffer * buf = (GstBuffer *)g_async_queue_timeout_pop (
      h->buffer_queue, G_USEC_PER_SEC * 60);

  if (h->pull_mode_active) {
    g_mutex_lock (&h->pull_mutex);
    g_cond_signal (&h->pull_cond);
    g_mutex_unlock (&h->pull_mutex);
  }

  return buf;
}

GstBuffer *
gst_harness_try_pull (GstHarness * h)
{
  return (GstBuffer *)g_async_queue_try_pop (h->buffer_queue);
}

void
gst_harness_set_drop_buffers (GstHarness * h, gboolean drop_buffers)
{
  h->drop_buffers = drop_buffers;
}

GstEvent *
gst_harness_pull_upstream_event (GstHarness * h)
{
  return (GstEvent *)g_async_queue_timeout_pop (
      h->src_event_queue, G_USEC_PER_SEC * 60);
}

GstEvent *
gst_harness_try_pull_upstream_event (GstHarness *h)
{
  return (GstEvent *)g_async_queue_try_pop (h->src_event_queue);
}

gint
gst_harness_upstream_events_received (GstHarness * h)
{
  return g_async_queue_length (h->src_event_queue);
}

gint
gst_harness_events_received (GstHarness * h)
{
  return g_async_queue_length (h->sink_event_queue);
}

GstEvent *
gst_harness_pull_event (GstHarness * h)
{
  return (GstEvent *)g_async_queue_timeout_pop (
      h->sink_event_queue, G_USEC_PER_SEC * 60);
}

GstEvent *
gst_harness_try_pull_event (GstHarness * h)
{
  return (GstEvent *)g_async_queue_try_pop (h->sink_event_queue);
}

static void
gst_harness_setup_src_harness (GstHarness * h, gboolean has_clock_wait)
{
  h->src_harness->sink_forward_pad = gst_object_ref (h->srcpad);
  gst_harness_use_testclock (h->src_harness);
  h->src_harness->has_clock_wait = has_clock_wait;
}

void
gst_harness_add_src (GstHarness * h,
    const gchar * src_element_name, gboolean has_clock_wait)
{
  h->src_harness = gst_harness_new (src_element_name);
  gst_harness_setup_src_harness (h, has_clock_wait);
}

void
gst_harness_add_src_parse (GstHarness * h,
    const gchar * launchline, gboolean has_clock_wait)
{
  h->src_harness = gst_harness_new_parse (launchline);
  gst_harness_setup_src_harness (h, has_clock_wait);
}

GstFlowReturn
gst_harness_push_from_src (GstHarness * h)
{
  GstBuffer * buf;

  g_assert (h->src_harness);

  /* FIXME: this *is* the right time to start the src,
     but maybe a flag so we don't keep telling it to play? */
  gst_harness_play (h->src_harness);

  if (h->src_harness->has_clock_wait) {
    g_assert (gst_harness_crank_single_clock_wait (h->src_harness));
  }

  g_assert ((buf = gst_harness_pull (h->src_harness)) != NULL);
  return gst_harness_push (h, buf);
}

GstFlowReturn
gst_harness_src_crank_and_push_many (GstHarness * h, gint cranks, gint pushes)
{
  GstFlowReturn ret = GST_FLOW_OK;

  g_assert (h->src_harness);
  gst_harness_play (h->src_harness);

  for (int i = 0; i < cranks; i++)
    g_assert (gst_harness_crank_single_clock_wait (h->src_harness));

  for (int i = 0; i < pushes; i++) {
    GstBuffer * buf;
    g_assert ((buf = gst_harness_pull (h->src_harness)) != NULL);
    ret = gst_harness_push (h, buf);
    if (ret != GST_FLOW_OK)
      break;
  }

  return ret;
}

void
gst_harness_src_push_event (GstHarness * h)
{
  gst_harness_push_event (h, gst_harness_pull_event (h->src_harness));
}

void
gst_harness_add_sink (GstHarness * h, const gchar * sink_element_name)
{
  h->sink_harness = gst_harness_new (sink_element_name);
  h->sink_forward_pad = gst_object_ref (h->sink_harness->srcpad);
}

void
gst_harness_add_sink_parse (GstHarness * h, const gchar * launchline)
{
  h->sink_harness = gst_harness_new_parse (launchline);
  h->sink_forward_pad = gst_object_ref (h->sink_harness->srcpad);
}

void
gst_harness_push_to_sink (GstHarness * h)
{
  GstBuffer * buf;
  g_assert (h->sink_harness);
  g_assert ((buf = gst_harness_pull (h)) != NULL);
  gst_harness_push (h->sink_harness, buf);
}

void
gst_harness_sink_push_many (GstHarness * h, gint pushes)
{
  g_assert (h->sink_harness);
  for (int i = 0; i < pushes; i++) {
    GstBuffer * buf;
    g_assert ((buf = gst_harness_pull (h)) != NULL);
    gst_harness_push (h->sink_harness, buf);
  }
}

static gboolean
gst_pad_is_request_pad (GstPad * pad)
{
  GstPadTemplate * temp;
  if (pad == NULL)
    return FALSE;
  temp = gst_pad_get_pad_template (pad);
  if (temp == NULL)
    return FALSE;
  return GST_PAD_TEMPLATE_PRESENCE (temp) == GST_PAD_REQUEST;
}

void
gst_harness_teardown (GstHarness * h)
{
  if (h->pull_mode_active) {
    g_mutex_lock (&h->pull_mutex);
    h->pull_mode_active = FALSE;
    g_cond_signal (&h->pull_cond);
    g_mutex_unlock (&h->pull_mutex);
  }

  if (h->src_harness) {
    gst_harness_teardown (h->src_harness);
  }

  if (h->sink_harness) {
    gst_harness_teardown (h->sink_harness);
  }

  if (h->src_caps)
    gst_caps_unref (h->src_caps);

  if (h->sink_caps)
    gst_caps_unref (h->sink_caps);

  if (h->srcpad) {
    if (gst_pad_is_request_pad (GST_PAD_PEER (h->srcpad)))
      gst_element_release_request_pad (h->element, GST_PAD_PEER (h->srcpad));
    g_free (h->element_sinkpad_name);

    gst_pad_set_active (h->srcpad, FALSE);
    gst_object_unref (h->srcpad);

    g_async_queue_unref (h->src_event_queue);
  }

  if (h->sinkpad) {
    if (gst_pad_is_request_pad (GST_PAD_PEER (h->sinkpad)))
      gst_element_release_request_pad (h->element, GST_PAD_PEER (h->sinkpad));
    g_free (h->element_srcpad_name);

    gst_pad_set_active (h->sinkpad, FALSE);
    gst_object_unref (h->sinkpad);

    g_async_queue_unref (h->buffer_queue);
    g_async_queue_unref (h->sink_event_queue);
  }

  if (h->sink_forward_pad)
    gst_object_unref (h->sink_forward_pad);

  gst_object_replace ((GstObject**)&h->allocator, NULL);
  gst_object_replace ((GstObject**)&h->pool, NULL);

  /* if we hold the last ref, set to NULL */
  if (gst_harness_element_unref (h) == 0) {
    GstState state, pending;
    g_assert (gst_element_set_state (h->element, GST_STATE_NULL) ==
      GST_STATE_CHANGE_SUCCESS);
    g_assert (gst_element_get_state (h->element, &state, &pending, 0) ==
        GST_STATE_CHANGE_SUCCESS);
    g_assert (state == GST_STATE_NULL);
  }

  g_cond_clear (&h->pull_cond);
  g_mutex_clear (&h->pull_mutex);

  g_ptr_array_unref (h->stress);

  gst_object_unref (h->element);
  g_free(h);
}

void
gst_harness_use_systemclock (GstHarness * h)
{
  GstClock * clock = gst_system_clock_obtain ();
  g_assert (clock != NULL);
  gst_element_set_clock (h->element, clock);
  gst_object_unref (clock);
}

void
gst_harness_use_testclock (GstHarness * h)
{
  GstClock * clock = gst_test_clock_new ();
  g_assert (clock != NULL);
  gst_element_set_clock (h->element, clock);
  gst_object_unref (clock);
}

GstTestClock *
gst_harness_get_testclock (GstHarness * h)
{
  GstTestClock * testclock = NULL;
  GstClock * clock;

  clock = gst_element_get_clock (h->element);
  if (clock) {
    if (GST_IS_TEST_CLOCK (clock))
      testclock = GST_TEST_CLOCK (clock);
    else
      gst_object_unref (clock);
  }
  return testclock;
}

gboolean
gst_harness_set_time (GstHarness * h, GstClockTime time)
{
  GstTestClock * testclock;
  testclock = gst_harness_get_testclock (h);
  if (testclock == NULL)
    return FALSE;

  gst_test_clock_set_time (testclock, time);
  gst_object_unref (testclock);
  return TRUE;
}

gboolean
gst_harness_wait_for_clock_id_waits (GstHarness * h, guint waits, guint timeout)
{
  GstTestClock * testclock = gst_harness_get_testclock (h);
  gint64 start_time;
  gboolean ret;

  if (testclock == NULL)
    return FALSE;

  start_time = g_get_monotonic_time ();
  while (gst_test_clock_peek_id_count (testclock) < waits) {
    gint64 time_spent;

    g_usleep (G_USEC_PER_SEC / 1000);
    time_spent = g_get_monotonic_time () - start_time;
    if ((time_spent / G_USEC_PER_SEC) > timeout)
      break;
  }

  ret = (waits == gst_test_clock_peek_id_count (testclock));

  gst_object_unref (testclock);
  return ret;
}

gboolean
gst_harness_crank_single_clock_wait (GstHarness * h)
{
  GstTestClock * testclock = gst_harness_get_testclock (h);
  GstClockID res, pending;
  gboolean ret = FALSE;

  if (G_LIKELY (testclock != NULL)) {
    gst_test_clock_wait_for_next_pending_id (testclock, &pending);

    gst_test_clock_set_time (testclock, gst_clock_id_get_time (pending));
    res = gst_test_clock_process_next_clock_id (testclock);
    if (res == pending) {
      GST_DEBUG ("cranked time %" GST_TIME_FORMAT,
          GST_TIME_ARGS (gst_clock_get_time (GST_CLOCK (testclock))));
      ret = TRUE;
    } else {
      GST_WARNING ("testclock next id != pending (%p != %p)", res, pending);
    }

    gst_clock_id_unref (res);
    gst_clock_id_unref (pending);

    gst_object_unref (testclock);
  } else {
    GST_WARNING ("No testclock on element %s", GST_ELEMENT_NAME (h->element));
  }

  return ret;
}

gboolean
gst_harness_crank_multiple_clock_waits (GstHarness * h, guint waits)
{
  GstTestClock * testclock;
  GList * pending;
  guint processed;

  testclock = gst_harness_get_testclock (h);
  if (testclock == NULL)
    return FALSE;

  gst_test_clock_wait_for_multiple_pending_ids (testclock, waits, &pending);
  gst_harness_set_time (h, gst_test_clock_id_list_get_latest_time (pending));
  processed = gst_test_clock_process_id_list (testclock, pending);

  g_list_free_full (pending, gst_clock_id_unref);
  gst_object_unref (testclock);
  return processed == waits;
}

GstElement *
gst_harness_find_element (GstHarness * h, const char * element_name)
{
  gboolean done = FALSE;
  GstIterator * iter;
  GValue data = G_VALUE_INIT;

  iter = gst_bin_iterate_elements (GST_BIN (h->element));
  done = FALSE;

  while (!done) {
    switch (gst_iterator_next (iter, &data)) {
      case GST_ITERATOR_OK:
      {
        GstElement * element = g_value_get_object (&data);
        GstPluginFeature * feature = GST_PLUGIN_FEATURE (
            gst_element_get_factory (element));
        if (!strcmp (element_name, gst_plugin_feature_get_name (feature))) {
          gst_iterator_free (iter);
          return element;
        }
        g_value_reset (&data);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  gst_iterator_free (iter);

  return NULL;
}

void
gst_harness_add_probe (GstHarness * h,
    const gchar * element_name, const gchar * pad_name, GstPadProbeType mask,
    GstPadProbeCallback callback, gpointer user_data,
    GDestroyNotify destroy_data)
{
  GstElement * element = gst_harness_find_element (h, element_name);
  GstPad * pad = gst_element_get_static_pad (element, pad_name);
  gst_pad_add_probe (pad, mask, callback, user_data, destroy_data);
  gst_object_unref (pad);
  gst_object_unref (element);
}


void
gst_harness_set (GstHarness * h,
    const gchar * element_name, const gchar * first_property_name, ...)
{
  va_list var_args;
  GstElement * element = gst_harness_find_element (h, element_name);
  va_start (var_args, first_property_name);
  g_object_set_valist (G_OBJECT (element), first_property_name, var_args);
  va_end (var_args);
  gst_object_unref (element);
}

void
gst_harness_get (GstHarness * h,
    const gchar * element_name, const gchar * first_property_name, ...)
{
  va_list var_args;
  GstElement * element = gst_harness_find_element (h, element_name);
  va_start (var_args, first_property_name);
  g_object_get_valist (G_OBJECT (element), first_property_name, var_args);
  va_end (var_args);
  gst_object_unref (element);
}

void
gst_harness_signal_connect (GstHarness * h,
    const gchar * element_name, const gchar * signal_name,
    GCallback handler, gpointer data)
{
  GstElement * element = gst_harness_find_element (h, element_name);
  g_signal_connect (element, signal_name, handler, data);
  gst_object_unref (element);
}

void
gst_harness_signal (GstHarness * h,
    const gchar * element_name, const gchar * signal_name)
{
  GstElement * element = gst_harness_find_element (h, element_name);
  g_signal_emit_by_name (G_OBJECT (element), signal_name, NULL);
  gst_object_unref (element);
}

typedef void (*PushFunc)(GstHarness * h, GstMiniObject * obj);

typedef struct {
  GstHarness * h;
  GstMiniObject * obj;
  PushFunc func;
} AsyncTaskCtx;

static void
async_task_push (void *data)
{
  AsyncTaskCtx * udata = (AsyncTaskCtx *)data;
  udata->func (udata->h, udata->obj);
  g_free (data);
}

gpointer
gst_harness_push_async (GstHarness * h, GstBuffer * buffer, GstTaskPool * pool)
{
  AsyncTaskCtx * udata = g_new0 (AsyncTaskCtx, 1);
  udata->h = h;
  udata->obj = (GstMiniObject *)buffer;
  udata->func = (PushFunc)gst_harness_push;
  return gst_task_pool_push (pool, async_task_push, udata, NULL);
}

gpointer
gst_harness_push_event_async (GstHarness * h, GstEvent * ev, GstTaskPool * pool)
{
  AsyncTaskCtx * udata = g_new0 (AsyncTaskCtx, 1);
  udata->h = h;
  udata->obj = (GstMiniObject *)ev;
  udata->func = (PushFunc)gst_harness_push_event;
  return gst_task_pool_push (pool, async_task_push, udata, NULL);
}

void
gst_harness_dump_to_file (GstHarness * h, const gchar * filename)
{
  FILE * fd;
  GstBuffer * buf;
  fd = fopen (filename, "wb");
  g_assert (fd);

  while ((buf = g_async_queue_try_pop (h->buffer_queue))) {
    GstMapInfo info;
    gst_buffer_map (buf, &info, GST_MAP_READ);
    fwrite (info.data, 1, info.size, fd);
    gst_buffer_unmap (buf, &info);
    gst_buffer_unref (buf);
  }

  fflush (fd);
  fclose(fd);
}

/******************************************************************************/
/*       STRESS                                                               */
/******************************************************************************/
struct _GstHarnessThread {
  GstHarness * h;
  GThread * thread;
  gboolean running;

  gulong sleep;

  GDestroyNotify freefunc;
};

typedef struct {
  GstHarnessThread t;

  GFunc init;
  GFunc callback;
  gpointer data;
} GstHarnessCustomThread;

typedef struct {
  GstHarnessThread t;

  GstCaps * caps;
  GstSegment segment;
  GstHarnessPrepareBuffer func;
  gpointer data;
  GDestroyNotify notify;
} GstHarnessPushBufferThread;

typedef struct {
  GstHarnessThread t;

  GstEvent * event;
} GstHarnessPushEventThread;

typedef struct {
  GstHarnessThread t;

  gchar * name;
  GValue value;
} GstHarnessPropThread;

typedef struct {
  GstHarnessThread t;

  GstPadTemplate * templ;
  gchar * name;
  GstCaps * caps;
  gboolean release;

  GSList * pads;
} GstHarnessReqPadThread;

static void
gst_harness_thread_init (GstHarnessThread * t, GDestroyNotify freefunc,
    GstHarness * h, gulong sleep)
{
  t->freefunc = freefunc;
  t->h = h;
  t->sleep = sleep;

  g_ptr_array_add (h->stress, t);
}

static void
gst_harness_thread_free (GstHarnessThread * t)
{
  g_slice_free (GstHarnessThread, t);
}

static void
gst_harness_custom_thread_free (GstHarnessCustomThread * t)
{
  g_slice_free (GstHarnessCustomThread, t);
}

static void
gst_harness_push_buffer_thread_free (GstHarnessPushBufferThread * t)
{
  if (t != NULL) {
    gst_caps_replace (&t->caps, NULL);
    if (t->notify != NULL)
      t->notify (t->data);
    g_slice_free (GstHarnessPushBufferThread, t);
  }
}

static void
gst_harness_push_event_thread_free (GstHarnessPushEventThread * t)
{
  if (t != NULL) {
    gst_event_replace (&t->event, NULL);
    g_slice_free (GstHarnessPushEventThread, t);
  }
}

static void
gst_harness_property_thread_free (GstHarnessPropThread * t)
{
  if (t != NULL) {
    g_free (t->name);
    g_value_unset (&t->value);
    g_slice_free (GstHarnessPropThread, t);
  }
}

static void
gst_harness_requestpad_release (GstPad * pad, GstElement * element)
{
  gst_element_release_request_pad (element, pad);
  gst_object_unref (pad);
}

static void
gst_harness_requestpad_release_pads (GstHarnessReqPadThread * rpt)
{
  g_slist_foreach (rpt->pads, (GFunc)gst_harness_requestpad_release,
      rpt->t.h->element);
  g_slist_free (rpt->pads);
  rpt->pads = NULL;
}

static void
gst_harness_requestpad_thread_free (GstHarnessReqPadThread * t)
{
  if (t != NULL) {
    gst_object_replace ((GstObject **)&t->templ, NULL);
    g_free (t->name);
    gst_caps_replace (&t->caps, NULL);

    gst_harness_requestpad_release_pads (t);
    g_slice_free (GstHarnessReqPadThread, t);
  }
}

#define GST_HARNESS_THREAD_START(ID, t)                                        \
  (((GstHarnessThread *)t)->running = TRUE,                                    \
  ((GstHarnessThread *)t)->thread = g_thread_new (                             \
      "gst-harness-stress-"G_STRINGIFY(ID),                                    \
      (GThreadFunc)gst_harness_stress_##ID##_func, t))
#define GST_HARNESS_THREAD_END(t)                                              \
   (t->running = FALSE,                                                        \
   GPOINTER_TO_UINT (g_thread_join (t->thread)))

#define GST_HARNESS_STRESS_FUNC_BEGIN(ID, INIT)                                \
  static gpointer                                                              \
  gst_harness_stress_##ID##_func (GstHarnessThread * t)                        \
  {                                                                            \
    guint count = 0;                                                           \
    INIT;                                                                      \
                                                                               \
    while (t->running) {

#define GST_HARNESS_STRESS_FUNC_END()                                          \
      count++;                                                                 \
      g_usleep (t->sleep);                                                     \
    }                                                                          \
    return GUINT_TO_POINTER (count);                                           \
  }

static void
gst_harness_stress_free (GstHarnessThread * t)
{
  if (t != NULL && t->freefunc != NULL)
    t->freefunc (t);
}

GST_HARNESS_STRESS_FUNC_BEGIN (custom,
{
  GstHarnessCustomThread * ct = (GstHarnessCustomThread *)t;
  ct->init (ct, ct->data);
}
  )
{
  GstHarnessCustomThread * ct = (GstHarnessCustomThread *)t;
  ct->callback (ct, ct->data);
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (statechange, {})
{
  GstClock * clock = gst_element_get_clock (t->h->element);
  GstIterator * it;
  gboolean done = FALSE;

  g_assert (gst_element_set_state (t->h->element, GST_STATE_NULL) ==
      GST_STATE_CHANGE_SUCCESS);
  g_thread_yield ();

  it = gst_element_iterate_sink_pads (t->h->element);
  while (!done) {
    GValue item = G_VALUE_INIT;
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:
      {
        GstPad * sinkpad = g_value_get_object (&item);
        GstPad * srcpad = gst_pad_get_peer (sinkpad);
        if (srcpad != NULL) {
          gst_pad_unlink (srcpad, sinkpad);
          gst_pad_link (srcpad, sinkpad);
          gst_object_unref (srcpad);
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_ERROR:
        g_assert_not_reached ();
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
    g_value_unset (&item);
  }
  gst_iterator_free (it);

  g_assert (gst_element_set_state (t->h->element, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_SUCCESS);
  if (clock != NULL) {
    gst_element_set_clock (t->h->element, clock);
    gst_object_unref (clock);
  }
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (buffer,
{
  GstHarnessPushBufferThread * pt = (GstHarnessPushBufferThread *)t;
  gchar * sid;

  /* Push stream start, caps and segment events */
  sid = g_strdup_printf ("%s-%p", GST_OBJECT_NAME (t->h->element), t->h);
  g_assert (gst_pad_push_event (t->h->srcpad, gst_event_new_stream_start (sid)));
  g_free (sid);
  g_assert (gst_pad_push_event (t->h->srcpad, gst_event_new_caps (pt->caps)));
  g_assert (gst_pad_push_event (t->h->srcpad, gst_event_new_segment (&pt->segment)));
}
    )
{
  GstHarnessPushBufferThread * pt = (GstHarnessPushBufferThread *)t;
  gst_harness_push (t->h, pt->func (t->h, pt->data));
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (event, {})
{
  GstHarnessPushEventThread * pet = (GstHarnessPushEventThread *)t;
  gst_harness_push_event (t->h, gst_event_ref (pet->event));
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (upstream_event, {})
{
  GstHarnessPushEventThread * pet = (GstHarnessPushEventThread *)t;
  gst_harness_send_upstream_event (t->h, gst_event_ref (pet->event));
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (property, {})
{
  GstHarnessPropThread * pt = (GstHarnessPropThread *)t;
  GValue value = G_VALUE_INIT;

  g_object_set_property (G_OBJECT (t->h->element), pt->name, &pt->value);

  g_value_init (&value, G_VALUE_TYPE (&pt->value));
  g_object_get_property (G_OBJECT (t->h->element), pt->name, &value);
  g_value_reset (&value);
}
GST_HARNESS_STRESS_FUNC_END ()

GST_HARNESS_STRESS_FUNC_BEGIN (requestpad, {})
{
  GstHarnessReqPadThread * rpt = (GstHarnessReqPadThread *)t;
  GstPad * reqpad;

  if (rpt->release)
    gst_harness_requestpad_release_pads (rpt);
  g_thread_yield ();

  reqpad = gst_element_request_pad (t->h->element,
      rpt->templ, rpt->name, rpt->caps);
  g_assert (reqpad != NULL);

  rpt->pads = g_slist_prepend (rpt->pads, reqpad);
}
GST_HARNESS_STRESS_FUNC_END ()

guint
gst_harness_stress_thread_stop (GstHarnessThread * t)
{
  guint ret;

  g_return_val_if_fail (t != NULL, 0);

  ret = GST_HARNESS_THREAD_END (t);
  g_ptr_array_remove (t->h->stress, t);
  return ret;
}

GstHarnessThread *
gst_harness_stress_custom_start (GstHarness * h,
    GFunc init, GFunc callback, gpointer data, gulong sleep)
{
  GstHarnessCustomThread * t = g_slice_new0 (GstHarnessCustomThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_custom_thread_free, h, sleep);

  t->init = init;
  t->callback = callback;
  t->data = data;

  GST_HARNESS_THREAD_START (custom, t);
  return &t->t;
}

GstHarnessThread *
gst_harness_stress_statechange_start_full (GstHarness * h, gulong sleep)
{
  GstHarnessThread * t = g_slice_new0 (GstHarnessThread);
  gst_harness_thread_init (t,
      (GDestroyNotify)gst_harness_thread_free, h, sleep);
  GST_HARNESS_THREAD_START (statechange, t);
  return t;
}

static GstBuffer *
gst_harness_ref_buffer (GstHarness * h, gpointer data)
{
  (void) h;
  return gst_buffer_ref (GST_BUFFER_CAST (data));
}

GstHarnessThread *
gst_harness_stress_push_buffer_start_full (GstHarness * h,
    GstCaps * caps, const GstSegment * segment, GstBuffer * buf, gulong sleep)
{
  return gst_harness_stress_push_buffer_with_cb_start_full (h, caps, segment,
      gst_harness_ref_buffer, gst_buffer_ref (buf), (GDestroyNotify)gst_buffer_unref,
      sleep);
}

GstHarnessThread *
gst_harness_stress_push_buffer_with_cb_start_full (GstHarness * h,
    GstCaps * caps, const GstSegment * segment,
    GstHarnessPrepareBuffer func, gpointer data, GDestroyNotify notify,
    gulong sleep)
{
  GstHarnessPushBufferThread * t = g_slice_new0 (GstHarnessPushBufferThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_push_buffer_thread_free,
      h, sleep);

  gst_caps_replace (&t->caps, caps);
  t->segment = *segment;
  t->func = func;
  t->data = data;
  t->notify = notify;

  GST_HARNESS_THREAD_START (buffer, t);
  return &t->t;
}

GstHarnessThread *
gst_harness_stress_push_event_start_full (GstHarness * h,
    GstEvent * event, gulong sleep)
{
  GstHarnessPushEventThread * t = g_slice_new0 (GstHarnessPushEventThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_push_event_thread_free,
      h, sleep);

  t->event = gst_event_ref (event);
  GST_HARNESS_THREAD_START (event, t);
  return &t->t;
}

GstHarnessThread *
gst_harness_stress_send_upstream_event_start_full (GstHarness * h,
    GstEvent * event, gulong sleep)
{
  GstHarnessPushEventThread * t = g_slice_new0 (GstHarnessPushEventThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_push_event_thread_free,
      h, sleep);

  t->event = gst_event_ref (event);
  GST_HARNESS_THREAD_START (upstream_event, t);
  return &t->t;
}

GstHarnessThread *
gst_harness_stress_property_start_full (GstHarness * h,
    const gchar * name, const GValue * value, gulong sleep)
{
  GstHarnessPropThread * t = g_slice_new0 (GstHarnessPropThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_property_thread_free,
      h, sleep);

  t->name = g_strdup (name);
  g_value_init (&t->value, G_VALUE_TYPE (value));
  g_value_copy (value, &t->value);

  GST_HARNESS_THREAD_START (property, t);
  return &t->t;
}

GstHarnessThread *
gst_harness_stress_requestpad_start_full (GstHarness * h,
    GstPadTemplate * templ, const gchar * name, GstCaps * caps,
    gboolean release, gulong sleep)
{
  GstHarnessReqPadThread * t = g_slice_new0 (GstHarnessReqPadThread);
  gst_harness_thread_init (&t->t,
      (GDestroyNotify)gst_harness_requestpad_thread_free,
      h, sleep);

  t->templ = gst_object_ref (templ);
  t->name = g_strdup (name);
  gst_caps_replace (&t->caps, caps);
  t->release = release;

  GST_HARNESS_THREAD_START (requestpad, t);
  return &t->t;
}
