/* RTP Retransmission sender element for GStreamer
 *
 * gstrtprtxsend.h:
 *
 * Copyright (C) 2013 Collabora Ltd.
 *   @author Julien Isorce <julien.isorce@collabora.co.uk>
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

#ifndef __GST_RTP_RTX_SEND_H__
#define __GST_RTP_RTX_SEND_H__

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <gst/base/gstdataqueue.h>
#include "tokenbucket.h"

G_BEGIN_DECLS

typedef struct _GstRtpRtxSend GstRtpRtxSend;
typedef struct _GstRtpRtxSendClass GstRtpRtxSendClass;

#define GST_TYPE_RTP_RTX_SEND (gst_rtp_rtx_send_get_type())
#define GST_RTP_RTX_SEND(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTP_RTX_SEND, GstRtpRtxSend))
#define GST_RTP_RTX_SEND_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTP_RTX_SEND, GstRtpRtxSendClass))
#define GST_RTP_RTX_SEND_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_RTP_RTX_SEND, GstRtpRtxSendClass))
#define GST_IS_RTP_RTX_SEND(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTP_RTX_SEND))
#define GST_IS_RTP_RTX_SEND_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTP_RTX_SEND))
#define GST_RTP_RTX_SEND_CAST(obj) ((GstRtpRtxSend *)(obj))

struct _GstRtpRtxSend
{
  GstElement element;

  /* pad */
  GstPad *sinkpad;
  GstPad *srcpad;

  /* rtp packets that will be pushed out */
  GstDataQueue *queue;

  /* ssrc -> SSRCRtxData */
  GHashTable *ssrc_data;
  /* rtx ssrc -> master ssrc */
  GHashTable *rtx_ssrcs;

  /* master ssrc -> rtx ssrc (property) */
  GstStructure *external_ssrc_map;

  /* orig pt (uint) -> rtx pt (uint) */
  GHashTable *rtx_pt_map;
  /* orig pt (string) -> rtx pt (uint) */
  GstStructure *rtx_pt_map_structure;

  /* orig pt (uint) -> clock rate (uint) */
  GHashTable *clock_rate_map;
  /* orig pt (string) -> clock rate (uint) */
  GstStructure *clock_rate_map_structure;

  /* buffering control properties */
  guint max_size_time;
  guint max_size_packets;

  /* statistics */
  guint num_rtx_requests;
  guint num_rtx_packets;

  /* list of relevant RTP Header Extensions */
  GstRTPHeaderExtension *rid_stream;
  GstRTPHeaderExtension *rid_repaired;

  GstBuffer *dummy_writable;

  /* bucket */
  gint max_kbps;
  gint max_bucket_size;
  TokenBucket max_tb;

  /* stuffing properties */
  gint stuffing_kbps;

  /* last ssrc used for stuffing */
  gint last_stuffing_ssrc;

  TokenBucket stuff_tb;
};

struct _GstRtpRtxSendClass
{
  GstElementClass parent_class;
};

GType gst_rtp_rtx_send_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (rtprtxsend);

G_END_DECLS
#endif /* __GST_RTP_RTX_SEND_H__ */
