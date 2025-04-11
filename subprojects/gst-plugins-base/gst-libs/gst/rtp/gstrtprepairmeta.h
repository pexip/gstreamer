/* GStreamer
 * Copyright (C) <2024> Mikhail Baranov <mikhail.baranov@pexip.com>
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

#ifndef __GST_RTP_REPAIR_META_H__
#define __GST_RTP_REPAIR_META_H__

#include <gst/gst.h>
#include <glib.h>
#include <gst/rtp/rtp-prelude.h>

G_BEGIN_DECLS
#define GST_RTP_REPAIR_META_API_TYPE  (gst_rtp_repair_meta_api_get_type())
#define GST_RTP_REPAIR_META_INFO  (gst_rtp_repair_meta_get_info())
typedef struct _GstRTPRepairMeta GstRTPRepairMeta;

/**
 * GstRTPRepairMeta:
 * @meta: parent GstMeta structure
 * @idx_red_packets: index of this redundant packet for the block
 * @num_red_packets: number of redundant packets
 * @ssrc: SSRC
 * @seqnums: array of sequence numbers of data packets
 *
 * Meta describing the reduandant packet, e.g. FEC or RTX.
 * It is used to tie together the original packet and the redundant packets.
 */
struct _GstRTPRepairMeta
{
  GstMeta meta;

  guint16 idx_red_packets;
  guint16 num_red_packets;
  guint32 ssrc;
  GArray *seqnums;
};

GST_RTP_API GType gst_rtp_repair_meta_api_get_type (void);

GST_RTP_API
    GstRTPRepairMeta * gst_rtp_repair_meta_add (GstBuffer * buffer,
    const guint16 idx_red_packets, const guint16 num_red_packets,
    const guint32 ssrc, const guint16 * seqnum, guint seqnum_count);

GST_RTP_API GstRTPRepairMeta *gst_rtp_repair_meta_get (GstBuffer * buffer);

GST_RTP_API
    gboolean gst_rtp_repair_meta_seqnum_chk (GstBuffer * buffer, guint16 seqnum,
    guint32 ssrc);

GST_RTP_API
    gboolean gst_rtp_repair_meta_get_seqnums (GstBuffer * buffer,
    guint32 * ssrc, GArray ** seqnums);

GST_RTP_API gint gst_rtp_repair_meta_idx (GstBuffer * buffer);

GST_RTP_API gint gst_rtp_repair_meta_repair_num (GstBuffer * buffer);

GST_RTP_API const GstMetaInfo *gst_rtp_repair_meta_get_info (void);

G_END_DECLS
#endif /* __GST_RTP_REPAIR_META_H__ */
