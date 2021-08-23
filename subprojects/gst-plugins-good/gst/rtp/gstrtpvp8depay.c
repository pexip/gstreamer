/* gstrtpvp8depay.c - Source for GstRtpVP8Depay
 * Copyright (C) 2011 Sjoerd Simons <sjoerd@luon.net>
 * Copyright (C) 2011 Collabora Ltd.
 *   Contact: Youness Alaoui <youness.alaoui@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "gstrtpvp8depay.h"
#include "gstrtputils.h"

#include <gst/video/video.h>
#include <gst/video/gstvideovp8meta.h>

#include <stdio.h>

GST_DEBUG_CATEGORY_STATIC (gst_rtp_vp8_depay_debug);
#define GST_CAT_DEFAULT gst_rtp_vp8_depay_debug

static void gst_rtp_vp8_depay_dispose (GObject * object);
static GstBuffer *gst_rtp_vp8_depay_process (GstRTPBaseDepayload * depayload,
    GstRTPBuffer * rtp);
static GstStateChangeReturn gst_rtp_vp8_depay_change_state (GstElement *
    element, GstStateChange transition);
static gboolean gst_rtp_vp8_depay_handle_event (GstRTPBaseDepayload * depay,
    GstEvent * event);
static gboolean gst_rtp_vp8_depay_packet_lost (GstRTPBaseDepayload * depay,
    GstEvent * event);

G_DEFINE_TYPE (GstRtpVP8Depay, gst_rtp_vp8_depay, GST_TYPE_RTP_BASE_DEPAYLOAD);
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (rtpvp8depay, "rtpvp8depay",
    GST_RANK_MARGINAL, GST_TYPE_RTP_VP8_DEPAY, rtp_element_init (plugin));

static GstStaticPadTemplate gst_rtp_vp8_depay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-vp8"));

static GstStaticPadTemplate gst_rtp_vp8_depay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp, "
        "clock-rate = (int) 90000,"
        "media = (string) \"video\","
        "encoding-name = (string) { \"VP8\", \"VP8-DRAFT-IETF-01\" }"));

enum
{
  PROP_0,
  PROP_WAIT_FOR_KEYFRAME,
  PROP_HIDE_PICTURE_ID_GAP,
};

typedef struct _GstVP8PacketInfo
{
  guint size;
  gboolean frame_start;
  gboolean end_of_frame;
  guint8 part_start;
  guint8 is_non_ref_frame;
  gboolean temporally_scaled;
  guint8 layer_sync;
  guint8 part_idx;
  guint32 picture_id;
  guint8 tl0picidx;
  guint8 tid;
  guint8 temporal_key_idx;
  guint8 hdrsize;
  GstClockTime pts;
} GstVP8PacketInfo;

#define PICTURE_ID_NONE (UINT_MAX)
#define IS_PICTURE_ID_15BITS(pid) (((guint)(pid) & 0x8000) != 0)

#define DEFAULT_WAIT_FOR_KEYFRAME FALSE
#define DEFAULT_HIDE_PICTURE_ID_GAP FALSE

// VP8 Payload Descriptor Format
// (see RFC:7741 Section-4.2)
//         0 1 2 3 4 5 6 7                      0 1 2 3 4 5 6 7
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//        |X|R|N|S|R| PID | (REQUIRED)        |X|R|N|S|R| PID | (REQUIRED)
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//   X:   |I|L|T|K| RSV   | (OPTIONAL)   X:   |I|L|T|K| RSV   | (OPTIONAL)
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//   I:   |M| PictureID   | (OPTIONAL)   I:   |M| PictureID   | (OPTIONAL)
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//   L:   |   TL0PICIDX   | (OPTIONAL)        |   PictureID   |
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//   T/K: |TID|Y| KEYIDX  | (OPTIONAL)   L:   |   TL0PICIDX   | (OPTIONAL)
//        +-+-+-+-+-+-+-+-+                   +-+-+-+-+-+-+-+-+
//                                       T/K: |TID|Y| KEYIDX  | (OPTIONAL)
//                                            +-+-+-+-+-+-+-+-+
static gboolean
gst_rtp_vp8_depay_parse_header (GstRTPBuffer * rtp, GstVP8PacketInfo * out)
{
  guint8 has_ext_ctrl_bits = FALSE;
  guint8 *data = gst_rtp_buffer_get_payload (rtp);
  guint size = gst_rtp_buffer_get_payload_len (rtp);
  GstBitReader br = GST_BIT_READER_INIT (data, size);

  /* At least one header and one vp8 byte */
  if (G_UNLIKELY (size < 2)) return FALSE;

  out->size = size;
  out->pts = GST_BUFFER_PTS (rtp->buffer);
  out->end_of_frame = gst_rtp_buffer_get_marker (rtp);
  out->picture_id = PICTURE_ID_NONE;
  out->temporally_scaled = FALSE;
  out->tl0picidx = 0;
  out->tid = 0;
  out->layer_sync = 0;
  out->temporal_key_idx = 0;
  out->hdrsize = 0;

#define fail_if(val) if (G_UNLIKELY (val)) return FALSE;
  /* Extended control bits present (X bit) */
  fail_if (!gst_bit_reader_get_bits_uint8 (&br, &has_ext_ctrl_bits, 1));
  /* Reserved bit (R bit) */
  fail_if (!gst_bit_reader_skip (&br, 1));
  /* Non-reference frame (N bit) */
  fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->is_non_ref_frame, 1));
  /* Start of VP8 partition (S bit) */
  fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->part_start, 1));
  /* Reserved bit (R bit) */
  fail_if (!gst_bit_reader_skip (&br, 1));
  /* Partition index (PID bits) */
  fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->part_idx, 3));

  out->frame_start = (out->part_start && !out->part_idx);

  /* Check X optional header */
  if (has_ext_ctrl_bits) {
    guint8 has_picture_id = 0;
    guint8 has_tl0picidx = 0;
    guint8 tid_set = 0;
    guint8 keyidx_set = 0;

    /* Check I optional header */
    fail_if (!gst_bit_reader_get_bits_uint8 (&br, &has_picture_id, 1));
    /* Check L optional header */
    fail_if (!gst_bit_reader_get_bits_uint8 (&br, &has_tl0picidx, 1));
    /* Check T is set */
    fail_if (!gst_bit_reader_get_bits_uint8 (&br, &tid_set, 1));
    /* Check K is set */
    fail_if (!gst_bit_reader_get_bits_uint8 (&br, &keyidx_set, 1));
    /* Reserved bit (R bit) */
    fail_if (!gst_bit_reader_skip (&br, 4));

    /* Stream is temporally scaled if L or T bits are set */
    out->temporally_scaled = (has_tl0picidx || tid_set);

    if (has_picture_id) {
      guint8 is_ext_pic_id = 0;
      fail_if (!gst_bit_reader_peek_bits_uint8 (&br, &is_ext_pic_id, 1));
      fail_if (!gst_bit_reader_get_bits_uint32 (&br, &out->picture_id, is_ext_pic_id ? 16 : 8));
    }

    if (has_tl0picidx) {
      /* TL0PICIDX must be ignored unless T is set */
      if (tid_set) {
        fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->tl0picidx, 8));
      } else {
        fail_if (!gst_bit_reader_skip (&br, 8));
      }
    }

    if (tid_set || keyidx_set) {
      /* TID and Y must be ignored unless T is set */
      if (tid_set) {
        fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->tid, 2));
        fail_if (!gst_bit_reader_peek_bits_uint8 (&br, &out->layer_sync, 1));
      } else {
        fail_if (!gst_bit_reader_skip (&br, 3));
      }
      /* KEYIDX must be ignored unless K is set */
      if (keyidx_set) {
        fail_if (!gst_bit_reader_get_bits_uint8 (&br, &out->temporal_key_idx, 5));
      } else {
        fail_if (!gst_bit_reader_skip (&br, 5));
      }
    }
  }
#undef fail_if
  out->hdrsize = (guint8) ((gst_bit_reader_get_pos (&br) + 1) / 8);
  return TRUE;
}

typedef struct _GstVP8PFrameInfo
{
  gboolean is_keyframe;
  guint profile;
  guint width;
  guint height;
} GstVP8PFrameInfo;

static gboolean
gst_rtp_vp8_depay_parse_frame_descriptor (GstRtpVP8Depay * self,
    GstVP8PFrameInfo * out)
{
  guint8 header[10];

  if (gst_adapter_available (self->adapter) < 10) {
    return FALSE;
  }

  gst_adapter_copy (self->adapter, &header, 0, 10);
  out->is_keyframe = !(header[0] & 0x01);
  out->profile = (header[0] & 0x0e) >> 1;
  out->width = GST_READ_UINT16_LE (header + 6) & 0x3fff;
  out->height = GST_READ_UINT16_LE (header + 8) & 0x3fff;
  return TRUE;
}

static void
gst_rtp_vp8_depay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (object);
  switch (prop_id) {
    case PROP_WAIT_FOR_KEYFRAME:
      self->wait_for_keyframe = g_value_get_boolean (value);
      break;
    case PROP_HIDE_PICTURE_ID_GAP:
      self->hide_picture_id_gap = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_vp8_depay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (object);
  switch (prop_id) {
    case PROP_WAIT_FOR_KEYFRAME:
      g_value_set_boolean (value, self->wait_for_keyframe);
      break;
    case PROP_HIDE_PICTURE_ID_GAP:
      g_value_set_boolean (value, self->hide_picture_id_gap);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_vp8_depay_init (GstRtpVP8Depay * self)
{
  self->adapter = gst_adapter_new ();
  self->started = FALSE;
  self->wait_for_keyframe = DEFAULT_WAIT_FOR_KEYFRAME;
  self->last_pushed_was_lost_event = FALSE;
}

static void
gst_rtp_vp8_depay_class_init (GstRtpVP8DepayClass * gst_rtp_vp8_depay_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gst_rtp_vp8_depay_class);
  GstElementClass *element_class = GST_ELEMENT_CLASS (gst_rtp_vp8_depay_class);
  GstRTPBaseDepayloadClass *depay_class =
      (GstRTPBaseDepayloadClass *) (gst_rtp_vp8_depay_class);

  gst_element_class_add_static_pad_template (element_class,
      &gst_rtp_vp8_depay_sink_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_rtp_vp8_depay_src_template);

  gst_element_class_set_static_metadata (element_class, "RTP VP8 depayloader",
      "Codec/Depayloader/Network/RTP",
      "Extracts VP8 video from RTP packets)",
      "Sjoerd Simons <sjoerd@luon.net>");

  object_class->dispose = gst_rtp_vp8_depay_dispose;
  object_class->set_property = gst_rtp_vp8_depay_set_property;
  object_class->get_property = gst_rtp_vp8_depay_get_property;

  element_class->change_state = gst_rtp_vp8_depay_change_state;

  depay_class->process_rtp_packet = gst_rtp_vp8_depay_process;
  depay_class->handle_event = gst_rtp_vp8_depay_handle_event;
  depay_class->packet_lost = gst_rtp_vp8_depay_packet_lost;

  g_object_class_install_property (object_class, PROP_WAIT_FOR_KEYFRAME,
      g_param_spec_boolean ("wait-for-keyframe", "Wait for Keyframe",
          "Wait for the next keyframe after packet loss",
          DEFAULT_WAIT_FOR_KEYFRAME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_HIDE_PICTURE_ID_GAP,
      g_param_spec_boolean ("hide-picture-id-gap", "Hide Picture ID Gap",
          "Wether to trigger a key-unit request when there is a gap in "
          "the picture ID", DEFAULT_HIDE_PICTURE_ID_GAP,
          G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_rtp_vp8_depay_debug, "rtpvp8depay", 0,
      "VP8 Video RTP Depayloader");
}

static void
gst_rtp_vp8_depay_dispose (GObject * object)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY (object);

  if (self->adapter != NULL)
    g_object_unref (self->adapter);
  self->adapter = NULL;

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (gst_rtp_vp8_depay_parent_class)->dispose)
    G_OBJECT_CLASS (gst_rtp_vp8_depay_parent_class)->dispose (object);
}

static gint
picture_id_compare (guint16 id0, guint16 id1)
{
  guint shift = 16 - (IS_PICTURE_ID_15BITS (id1) ? 15 : 7);
  id0 = (guint16) (id0 << shift);
  id1 = (guint16) (id1 << shift);
  return ((gint16) (id1 - id0)) >> shift;
}

static void
send_last_lost_event (GstRtpVP8Depay * self, const gchar * reason)
{
  if (self->last_lost_event) {
    GST_DEBUG_OBJECT (self,
        "Sending the last stopped lost event: %" GST_PTR_FORMAT
        " reason \"%s\"", self->last_lost_event, reason ? reason : "None");
    GST_RTP_BASE_DEPAYLOAD_CLASS (gst_rtp_vp8_depay_parent_class)
        ->packet_lost (GST_RTP_BASE_DEPAYLOAD_CAST (self),
        self->last_lost_event);
    gst_event_unref (self->last_lost_event);
    self->last_lost_event = NULL;
    self->last_pushed_was_lost_event = TRUE;
  }
}

static void
send_new_lost_event (GstRtpVP8Depay * self, GstClockTime timestamp,
    guint new_picture_id, const gchar * reason)
{
  GstEvent *event;

  if (!GST_CLOCK_TIME_IS_VALID (timestamp)) {
    GST_WARNING_OBJECT (self, "Can't create lost event with invalid timestmap");
    return;
  }

  event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
      gst_structure_new ("GstRTPPacketLost",
          "timestamp", G_TYPE_UINT64, timestamp,
          "duration", G_TYPE_UINT64, 0,
          "no-packet-loss", G_TYPE_BOOLEAN, self->hide_picture_id_gap, NULL));

  GST_DEBUG_OBJECT (self, "Pushing lost event "
      "(picids 0x%x 0x%x, reason \"%s\"): %" GST_PTR_FORMAT,
      self->last_picture_id, new_picture_id, reason, event);

  GST_RTP_BASE_DEPAYLOAD_CLASS (gst_rtp_vp8_depay_parent_class)
      ->packet_lost (GST_RTP_BASE_DEPAYLOAD_CAST (self), event);

  gst_event_unref (event);
  self->last_pushed_was_lost_event = TRUE;
}

static void
send_lost_event (GstRtpVP8Depay * self, GstClockTime timestamp,
    guint picture_id, const gchar * reason)
{
  if (self->last_lost_event) {
    send_last_lost_event (self, reason);
  } else {
    /* FIXME: Add property to control whether to send GAP events */
    send_new_lost_event (self, timestamp, picture_id, reason);
  }
}


static void
send_keyframe_request (GstRtpVP8Depay * self)
{
  GST_DEBUG_OBJECT (self, "Sending keyframe request");
  if (!gst_pad_push_event (GST_RTP_BASE_DEPAYLOAD_SINKPAD (self),
          gst_video_event_new_upstream_force_key_unit (GST_CLOCK_TIME_NONE,
              TRUE, 0))) {
    GST_ERROR_OBJECT (self, "Failed to push keyframe request");
  }
}


static void
drop_last_lost_event (GstRtpVP8Depay * self)
{
  if (self->last_lost_event) {
    gst_event_unref (self->last_lost_event);
    self->last_lost_event = NULL;
  }
}

static void
gst_rtp_vp8_depay_hadle_picture_id_gap (GstRtpVP8Depay * self,
    guint new_picture_id, GstClockTime lost_event_timestamp)
{
  const gchar *reason = NULL;
  gboolean fwd_last_lost_event = FALSE;
  gboolean create_lost_event = FALSE;
  gboolean gap_event_sent = FALSE;

  if (self->last_picture_id == PICTURE_ID_NONE)
    return;

  if (new_picture_id == PICTURE_ID_NONE) {
    reason = "picture id does not exist";
    fwd_last_lost_event = TRUE;
  } else if (IS_PICTURE_ID_15BITS (self->last_picture_id) &&
      !IS_PICTURE_ID_15BITS (new_picture_id)) {
    reason = "picture id has less bits than before";
    fwd_last_lost_event = TRUE;
  } else if (picture_id_compare ((guint16) self->last_picture_id,
          (guint16) new_picture_id) != 1) {
    reason = "picture id gap";
    fwd_last_lost_event = TRUE;
    /* Only create a new one if we just didn't push a lost event */
    create_lost_event = self->last_pushed_was_lost_event == FALSE;
  }

  if (self->last_lost_event) {
    if (fwd_last_lost_event) {
      send_last_lost_event (self, reason);
      gap_event_sent = TRUE;
    } else {
      drop_last_lost_event (self);
    }
  }

  if (create_lost_event && !gap_event_sent) {
    send_new_lost_event (self, lost_event_timestamp, new_picture_id, reason);
    gap_event_sent = TRUE;
  }

  if (gap_event_sent && self->waiting_for_keyframe) {
    send_keyframe_request (self);
  }
}


static gboolean
gst_rtp_vp8_depay_reset_current_frame (GstRtpVP8Depay * self,
    GstVP8PacketInfo * packet_info, const gchar * reason)
{
  self->started = FALSE;

  if (!gst_adapter_available (self->adapter)) return FALSE;

  GST_DEBUG_OBJECT (self, "%s, flushing adapter", reason);
  gst_adapter_clear (self->adapter);

  // Preventing for flooding with gap_events
  if (!self->last_pushed_was_lost_event) {
    send_lost_event (self, packet_info->pts, packet_info->picture_id, reason);
  }

  if (self->wait_for_keyframe) {
    self->waiting_for_keyframe = TRUE;
  }
  if (self->waiting_for_keyframe) {
    send_keyframe_request (self);
  }
  return TRUE;
}

static GstBuffer *
gst_rtp_vp8_depay_get_frame (GstRtpVP8Depay * self,
    const GstVP8PacketInfo * packet_info, const GstVP8PFrameInfo * frame_info)
{
  /* mark keyframes */
  GstBuffer *out = gst_adapter_take_buffer (self->adapter,
      gst_adapter_available (self->adapter));

  out = gst_buffer_make_writable (out);

  /* Filter away all metas that are not sensible to copy */
  gst_rtp_drop_non_video_meta (self, out);
  gst_buffer_add_video_vp8_meta_full (out, packet_info->temporally_scaled, packet_info->layer_sync,     /* Unpack Y bit */
      packet_info->tid,         /* Unpack TID */
      packet_info->tl0picidx);
  if (frame_info->is_keyframe) {
    GST_BUFFER_FLAG_UNSET (out, GST_BUFFER_FLAG_DELTA_UNIT);
    GST_DEBUG_OBJECT (self, "Processed keyframe");

    if (G_UNLIKELY (self->last_width != frame_info->width ||
            self->last_height != frame_info->height ||
            self->last_profile != frame_info->profile)) {
      gchar profile_str[3];
      GstCaps *srccaps;

      snprintf (profile_str, 3, "%u", frame_info->profile);
      srccaps = gst_caps_new_simple ("video/x-vp8",
          "framerate", GST_TYPE_FRACTION, 0, 1,
          "height", G_TYPE_INT, frame_info->height,
          "width", G_TYPE_INT, frame_info->width,
          "profile", G_TYPE_STRING, profile_str, NULL);

      gst_pad_set_caps (GST_RTP_BASE_DEPAYLOAD_SRCPAD (self), srccaps);
      gst_caps_unref (srccaps);

      self->last_width = frame_info->width;
      self->last_height = frame_info->height;
      self->last_profile = frame_info->profile;
    }
    self->waiting_for_keyframe = FALSE;
  } else {
    GST_BUFFER_FLAG_SET (out, GST_BUFFER_FLAG_DELTA_UNIT);
    GST_DEBUG_OBJECT (self, "Processed interframe");

    if (self->waiting_for_keyframe) {
      gst_buffer_unref (out);
      out = NULL;
      GST_INFO_OBJECT (self, "Dropping inter-frame before intra-frame");
      send_keyframe_request (self);
    }
  }

  return out;
}

static GstBuffer *
gst_rtp_vp8_depay_process (GstRTPBaseDepayload * depay, GstRTPBuffer * rtp)
{
  GstBuffer *out = NULL;
  GstVP8PacketInfo packet_info;
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (depay);

  if (G_UNLIKELY (!gst_rtp_vp8_depay_parse_header (rtp, &packet_info))) {
    gst_rtp_vp8_depay_reset_current_frame (self, &packet_info,
        "Invalid rtp packet detected");
    return NULL;
  }

  GST_LOG_OBJECT (depay,
      "hdrsize %u, size %u, picture id 0x%x, s %u, part_id %u",
      packet_info.hdrsize, packet_info.size, packet_info.picture_id,
      packet_info.part_start, packet_info.part_idx);

  if (G_UNLIKELY (GST_BUFFER_IS_DISCONT (rtp->buffer))) {
    gst_rtp_vp8_depay_reset_current_frame (self, &packet_info,
        "Discontinuity detected");
  }

  if (G_UNLIKELY (packet_info.frame_start == self->started)) {
    // We either didn't completed previous frame
    // or didn't start next frame
    gst_rtp_vp8_depay_reset_current_frame (self, &packet_info,
        "Incomplete frame detected");
  }

  if (packet_info.frame_start) {
    GST_LOG_OBJECT (depay, "Found the start of the frame");

    gst_rtp_vp8_depay_hadle_picture_id_gap (self, packet_info.picture_id,
        packet_info.pts);

    self->started = TRUE;
    self->stop_lost_events = FALSE;
    self->last_picture_id = packet_info.picture_id;

  } else if (self->started) {
    // PictureID gap in a middle of the frame
    if (self->last_picture_id != packet_info.picture_id)
      gst_rtp_vp8_depay_reset_current_frame (self, &packet_info,
          "picture id gap");
  }

  if (self->started) {
    /* Store rtp payload data in adapter */
    gst_adapter_push (self->adapter, gst_rtp_buffer_get_payload_subbuffer (rtp,
            packet_info.hdrsize, -1));

    /* Marker indicates that it was the last rtp packet for this frame */
    if (packet_info.end_of_frame) {
      GstVP8PFrameInfo frame_info;

      GST_LOG_OBJECT (depay,
          "Found the end of the frame (%" G_GSIZE_FORMAT " bytes)",
          gst_adapter_available (self->adapter));

      if (gst_rtp_vp8_depay_parse_frame_descriptor (self, &frame_info)) {
        out = gst_rtp_vp8_depay_get_frame (self, &packet_info, &frame_info);

        if (packet_info.picture_id != PICTURE_ID_NONE)
          self->stop_lost_events = TRUE;

        self->last_pushed_was_lost_event = FALSE;
        self->started = FALSE;
      } else {
        gst_rtp_vp8_depay_reset_current_frame (self, &packet_info,
            "Invalid rtp packet detected");
      }
    }
  } else {
    // Wating for start of the new frame
    GST_DEBUG_OBJECT (depay,
        "The frame is missing the first packet, ignoring the packet");
  }

  return out;
}

static GstStateChangeReturn
gst_rtp_vp8_depay_change_state (GstElement * element, GstStateChange transition)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->last_profile = -1;
      self->last_height = -1;
      self->last_width = -1;
      self->waiting_for_keyframe = TRUE;
      self->caps_sent = FALSE;
      self->last_picture_id = PICTURE_ID_NONE;
      if (self->last_lost_event) {
        gst_event_unref (self->last_lost_event);
        self->last_lost_event = NULL;
      }
      self->stop_lost_events = FALSE;
      break;
    default:
      break;
  }

  return
      GST_ELEMENT_CLASS (gst_rtp_vp8_depay_parent_class)->change_state (element,
      transition);
}

static gboolean
gst_rtp_vp8_depay_handle_event (GstRTPBaseDepayload * depay, GstEvent * event)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (depay);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      self->last_profile = -1;
      self->last_height = -1;
      self->last_width = -1;
      self->last_picture_id = PICTURE_ID_NONE;
      if (self->last_lost_event) {
        gst_event_unref (self->last_lost_event);
        self->last_lost_event = NULL;
      }
      self->stop_lost_events = FALSE;
      break;
    default:
      break;
  }

  return
      GST_RTP_BASE_DEPAYLOAD_CLASS
      (gst_rtp_vp8_depay_parent_class)->handle_event (depay, event);
}

static gboolean
gst_rtp_vp8_depay_packet_lost (GstRTPBaseDepayload * depay, GstEvent * event)
{
  GstRtpVP8Depay *self = GST_RTP_VP8_DEPAY_CAST (depay);
  if (self->stop_lost_events) {
    GST_DEBUG_OBJECT (depay, "Stopping lost event %" GST_PTR_FORMAT, event);
    if (self->last_lost_event)
      gst_event_unref (self->last_lost_event);
    self->last_lost_event = gst_event_ref (event);
    return TRUE;
  }

  self->last_pushed_was_lost_event = TRUE;

  return
      GST_RTP_BASE_DEPAYLOAD_CLASS
      (gst_rtp_vp8_depay_parent_class)->packet_lost (depay, event);
}
