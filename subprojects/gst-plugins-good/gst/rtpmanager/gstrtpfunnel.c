/* RTP funnel element for GStreamer
 *
 * gstrtpfunnel.c:
 *
 * Copyright (C) <2017> Pexip.
 *   Contact: Havard Graff <havard@pexip.com>
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
 * SECTION:element-rtpfunnel
 * @title: rtpfunnel
 * @see_also: rtpbasepaylaoder, rtpsession
 *
 * RTP funnel is basically like a normal funnel with a few added
 * functionalities to support bundling.
 *
 * Bundle is the concept of sending multiple streams in a single RTP session.
 * These can be both audio and video streams, and several of both.
 * One of the advantages with bundling is that you can get away with fewer
 * ports for sending and receiving media. Also the RTCP traffic gets more
 * compact if you can report on multiple streams in a single sender/receiver
 * report.
 *
 * One of the reasons for a specialized RTP funnel is that some messages
 * coming upstream want to find their way back to the right stream,
 * and a normal funnel can't know which of its sinkpads it should send
 * these messages to. The RTP funnel achieves this by keeping track of the
 * SSRC of each stream on its sinkpad, and then uses the fact that upstream
 * events are tagged inside rtpbin with the appropriate SSRC, so that upon
 * receiving such an event, the RTP funnel can do a simple lookup for the
 * right pad to forward the event to.
 *
 * A good example here is the KeyUnit event. If several video encoders are
 * being bundled together using the RTP funnel, and one of the decoders on
 * the receiving side asks for a KeyUnit, typically a RTCP PLI message will
 * be sent from the receiver to the sender, and this will be transformed into
 * a GstForceKeyUnit event inside GstRTPSession, and sent upstream. The
 * RTP funnel can than make sure that this event hits the right encoder based
 * on the SSRC embedded in the event.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtphdrext.h>

#include "gstrtpfunnel.h"
#include "gstrtputils.h"

GST_DEBUG_CATEGORY_STATIC (gst_rtp_funnel_debug);
#define GST_CAT_DEFAULT gst_rtp_funnel_debug

/**************** GstRTPFunnelPad ****************/

struct _GstRtpFunnelPadClass
{
  GstPadClass class;
};

struct _GstRtpFunnelPad
{
  GstPad pad;
  guint32 ssrc;
  GstRTPBufferFlags buffer_flag;
  GstClockTime us_latency;
  gboolean has_latency;
};

G_DEFINE_TYPE (GstRtpFunnelPad, gst_rtp_funnel_pad, GST_TYPE_PAD);
GST_ELEMENT_REGISTER_DEFINE (rtpfunnel, "rtpfunnel", GST_RANK_NONE,
    GST_TYPE_RTP_FUNNEL);

static void
gst_rtp_funnel_pad_class_init (G_GNUC_UNUSED GstRtpFunnelPadClass * klass)
{
}

static void
gst_rtp_funnel_pad_init (G_GNUC_UNUSED GstRtpFunnelPad * pad)
{
}

static void
gst_rtp_funnel_pad_set_buffer_flag (GstRtpFunnelPad * pad, GstBuffer * buf)
{
  if (pad->buffer_flag)
    GST_BUFFER_FLAG_SET (buf, pad->buffer_flag);
}

static void
gst_rtp_funnel_pad_set_media_type (GstRtpFunnelPad * pad,
    const GstStructure * s)
{
  const gchar *media_type = gst_structure_get_string (s, "media");
  if (g_strcmp0 (media_type, "audio") == 0)
    pad->buffer_flag = GST_RTP_BUFFER_FLAG_MEDIA_AUDIO;
  else if (g_strcmp0 (media_type, "video") == 0)
    pad->buffer_flag = GST_RTP_BUFFER_FLAG_MEDIA_VIDEO;
}

static gboolean
gst_rtp_funnel_pad_query_latency (GstRtpFunnelPad * pad,
    gboolean * _live, GstClockTime * _max)
{
  GstQuery *query = gst_query_new_latency ();
  gboolean res;

  /* Ask peer for latency */
  res = gst_pad_peer_query (GST_PAD_CAST (pad), query);

  if (res) {
    gboolean live;
    GstClockTime min, max;
    gst_query_parse_latency (query, &live, &min, &max);

    pad->us_latency = min;
    GST_INFO_OBJECT (pad, "us_latency: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (pad->us_latency));

    if (_live)
      *_live = live;
    if (_max)
      *_max = max;
  }

  pad->has_latency = TRUE;

  gst_query_unref (query);
  return res;
}

/**************** GstRTPFunnel ****************/

enum
{
  PROP_0,
  PROP_COMMON_TS_OFFSET,
};

#define DEFAULT_COMMON_TS_OFFSET -1

struct _GstRtpFunnelClass
{
  GstElementClass class;
};

struct _GstRtpFunnel
{
  GstElement element;

  GstPad *srcpad;
  GstCaps *srccaps;             /* protected by OBJECT_LOCK */
  gboolean send_sticky_events;
  GHashTable *ssrc_to_pad;      /* protected by OBJECT_LOCK */
  /* The last pad data was chained on */
  GstPad *current_pad;

  /* properties */
  gint common_ts_offset;
};

#define RTP_CAPS "application/x-rtp"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (RTP_CAPS));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RTP_CAPS));

#define gst_rtp_funnel_parent_class parent_class
G_DEFINE_TYPE (GstRtpFunnel, gst_rtp_funnel, GST_TYPE_ELEMENT);


static void
gst_rtp_funnel_send_sticky (GstRtpFunnel * funnel, GstPad * pad)
{
  GstEvent *stream_start;
  GstCaps *caps;
  GstEvent *caps_ev;

  if (!funnel->send_sticky_events)
    goto done;

  stream_start = gst_pad_get_sticky_event (pad, GST_EVENT_STREAM_START, 0);
  if (stream_start && !gst_pad_push_event (funnel->srcpad, stream_start)) {
    GST_ERROR_OBJECT (funnel, "Could not push stream start");
    goto done;
  }

  /* We modify these caps in our sink pad event handlers, so make sure to
   * send a copy downstream so that we can keep our internal caps writable */
  GST_OBJECT_LOCK (funnel);
  caps = gst_caps_copy (funnel->srccaps);
  GST_OBJECT_UNLOCK (funnel);

  caps_ev = gst_event_new_caps (caps);
  gst_caps_unref (caps);
  if (caps_ev && !gst_pad_push_event (funnel->srcpad, caps_ev)) {
    GST_ERROR_OBJECT (funnel, "Could not push caps");
    goto done;
  }

  funnel->send_sticky_events = FALSE;

done:
  return;
}

static void
gst_rtp_funnel_forward_segment (GstRtpFunnel * funnel, GstPad * pad)
{
  GstEvent *event;
  guint i;

  if (pad == funnel->current_pad) {
    goto done;
  }

  event = gst_pad_get_sticky_event (pad, GST_EVENT_SEGMENT, 0);
  if (event && !gst_pad_push_event (funnel->srcpad, event)) {
    GST_ERROR_OBJECT (funnel, "Could not push segment");
    goto done;
  }

  for (i = 0;; i++) {
    event = gst_pad_get_sticky_event (pad, GST_EVENT_CUSTOM_DOWNSTREAM_STICKY,
        i);
    if (event == NULL)
      break;
    if (!gst_pad_push_event (funnel->srcpad, event))
      GST_ERROR_OBJECT (funnel, "Could not push custom event");
  }

  funnel->current_pad = pad;

done:
  return;
}

static GstFlowReturn
gst_rtp_funnel_sink_chain_object (GstPad * pad, GstRtpFunnel * funnel,
    gboolean is_list, GstMiniObject * obj)
{
  GstRtpFunnelPad *fpad = GST_RTP_FUNNEL_PAD_CAST (pad);
  GstFlowReturn res;

  GST_DEBUG_OBJECT (pad, "received %" GST_PTR_FORMAT, obj);

  GST_PAD_STREAM_LOCK (funnel->srcpad);

  if (!fpad->has_latency) {
    gst_rtp_funnel_pad_query_latency (fpad, NULL, NULL);
  }

  gst_rtp_funnel_send_sticky (funnel, pad);
  gst_rtp_funnel_forward_segment (funnel, pad);

  if (is_list) {
    res = gst_pad_push_list (funnel->srcpad, GST_BUFFER_LIST_CAST (obj));
  } else {
    GstBuffer *buf = GST_BUFFER_CAST (obj);
    gst_rtp_funnel_pad_set_buffer_flag (fpad, buf);
    GST_BUFFER_PTS (buf) += fpad->us_latency;
    res = gst_pad_push (funnel->srcpad, buf);
  }
  GST_PAD_STREAM_UNLOCK (funnel->srcpad);

  return res;
}

static GstFlowReturn
gst_rtp_funnel_sink_chain_list (GstPad * pad, GstObject * parent,
    GstBufferList * list)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);

  return gst_rtp_funnel_sink_chain_object (pad, funnel, TRUE,
      GST_MINI_OBJECT_CAST (list));
}

static GstFlowReturn
gst_rtp_funnel_sink_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);

  return gst_rtp_funnel_sink_chain_object (pad, funnel, FALSE,
      GST_MINI_OBJECT_CAST (buffer));
}

static gboolean
gst_rtp_funnel_query_latency (GstRtpFunnel * funnel, GstQuery * query)
{
  GstClockTime max = GST_CLOCK_TIME_NONE;
  gboolean live = FALSE, done = FALSE;
  GValue item = G_VALUE_INIT;
  GstIterator *it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (funnel));

  /* Take maximum of all latency values */
  while (!done) {
    GstIteratorResult ires = gst_iterator_next (it, &item);
    switch (ires) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstRtpFunnelPad *pad = g_value_get_object (&item);
        gboolean live_cur;
        GstClockTime max_cur;

        if (gst_rtp_funnel_pad_query_latency (pad, &live_cur, &max_cur)) {
          /* max is the buffering-potential upstream, and we are only
             ever as good as our weakest link here, so take the smallest
             of the max values */
          max = MIN (max_cur, max);
          /* if one branch is live, we are live */
          live = live || live_cur;
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        max = GST_CLOCK_TIME_NONE;
        live = FALSE;
        gst_iterator_resync (it);
        break;
      default:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  /* since we terminate upstream latency, we report 0 downstream */
  gst_query_set_latency (query, live, 0, max);

  /* store the results */
  GST_INFO_OBJECT (funnel, "Replying to latency query with: %" GST_PTR_FORMAT,
      query);

  return TRUE;
}

static gboolean
gst_rtp_funnel_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);
  GstRtpFunnelPad *fpad = GST_RTP_FUNNEL_PAD_CAST (pad);

  gboolean forward = TRUE;
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (pad, "received event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
    case GST_EVENT_SEGMENT:
      forward = FALSE;
      break;
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      GstStructure *s;
      guint ssrc;
      GstCaps *rtpcaps = gst_caps_new_empty_simple (RTP_CAPS);

      gst_event_parse_caps (event, &caps);

      GST_OBJECT_LOCK (funnel);
      if (!gst_caps_can_intersect (rtpcaps, caps)) {
        GST_ERROR_OBJECT (funnel, "Can't intersect with caps %" GST_PTR_FORMAT,
            caps);
        g_assert_not_reached ();
      }

      gst_caps_unref (rtpcaps);

      s = gst_caps_get_structure (caps, 0);
      if (gst_structure_get_uint (s, "ssrc", &ssrc)) {
        fpad->ssrc = ssrc;
        GST_DEBUG_OBJECT (pad, "Got ssrc: %u", ssrc);
        g_hash_table_insert (funnel->ssrc_to_pad, GUINT_TO_POINTER (ssrc), pad);
      }

      gst_rtp_funnel_pad_set_media_type (fpad, s);
      GST_OBJECT_UNLOCK (funnel);

      forward = FALSE;
      break;
    }
    default:
      break;
  }

  if (forward) {
    ret = gst_pad_event_default (pad, parent, event);
  } else {
    gst_event_unref (event);
  }

  return ret;
}

static gboolean
gst_rtp_funnel_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);
  gboolean res = TRUE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter_caps;
      GstCaps *new_caps;
      GstCaps *rtpcaps = gst_caps_new_empty_simple (RTP_CAPS);

      gst_query_parse_caps (query, &filter_caps);

      GST_OBJECT_LOCK (funnel);
      if (filter_caps) {
        new_caps = gst_caps_intersect_full (rtpcaps, filter_caps,
            GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (rtpcaps);
      } else {
        new_caps = rtpcaps;
      }
      GST_OBJECT_UNLOCK (funnel);

      if (funnel->common_ts_offset >= 0)
        gst_caps_set_simple (new_caps, "timestamp-offset", G_TYPE_UINT,
            (guint) funnel->common_ts_offset, NULL);

      gst_query_set_caps_result (query, new_caps);
      GST_DEBUG_OBJECT (pad, "Answering caps-query with caps: %"
          GST_PTR_FORMAT, new_caps);
      gst_caps_unref (new_caps);
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;
      gboolean result;

      gst_query_parse_accept_caps (query, &caps);

      GST_OBJECT_LOCK (funnel);
      result = gst_caps_can_intersect (caps, funnel->srccaps);
      if (!result) {
        GST_ERROR_OBJECT (pad,
            "caps: %" GST_PTR_FORMAT " were not compatible with: %"
            GST_PTR_FORMAT, caps, funnel->srccaps);
      }
      GST_OBJECT_UNLOCK (funnel);

      gst_query_set_accept_caps_result (query, result);
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static gboolean
gst_rtp_funnel_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);
  gboolean handled = FALSE;
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (pad, "received event %" GST_PTR_FORMAT, event);

  if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_UPSTREAM) {
    const GstStructure *s = gst_event_get_structure (event);
    GstPad *fpad;
    guint ssrc;
    if (s && gst_structure_get_uint (s, "ssrc", &ssrc)) {
      handled = TRUE;

      GST_OBJECT_LOCK (funnel);
      fpad = g_hash_table_lookup (funnel->ssrc_to_pad, GUINT_TO_POINTER (ssrc));
      if (fpad)
        gst_object_ref (fpad);
      GST_OBJECT_UNLOCK (funnel);

      if (fpad) {
        GST_INFO_OBJECT (pad, "Sending %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
            event, fpad);
        ret = gst_pad_push_event (fpad, event);
        gst_object_unref (fpad);
      } else {
        gst_event_unref (event);
      }
    }
  }

  if (!handled) {
    gst_pad_event_default (pad, parent, event);
  }

  return ret;
}

static gboolean
gst_rtp_funnel_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      res = gst_rtp_funnel_query_latency (funnel, query);
      break;
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}

static GstPad *
gst_rtp_funnel_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * name, const GstCaps * caps)
{
  GstPad *sinkpad;
  (void) caps;

  GST_DEBUG_OBJECT (element, "requesting pad");

  sinkpad = GST_PAD_CAST (g_object_new (GST_TYPE_RTP_FUNNEL_PAD,
          "name", name, "direction", templ->direction, "template", templ,
          NULL));

  gst_pad_set_chain_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_sink_chain));
  gst_pad_set_chain_list_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_sink_chain_list));
  gst_pad_set_event_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_sink_event));
  gst_pad_set_query_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_sink_query));

  GST_OBJECT_FLAG_SET (sinkpad, GST_PAD_FLAG_PROXY_CAPS);
  GST_OBJECT_FLAG_SET (sinkpad, GST_PAD_FLAG_PROXY_ALLOCATION);

  gst_pad_set_active (sinkpad, TRUE);

  gst_element_add_pad (element, sinkpad);

  GST_DEBUG_OBJECT (element, "requested pad %s:%s",
      GST_DEBUG_PAD_NAME (sinkpad));

  return sinkpad;
}

static void
gst_rtp_funnel_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (object);

  switch (prop_id) {
    case PROP_COMMON_TS_OFFSET:
      funnel->common_ts_offset = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_funnel_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (object);

  switch (prop_id) {
    case PROP_COMMON_TS_OFFSET:
      g_value_set_int (value, funnel->common_ts_offset);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_rtp_funnel_change_state (GstElement * element, GstStateChange transition)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (element);
  GstStateChangeReturn ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      funnel->send_sticky_events = TRUE;
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
_remove_pad_func (gpointer key, gpointer value, gpointer user_data)
{
  (void) key;
  if (GST_PAD_CAST (value) == GST_PAD_CAST (user_data))
    return TRUE;
  return FALSE;
}

static void
gst_rtp_funnel_release_pad (GstElement * element, GstPad * pad)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (element);

  GST_DEBUG_OBJECT (funnel, "releasing pad %s:%s", GST_DEBUG_PAD_NAME (pad));

  g_hash_table_foreach_remove (funnel->ssrc_to_pad, _remove_pad_func, pad);

  gst_pad_set_active (pad, FALSE);
  gst_element_remove_pad (GST_ELEMENT_CAST (funnel), pad);
}

static void
gst_rtp_funnel_finalize (GObject * object)
{
  GstRtpFunnel *funnel = GST_RTP_FUNNEL_CAST (object);

  gst_caps_unref (funnel->srccaps);
  g_hash_table_destroy (funnel->ssrc_to_pad);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_rtp_funnel_class_init (GstRtpFunnelClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_rtp_funnel_finalize);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_rtp_funnel_get_property);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_rtp_funnel_set_property);
  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_release_pad);
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_change_state);

  gst_element_class_set_static_metadata (gstelement_class, "RTP funnel",
      "RTP Funneling",
      "Funnel RTP buffers together for multiplexing",
      "Havard Graff <havard@gstip.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);
  gst_element_class_add_static_pad_template (gstelement_class, &src_template);

  g_object_class_install_property (gobject_class, PROP_COMMON_TS_OFFSET,
      g_param_spec_int ("common-ts-offset", "Common Timestamp Offset",
          "Use the same RTP timestamp offset for all sinkpads (-1 = disable)",
          -1, G_MAXINT32, DEFAULT_COMMON_TS_OFFSET,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_rtp_funnel_debug,
      "gstrtpfunnel", 0, "funnel element");
}

static void
gst_rtp_funnel_init (GstRtpFunnel * funnel)
{
  funnel->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_use_fixed_caps (funnel->srcpad);
  gst_pad_set_event_function (funnel->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_src_event));
  gst_pad_set_query_function (funnel->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_funnel_src_query));

  gst_element_add_pad (GST_ELEMENT (funnel), funnel->srcpad);

  funnel->send_sticky_events = TRUE;
  funnel->srccaps = gst_caps_new_empty_simple (RTP_CAPS);
  funnel->ssrc_to_pad = g_hash_table_new (NULL, NULL);
  funnel->current_pad = NULL;
}
