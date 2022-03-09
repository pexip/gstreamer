/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *               2000,2005 Wim Taymans <wim@fluendo.com>
 *
 * gstbaseidlesrc.c:
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

/**
 * SECTION:gstbaseidlesrc
 * @title: GstBaseIdleSrc
 * @short_description: Base class for getrange based source elements
 * @see_also: #GstPushSrc, #GstBaseTransform, #GstBaseSink
 *
 
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <gst/gst_private.h>
#include <gst/glib-compat-private.h>

#include "gstbaseidlesrc.h"
#include <gst/gst-i18n-lib.h>

GST_DEBUG_CATEGORY_STATIC (gst_base_idle_src_debug);
#define GST_CAT_DEFAULT gst_base_idle_src_debug

/* BaseIdleSrc signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

#define DEFAULT_DO_TIMESTAMP    FALSE

enum
{
  PROP_0,
  PROP_DO_TIMESTAMP
};

/* The src implementation need to respect the following locking order:
 *   1. STREAM_LOCK
 *   2. LIVE_LOCK
 *   3. OBJECT_LOCK
 */
struct _GstBaseIdleSrcPrivate
{
  /* if a stream-start event should be sent */
  gboolean stream_start_pending;        /* STREAM_LOCK */

  /* if segment should be sent and a
   * seqnum if it was originated by a seek */
  gboolean segment_pending;     /* OBJECT_LOCK */
  guint32 segment_seqnum;       /* OBJECT_LOCK */

  /* startup latency is the time it takes between going to PLAYING and producing
   * the first BUFFER with running_time 0. This value is included in the latency
   * reporting. */
  GstClockTime latency;         /* OBJECT_LOCK */
  /* timestamp offset, this is the offset add to the values of gst_times for
   * pseudo live sources */
  GstClockTimeDiff ts_offset;   /* OBJECT_LOCK */

  gboolean do_timestamp;        /* OBJECT_LOCK */

  /* QoS *//* with LOCK */
  gdouble proportion;           /* OBJECT_LOCK */
  GstClockTime earliest_time;   /* OBJECT_LOCK */

  GstBufferPool *pool;          /* OBJECT_LOCK */
  GstAllocator *allocator;      /* OBJECT_LOCK */
  GstAllocationParams params;   /* OBJECT_LOCK */

  GQueue *obj_queue;
  GThreadPool *thread_pool;
  gboolean caps_set;
};

static GstElementClass *parent_class = NULL;
static gint private_offset = 0;


static inline GstBaseIdleSrcPrivate *
gst_base_idle_src_get_instance_private (GstBaseIdleSrc * self)
{
  return (G_STRUCT_MEMBER_P (self, private_offset));
}

/**
 * gst_base_idle_src_set_format:
 * @src: base source instance
 * @format: the format to use
 *
 * Sets the default format of the source. This will be the format used
 * for sending SEGMENT events and for performing seeks.
 *
 * If a format of GST_FORMAT_BYTES is set, the element will be able to
 * operate in pull mode if the #GstBaseIdleSrcClass::is_seekable returns %TRUE.
 *
 * This function must only be called in states < %GST_STATE_PAUSED.
 */
void
gst_base_idle_src_set_format (GstBaseIdleSrc * src, GstFormat format)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));
  g_return_if_fail (GST_STATE (src) <= GST_STATE_READY);

  GST_OBJECT_LOCK (src);
  gst_segment_init (&src->segment, format);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_query_latency:
 * @src: the source
 * @live: (out) (allow-none): if the source is live
 * @min_latency: (out) (allow-none): the min latency of the source
 * @max_latency: (out) (allow-none): the max latency of the source
 *
 * Query the source for the latency parameters. @live will be %TRUE when @src is
 * configured as a live source. @min_latency and @max_latency will be set
 * to the difference between the running time and the timestamp of the first
 * buffer.
 *
 * This function is mostly used by subclasses.
 *
 * Returns: %TRUE if the query succeeded.
 */
gboolean
gst_base_idle_src_query_latency (GstBaseIdleSrc * src, gboolean * live,
    GstClockTime * min_latency, GstClockTime * max_latency)
{
  GstClockTime min;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  if (live)
    *live = src->is_live;

  /* if we have a startup latency, report this one, else report 0. Subclasses
   * are supposed to override the query function if they want something
   * else. */
  if (src->priv->latency != -1)
    min = src->priv->latency;
  else
    min = 0;

  if (min_latency)
    *min_latency = min;
  if (max_latency)
    *max_latency = min;

  GST_LOG_OBJECT (src, "latency: live %d, min %" GST_TIME_FORMAT
      ", max %" GST_TIME_FORMAT, src->is_live, GST_TIME_ARGS (min),
      GST_TIME_ARGS (min));
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

/**
 * gst_base_idle_src_set_do_timestamp:
 * @src: the source
 * @timestamp: enable or disable timestamping
 *
 * Configure @src to automatically timestamp outgoing buffers based on the
 * current running_time of the pipeline. This property is mostly useful for live
 * sources.
 */
void
gst_base_idle_src_set_do_timestamp (GstBaseIdleSrc * src, gboolean timestamp)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));

  GST_OBJECT_LOCK (src);
  src->priv->do_timestamp = timestamp;
  if (timestamp && src->segment.format != GST_FORMAT_TIME)
    gst_segment_init (&src->segment, GST_FORMAT_TIME);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_get_do_timestamp:
 * @src: the source
 *
 * Query if @src timestamps outgoing buffers based on the current running_time.
 *
 * Returns: %TRUE if the base class will automatically timestamp outgoing buffers.
 */
gboolean
gst_base_idle_src_get_do_timestamp (GstBaseIdleSrc * src)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_OBJECT_LOCK (src);
  res = src->priv->do_timestamp;
  GST_OBJECT_UNLOCK (src);

  return res;
}

/**
 * gst_base_idle_src_new_segment:
 * @src: a #GstBaseIdleSrc
 * @segment: a pointer to a #GstSegment
 *
 * Prepare a new segment for emission downstream. This function must
 * only be called by derived sub-classes, and only from the #GstBaseIdleSrcClass::create function,
 * as the stream-lock needs to be held.
 *
 * The format for the @segment must be identical with the current format
 * of the source, as configured with gst_base_idle_src_set_format().
 *
 * The format of @src must not be %GST_FORMAT_UNDEFINED and the format
 * should be configured via gst_base_idle_src_set_format() before calling this method.
 *
 * Returns: %TRUE if preparation of new segment succeeded.
 *
 * Since: 1.18
 */
gboolean
gst_base_idle_src_new_segment (GstBaseIdleSrc * src, const GstSegment * segment)
{
  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);
  g_return_val_if_fail (segment != NULL, FALSE);

  GST_OBJECT_LOCK (src);

  if (src->segment.format == GST_FORMAT_UNDEFINED) {
    /* subclass must set valid format before calling this method */
    GST_WARNING_OBJECT (src, "segment format is not configured yet, ignore");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }

  if (src->segment.format != segment->format) {
    GST_WARNING_OBJECT (src, "segment format mismatched, ignore");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }

  gst_segment_copy_into (segment, &src->segment);

  /* Mark pending segment. Will be sent before next data */
  src->priv->segment_pending = TRUE;
  src->priv->segment_seqnum = gst_util_seqnum_next ();

  GST_DEBUG_OBJECT (src, "Starting new segment %" GST_SEGMENT_FORMAT, segment);

  GST_OBJECT_UNLOCK (src);

  src->running = TRUE;

  return TRUE;
}

static void
gst_base_idle_src_queue_object (GstBaseIdleSrc * src, GstMiniObject * obj)
{
  GST_LOG_OBJECT (src, "Queuing: %" GST_PTR_FORMAT, obj);
  GST_OBJECT_LOCK (src);
  g_queue_push_tail (src->priv->obj_queue, obj);
  GST_OBJECT_UNLOCK (src);
}

/**
 * gst_base_idle_src_set_caps:
 * @src: a #GstBaseIdleSrc
 * @caps: (transfer none): a #GstCaps
 *
 * Set new caps on the src source pad.
 * This *MUST* be called before submitting any buffers!
 * 
 * Returns: %TRUE if the caps could be set
 */
gboolean
gst_base_idle_src_set_caps (GstBaseIdleSrc * src, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;
  gboolean res = TRUE;
  GstCaps *current_caps;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  current_caps = gst_pad_get_current_caps (GST_BASE_IDLE_SRC_PAD (src));
  if (current_caps && gst_caps_is_equal (current_caps, caps)) {
    GST_DEBUG_OBJECT (src, "New caps equal to old ones: %" GST_PTR_FORMAT,
        caps);
    res = TRUE;
  } else {
    if (bclass->set_caps)
      res = bclass->set_caps (src, caps);

    if (res) {
      gst_base_idle_src_queue_object (src,
          (GstMiniObject *) gst_event_new_caps (caps));
      src->priv->caps_set = TRUE;
    }
  }

  if (current_caps)
    gst_caps_unref (current_caps);

  return res;
}

static GstCaps *
gst_base_idle_src_default_get_caps (GstBaseIdleSrc * bsrc, GstCaps * filter)
{
  GstCaps *caps = NULL;
  GstPadTemplate *pad_template;
  GstBaseIdleSrcClass *bclass;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (bsrc);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (bclass), "src");

  if (pad_template != NULL) {
    caps = gst_pad_template_get_caps (pad_template);

    if (filter) {
      GstCaps *intersection;

      intersection =
          gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
      caps = intersection;
    }
  }
  return caps;
}

static GstCaps *
gst_base_idle_src_default_fixate (GstBaseIdleSrc * bsrc, GstCaps * caps)
{
  GST_DEBUG_OBJECT (bsrc, "using default caps fixate function");
  return gst_caps_fixate (caps);
}

static GstCaps *
gst_base_idle_src_fixate (GstBaseIdleSrc * bsrc, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (bsrc);

  if (bclass->fixate)
    caps = bclass->fixate (bsrc, caps);

  return caps;
}

static gboolean
gst_base_idle_src_default_query (GstBaseIdleSrc * src, GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      GST_DEBUG_OBJECT (src, "position query in format %s",
          gst_format_get_name (format));

      switch (format) {
        case GST_FORMAT_PERCENT:
        {
          gint64 percent;
          gint64 position;
          gint64 duration;

          GST_OBJECT_LOCK (src);
          position = src->segment.position;
          duration = src->segment.duration;
          GST_OBJECT_UNLOCK (src);

          if (position != -1 && duration != -1) {
            if (position < duration)
              percent = gst_util_uint64_scale (GST_FORMAT_PERCENT_MAX, position,
                  duration);
            else
              percent = GST_FORMAT_PERCENT_MAX;
          } else
            percent = -1;

          gst_query_set_position (query, GST_FORMAT_PERCENT, percent);
          res = TRUE;
          break;
        }
        default:
        {
          gint64 position;
          GstFormat seg_format;

          GST_OBJECT_LOCK (src);
          position =
              gst_segment_to_stream_time (&src->segment, src->segment.format,
              src->segment.position);
          seg_format = src->segment.format;
          GST_OBJECT_UNLOCK (src);

          if (position != -1) {
            /* convert to requested format */
            res =
                gst_pad_query_convert (src->srcpad, seg_format,
                position, format, &position);
          } else
            res = TRUE;

          if (res)
            gst_query_set_position (query, format, position);

          break;
        }
      }
      break;
    }

    case GST_QUERY_SEGMENT:
    {
      GstFormat format;
      gint64 start, stop;

      GST_OBJECT_LOCK (src);

      format = src->segment.format;

      start =
          gst_segment_to_stream_time (&src->segment, format,
          src->segment.start);
      if ((stop = src->segment.stop) == -1)
        stop = src->segment.duration;
      else
        stop = gst_segment_to_stream_time (&src->segment, format, stop);

      gst_query_set_segment (query, src->segment.rate, format, start, stop);

      GST_OBJECT_UNLOCK (src);
      res = TRUE;
      break;
    }

    case GST_QUERY_FORMATS:
    {
      gst_query_set_formats (query, 3, GST_FORMAT_DEFAULT,
          GST_FORMAT_BYTES, GST_FORMAT_PERCENT);
      res = TRUE;
      break;
    }
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);

      /* we can only convert between equal formats... */
      if (src_fmt == dest_fmt) {
        dest_val = src_val;
        res = TRUE;
      } else
        res = FALSE;

      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    case GST_QUERY_LATENCY:
    {
      GstClockTime min, max;
      gboolean live;

      /* Subclasses should override and implement something useful */
      res = gst_base_idle_src_query_latency (src, &live, &min, &max);

      GST_LOG_OBJECT (src, "report latency: live %d, min %" GST_TIME_FORMAT
          ", max %" GST_TIME_FORMAT, live, GST_TIME_ARGS (min),
          GST_TIME_ARGS (max));

      gst_query_set_latency (query, live, min, max);
      break;
    }
    case GST_QUERY_JITTER:
    case GST_QUERY_RATE:
      res = FALSE;
      break;
    case GST_QUERY_BUFFERING:
    {
      GstFormat format, seg_format;
      gint64 start, stop, estimated;

      gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);

      GST_DEBUG_OBJECT (src, "buffering query in format %s",
          gst_format_get_name (format));

      GST_OBJECT_LOCK (src);
      estimated = -1;
      start = -1;
      stop = -1;
      seg_format = src->segment.format;
      GST_OBJECT_UNLOCK (src);

      /* convert to required format. When the conversion fails, we can't answer
       * the query. When the value is unknown, we can don't perform conversion
       * but report TRUE. */
      if (format != GST_FORMAT_PERCENT && stop != -1) {
        res = gst_pad_query_convert (src->srcpad, seg_format,
            stop, format, &stop);
      } else {
        res = TRUE;
      }
      if (res && format != GST_FORMAT_PERCENT && start != -1)
        res = gst_pad_query_convert (src->srcpad, seg_format,
            start, format, &start);

      gst_query_set_buffering_range (query, format, start, stop, estimated);
      break;
    }
    case GST_QUERY_CAPS:
    {
      GstBaseIdleSrcClass *bclass;
      GstCaps *caps, *filter;

      bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);
      if (bclass->get_caps) {
        gst_query_parse_caps (query, &filter);
        if ((caps = bclass->get_caps (src, filter))) {
          gst_query_set_caps_result (query, caps);
          gst_caps_unref (caps);
          res = TRUE;
        } else {
          res = FALSE;
        }
      } else
        res = FALSE;
      break;
    }
    case GST_QUERY_URI:{
      if (GST_IS_URI_HANDLER (src)) {
        gchar *uri = gst_uri_handler_get_uri (GST_URI_HANDLER (src));

        if (uri != NULL) {
          gst_query_set_uri (query, uri);
          g_free (uri);
          res = TRUE;
        } else {
          res = FALSE;
        }
      } else {
        res = FALSE;
      }
      break;
    }
    default:
      res = FALSE;
      break;
  }
  GST_DEBUG_OBJECT (src, "query %s returns %d", GST_QUERY_TYPE_NAME (query),
      res);

  return res;
}

static gboolean
gst_base_idle_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstBaseIdleSrc *src;
  GstBaseIdleSrcClass *bclass;
  gboolean result = FALSE;

  src = GST_BASE_IDLE_SRC (parent);
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  if (bclass->query)
    result = bclass->query (src, query);

  return result;
}

static void
gst_base_idle_src_set_pool_flushing (GstBaseIdleSrc * src, gboolean flushing)
{
  GstBaseIdleSrcPrivate *priv = src->priv;
  GstBufferPool *pool;

  GST_OBJECT_LOCK (src);
  if ((pool = priv->pool))
    pool = gst_object_ref (pool);
  GST_OBJECT_UNLOCK (src);

  if (pool) {
    gst_buffer_pool_set_flushing (pool, flushing);
    gst_object_unref (pool);
  }
}

static GstFlowReturn
gst_base_idle_src_default_alloc (GstBaseIdleSrc * src, guint64 offset,
    guint size, GstBuffer ** buffer)
{
  GstFlowReturn ret;
  GstBaseIdleSrcPrivate *priv = src->priv;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;

  GST_OBJECT_LOCK (src);
  if (priv->pool) {
    pool = gst_object_ref (priv->pool);
  } else if (priv->allocator) {
    allocator = gst_object_ref (priv->allocator);
  }
  params = priv->params;
  GST_OBJECT_UNLOCK (src);

  if (pool) {
    ret = gst_buffer_pool_acquire_buffer (pool, buffer, NULL);
  } else if (size != -1) {
    *buffer = gst_buffer_new_allocate (allocator, size, &params);
    if (G_UNLIKELY (*buffer == NULL))
      goto alloc_failed;

    ret = GST_FLOW_OK;
  } else {
    GST_WARNING_OBJECT (src, "Not trying to alloc %u bytes. Blocksize not set?",
        size);
    goto alloc_failed;
  }

done:
  if (pool)
    gst_object_unref (pool);
  if (allocator)
    gst_object_unref (allocator);

  return ret;

  /* ERRORS */
alloc_failed:
  {
    GST_ERROR_OBJECT (src, "Failed to allocate %u bytes", size);
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

/* all events send to this element directly. This is mainly done from the
 * application.
 */
static gboolean
gst_base_idle_src_send_event (GstElement * element, GstEvent * event)
{
  GstBaseIdleSrc *src;
  gboolean result = FALSE;

  src = GST_BASE_IDLE_SRC (element);

  GST_DEBUG_OBJECT (src, "handling event %p %" GST_PTR_FORMAT, event, event);

  switch (GST_EVENT_TYPE (event)) {
      /* downstream serialized events */
    case GST_EVENT_EOS:
    {
      gst_base_idle_src_set_pool_flushing (src, TRUE);
      event = NULL;
      result = TRUE;
      break;
    }
    case GST_EVENT_SEGMENT:
      /* sending random SEGMENT downstream can break sync. */
      break;
    case GST_EVENT_TAG:
    case GST_EVENT_SINK_MESSAGE:
    case GST_EVENT_CUSTOM_DOWNSTREAM:
    case GST_EVENT_CUSTOM_BOTH:
    case GST_EVENT_PROTECTION:
      /* Insert TAG, CUSTOM_DOWNSTREAM, CUSTOM_BOTH, PROTECTION in the dataflow */
      gst_base_idle_src_queue_object (src, (GstMiniObject *) event);
      event = NULL;
      result = TRUE;
      break;
    case GST_EVENT_BUFFERSIZE:
      /* does not seem to make much sense currently */
      break;

      /* upstream events */
    case GST_EVENT_QOS:
      /* elements should override send_event and do something */
      break;
    case GST_EVENT_NAVIGATION:
      /* could make sense for elements that do something with navigation events
       * but then they would need to override the send_event function */
      break;
    case GST_EVENT_LATENCY:
      /* does not seem to make sense currently */
      break;

      /* custom events */
    case GST_EVENT_CUSTOM_UPSTREAM:
      /* override send_event if you want this */
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM_OOB:
    case GST_EVENT_CUSTOM_BOTH_OOB:
      /* insert a random custom event into the pipeline */
      GST_DEBUG_OBJECT (src, "pushing custom OOB event downstream");
      result = gst_pad_push_event (src->srcpad, event);
      /* we gave away the ref to the event in the push */
      event = NULL;
      break;
    default:
      break;
  }


  return result;
}

static void
gst_base_idle_src_update_qos (GstBaseIdleSrc * src,
    gdouble proportion, GstClockTimeDiff diff, GstClockTime timestamp)
{
  GST_CAT_DEBUG_OBJECT (GST_CAT_QOS, src,
      "qos: proportion: %lf, diff %" G_GINT64_FORMAT ", timestamp %"
      GST_TIME_FORMAT, proportion, diff, GST_TIME_ARGS (timestamp));

  GST_OBJECT_LOCK (src);
  src->priv->proportion = proportion;
  src->priv->earliest_time = timestamp + diff;
  GST_OBJECT_UNLOCK (src);
}


static gboolean
gst_base_idle_src_default_event (GstBaseIdleSrc * src, GstEvent * event)
{
  gboolean result;

  GST_DEBUG_OBJECT (src, "handle event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      gdouble proportion;
      GstClockTimeDiff diff;
      GstClockTime timestamp;

      gst_event_parse_qos (event, NULL, &proportion, &diff, &timestamp);
      gst_base_idle_src_update_qos (src, proportion, diff, timestamp);
      result = TRUE;
      break;
    }
    case GST_EVENT_RECONFIGURE:
      result = TRUE;
      break;
    case GST_EVENT_LATENCY:
      result = TRUE;
      break;
    default:
      result = FALSE;
      break;
  }
  return result;
}

static gboolean
gst_base_idle_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstBaseIdleSrc *src;
  GstBaseIdleSrcClass *bclass;
  gboolean result = FALSE;

  src = GST_BASE_IDLE_SRC (parent);
  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  if (bclass->event) {
    if (!(result = bclass->event (src, event)))
      goto subclass_failed;
  }

done:
  gst_event_unref (event);

  return result;

  /* ERRORS */
subclass_failed:
  {
    GST_DEBUG_OBJECT (src, "subclass refused event");
    goto done;
  }
}

static void
gst_base_idle_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBaseIdleSrc *src;

  src = GST_BASE_IDLE_SRC (object);

  switch (prop_id) {
    case PROP_DO_TIMESTAMP:
      gst_base_idle_src_set_do_timestamp (src, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_base_idle_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstBaseIdleSrc *src;

  src = GST_BASE_IDLE_SRC (object);

  switch (prop_id) {
    case PROP_DO_TIMESTAMP:
      g_value_set_boolean (value, gst_base_idle_src_get_do_timestamp (src));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_base_idle_src_set_allocation (GstBaseIdleSrc * src,
    GstBufferPool * pool, GstAllocator * allocator,
    const GstAllocationParams * params)
{
  GstAllocator *oldalloc;
  GstBufferPool *oldpool;
  GstBaseIdleSrcPrivate *priv = src->priv;

  if (pool) {
    GST_DEBUG_OBJECT (src, "activate pool");
    if (!gst_buffer_pool_set_active (pool, TRUE))
      goto activate_failed;
  }

  GST_OBJECT_LOCK (src);
  oldpool = priv->pool;
  priv->pool = pool;

  oldalloc = priv->allocator;
  priv->allocator = allocator;

  if (priv->pool)
    gst_object_ref (priv->pool);
  if (priv->allocator)
    gst_object_ref (priv->allocator);

  if (params)
    priv->params = *params;
  else
    gst_allocation_params_init (&priv->params);
  GST_OBJECT_UNLOCK (src);

  if (oldpool) {
    /* only deactivate if the pool is not the one we're using */
    if (oldpool != pool) {
      GST_DEBUG_OBJECT (src, "deactivate old pool");
      gst_buffer_pool_set_active (oldpool, FALSE);
    }
    gst_object_unref (oldpool);
  }
  if (oldalloc) {
    gst_object_unref (oldalloc);
  }
  return TRUE;

  /* ERRORS */
activate_failed:
  {
    GST_ERROR_OBJECT (src, "failed to activate bufferpool.");
    return FALSE;
  }
}

static gboolean
gst_base_idle_src_decide_allocation_default (GstBaseIdleSrc * src,
    GstQuery * query)
{
  GstCaps *outcaps;
  GstBufferPool *pool;
  guint size, min, max;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstStructure *config;
  gboolean update_allocator;

  gst_query_parse_allocation (query, &outcaps, NULL);

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params (query) > 0) {
    /* try the allocator */
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
    update_allocator = TRUE;
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
    update_allocator = FALSE;
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    if (pool == NULL) {
      /* no pool, we can make our own */
      GST_DEBUG_OBJECT (src, "no pool, making new pool");
      pool = gst_buffer_pool_new ();
    }
  } else {
    pool = NULL;
    size = min = max = 0;
  }

  /* now configure */
  if (pool) {
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);

    /* buffer pool may have to do some changes */
    if (!gst_buffer_pool_set_config (pool, config)) {
      config = gst_buffer_pool_get_config (pool);

      /* If change are not acceptable, fallback to generic pool */
      if (!gst_buffer_pool_config_validate_params (config, outcaps, size, min,
              max)) {
        GST_DEBUG_OBJECT (src, "unsupported pool, making new pool");

        gst_object_unref (pool);
        pool = gst_buffer_pool_new ();
        gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
        gst_buffer_pool_config_set_allocator (config, allocator, &params);
      }

      if (!gst_buffer_pool_set_config (pool, config))
        goto config_failed;
    }
  }

  if (update_allocator)
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  else
    gst_query_add_allocation_param (query, allocator, &params);
  if (allocator)
    gst_object_unref (allocator);

  if (pool) {
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
    gst_object_unref (pool);
  }

  return TRUE;

config_failed:
  GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
      ("Failed to configure the buffer pool"),
      ("Configuration is most likely invalid, please report this issue."));
  gst_object_unref (pool);
  return FALSE;
}

static gboolean
gst_base_idle_src_prepare_allocation (GstBaseIdleSrc * src, GstCaps * caps)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result = TRUE;
  GstQuery *query;
  GstBufferPool *pool = NULL;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  /* make query and let peer pad answer, we don't really care if it worked or
   * not, if it failed, the allocation query would contain defaults and the
   * subclass would then set better values if needed */
  query = gst_query_new_allocation (caps, TRUE);
  if (!gst_pad_peer_query (src->srcpad, query)) {
    /* not a problem, just debug a little */
    GST_DEBUG_OBJECT (src, "peer ALLOCATION query failed");
  }

  g_assert (bclass->decide_allocation != NULL);
  result = bclass->decide_allocation (src, query);

  GST_DEBUG_OBJECT (src, "ALLOCATION (%d) params: %" GST_PTR_FORMAT, result,
      query);

  if (!result)
    goto no_decide_allocation;

  /* we got configuration from our peer or the decide_allocation method,
   * parse them */
  if (gst_query_get_n_allocation_params (query) > 0) {
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
  } else {
    allocator = NULL;
    gst_allocation_params_init (&params);
  }

  if (gst_query_get_n_allocation_pools (query) > 0)
    gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);

  result = gst_base_idle_src_set_allocation (src, pool, allocator, &params);

  if (allocator)
    gst_object_unref (allocator);
  if (pool)
    gst_object_unref (pool);

  gst_query_unref (query);

  return result;

  /* Errors */
no_decide_allocation:
  {
    GST_WARNING_OBJECT (src, "Subclass failed to decide allocation");
    gst_query_unref (query);

    return result;
  }
}

/* default negotiation code.
 *
 * Take intersection between src and sink pads, take first
 * caps and fixate.
 */
static gboolean
gst_base_idle_src_default_negotiate (GstBaseIdleSrc * src)
{
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_IDLE_SRC_PAD (src), NULL);
  GST_DEBUG_OBJECT (src, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL || gst_caps_is_any (thiscaps))
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_IDLE_SRC_PAD (src), thiscaps);
  GST_DEBUG_OBJECT (src, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps && !gst_caps_is_empty (caps)) {
    /* now fixate */
    GST_DEBUG_OBJECT (src, "have caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (src, "any caps, we stop");
      /* hmm, still anything, so element can do anything and
       * nego is not needed */
      result = TRUE;
    } else {
      caps = gst_base_idle_src_fixate (src, caps);
      GST_DEBUG_OBJECT (src, "fixated to: %" GST_PTR_FORMAT, caps);
      if (gst_caps_is_fixed (caps)) {
        /* yay, fixed caps, use those then, it's possible that the subclass does
         * not accept this caps after all and we have to fail. */
        result = gst_base_idle_src_set_caps (src, caps);
      }
    }
    gst_caps_unref (caps);
  } else {
    if (caps)
      gst_caps_unref (caps);
    GST_DEBUG_OBJECT (src, "no common caps");
  }
  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (src, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (src, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
}

static gboolean
gst_base_idle_src_negotiate_unlocked (GstBaseIdleSrc * src)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result;

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);

  GST_DEBUG_OBJECT (src, "starting negotiation");

  if (G_LIKELY (bclass->negotiate))
    result = bclass->negotiate (src);
  else
    result = TRUE;

  if (G_LIKELY (result)) {
    GstCaps *caps;

    caps = gst_pad_get_current_caps (src->srcpad);

    result = gst_base_idle_src_prepare_allocation (src, caps);

    if (caps)
      gst_caps_unref (caps);
  }
  return result;
}

/**
 * gst_base_idle_src_negotiate:
 * @src: base source instance
 *
 * Negotiates src pad caps with downstream elements.
 * Unmarks GST_PAD_FLAG_NEED_RECONFIGURE in any case. But marks it again
 * if #GstBaseIdleSrcClass::negotiate fails.
 *
 * Do not call this in the #GstBaseIdleSrcClass::fill vmethod. Call this in
 * #GstBaseIdleSrcClass::create or in #GstBaseIdleSrcClass::alloc, _before_ any
 * buffer is allocated.
 *
 * Returns: %TRUE if the negotiation succeeded, else %FALSE.
 *
 * Since: 1.18
 */
gboolean
gst_base_idle_src_negotiate (GstBaseIdleSrc * src)
{
  gboolean ret = TRUE;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), FALSE);

  GST_PAD_STREAM_LOCK (src->srcpad);
  gst_pad_check_reconfigure (src->srcpad);
  ret = gst_base_idle_src_negotiate_unlocked (src);
  if (!ret)
    gst_pad_mark_reconfigure (src->srcpad);
  GST_PAD_STREAM_UNLOCK (src->srcpad);

  return ret;
}

static gboolean
gst_base_idle_src_start (GstBaseIdleSrc * src)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result;

  GST_OBJECT_LOCK (src);

  gst_segment_init (&src->segment, src->segment.format);
  GST_OBJECT_UNLOCK (src);

  src->running = FALSE;
  src->priv->segment_pending = FALSE;
  src->priv->segment_seqnum = gst_util_seqnum_next ();

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);
  if (bclass->start)
    result = bclass->start (src);
  else
    result = TRUE;

  if (!result)
    goto could_not_start;

  return result;

could_not_start:
  {
    GST_DEBUG_OBJECT (src, "could not start");
    /* subclass is supposed to post a message but we post one as a fallback
     * just in case. We don't have to call _stop. */
    GST_ELEMENT_ERROR (src, CORE, STATE_CHANGE, (NULL), ("Failed to start"));
    return FALSE;
  }
}

static gboolean
gst_base_idle_src_stop (GstBaseIdleSrc * src)
{
  GstBaseIdleSrcClass *bclass;
  gboolean result = TRUE;

  GST_DEBUG_OBJECT (src, "stopping source");

  bclass = GST_BASE_IDLE_SRC_GET_CLASS (src);
  if (bclass->stop)
    result = bclass->stop (src);

  gst_base_idle_src_set_allocation (src, NULL, NULL, NULL);

  return result;
}

static gboolean
gst_base_idle_src_activate_push (GstPad * pad, GstObject * parent,
    gboolean active)
{
  GstBaseIdleSrc *basesrc;

  basesrc = GST_BASE_IDLE_SRC (parent);

  /* prepare subclass first */
  if (active) {
    GST_DEBUG_OBJECT (basesrc, "Activating in push mode");

    if (G_UNLIKELY (!gst_base_idle_src_start (basesrc)))
      goto error_start;
  } else {
    GST_DEBUG_OBJECT (basesrc, "Deactivating in push mode");
    /* now we can stop the source */
    if (G_UNLIKELY (!gst_base_idle_src_stop (basesrc)))
      goto error_stop;
  }
  return TRUE;

error_start:
  {
    GST_WARNING_OBJECT (basesrc, "Failed to start in push mode");
    return FALSE;
  }
error_stop:
  {
    GST_DEBUG_OBJECT (basesrc, "Failed to stop in push mode");
    return FALSE;
  }
}

static gboolean
gst_base_idle_src_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean res;

  GST_DEBUG_OBJECT (pad, "activating in mode %d", mode);

  switch (mode) {
    case GST_PAD_MODE_PULL:
      g_assert_not_reached ();
      break;
    case GST_PAD_MODE_PUSH:
      res = gst_base_idle_src_activate_push (pad, parent, active);
      break;
    default:
      GST_LOG_OBJECT (pad, "unknown activation mode %d", mode);
      res = FALSE;
      break;
  }
  return res;
}

/**
 * gst_base_idle_src_get_buffer_pool:
 * @src: a #GstBaseIdleSrc
 *
 * Returns: (nullable) (transfer full): the instance of the #GstBufferPool used
 * by the src; unref it after usage.
 */
GstBufferPool *
gst_base_idle_src_get_buffer_pool (GstBaseIdleSrc * src)
{
  GstBufferPool *ret = NULL;

  g_return_val_if_fail (GST_IS_BASE_IDLE_SRC (src), NULL);

  GST_OBJECT_LOCK (src);
  if (src->priv->pool)
    ret = gst_object_ref (src->priv->pool);
  GST_OBJECT_UNLOCK (src);

  return ret;
}

/**
 * gst_base_idle_src_get_allocator:
 * @src: a #GstBaseIdleSrc
 * @allocator: (out) (optional) (nullable) (transfer full): the #GstAllocator
 * used
 * @params: (out caller-allocates) (optional): the #GstAllocationParams of @allocator
 *
 * Lets #GstBaseIdleSrc sub-classes to know the memory @allocator
 * used by the base class and its @params.
 *
 * Unref the @allocator after usage.
 */
void
gst_base_idle_src_get_allocator (GstBaseIdleSrc * src,
    GstAllocator ** allocator, GstAllocationParams * params)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));

  GST_OBJECT_LOCK (src);
  if (allocator)
    *allocator = src->priv->allocator ?
        gst_object_ref (src->priv->allocator) : NULL;

  if (params)
    *params = src->priv->params;
  GST_OBJECT_UNLOCK (src);
}


static void
gst_base_idle_src_process_object (GstBaseIdleSrc * src, GstMiniObject * obj)
{
  GstPad *pad = src->srcpad;

  GST_PAD_STREAM_LOCK (pad);

  if (GST_IS_BUFFER (obj)) {
    GstBuffer *buf = GST_BUFFER_CAST (obj);
    GstFlowReturn ret;
    ret = gst_pad_push (pad, buf);
    if (ret != GST_FLOW_OK)
      GST_ERROR ("HUUUUAA");
  } else if (GST_IS_EVENT (obj)) {
    GstEvent *event = GST_EVENT_CAST (obj);
    gboolean ret;
    ret = gst_pad_push_event (pad, event);
    if (!ret)
      GST_ERROR ("HUUUUAA");
  }

  GST_PAD_STREAM_UNLOCK (pad);
}

static void
gst_base_idle_src_func (G_GNUC_UNUSED gpointer data, gpointer user_data)
{
  GstBaseIdleSrc *src = GST_BASE_IDLE_SRC (user_data);
  GstMiniObject *obj;

  GST_PAD_STREAM_LOCK (src->srcpad);
  if (gst_pad_check_reconfigure (src->srcpad)) {
    if (!gst_base_idle_src_negotiate_unlocked (src)) {
      GST_ERROR ("now what!?");
      g_assert_not_reached ();
      //gst_pad_mark_reconfigure (pad);
    }
  }
  GST_PAD_STREAM_UNLOCK (src->srcpad);

  GST_OBJECT_LOCK (src);
  while ((obj = g_queue_pop_head (src->priv->obj_queue))) {

    GST_OBJECT_UNLOCK (src);
    gst_base_idle_src_process_object (src, obj);
    GST_OBJECT_LOCK (src);
  }
  GST_OBJECT_UNLOCK (src);
}


static GThreadPool *
gst_base_idle_src_get_thread_pool (GstBaseIdleSrc * src)
{
  if (src->priv->thread_pool)
    return src->priv->thread_pool;

  src->priv->thread_pool = g_thread_pool_new (gst_base_idle_src_func,
      src, 0, FALSE, NULL);
  return src->priv->thread_pool;
}

static void
gst_base_idle_src_start_task (GstBaseIdleSrc * src)
{
  GThreadPool *pool = gst_base_idle_src_get_thread_pool (src);

  GError *err = NULL;
  if (!g_thread_pool_push (pool, NULL, &err)) {
    GST_ERROR_OBJECT (src, "Could not push to thread pool. Error: %s",
        err->message);
    g_assert_not_reached ();
  }
}

static void
gst_base_idle_src_check_pending_segment (GstBaseIdleSrc * src)
{
  /* push events to close/start our segment before we push the buffer. */
  if (G_UNLIKELY (src->priv->segment_pending)) {
    GstEvent *seg_event = gst_event_new_segment (&src->segment);

    gst_event_set_seqnum (seg_event, src->priv->segment_seqnum);
    src->priv->segment_seqnum = gst_util_seqnum_next ();
    gst_base_idle_src_queue_object (src, (GstMiniObject *) seg_event);
    src->priv->segment_pending = FALSE;
  }
}

/**
 * gst_base_idle_src_submit_buffer:
 * @src: a #GstBaseIdleSrc
 * @buffer: (transfer full): a #GstBuffer
 *
 * Subclasses can call this to submit a buffer to be pushed out later.
 *
 * Since: 1.22
 */
void
gst_base_idle_src_submit_buffer (GstBaseIdleSrc * src, GstBuffer * buffer)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));
  g_return_if_fail (GST_IS_BUFFER (buffer));

  g_assert (src->priv->caps_set);
  gst_base_idle_src_check_pending_segment (src);

  /* we need it to be writable later in get_range() where we use get_writable */
  gst_base_idle_src_queue_object (src, (GstMiniObject *) buffer);

  gst_base_idle_src_start_task (src);
}

/**
 * gst_base_idle_src_submit_buffer_list:
 * @src: a #GstBaseIdleSrc
 * @buffer_list: (transfer full): a #GstBufferList
 *
 * Subclasses can call this to submit a buffer list to be pushed out later.
 *
 * Since: 1.22
 */
void
gst_base_idle_src_submit_buffer_list (GstBaseIdleSrc * src,
    GstBufferList * buffer_list)
{
  g_return_if_fail (GST_IS_BASE_IDLE_SRC (src));
  g_return_if_fail (GST_IS_BUFFER_LIST (buffer_list));

  g_assert (src->priv->caps_set);
  gst_base_idle_src_check_pending_segment (src);

  /* we need it to be writable later in get_range() where we use get_writable */
  gst_base_idle_src_queue_object (src, (GstMiniObject *) buffer_list);

  GST_LOG_OBJECT (src, "%u buffers submitted in buffer list",
      gst_buffer_list_length (buffer_list));

  gst_base_idle_src_start_task (src);
}

static void
gst_base_idle_src_add_stream_start (GstBaseIdleSrc * src)
{
  gchar *stream_id;
  GstEvent *event;

  stream_id =
      gst_pad_create_stream_id (src->srcpad, GST_ELEMENT_CAST (src), NULL);

  GST_DEBUG_OBJECT (src, "Pushing STREAM_START");
  event = gst_event_new_stream_start (stream_id);
  gst_event_set_group_id (event, gst_util_group_id_next ());

  gst_base_idle_src_queue_object (src, (GstMiniObject *) event);
  g_free (stream_id);
}

static void
gst_base_idle_src_finalize (GObject * object)
{
  GstBaseIdleSrc *src;
  src = GST_BASE_IDLE_SRC (object);

  g_queue_free (src->priv->obj_queue);
  /* FIXME: empty this queue potentially... */

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_base_idle_src_class_init (GstBaseIdleSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);

  if (private_offset != 0)
    g_type_class_adjust_private_offset (klass, &private_offset);

  GST_DEBUG_CATEGORY_INIT (gst_base_idle_src_debug, "src", 0, "src element");

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_base_idle_src_finalize;
  gobject_class->set_property = gst_base_idle_src_set_property;
  gobject_class->get_property = gst_base_idle_src_get_property;

  g_object_class_install_property (gobject_class, PROP_DO_TIMESTAMP,
      g_param_spec_boolean ("do-timestamp", "Do timestamp",
          "Apply current stream time to buffers", DEFAULT_DO_TIMESTAMP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->send_event =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_send_event);

  klass->get_caps = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_get_caps);
  klass->negotiate = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_negotiate);
  klass->fixate = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_fixate);
  klass->query = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_query);
  klass->event = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_event);
  klass->alloc = GST_DEBUG_FUNCPTR (gst_base_idle_src_default_alloc);
  klass->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_base_idle_src_decide_allocation_default);

  /* Registering debug symbols for function pointers */

  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_event);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_query);
  GST_DEBUG_REGISTER_FUNCPTR (gst_base_idle_src_fixate);
}

static void
gst_base_idle_src_init (GstBaseIdleSrc * src, gpointer g_class)
{
  GstPad *pad;
  GstPadTemplate *pad_template;

  src->priv = gst_base_idle_src_get_instance_private (src);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (g_class), "src");
  g_return_if_fail (pad_template != NULL);

  GST_DEBUG_OBJECT (src, "creating src pad");
  pad = gst_pad_new_from_template (pad_template, "src");

  GST_DEBUG_OBJECT (src, "setting functions on src pad");
  gst_pad_set_activatemode_function (pad, gst_base_idle_src_activate_mode);
  gst_pad_set_event_function (pad, gst_base_idle_src_event);
  gst_pad_set_query_function (pad, gst_base_idle_src_query);

  /* hold pointer to pad */
  src->srcpad = pad;
  GST_DEBUG_OBJECT (src, "adding src pad");
  gst_element_add_pad (GST_ELEMENT (src), pad);

  /* we operate in BYTES by default */
  gst_base_idle_src_set_format (src, GST_FORMAT_BYTES);
  src->priv->do_timestamp = DEFAULT_DO_TIMESTAMP;

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_SOURCE);

  src->priv->obj_queue = g_queue_new ();

  gst_base_idle_src_add_stream_start (src);

  GST_DEBUG_OBJECT (src, "init done");
}

GType
gst_base_idle_src_get_type (void)
{
  static gsize base_idle_src_type = 0;

  if (g_once_init_enter (&base_idle_src_type)) {
    GType _type;
    static const GTypeInfo base_idle_src_info = {
      sizeof (GstBaseIdleSrcClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_base_idle_src_class_init,
      NULL,
      NULL,
      sizeof (GstBaseIdleSrc),
      0,
      (GInstanceInitFunc) gst_base_idle_src_init,
    };

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstBaseIdleSrc", &base_idle_src_info, G_TYPE_FLAG_ABSTRACT);

    private_offset =
        g_type_add_instance_private (_type, sizeof (GstBaseIdleSrcPrivate));

    g_once_init_leave (&base_idle_src_type, _type);
  }
  return base_idle_src_type;
}
