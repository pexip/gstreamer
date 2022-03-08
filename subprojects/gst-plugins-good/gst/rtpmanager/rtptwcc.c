/* GStreamer
 * Copyright (C)  2019 Pexip (http://pexip.com/)
 *   @author: Havard Graff <havard@pexip.com>
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
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include "rtptwcc.h"
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/base/gstbitreader.h>
#include <gst/base/gstbitwriter.h>
#include <gst/net/gsttxfeedback.h>

#include "gstrtputils.h"

GST_DEBUG_CATEGORY_EXTERN (rtp_session_debug);
#define GST_CAT_DEFAULT rtp_session_debug

#define WEIGHT(a, b, w) (((a) * (w)) + ((b) * (1.0 - (w))))

#define TWCC_EXTMAP_STR "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"

#define REF_TIME_UNIT (64 * GST_MSECOND)
#define DELTA_UNIT (250 * GST_USECOND)
#define MAX_TS_DELTA (0xff * DELTA_UNIT)

#define STATUS_VECTOR_MAX_CAPACITY 14
#define STATUS_VECTOR_TWO_BIT_MAX_CAPACITY 7

#define MAX_PACKETS_PER_FEEDBACK 65536

typedef enum
{
  RTP_TWCC_CHUNK_TYPE_RUN_LENGTH = 0,
  RTP_TWCC_CHUNK_TYPE_STATUS_VECTOR = 1,
} RTPTWCCChunkType;

typedef struct
{
  guint8 base_seqnum[2];
  guint8 packet_count[2];
  guint8 base_time[3];
  guint8 fb_pkt_count[1];
} RTPTWCCHeader;

typedef enum
{
  RTP_TWCC_PACKET_STATUS_NOT_RECV = 0,
  RTP_TWCC_PACKET_STATUS_SMALL_DELTA = 1,
  RTP_TWCC_PACKET_STATUS_LARGE_NEGATIVE_DELTA = 2,
} RTPTWCCPacketStatus;

typedef struct
{
  GstClockTime ts;
  guint16 seqnum;

  gint64 delta;
  RTPTWCCPacketStatus status;
  guint16 missing_run;
  guint equal_run;
} RecvPacket;

typedef struct
{
  GstClockTime local_ts;
  GstClockTime socket_ts;
  GstClockTime remote_ts;
  guint16 seqnum;
  guint8 pt;
  guint size;
  gboolean lost;
  gint32 rtx_osn;
  guint32 rtx_ssrc;
} SentPacket;

typedef struct
{
  RTPTWCCPacketStatus status;
  guint16 seqnum;
  GstClockTime remote_ts;
} ParsedPacket;

typedef struct
{
  GstClockTime org_ts;
  GstClockTime local_ts;
  GstClockTime remote_ts;
  guint16 seqnum;
  guint size;
  guint8 pt;

  GstClockTimeDiff delta_delta;
  gboolean recovered;
} StatsPacket;

typedef struct
{
  GArray *new_packets;
  GArray *win_packets;

  GstClockTime window_size;

  GstClockTime last_local_ts;
  GstClockTime last_remote_ts;

  guint last_packets_sent;
  guint last_packets_recv;

  /* windowed stats */
  guint bitrate_sent;
  guint bitrate_recv;
  gdouble packet_loss_pct;
  gdouble recovery_pct;
  gint64 avg_delta_of_delta;

  /* totals */
  guint packets_sent;
  guint packets_recv;

} TWCCStatsCtx;

static void
_append_structure_to_value_array (GValueArray * array, GstStructure * s)
{
  GValue *val;
  g_value_array_append (array, NULL);
  val = g_value_array_get_nth (array, array->n_values - 1);
  g_value_init (val, GST_TYPE_STRUCTURE);
  g_value_take_boxed (val, s);
}

static void
_structure_take_value_array (GstStructure * s,
    const gchar * field_name, GValueArray * array)
{
  GValue value = G_VALUE_INIT;
  g_value_init (&value, G_TYPE_VALUE_ARRAY);
  g_value_take_boxed (&value, array);
  gst_structure_take_value (s, field_name, &value);
  g_value_unset (&value);
}

static TWCCStatsCtx *
twcc_stats_ctx_new (GstClockTime window_size)
{
  TWCCStatsCtx *ctx = g_new0 (TWCCStatsCtx, 1);

  ctx->new_packets = g_array_new (FALSE, FALSE, sizeof (StatsPacket));
  ctx->win_packets = g_array_new (FALSE, FALSE, sizeof (StatsPacket));

  ctx->window_size = window_size;

  ctx->last_local_ts = GST_CLOCK_TIME_NONE;
  ctx->last_remote_ts = GST_CLOCK_TIME_NONE;

  return ctx;
}

static void
twcc_stats_ctx_free (TWCCStatsCtx * ctx)
{
  g_array_unref (ctx->new_packets);
  g_array_unref (ctx->win_packets);
  g_free (ctx);
}

static void
twcc_stats_ctx_add_packet (TWCCStatsCtx * ctx, StatsPacket * pkt)
{
  g_array_append_val (ctx->new_packets, *pkt);
}

static void
twcc_stats_ctx_process_new_stats (TWCCStatsCtx * ctx)
{
  guint i;

  ctx->last_packets_sent = ctx->new_packets->len;
  ctx->last_packets_recv = 0;

  for (i = 0; i < ctx->new_packets->len; i++) {
    StatsPacket *pkt = &g_array_index (ctx->new_packets, StatsPacket, i);
    GstClockTimeDiff local_delta = GST_CLOCK_STIME_NONE;
    GstClockTimeDiff remote_delta = GST_CLOCK_STIME_NONE;

    if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts))
      ctx->last_packets_recv++;

    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts) &&
        GST_CLOCK_TIME_IS_VALID (ctx->last_local_ts)) {
      local_delta = GST_CLOCK_DIFF (ctx->last_local_ts, pkt->local_ts);
    }

    if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) &&
        GST_CLOCK_TIME_IS_VALID (ctx->last_remote_ts)) {
      remote_delta = GST_CLOCK_DIFF (ctx->last_remote_ts, pkt->remote_ts);
    }

    if (GST_CLOCK_STIME_IS_VALID (local_delta) &&
        GST_CLOCK_STIME_IS_VALID (remote_delta)) {
      pkt->delta_delta = remote_delta - local_delta;
    }

    ctx->last_local_ts = pkt->local_ts;
    ctx->last_remote_ts = pkt->remote_ts;
  }

  /* we add our packets send/recv to the total */
  ctx->packets_recv += ctx->last_packets_recv;
  ctx->packets_sent += ctx->last_packets_sent;

  /* and add the processed stats to the windowed stats */
  g_array_append_vals (ctx->win_packets, ctx->new_packets->data,
      ctx->new_packets->len);

  /* and we reset the new packets, ready for more */
  g_array_set_size (ctx->new_packets, 0);
}


static gint
_get_window_start_index (GArray * packets, GstClockTime duration,
    GstClockTime * local_duration, GstClockTime * remote_duration)
{
  StatsPacket *last = NULL;
  guint i;

  if (packets->len < 2)
    return -1;

  for (i = 0; i < packets->len; i++) {
    guint start_index = packets->len - 1 - i;
    StatsPacket *pkt = &g_array_index (packets, StatsPacket, start_index);
    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts)
        && GST_CLOCK_TIME_IS_VALID (pkt->remote_ts)) {
      /* first find the last valid packet */
      if (last == NULL) {
        last = pkt;
      } else {
        /* and then get the duration in local ts */
        GstClockTimeDiff ld = GST_CLOCK_DIFF (pkt->local_ts, last->local_ts);
        if (ld >= duration) {
          *local_duration = ld;
          *remote_duration = GST_CLOCK_DIFF (pkt->remote_ts, last->remote_ts);
          return start_index;
        }
      }
    }
  }

  return -1;
}

static void
twcc_stats_ctx_calculate_windowed_stats (TWCCStatsCtx * ctx,
    GstClockTimeDiff recovery_deadline)
{
  GArray *packets = ctx->win_packets;
  guint packets_sent = 0;
  guint packets_recv = 0;
  guint packets_recovered = 0;
  guint deadline_lost = 0;
  guint packets_lost;

  guint i;
  gint start_idx;
  guint bits_sent = 0;
  guint bits_recv = 0;
  GstClockTimeDiff delta_delta_sum = 0;
  guint delta_delta_count = 0;
  GstClockTime local_duration = 0;
  GstClockTime remote_duration = 0;

  ctx->packet_loss_pct = 0.0;
  ctx->avg_delta_of_delta = GST_CLOCK_STIME_NONE;
  ctx->bitrate_sent = 0;
  ctx->bitrate_recv = 0;
  ctx->recovery_pct = -1.0;

  twcc_stats_ctx_process_new_stats (ctx);

  start_idx = _get_window_start_index (packets, ctx->window_size,
      &local_duration, &remote_duration);
  if (start_idx == -1)
    return;

  /* remove the old packets */
  if (start_idx > 0)
    g_array_remove_range (packets, 0, start_idx);

  packets_sent = packets->len;

  for (i = 0; i < packets_sent; i++) {
    StatsPacket *pkt = &g_array_index (packets, StatsPacket, i);
    /* we ignore the bit-contribution from the first packet,
       since that falls outside our window */
    gboolean first_packet = (i == 0);

    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts)) {
      if (!first_packet)
        bits_sent += pkt->size * 8;
    }

    if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts)) {
      if (!first_packet)
        bits_recv += pkt->size * 8;
      packets_recv++;
    } else {
      /* we have a lost packet */
      if (pkt->org_ts <= recovery_deadline) {
        GST_LOG ("pkt: #%u pt: %u is lost: recovered: %u, "
            "org_ts = %" GST_TIME_FORMAT " deadline = %" GST_TIME_FORMAT,
            pkt->seqnum, pkt->pt, pkt->recovered,
            GST_TIME_ARGS (pkt->org_ts), GST_TIME_ARGS (recovery_deadline));

        deadline_lost++;
        if (pkt->recovered) {
          packets_recovered++;
        }
      }
    }

    if (GST_CLOCK_STIME_IS_VALID (pkt->delta_delta)) {
      delta_delta_sum += pkt->delta_delta;
      delta_delta_count++;
    }
  }

  packets_lost = packets_sent - packets_recv;
  ctx->packet_loss_pct = (packets_lost * 100) / (gfloat) packets_sent;

  if (deadline_lost) {
    ctx->recovery_pct = (packets_recovered * 100) / (gfloat) deadline_lost;
  }

  if (delta_delta_count) {
    ctx->avg_delta_of_delta = delta_delta_sum / delta_delta_count;
  }

  if (local_duration > 0)
    ctx->bitrate_sent =
        gst_util_uint64_scale (bits_sent, GST_SECOND, local_duration);
  if (remote_duration > 0)
    ctx->bitrate_recv =
        gst_util_uint64_scale (bits_recv, GST_SECOND, remote_duration);

  GST_LOG ("Got stats: bits_sent: %u, bits_recv: %u, packets_sent = %u, "
      "packets_recv: %u, packetlost_pct = %f, sent_bitrate = %u, "
      "recv_bitrate = %u, delta-delta-avg = %" GST_STIME_FORMAT,
      bits_sent, bits_recv, packets_sent,
      packets_recv, ctx->packet_loss_pct, ctx->bitrate_sent,
      ctx->bitrate_recv, GST_STIME_ARGS (ctx->avg_delta_of_delta));
}

static GstStructure *
twcc_stats_ctx_get_structure (TWCCStatsCtx * ctx)
{
  return gst_structure_new ("RTPTWCCStats",
      "packets-sent", G_TYPE_UINT, ctx->last_packets_sent,
      "packets-recv", G_TYPE_UINT, ctx->last_packets_recv,
      "bitrate-sent", G_TYPE_UINT, ctx->bitrate_sent,
      "bitrate-recv", G_TYPE_UINT, ctx->bitrate_recv,
      "packet-loss-pct", G_TYPE_DOUBLE, ctx->packet_loss_pct,
      "recovery-pct", G_TYPE_DOUBLE, ctx->recovery_pct,
      "avg-delta-of-delta", G_TYPE_INT64, ctx->avg_delta_of_delta, NULL);
}

static StatsPacket *
_get_packet_for_seqnum (GArray * array, guint16 seqnum)
{
  guint i;

  for (i = 0; i < array->len; i++) {
    StatsPacket *pkt = &g_array_index (array, StatsPacket, i);
    if (pkt->seqnum == seqnum) {
      return pkt;
    }
  }

  return NULL;
}

static StatsPacket *
twcc_stats_ctx_get_packet_for_seqnum (TWCCStatsCtx * ctx, guint16 seqnum)
{
  StatsPacket *pkt;

  if ((pkt = _get_packet_for_seqnum (ctx->new_packets, seqnum)) != NULL)
    return pkt;

  if ((pkt = _get_packet_for_seqnum (ctx->win_packets, seqnum)) != NULL)
    return pkt;

  return NULL;
}

static StatsPacket *
_get_packet_for_seqnum_fast (GArray * array, guint16 seqnum)
{
  StatsPacket *first;
  guint16 idx;

  if (array->len == 0)
    return NULL;

  first = &g_array_index (array, StatsPacket, 0);
  idx = seqnum - first->seqnum;

  if (idx < array->len) {
    StatsPacket *found = &g_array_index (array, StatsPacket, idx);
    if (found->seqnum == seqnum) {
      return found;
    }
  }

  return NULL;
}

/* assumes all seqnum are in order */
static StatsPacket *
twcc_stats_ctx_get_packet_for_seqnum_fast (TWCCStatsCtx * ctx, guint16 seqnum)
{
  StatsPacket *pkt;

  if ((pkt = _get_packet_for_seqnum_fast (ctx->new_packets, seqnum)) != NULL)
    return pkt;

  if ((pkt = _get_packet_for_seqnum_fast (ctx->win_packets, seqnum)) != NULL)
    return pkt;

  return NULL;
}


/******************************************************/

struct _RTPTWCCManager
{
  GObject object;

  GHashTable *ssrc_to_seqmap;

  GstClockTime stats_window_size;
  TWCCStatsCtx *stats_ctx;
  GHashTable *stats_ctx_by_pt;

  guint8 send_ext_id;
  guint8 recv_ext_id;
  guint16 send_seqnum;

  guint mtu;
  guint max_packets_per_rtcp;
  GArray *recv_packets;

  guint64 fb_pkt_count;
  gint32 last_seqnum;

  GArray *sent_packets;
  GArray *parsed_packets;
  GQueue *rtcp_buffers;

  guint64 recv_sender_ssrc;
  guint64 recv_media_ssrc;

  guint16 expected_recv_seqnum;
  guint16 packet_count_no_marker;

  gboolean first_fci_parse;
  guint16 expected_parsed_seqnum;
  guint8 expected_parsed_fb_pkt_count;

  GstClockTime next_feedback_send_time;
  GstClockTime feedback_interval;

  GstClockTimeDiff avg_rtt;
  GstClockTime last_report_time;
};


static void rtp_twcc_manager_tx_feedback (GstTxFeedback * parent,
    guint64 buffer_id, GstClockTime ts);

static void
_tx_feedback_init (gpointer g_iface, G_GNUC_UNUSED gpointer iface_data)
{
  GstTxFeedbackInterface *iface = g_iface;
  iface->tx_feedback = rtp_twcc_manager_tx_feedback;
}

G_DEFINE_TYPE_WITH_CODE (RTPTWCCManager, rtp_twcc_manager, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_TX_FEEDBACK, _tx_feedback_init));

static void
rtp_twcc_manager_init (RTPTWCCManager * twcc)
{
  twcc->ssrc_to_seqmap = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_hash_table_destroy);

  twcc->recv_packets = g_array_new (FALSE, FALSE, sizeof (RecvPacket));
  twcc->sent_packets = g_array_new (FALSE, FALSE, sizeof (SentPacket));
  twcc->parsed_packets = g_array_new (FALSE, FALSE, sizeof (ParsedPacket));

  twcc->rtcp_buffers = g_queue_new ();

  twcc->stats_window_size = 300 * GST_MSECOND;
  twcc->stats_ctx = twcc_stats_ctx_new (twcc->stats_window_size);
  twcc->stats_ctx_by_pt = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) twcc_stats_ctx_free);

  twcc->last_seqnum = -1;
  twcc->recv_media_ssrc = -1;
  twcc->recv_sender_ssrc = -1;

  twcc->first_fci_parse = TRUE;

  twcc->feedback_interval = GST_CLOCK_TIME_NONE;
  twcc->next_feedback_send_time = GST_CLOCK_TIME_NONE;
  twcc->last_report_time = GST_CLOCK_TIME_NONE;
}

static void
rtp_twcc_manager_finalize (GObject * object)
{
  RTPTWCCManager *twcc = RTP_TWCC_MANAGER_CAST (object);

  g_hash_table_destroy (twcc->ssrc_to_seqmap);

  g_array_unref (twcc->recv_packets);
  g_array_unref (twcc->sent_packets);
  g_array_unref (twcc->parsed_packets);
  g_queue_free_full (twcc->rtcp_buffers, (GDestroyNotify) gst_buffer_unref);

  g_hash_table_destroy (twcc->stats_ctx_by_pt);
  twcc_stats_ctx_free (twcc->stats_ctx);

  G_OBJECT_CLASS (rtp_twcc_manager_parent_class)->finalize (object);
}

static void
rtp_twcc_manager_class_init (RTPTWCCManagerClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  gobject_class->finalize = rtp_twcc_manager_finalize;
}

RTPTWCCManager *
rtp_twcc_manager_new (guint mtu)
{
  RTPTWCCManager *twcc = g_object_new (RTP_TYPE_TWCC_MANAGER, NULL);

  rtp_twcc_manager_set_mtu (twcc, mtu);

  return twcc;
}

static TWCCStatsCtx *
_get_ctx_for_pt (RTPTWCCManager * twcc, guint pt)
{
  TWCCStatsCtx *ctx =
      g_hash_table_lookup (twcc->stats_ctx_by_pt, GUINT_TO_POINTER (pt));
  if (!ctx) {
    ctx = twcc_stats_ctx_new (twcc->stats_window_size);
    g_hash_table_insert (twcc->stats_ctx_by_pt, GUINT_TO_POINTER (pt), ctx);
  }
  return ctx;
}

static void
_add_packet_to_stats (RTPTWCCManager * twcc, SentPacket * packet)
{
  TWCCStatsCtx *ctx;
  StatsPacket pkt;

  pkt.org_ts = packet->local_ts;
  pkt.local_ts = packet->local_ts;
  pkt.remote_ts = packet->remote_ts;
  pkt.seqnum = packet->seqnum;
  pkt.size = packet->size;
  pkt.pt = packet->pt;
  pkt.recovered = FALSE;
  pkt.delta_delta = GST_CLOCK_STIME_NONE;

  /* if we have a socket-timestamp, use that instead */
  if (GST_CLOCK_TIME_IS_VALID (packet->socket_ts)) {
    pkt.local_ts = packet->socket_ts;
  }

  twcc_stats_ctx_add_packet (twcc->stats_ctx, &pkt);

  ctx = _get_ctx_for_pt (twcc, packet->pt);
  twcc_stats_ctx_add_packet (ctx, &pkt);
}

static void
_update_stats_with_recovered (RTPTWCCManager * twcc, guint16 seqnum)
{
  TWCCStatsCtx *ctx;
  StatsPacket *pkt =
      twcc_stats_ctx_get_packet_for_seqnum_fast (twcc->stats_ctx, seqnum);

  if (pkt == NULL) {
    pkt = twcc_stats_ctx_get_packet_for_seqnum (twcc->stats_ctx, seqnum);
    if (pkt)
      GST_INFO ("Could not find #%u fast, but found it slow?!?!?", seqnum);
  }

  if (pkt == NULL) {
    GST_INFO ("Could not find seqnum %u", seqnum);
    return;
  }

  pkt->recovered = TRUE;

  /* now find the equivalent packet in the payload */
  ctx = _get_ctx_for_pt (twcc, pkt->pt);
  pkt = twcc_stats_ctx_get_packet_for_seqnum (ctx, seqnum);

  if (pkt) {
    pkt->recovered = TRUE;
  }
}

static void
recv_packet_init (RecvPacket * packet, guint16 seqnum, RTPPacketInfo * pinfo)
{
  memset (packet, 0, sizeof (RecvPacket));
  packet->seqnum = seqnum;

  if (GST_CLOCK_TIME_IS_VALID (pinfo->arrival_time))
    packet->ts = pinfo->arrival_time;
  else
    packet->ts = pinfo->current_time;
}

void
rtp_twcc_manager_parse_recv_ext_id (RTPTWCCManager * twcc,
    const GstStructure * s)
{
  guint8 recv_ext_id = gst_rtp_get_extmap_id_for_attribute (s, TWCC_EXTMAP_STR);
  if (recv_ext_id > 0) {
    twcc->recv_ext_id = recv_ext_id;
    GST_INFO ("TWCC enabled for recv using extension id: %u",
        twcc->recv_ext_id);
  }
}

void
rtp_twcc_manager_parse_send_ext_id (RTPTWCCManager * twcc,
    const GstStructure * s)
{
  guint8 send_ext_id = gst_rtp_get_extmap_id_for_attribute (s, TWCC_EXTMAP_STR);
  if (send_ext_id > 0) {
    twcc->send_ext_id = send_ext_id;
    GST_INFO ("TWCC enabled for send using extension id: %u",
        twcc->send_ext_id);
  }
}

void
rtp_twcc_manager_set_mtu (RTPTWCCManager * twcc, guint mtu)
{
  twcc->mtu = mtu;

  /* the absolute worst case is that 7 packets uses
     header (4 * 4 * 4) 32 bytes) and 
     packet_chunk 2 bytes +  
     recv_deltas (2 * 7) 14 bytes */
  twcc->max_packets_per_rtcp = ((twcc->mtu - 32) * 7) / (2 + 14);
}

void
rtp_twcc_manager_set_feedback_interval (RTPTWCCManager * twcc,
    GstClockTime feedback_interval)
{
  twcc->feedback_interval = feedback_interval;
}

GstClockTime
rtp_twcc_manager_get_feedback_interval (RTPTWCCManager * twcc)
{
  return twcc->feedback_interval;
}

static gboolean
_get_twcc_seqnum_data (RTPPacketInfo * pinfo, guint8 ext_id, gpointer * data)
{
  gboolean ret = FALSE;
  guint size;

  if (pinfo->header_ext &&
      gst_rtp_buffer_get_extension_onebyte_header_from_bytes (pinfo->header_ext,
          pinfo->header_ext_bit_pattern, ext_id, 0, data, &size)) {
    if (size == 2)
      ret = TRUE;
  }
  return ret;
}

static void
sent_packet_init (SentPacket * packet, guint16 seqnum, RTPPacketInfo * pinfo,
    GstRTPBuffer * rtp)
{
  packet->seqnum = seqnum;
  packet->local_ts = pinfo->current_time;
  packet->size = gst_rtp_buffer_get_payload_len (rtp);
  packet->pt = gst_rtp_buffer_get_payload_type (rtp);
  packet->remote_ts = GST_CLOCK_TIME_NONE;
  packet->socket_ts = GST_CLOCK_TIME_NONE;
  packet->lost = FALSE;
  packet->rtx_osn = pinfo->rtx_osn;
  packet->rtx_ssrc = pinfo->rtx_ssrc;
}

static void
rtp_twcc_manager_register_seqnum (RTPTWCCManager * twcc,
    guint32 ssrc, guint16 seqnum, guint16 twcc_seqnum)
{
  GHashTable *seq_to_twcc =
      g_hash_table_lookup (twcc->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
  if (!seq_to_twcc) {
    seq_to_twcc = g_hash_table_new (NULL, NULL);
    g_hash_table_insert (twcc->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc),
        seq_to_twcc);
  }
  g_hash_table_insert (seq_to_twcc, GUINT_TO_POINTER (seqnum),
      GUINT_TO_POINTER (twcc_seqnum));
  GST_LOG ("Registering OSN: %u to twcc-twcc_seqnum: %u with ssrc: %u", seqnum,
      twcc_seqnum, ssrc);
}

/* Remove old sent packets and keep them under a maximum threshold, as we
   can't accumulate them if we don't get a feedback message from the
   receiver. */
static void
_prune_old_sent_packets (RTPTWCCManager * twcc)
{
  guint length;

  if (twcc->sent_packets->len <= MAX_PACKETS_PER_FEEDBACK)
    return;

  length = twcc->sent_packets->len - MAX_PACKETS_PER_FEEDBACK;
  g_array_remove_range (twcc->sent_packets, 0, length);
}

static void
_set_twcc_seqnum_data (RTPTWCCManager * twcc, RTPPacketInfo * pinfo,
    GstBuffer * buf, guint8 ext_id)
{
  SentPacket packet;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gpointer data;

  if (gst_rtp_buffer_map (buf, GST_MAP_READWRITE, &rtp)) {
    if (gst_rtp_buffer_get_extension_onebyte_header (&rtp,
            ext_id, 0, &data, NULL)) {
      guint16 seqnum = twcc->send_seqnum++;

      GST_WRITE_UINT16_BE (data, seqnum);
      sent_packet_init (&packet, seqnum, pinfo, &rtp);
      g_array_append_val (twcc->sent_packets, packet);
      _prune_old_sent_packets (twcc);

      rtp_twcc_manager_register_seqnum (twcc, pinfo->ssrc, pinfo->seqnum,
          seqnum);

      gst_buffer_add_tx_feedback_meta (pinfo->data, seqnum,
          GST_TX_FEEDBACK_CAST (twcc));

      GST_LOG ("Send: twcc-seqnum: %u, pt: %u, marker: %d, len: %u, ts: %"
          GST_TIME_FORMAT, seqnum, packet.pt, pinfo->marker, packet.size,
          GST_TIME_ARGS (pinfo->current_time));
    }
    gst_rtp_buffer_unmap (&rtp);
  }
}

static void
rtp_twcc_manager_set_send_twcc_seqnum (RTPTWCCManager * twcc,
    RTPPacketInfo * pinfo)
{
  if (GST_IS_BUFFER_LIST (pinfo->data)) {
    GstBufferList *list;
    guint i = 0;

    pinfo->data = gst_buffer_list_make_writable (pinfo->data);

    list = GST_BUFFER_LIST (pinfo->data);

    for (i = 0; i < gst_buffer_list_length (list); i++) {
      GstBuffer *buffer = gst_buffer_list_get_writable (list, i);

      _set_twcc_seqnum_data (twcc, pinfo, buffer, twcc->send_ext_id);
    }
  } else {
    pinfo->data = gst_buffer_make_writable (pinfo->data);
    _set_twcc_seqnum_data (twcc, pinfo, pinfo->data, twcc->send_ext_id);
  }
}

static gint32
rtp_twcc_manager_get_recv_twcc_seqnum (RTPTWCCManager * twcc,
    RTPPacketInfo * pinfo)
{
  gint32 val = -1;
  gpointer data;

  if (twcc->recv_ext_id == 0) {
    GST_DEBUG ("Received TWCC packet, but no extension registered; ignoring");
    return val;
  }

  if (_get_twcc_seqnum_data (pinfo, twcc->recv_ext_id, &data)) {
    val = GST_READ_UINT16_BE (data);
  }

  return val;
}

GstClockTime
rtp_twcc_manager_get_next_timeout (RTPTWCCManager * twcc,
    GstClockTime current_time)
{
  if (GST_CLOCK_TIME_IS_VALID (twcc->feedback_interval)) {
    if (!GST_CLOCK_TIME_IS_VALID (twcc->next_feedback_send_time)) {
      /* First time through: initialise feedback time */
      twcc->next_feedback_send_time = current_time + twcc->feedback_interval;
    }
    return twcc->next_feedback_send_time;
  }
  return GST_CLOCK_TIME_NONE;
}

static gint
_twcc_seqnum_sort (gconstpointer a, gconstpointer b)
{
  gint32 seqa = ((RecvPacket *) a)->seqnum;
  gint32 seqb = ((RecvPacket *) b)->seqnum;
  gint res = seqa - seqb;
  if (res < -65000)
    res = 1;
  if (res > 65000)
    res = -1;
  return res;
}

static void
rtp_twcc_write_recv_deltas (guint8 * fci_data, GArray * twcc_packets)
{
  guint i;
  for (i = 0; i < twcc_packets->len; i++) {
    RecvPacket *pkt = &g_array_index (twcc_packets, RecvPacket, i);

    if (pkt->status == RTP_TWCC_PACKET_STATUS_SMALL_DELTA) {
      GST_WRITE_UINT8 (fci_data, pkt->delta);
      fci_data += 1;
    } else if (pkt->status == RTP_TWCC_PACKET_STATUS_LARGE_NEGATIVE_DELTA) {
      GST_WRITE_UINT16_BE (fci_data, pkt->delta);
      fci_data += 2;
    }
  }
}

static void
rtp_twcc_write_run_length_chunk (GArray * packet_chunks,
    RTPTWCCPacketStatus status, guint run_length)
{
  guint written = 0;
  while (written < run_length) {
    GstBitWriter writer;
    guint16 data = 0;
    guint len = MIN (run_length - written, 8191);

    GST_LOG ("Writing a run-length of %u with status %u", len, status);

    gst_bit_writer_init_with_data (&writer, (guint8 *) & data, 2, FALSE);
    gst_bit_writer_put_bits_uint8 (&writer, RTP_TWCC_CHUNK_TYPE_RUN_LENGTH, 1);
    gst_bit_writer_put_bits_uint8 (&writer, status, 2);
    gst_bit_writer_put_bits_uint16 (&writer, len, 13);
    g_array_append_val (packet_chunks, data);
    written += len;
  }
}

typedef struct
{
  GArray *packet_chunks;
  GstBitWriter writer;
  guint16 data;
  guint symbol_size;
} ChunkBitWriter;

static void
chunk_bit_writer_reset (ChunkBitWriter * writer)
{
  writer->data = 0;
  gst_bit_writer_init_with_data (&writer->writer,
      (guint8 *) & writer->data, 2, FALSE);

  gst_bit_writer_put_bits_uint8 (&writer->writer,
      RTP_TWCC_CHUNK_TYPE_STATUS_VECTOR, 1);
  /* 1 for 2-bit symbol-size, 0 for 1-bit */
  gst_bit_writer_put_bits_uint8 (&writer->writer, writer->symbol_size - 1, 1);
}

static void
chunk_bit_writer_configure (ChunkBitWriter * writer, guint symbol_size)
{
  writer->symbol_size = symbol_size;
  chunk_bit_writer_reset (writer);
}

static gboolean
chunk_bit_writer_is_empty (ChunkBitWriter * writer)
{
  return writer->writer.bit_size == 2;
}

static gboolean
chunk_bit_writer_is_full (ChunkBitWriter * writer)
{
  return writer->writer.bit_size == 16;
}

static guint
chunk_bit_writer_get_available_slots (ChunkBitWriter * writer)
{
  return (16 - writer->writer.bit_size) / writer->symbol_size;
}

static guint
chunk_bit_writer_get_total_slots (ChunkBitWriter * writer)
{
  return STATUS_VECTOR_MAX_CAPACITY / writer->symbol_size;
}

static void
chunk_bit_writer_flush (ChunkBitWriter * writer)
{
  /* don't append a chunk if no bits have been written */
  if (!chunk_bit_writer_is_empty (writer)) {
    g_array_append_val (writer->packet_chunks, writer->data);
    chunk_bit_writer_reset (writer);
  }
}

static void
chunk_bit_writer_init (ChunkBitWriter * writer,
    GArray * packet_chunks, guint symbol_size)
{
  writer->packet_chunks = packet_chunks;
  chunk_bit_writer_configure (writer, symbol_size);
}

static void
chunk_bit_writer_write (ChunkBitWriter * writer, RTPTWCCPacketStatus status)
{
  gst_bit_writer_put_bits_uint8 (&writer->writer, status, writer->symbol_size);
  if (chunk_bit_writer_is_full (writer)) {
    chunk_bit_writer_flush (writer);
  }
}

static void
rtp_twcc_write_status_vector_chunk (ChunkBitWriter * writer, RecvPacket * pkt)
{
  if (pkt->missing_run > 0) {
    guint available = chunk_bit_writer_get_available_slots (writer);
    guint total = chunk_bit_writer_get_total_slots (writer);
    guint i;

    if (pkt->missing_run > (available + total)) {
      /* here it is better to finish up the current status-chunk and then
         go for run-length */
      for (i = 0; i < available; i++) {
        chunk_bit_writer_write (writer, RTP_TWCC_PACKET_STATUS_NOT_RECV);
      }
      rtp_twcc_write_run_length_chunk (writer->packet_chunks,
          RTP_TWCC_PACKET_STATUS_NOT_RECV, pkt->missing_run - available);
    } else {
      for (i = 0; i < pkt->missing_run; i++) {
        chunk_bit_writer_write (writer, RTP_TWCC_PACKET_STATUS_NOT_RECV);
      }
    }
  }

  chunk_bit_writer_write (writer, pkt->status);
}

typedef struct
{
  RecvPacket *equal;
} RunLengthHelper;

static void
run_lenght_helper_update (RunLengthHelper * rlh, RecvPacket * pkt)
{
  /* for missing packets we reset */
  if (pkt->missing_run > 0) {
    rlh->equal = NULL;
  }

  /* all status equal run */
  if (rlh->equal == NULL) {
    rlh->equal = pkt;
    rlh->equal->equal_run = 0;
  }

  if (rlh->equal->status == pkt->status) {
    rlh->equal->equal_run++;
  } else {
    rlh->equal = pkt;
    rlh->equal->equal_run = 1;
  }
}

static guint
_get_max_packets_capacity (guint symbol_size)
{
  if (symbol_size == 2)
    return STATUS_VECTOR_TWO_BIT_MAX_CAPACITY;

  return STATUS_VECTOR_MAX_CAPACITY;
}

static gboolean
_pkt_fits_run_length_chunk (RecvPacket * pkt, guint packets_per_chunks,
    guint remaining_packets)
{
  if (pkt->missing_run == 0) {
    /* we have more or the same equal packets than the ones we can write in to a status chunk */
    if (pkt->equal_run >= packets_per_chunks)
      return TRUE;

    /* we have more than one equal and not enough space for the remainings */
    if (pkt->equal_run > 1 && remaining_packets > STATUS_VECTOR_MAX_CAPACITY)
      return TRUE;

    /* we have all equal packets for the remaining to write */
    if (pkt->equal_run == remaining_packets)
      return TRUE;
  }

  return FALSE;
}

static void
rtp_twcc_write_chunks (GArray * packet_chunks,
    GArray * twcc_packets, guint symbol_size)
{
  ChunkBitWriter writer;
  guint i;
  guint packets_per_chunks = _get_max_packets_capacity (symbol_size);

  chunk_bit_writer_init (&writer, packet_chunks, symbol_size);

  for (i = 0; i < twcc_packets->len; i++) {
    RecvPacket *pkt = &g_array_index (twcc_packets, RecvPacket, i);
    guint remaining_packets = twcc_packets->len - i;

    GST_LOG
        ("About to write pkt: #%u missing_run: %u equal_run: %u status: %u, remaining_packets: %u",
        pkt->seqnum, pkt->missing_run, pkt->equal_run, pkt->status,
        remaining_packets);

    /* we can only start a run-length chunk if the status-chunk is
       completed */
    if (chunk_bit_writer_is_empty (&writer)) {
      /* first write in any preceeding gaps, we use run-length
         if it would take up more than one chunk (14/7) */
      if (pkt->missing_run > packets_per_chunks) {
        rtp_twcc_write_run_length_chunk (packet_chunks,
            RTP_TWCC_PACKET_STATUS_NOT_RECV, pkt->missing_run);
      }

      /* we have a run of the same status, write a run-length chunk and skip
         to the next point */
      if (_pkt_fits_run_length_chunk (pkt, packets_per_chunks,
              remaining_packets)) {

        rtp_twcc_write_run_length_chunk (packet_chunks,
            pkt->status, pkt->equal_run);
        i += pkt->equal_run - 1;
        continue;
      }
    }

    GST_LOG ("i=%u: Writing a %u-bit vector of status: %u",
        i, symbol_size, pkt->status);
    rtp_twcc_write_status_vector_chunk (&writer, pkt);
  }
  chunk_bit_writer_flush (&writer);
}

static void
rtp_twcc_manager_add_fci (RTPTWCCManager * twcc, GstRTCPPacket * packet)
{
  RecvPacket *first, *last, *prev;
  guint16 packet_count;
  GstClockTime base_time;
  GstClockTime ts_rounded;
  guint i;
  GArray *packet_chunks = g_array_new (FALSE, FALSE, 2);
  RTPTWCCHeader header;
  guint header_size = sizeof (RTPTWCCHeader);
  guint packet_chunks_size;
  guint recv_deltas_size = 0;
  guint16 fci_length;
  guint16 fci_chunks;
  guint8 *fci_data;
  guint8 *fci_data_ptr;
  RunLengthHelper rlh = { NULL };
  guint symbol_size = 1;
  GstClockTimeDiff delta_ts;
  gint64 delta_ts_rounded;
  guint8 fb_pkt_count;

  g_array_sort (twcc->recv_packets, _twcc_seqnum_sort);

  /* get first and last packet */
  first = &g_array_index (twcc->recv_packets, RecvPacket, 0);
  last =
      &g_array_index (twcc->recv_packets, RecvPacket,
      twcc->recv_packets->len - 1);

  packet_count = last->seqnum - first->seqnum + 1;
  base_time = first->ts / REF_TIME_UNIT;
  fb_pkt_count = (guint8) (twcc->fb_pkt_count % G_MAXUINT8);

  GST_WRITE_UINT16_BE (header.base_seqnum, first->seqnum);
  GST_WRITE_UINT16_BE (header.packet_count, packet_count);
  GST_WRITE_UINT24_BE (header.base_time, base_time);
  GST_WRITE_UINT8 (header.fb_pkt_count, fb_pkt_count);

  base_time *= REF_TIME_UNIT;
  ts_rounded = base_time;

  GST_DEBUG ("Created TWCC feedback: base_seqnum: #%u, packet_count: %u, "
      "base_time %" GST_TIME_FORMAT " fb_pkt_count: %u",
      first->seqnum, packet_count, GST_TIME_ARGS (base_time), fb_pkt_count);

  twcc->fb_pkt_count++;
  twcc->expected_recv_seqnum = first->seqnum + packet_count;

  /* calculate all deltas and check for gaps etc */
  prev = first;
  for (i = 0; i < twcc->recv_packets->len; i++) {
    RecvPacket *pkt = &g_array_index (twcc->recv_packets, RecvPacket, i);
    if (i != 0) {
      pkt->missing_run = pkt->seqnum - prev->seqnum - 1;
    }

    delta_ts = GST_CLOCK_DIFF (ts_rounded, pkt->ts);
    pkt->delta = delta_ts / DELTA_UNIT;
    delta_ts_rounded = pkt->delta * DELTA_UNIT;
    ts_rounded += delta_ts_rounded;

    if (delta_ts_rounded < 0 || delta_ts_rounded > MAX_TS_DELTA) {
      pkt->status = RTP_TWCC_PACKET_STATUS_LARGE_NEGATIVE_DELTA;
      recv_deltas_size += 2;
      symbol_size = 2;
    } else {
      pkt->status = RTP_TWCC_PACKET_STATUS_SMALL_DELTA;
      recv_deltas_size += 1;
    }
    run_lenght_helper_update (&rlh, pkt);

    GST_LOG ("pkt: #%u, ts: %" GST_TIME_FORMAT
        " ts_rounded: %" GST_TIME_FORMAT
        " delta_ts: %" GST_STIME_FORMAT
        " delta_ts_rounded: %" GST_STIME_FORMAT
        " missing_run: %u, status: %u", pkt->seqnum,
        GST_TIME_ARGS (pkt->ts), GST_TIME_ARGS (ts_rounded),
        GST_STIME_ARGS (delta_ts), GST_STIME_ARGS (delta_ts_rounded),
        pkt->missing_run, pkt->status);
    prev = pkt;
  }

  rtp_twcc_write_chunks (packet_chunks, twcc->recv_packets, symbol_size);

  packet_chunks_size = packet_chunks->len * 2;
  fci_length = header_size + packet_chunks_size + recv_deltas_size;
  fci_chunks = (fci_length - 1) / sizeof (guint32) + 1;

  if (!gst_rtcp_packet_fb_set_fci_length (packet, fci_chunks)) {
    GST_ERROR ("Could not fit: %u packets", packet_count);
    g_assert_not_reached ();
  }

  fci_data = gst_rtcp_packet_fb_get_fci (packet);
  fci_data_ptr = fci_data;

  memcpy (fci_data_ptr, &header, header_size);
  fci_data_ptr += header_size;

  memcpy (fci_data_ptr, packet_chunks->data, packet_chunks_size);
  fci_data_ptr += packet_chunks_size;

  rtp_twcc_write_recv_deltas (fci_data_ptr, twcc->recv_packets);

  GST_MEMDUMP ("twcc-header:", (guint8 *) & header, header_size);
  GST_MEMDUMP ("packet-chunks:", (guint8 *) packet_chunks->data,
      packet_chunks_size);
  GST_MEMDUMP ("full fci:", fci_data, fci_length);

  g_array_unref (packet_chunks);
  g_array_set_size (twcc->recv_packets, 0);
}

static void
rtp_twcc_manager_create_feedback (RTPTWCCManager * twcc)
{
  GstBuffer *buf;
  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  GstRTCPPacket packet;

  buf = gst_rtcp_buffer_new (twcc->mtu);

  gst_rtcp_buffer_map (buf, GST_MAP_READWRITE, &rtcp);

  gst_rtcp_buffer_add_packet (&rtcp, GST_RTCP_TYPE_RTPFB, &packet);

  gst_rtcp_packet_fb_set_type (&packet, GST_RTCP_RTPFB_TYPE_TWCC);
  if (twcc->recv_sender_ssrc != 1)
    gst_rtcp_packet_fb_set_sender_ssrc (&packet, twcc->recv_sender_ssrc);
  gst_rtcp_packet_fb_set_media_ssrc (&packet, twcc->recv_media_ssrc);

  rtp_twcc_manager_add_fci (twcc, &packet);

  gst_rtcp_buffer_unmap (&rtcp);

  g_queue_push_tail (twcc->rtcp_buffers, buf);
}

/* we have calculated a (very pessimistic) max-packets per RTCP feedback,
   so this is to make sure we don't exceed that */
static gboolean
_exceeds_max_packets (RTPTWCCManager * twcc, guint16 seqnum)
{
  if (twcc->recv_packets->len + 1 > twcc->max_packets_per_rtcp)
    return TRUE;

  return FALSE;
}

/* in this case we could have lost the packet with the marker bit,
   so with a large (30) amount of packets, lost packets and still no marker,
   we send a feedback anyway */
static gboolean
_many_packets_some_lost (RTPTWCCManager * twcc, guint16 seqnum)
{
  RecvPacket *first;
  guint16 packet_count;
  guint received_packets = twcc->recv_packets->len;
  guint lost_packets;
  if (received_packets == 0)
    return FALSE;

  first = &g_array_index (twcc->recv_packets, RecvPacket, 0);
  packet_count = seqnum - first->seqnum + 1;

  /* check if we lost half of the threshold */
  lost_packets = packet_count - received_packets;
  if (received_packets >= 30 && lost_packets >= 60)
    return TRUE;

  /* we have lost the marker bit for some and lost some */
  if (twcc->packet_count_no_marker >= 10 && lost_packets >= 60)
    return TRUE;

  return FALSE;
}

gboolean
rtp_twcc_manager_recv_packet (RTPTWCCManager * twcc, RTPPacketInfo * pinfo)
{
  gboolean send_feedback = FALSE;
  RecvPacket packet;
  gint32 seqnum;
  gint diff;

  seqnum = rtp_twcc_manager_get_recv_twcc_seqnum (twcc, pinfo);
  if (seqnum == -1)
    return FALSE;

  /* if this packet would exceed the capacity of our MTU, we create a feedback
     with the current packets, and start over with this one */
  if (_exceeds_max_packets (twcc, seqnum)) {
    GST_INFO ("twcc-seqnum: %u would overflow max packets: %u, create feedback"
        " with current packets", seqnum, twcc->max_packets_per_rtcp);
    rtp_twcc_manager_create_feedback (twcc);
    send_feedback = TRUE;
  }

  /* we can have multiple ssrcs here, so just pick the first one */
  if (twcc->recv_media_ssrc == -1)
    twcc->recv_media_ssrc = pinfo->ssrc;

  /* check if we are reordered, and treat it as lost if we already sent
     a feedback msg with a higher seqnum. If the diff is huge, treat
     it as a restart of a stream */
  diff = gst_rtp_buffer_compare_seqnum (twcc->expected_recv_seqnum, seqnum);
  if (twcc->fb_pkt_count > 0 && diff < 0) {
    GST_INFO ("Received out of order packet (%u after %u), treating as lost",
        seqnum, twcc->expected_recv_seqnum);
    return FALSE;
  }

  if (twcc->recv_packets->len > 0) {
    RecvPacket *last = &g_array_index (twcc->recv_packets, RecvPacket,
        twcc->recv_packets->len - 1);

    diff = gst_rtp_buffer_compare_seqnum (last->seqnum, seqnum);
    if (diff == 0) {
      GST_INFO ("Received duplicate packet (%u), dropping", seqnum);
      return FALSE;
    }
  }

  /* store the packet for Transport-wide RTCP feedback message */
  recv_packet_init (&packet, seqnum, pinfo);
  g_array_append_val (twcc->recv_packets, packet);
  twcc->last_seqnum = seqnum;

  GST_LOG ("Receive: twcc-seqnum: %u, pt: %u, marker: %d, ts: %"
      GST_TIME_FORMAT, seqnum, pinfo->pt, pinfo->marker,
      GST_TIME_ARGS (pinfo->arrival_time));

  if (!pinfo->marker)
    twcc->packet_count_no_marker++;

  /* Create feedback, if sending based on marker bit */
  if (!GST_CLOCK_TIME_IS_VALID (twcc->feedback_interval) &&
      (pinfo->marker || _many_packets_some_lost (twcc, seqnum))) {
    rtp_twcc_manager_create_feedback (twcc);
    send_feedback = TRUE;

    twcc->packet_count_no_marker = 0;
  }

  return send_feedback;
}

static void
_change_rtcp_fb_sender_ssrc (GstBuffer * buf, guint32 sender_ssrc)
{
  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  GstRTCPPacket packet;
  gst_rtcp_buffer_map (buf, GST_MAP_READWRITE, &rtcp);
  gst_rtcp_buffer_get_first_packet (&rtcp, &packet);
  gst_rtcp_packet_fb_set_sender_ssrc (&packet, sender_ssrc);
  gst_rtcp_buffer_unmap (&rtcp);
}

GstBuffer *
rtp_twcc_manager_get_feedback (RTPTWCCManager * twcc, guint sender_ssrc,
    GstClockTime current_time)
{
  GstBuffer *buf;

  GST_DEBUG ("considering twcc. now: %" GST_TIME_FORMAT
      " twcc-time: %" GST_TIME_FORMAT " packets: %d",
      GST_TIME_ARGS (current_time),
      GST_TIME_ARGS (twcc->next_feedback_send_time), twcc->recv_packets->len);

  if (GST_CLOCK_TIME_IS_VALID (twcc->feedback_interval) &&
      GST_CLOCK_TIME_IS_VALID (twcc->next_feedback_send_time) &&
      twcc->next_feedback_send_time <= current_time) {
    /* Sending on a fixed interval: compute next send time */
    while (twcc->next_feedback_send_time <= current_time)
      twcc->next_feedback_send_time += twcc->feedback_interval;

    /* Generate feedback, if there is some to send */
    if (twcc->recv_packets->len > 0)
      rtp_twcc_manager_create_feedback (twcc);
  }

  buf = g_queue_pop_head (twcc->rtcp_buffers);

  if (buf && twcc->recv_sender_ssrc != sender_ssrc) {
    _change_rtcp_fb_sender_ssrc (buf, sender_ssrc);
    twcc->recv_sender_ssrc = sender_ssrc;
  }

  return buf;
}

static gint32
rtp_twcc_manager_lookup_seqnum (RTPTWCCManager * twcc,
    guint32 ssrc, guint16 seqnum)
{
  gint32 ret = -1;

  GHashTable *seq_to_twcc =
      g_hash_table_lookup (twcc->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
  if (seq_to_twcc) {
    ret =
        GPOINTER_TO_UINT (g_hash_table_lookup (seq_to_twcc,
            GUINT_TO_POINTER (seqnum)));
  }
  return ret;
}

void
rtp_twcc_manager_send_packet (RTPTWCCManager * twcc, RTPPacketInfo * pinfo)
{
  if (twcc->send_ext_id == 0)
    return;

  rtp_twcc_manager_set_send_twcc_seqnum (twcc, pinfo);
}

static void
rtp_twcc_manager_tx_feedback (GstTxFeedback * parent, guint64 buffer_id,
    GstClockTime ts)
{
  RTPTWCCManager *twcc = RTP_TWCC_MANAGER_CAST (parent);
  guint16 seqnum = (guint16) buffer_id;
  SentPacket *first = NULL;
  SentPacket *pkt = NULL;
  guint idx;

  first = &g_array_index (twcc->sent_packets, SentPacket, 0);
  if (first == NULL) {
    GST_WARNING ("Received a tx-feedback without having sent any packets?!?");
    return;
  }

  idx = seqnum - first->seqnum;

  if (idx < twcc->sent_packets->len) {
    pkt = &g_array_index (twcc->sent_packets, SentPacket, idx);
    if (pkt && pkt->seqnum == seqnum) {
      pkt->socket_ts = ts;
      GST_LOG ("packet #%u, setting socket-ts %" GST_TIME_FORMAT,
          seqnum, GST_TIME_ARGS (ts));
    }
  } else {
    GST_WARNING ("Unable to update send-time for twcc-seqnum #%u", seqnum);
  }
}

static void
_add_parsed_packet (GArray * parsed_packets, guint16 seqnum, guint status)
{
  ParsedPacket packet;
  packet.seqnum = seqnum;
  packet.status = status;
  packet.remote_ts = GST_CLOCK_TIME_NONE;
  g_array_append_val (parsed_packets, packet);
}

static guint
_parse_run_length_chunk (GstBitReader * reader, GArray * parsed_packets,
    guint16 seqnum_offset, guint remaining_packets)
{
  guint16 run_length;
  guint8 status_code;
  guint i;

  gst_bit_reader_get_bits_uint8 (reader, &status_code, 2);
  gst_bit_reader_get_bits_uint16 (reader, &run_length, 13);

  run_length = MIN (remaining_packets, run_length);

  for (i = 0; i < run_length; i++) {
    _add_parsed_packet (parsed_packets, seqnum_offset + i, status_code);
  }

  return run_length;
}

static guint
_parse_status_vector_chunk (GstBitReader * reader, GArray * parsed_packets,
    guint16 seqnum_offset, guint remaining_packets)
{
  guint8 symbol_size;
  guint num_bits;
  guint i;

  gst_bit_reader_get_bits_uint8 (reader, &symbol_size, 1);
  symbol_size += 1;
  num_bits = MIN (remaining_packets, 14 / symbol_size);

  for (i = 0; i < num_bits; i++) {
    guint8 status_code;
    if (gst_bit_reader_get_bits_uint8 (reader, &status_code, symbol_size))
      _add_parsed_packet (parsed_packets, seqnum_offset + i, status_code);
  }

  return num_bits;
}

/* Remove all locally stored packets that has been reported
   back to us */
static void
_prune_sent_packets (RTPTWCCManager * twcc, GArray * parsed_packets)
{
  SentPacket *first;
  ParsedPacket *last;
  guint16 last_idx;

  if (parsed_packets->len == 0 || twcc->sent_packets->len == 0)
    return;

  first = &g_array_index (twcc->sent_packets, SentPacket, 0);
  last = &g_array_index (parsed_packets, ParsedPacket, parsed_packets->len - 1);

  last_idx = last->seqnum - first->seqnum;

  if (last_idx < twcc->sent_packets->len)
    g_array_remove_range (twcc->sent_packets, 0, last_idx);
}

static void
_check_for_lost_packets (RTPTWCCManager * twcc, GArray * parsed_packets,
    guint16 base_seqnum, guint16 packet_count, guint8 fb_pkt_count)
{
  guint packets_lost;
  gint8 fb_pkt_count_diff;
  guint i;

  /* first packet */
  if (twcc->first_fci_parse) {
    twcc->first_fci_parse = FALSE;
    goto done;
  }

  fb_pkt_count_diff =
      (gint8) (fb_pkt_count - twcc->expected_parsed_fb_pkt_count);

  /* we have gone backwards, don't reset the expectations,
     but process the packet nonetheless */
  if (fb_pkt_count_diff < 0) {
    GST_DEBUG ("feedback packet count going backwards (%u < %u)",
        fb_pkt_count, twcc->expected_parsed_fb_pkt_count);
    return;
  }

  /* we have jumped forwards, reset expectations, but don't trigger
     lost packets in case the missing fb-packet(s) arrive later */
  if (fb_pkt_count_diff > 0) {
    GST_DEBUG ("feedback packet count jumped ahead (%u > %u)",
        fb_pkt_count, twcc->expected_parsed_fb_pkt_count);
    goto done;
  }

  if (base_seqnum < twcc->expected_parsed_seqnum) {
    GST_DEBUG ("twcc seqnum is older than expected  (%u < %u)", base_seqnum,
        twcc->expected_parsed_seqnum);
    return;
  }

  packets_lost = base_seqnum - twcc->expected_parsed_seqnum;
  for (i = 0; i < packets_lost; i++) {
    _add_parsed_packet (parsed_packets, twcc->expected_parsed_seqnum + i,
        RTP_TWCC_PACKET_STATUS_NOT_RECV);
  }

done:
  twcc->expected_parsed_seqnum = base_seqnum + packet_count;
  twcc->expected_parsed_fb_pkt_count = fb_pkt_count + 1;
  return;
}

static void
_add_found_packet_to_value_array (GValueArray * array, SentPacket * pkt)
{
  GstClockTime local_ts = pkt->local_ts;

  /* if we have a socket-timestamp, use that instead */
  if (GST_CLOCK_TIME_IS_VALID (pkt->socket_ts)) {
    local_ts = pkt->socket_ts;
  }

  _append_structure_to_value_array (array,
      gst_structure_new ("RTPTWCCPacket",
          "seqnum", G_TYPE_UINT, pkt->seqnum,
          "local-ts", G_TYPE_UINT64, local_ts,
          "remote-ts", G_TYPE_UINT64, pkt->remote_ts,
          "payload-type", G_TYPE_UCHAR, pkt->pt,
          "size", G_TYPE_UINT, pkt->size,
          "lost", G_TYPE_BOOLEAN, !GST_CLOCK_TIME_IS_VALID (pkt->remote_ts),
          NULL));
}

GstStructure *
rtp_twcc_manager_parse_fci (RTPTWCCManager * twcc,
    guint8 * fci_data, guint fci_length, GstClockTime current_time)
{
  GstStructure *ret;
  GValueArray *array;

  guint16 base_seqnum;
  guint16 packet_count;
  GstClockTime base_time;
  GstClockTime ts_rounded;
  guint8 fb_pkt_count;
  guint packets_parsed = 0;
  guint fci_parsed;
  guint i;
  SentPacket *first_sent_pkt = NULL;
  GstClockTimeDiff rtt = GST_CLOCK_STIME_NONE;

  if (fci_length < 10) {
    GST_WARNING ("Malformed TWCC RTCP feedback packet");
    return NULL;
  }

  ret = gst_structure_new_empty ("RTPTWCCPackets");
  array = g_value_array_new (0);

  base_seqnum = GST_READ_UINT16_BE (&fci_data[0]);
  packet_count = GST_READ_UINT16_BE (&fci_data[2]);
  base_time = GST_READ_UINT24_BE (&fci_data[4]) * REF_TIME_UNIT;
  fb_pkt_count = fci_data[7];

  GST_DEBUG ("Parsed TWCC feedback: base_seqnum: #%u, packet_count: %u, "
      "base_time %" GST_TIME_FORMAT " fb_pkt_count: %u",
      base_seqnum, packet_count, GST_TIME_ARGS (base_time), fb_pkt_count);

  g_array_set_size (twcc->parsed_packets, 0);

  _check_for_lost_packets (twcc, twcc->parsed_packets,
      base_seqnum, packet_count, fb_pkt_count);

  fci_parsed = 8;
  while (packets_parsed < packet_count && (fci_parsed + 1) < fci_length) {
    GstBitReader reader = GST_BIT_READER_INIT (&fci_data[fci_parsed], 2);
    guint8 chunk_type;
    guint seqnum_offset = base_seqnum + packets_parsed;
    guint remaining_packets = packet_count - packets_parsed;

    gst_bit_reader_get_bits_uint8 (&reader, &chunk_type, 1);

    if (chunk_type == RTP_TWCC_CHUNK_TYPE_RUN_LENGTH) {
      packets_parsed += _parse_run_length_chunk (&reader,
          twcc->parsed_packets, seqnum_offset, remaining_packets);
    } else {
      packets_parsed += _parse_status_vector_chunk (&reader,
          twcc->parsed_packets, seqnum_offset, remaining_packets);
    }
    fci_parsed += 2;
  }

  if (twcc->sent_packets->len > 0)
    first_sent_pkt = &g_array_index (twcc->sent_packets, SentPacket, 0);

  ts_rounded = base_time;
  for (i = 0; i < twcc->parsed_packets->len; i++) {
    ParsedPacket *pkt = &g_array_index (twcc->parsed_packets, ParsedPacket, i);
    gint16 delta = 0;
    GstClockTimeDiff delta_ts;

    if (pkt->status == RTP_TWCC_PACKET_STATUS_SMALL_DELTA) {
      delta = fci_data[fci_parsed];
      fci_parsed += 1;
    } else if (pkt->status == RTP_TWCC_PACKET_STATUS_LARGE_NEGATIVE_DELTA) {
      delta = GST_READ_UINT16_BE (&fci_data[fci_parsed]);
      fci_parsed += 2;
    }

    if (fci_parsed > fci_length) {
      GST_WARNING ("Malformed TWCC RTCP feedback packet");
      g_array_set_size (twcc->parsed_packets, 0);
      break;
    }

    if (pkt->status != RTP_TWCC_PACKET_STATUS_NOT_RECV) {
      delta_ts = delta * DELTA_UNIT;
      ts_rounded += delta_ts;
      pkt->remote_ts = ts_rounded;

      GST_LOG ("pkt: #%u, remote_ts: %" GST_TIME_FORMAT
          " delta_ts: %" GST_STIME_FORMAT
          " status: %u", pkt->seqnum,
          GST_TIME_ARGS (pkt->remote_ts), GST_STIME_ARGS (delta_ts),
          pkt->status);
    }

    if (first_sent_pkt) {
      SentPacket *found = NULL;
      guint16 sent_idx = pkt->seqnum - first_sent_pkt->seqnum;
      if (sent_idx < twcc->sent_packets->len)
        found = &g_array_index (twcc->sent_packets, SentPacket, sent_idx);
      if (found && found->seqnum == pkt->seqnum) {
        found->remote_ts = pkt->remote_ts;

        GST_LOG ("Adding packet %u to stats", found->seqnum);
        _add_packet_to_stats (twcc, found);

        _add_found_packet_to_value_array (array, found);

        if (pkt->status != RTP_TWCC_PACKET_STATUS_NOT_RECV
            && found->rtx_osn != -1) {
          gint32 recovered_seq =
              rtp_twcc_manager_lookup_seqnum (twcc, found->rtx_ssrc,
              found->rtx_osn);
          if (recovered_seq != -1) {
            GST_LOG ("RTX Packet %u protects seqnum %d", found->seqnum,
                recovered_seq);
            _update_stats_with_recovered (twcc, recovered_seq);
          }
        }

        /* calculate the round-trip time */
        rtt = GST_CLOCK_DIFF (found->local_ts, current_time);

        GST_LOG ("matching pkt: #%u with local_ts: %" GST_TIME_FORMAT
            " size: %u", pkt->seqnum, GST_TIME_ARGS (found->local_ts),
            found->size);
      }
    }
  }

  if (GST_CLOCK_STIME_IS_VALID (rtt))
    twcc->avg_rtt = WEIGHT (rtt, twcc->avg_rtt, 0.1);
  twcc->last_report_time = current_time;

  _prune_sent_packets (twcc, twcc->parsed_packets);

  _structure_take_value_array (ret, "packets", array);

  return ret;
}

GstStructure *
rtp_twcc_manager_get_windowed_stats (RTPTWCCManager * twcc)
{
  GstStructure *ret;
  GValueArray *array = g_value_array_new (0);
  GstClockTimeDiff recovery_deadline;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_assert (GST_CLOCK_TIME_IS_VALID (twcc->last_report_time));
  recovery_deadline =
      twcc->last_report_time - twcc->avg_rtt - 100 * GST_MSECOND;

  twcc_stats_ctx_calculate_windowed_stats (twcc->stats_ctx, recovery_deadline);
  ret = twcc_stats_ctx_get_structure (twcc->stats_ctx);

  g_hash_table_iter_init (&iter, twcc->stats_ctx_by_pt);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStructure *s;
    guint pt = GPOINTER_TO_UINT (key);
    TWCCStatsCtx *ctx = value;

    twcc_stats_ctx_calculate_windowed_stats (ctx, recovery_deadline);
    s = twcc_stats_ctx_get_structure (ctx);
    gst_structure_set (s, "pt", G_TYPE_UINT, pt, NULL);

    _append_structure_to_value_array (array, s);
  }

  _structure_take_value_array (ret, "payload-stats", array);

  return ret;
}
