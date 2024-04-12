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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrtprepairmeta.h"
#include <string.h>

static gboolean gst_rtp_repair_meta_init(GstRTPRepairMeta * meta, 
    G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer * buffer);
static void gst_rtp_repair_meta_free(GstRTPRepairMeta *meta,
    G_GNUC_UNUSED GstBuffer *buffer);


GType gst_rtp_repair_meta_api_get_type(void)
{
  static GType type = 0;
  static const gchar *tags[] = {NULL};

  if (g_once_init_enter(&type)) {
    GType _type = gst_meta_api_type_register("GstRTPRepairMetaAPI", tags);
    g_once_init_leave(&type, _type);
  }

  return type;
}

const GstMetaInfo *gst_rtp_repair_meta_get_info(void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter(&meta_info)) {
    const GstMetaInfo *mi = gst_meta_register( GST_RTP_REPAIR_META_API_TYPE,
        "GstRTPRepairMeta",
        sizeof(GstRTPRepairMeta),
        (GstMetaInitFunction) gst_rtp_repair_meta_init,
        (GstMetaFreeFunction) gst_rtp_repair_meta_free,
        NULL );
    g_once_init_leave(&meta_info, mi);
  }

  return meta_info;
}

GstRTPRepairMeta *gst_buffer_get_rtp_repair_meta(GstBuffer *buffer)
{
  return (GstRTPRepairMeta *)gst_buffer_get_meta(buffer,
    gst_rtp_repair_meta_api_get_type());
}

GstRTPRepairMeta *gst_buffer_add_rtp_repair_meta(GstBuffer *buffer,
  const guint16 idx_red_packets, const guint16 num_red_packets,
  const guint32 ssrc, const guint16 *seqnum, guint seqnum_count)
{
  GstRTPRepairMeta *repair_meta = (GstRTPRepairMeta *) gst_buffer_add_meta (buffer,
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

gboolean gst_buffer_repairs_seqnum(GstBuffer *buffer, guint16 seqnum, guint32 ssrc)
{
  GstRTPRepairMeta *repair_meta = gst_buffer_get_rtp_repair_meta(buffer);
  if (repair_meta) {
    if (repair_meta->ssrc != ssrc) {
      return FALSE;
    }

    for (guint i = 0; i < repair_meta->seqnums->len; i++) {
      guint16 stored_seqnum = g_array_index(repair_meta->seqnums, guint16, i);
      if (stored_seqnum == seqnum) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

gint gst_buffer_get_repair_idx(GstBuffer *buffer)
{
  GstRTPRepairMeta *repair_meta = gst_buffer_get_rtp_repair_meta(buffer);
  if (repair_meta) {
    return repair_meta->idx_red_packets;
  }
  return -1;
}

gint gst_buffer_get_repair_num(GstBuffer *buffer)
{
  GstRTPRepairMeta *repair_meta = gst_buffer_get_rtp_repair_meta(buffer);
  if (repair_meta) {
    return repair_meta->num_red_packets;
  }
  return -1;
}

gboolean gst_buffer_get_repair_seqnums(GstBuffer *buffer, guint32 *ssrc,
    GArray **seqnums)
{
  GstRTPRepairMeta *repair_meta = gst_buffer_get_rtp_repair_meta(buffer);
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
gst_rtp_repair_meta_init(GstRTPRepairMeta * meta, G_GNUC_UNUSED gpointer params, 
    G_GNUC_UNUSED GstBuffer * buffer)
{
  meta->idx_red_packets = 0;
  meta->num_red_packets = 0;
  meta->ssrc = 0;
  meta->seqnums = g_array_new(FALSE, FALSE, sizeof(guint16));

  return TRUE;
}

static void
gst_rtp_repair_meta_free(GstRTPRepairMeta *meta,
    G_GNUC_UNUSED GstBuffer *buffer)
{
  g_array_unref (meta->seqnums);
}
