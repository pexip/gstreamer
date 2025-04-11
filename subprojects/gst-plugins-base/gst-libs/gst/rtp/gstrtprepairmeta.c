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

 /**
 * SECTION:gstrepairmeta
 * @title: GstRepairMeta
 * @short_description: Methods for dealing with repairment capabilities meta.
 * 
 * Some buffers may contain repairment capabilities, such as FEC or RTX packets.
 * This meta is used to tie together the redundant packets with the original
 * packet, in order to allow TWCC Manager to understand the underlying
 * connections between the packets and calculate the statistics
 * accordingly.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrtprepairmeta.h"
#include <string.h>

static gboolean gst_rtp_repair_meta_init (GstRTPRepairMeta * meta,
    G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer * buffer);
static void gst_rtp_repair_meta_free (GstRTPRepairMeta * meta,
    G_GNUC_UNUSED GstBuffer * buffer);


GType
gst_rtp_repair_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstRTPRepairMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }

  return type;
}

const GstMetaInfo *
gst_rtp_repair_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter (&meta_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_RTP_REPAIR_META_API_TYPE,
        "GstRTPRepairMeta",
        sizeof (GstRTPRepairMeta),
        (GstMetaInitFunction) gst_rtp_repair_meta_init,
        (GstMetaFreeFunction) gst_rtp_repair_meta_free,
        NULL);
    g_once_init_leave (&meta_info, mi);
  }

  return meta_info;
}

GstRTPRepairMeta *
gst_rtp_repair_meta_get (GstBuffer * buffer)
{
  return (GstRTPRepairMeta *) gst_buffer_get_meta (buffer,
      gst_rtp_repair_meta_api_get_type ());
}

/*
 * Add a new repair meta to the buffer. The seqnum array is copied into the
 * meta, so it can be freed after this call.
*/
GstRTPRepairMeta *
gst_rtp_repair_meta_add (GstBuffer * buffer,
    const guint16 idx_red_packets, const guint16 num_red_packets,
    const guint32 ssrc, const guint16 * seqnum, guint seqnum_count)
{
  GstRTPRepairMeta *repair_meta =
      (GstRTPRepairMeta *) gst_buffer_add_meta (buffer,
      GST_RTP_REPAIR_META_INFO, NULL);
  if (repair_meta == NULL) {
    return NULL;
  }

  repair_meta->idx_red_packets = idx_red_packets;
  repair_meta->num_red_packets = num_red_packets;
  repair_meta->ssrc = ssrc;
  g_array_insert_vals (repair_meta->seqnums, 0, seqnum, seqnum_count);

  return repair_meta;
}

/* Check if SSRC and seqnum is covered by the redundant packet. */
gboolean
gst_rtp_repair_meta_seqnum_chk (GstBuffer * buffer, guint16 seqnum,
    guint32 ssrc)
{
  GstRTPRepairMeta *repair_meta = gst_rtp_repair_meta_get (buffer);
  if (!repair_meta) {
    return FALSE;
  }
  if (repair_meta->ssrc != ssrc) {
    return FALSE;
  }

  for (guint i = 0; i < repair_meta->seqnums->len; i++) {
    guint16 stored_seqnum = g_array_index (repair_meta->seqnums, guint16, i);
    if (stored_seqnum == seqnum) {
      return TRUE;
    }
  }
  return FALSE;
}

/* If this packet is a FEC/RTX packet, what is it sequential number a block */
gint
gst_rtp_repair_meta_idx (GstBuffer * buffer)
{
  GstRTPRepairMeta *repair_meta = gst_rtp_repair_meta_get (buffer);
  if (repair_meta) {
    return repair_meta->idx_red_packets;
  }
  return -1;
}

/* If this packet is a FEC/RTX packet, how many redundancy packets are 
 * in the same block.
 * -1 if not a repair packet
 */
gint
gst_rtp_repair_meta_repair_num (GstBuffer * buffer)
{
  GstRTPRepairMeta *repair_meta = gst_rtp_repair_meta_get (buffer);
  if (repair_meta) {
    return repair_meta->num_red_packets;
  }
  return -1;
}

/* 
 * If this packet is a FEC/RTX packet, return the SSRC and the array of
 * sequence numbers of the data packets being protected by this packet.
 * The array is ref-counted, so it should be unrefed when not needed anymore.
 * Returns TRUE if the packet is a repair packet, FALSE otherwise.
*/
gboolean
gst_rtp_repair_meta_get_seqnums (GstBuffer * buffer, guint32 * ssrc,
    GArray ** seqnums)
{
  GstRTPRepairMeta *repair_meta = gst_rtp_repair_meta_get (buffer);
  if (repair_meta && repair_meta->seqnums->len > 0) {
    if (ssrc) {
      *ssrc = repair_meta->ssrc;
    }
    if (seqnums) {
      *seqnums = g_array_ref (repair_meta->seqnums);
    }
    return TRUE;
  } else {
    *ssrc = 0;
    *seqnums = NULL;
  }
  return FALSE;
}

static gboolean
gst_rtp_repair_meta_init (GstRTPRepairMeta * meta,
    G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer * buffer)
{
  meta->idx_red_packets = 0;
  meta->num_red_packets = 0;
  meta->ssrc = 0;
  meta->seqnums = g_array_new (FALSE, FALSE, sizeof (guint16));

  return TRUE;
}

static void
gst_rtp_repair_meta_free (GstRTPRepairMeta * meta,
    G_GNUC_UNUSED GstBuffer * buffer)
{
  g_array_unref (meta->seqnums);
}
