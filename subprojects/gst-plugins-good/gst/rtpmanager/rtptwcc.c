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

GST_DEBUG_CATEGORY (rtp_twcc_debug);
#define GST_CAT_DEFAULT rtp_twcc_debug

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

  GstClockTimeDiff local_delta;
  GstClockTimeDiff remote_delta;
  GstClockTimeDiff delta_delta;
  gboolean recovered;
} StatsPacket;

static void
stats_packet_init_from_sent_packet (StatsPacket * pkt, SentPacket * sent)
{
  pkt->org_ts = sent->local_ts;
  pkt->local_ts = sent->local_ts;
  pkt->remote_ts = sent->remote_ts;
  pkt->seqnum = sent->seqnum;
  pkt->size = sent->size;
  pkt->pt = sent->pt;
  pkt->recovered = FALSE;
  pkt->local_delta = GST_CLOCK_STIME_NONE;
  pkt->remote_delta = GST_CLOCK_STIME_NONE;
  pkt->delta_delta = GST_CLOCK_STIME_NONE;

  /* if we have a socket-timestamp, use that instead */
  if (GST_CLOCK_TIME_IS_VALID (sent->socket_ts)) {
    pkt->local_ts = sent->socket_ts;
  }
}

typedef struct
{
  GArray *packets;
  gint64 new_packets_idx;

  /* windowed stats */
  guint packets_sent;
  guint packets_recv;
  guint bitrate_sent;
  guint bitrate_recv;
  gdouble packet_loss_pct;
  gdouble recovery_pct;
  gint64 avg_delta_of_delta;
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
twcc_stats_ctx_new (void)
{
  TWCCStatsCtx *ctx = g_new0 (TWCCStatsCtx, 1);

  ctx->packets = g_array_new (FALSE, FALSE, sizeof (StatsPacket));

  return ctx;
}

static void
twcc_stats_ctx_free (TWCCStatsCtx * ctx)
{
  g_array_unref (ctx->packets);
  g_free (ctx);
}

static GstClockTime
twcc_stats_ctx_get_last_local_ts (TWCCStatsCtx * ctx)
{
  GstClockTime ret = GST_CLOCK_TIME_NONE;
  StatsPacket *pkt = NULL;
  if (ctx->packets->len > 0)
    pkt = &g_array_index (ctx->packets, StatsPacket, ctx->packets->len - 1);
  if (pkt)
    ret = pkt->local_ts;
  return ret;
}

static gboolean
_get_stats_packets_window (GArray * array,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time, guint * packets)
{
  gboolean ret = FALSE;
  guint start_idx = 0;
  guint end_idx = 0;
  guint i;

  if (array->len < 2) {
    GST_DEBUG ("Not enough starts to do a window");
    return FALSE;
  }

  for (i = 0; i < array->len; i++) {
    StatsPacket *pkt = &g_array_index (array, StatsPacket, i);
    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts)) {
      GstClockTimeDiff offset = GST_CLOCK_DIFF (pkt->local_ts, start_time);
      start_idx = i;
      /* positive number here means it is older than our start time */
      if (offset > 0) {
        GST_LOG ("Packet #%u is too old: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt->local_ts));
      } else {
        break;
      }
    }
  }

  /* if we have found a start_idx, and it is not the first packet, trim
     the start of the stats array to the start_idx */
  if (start_idx > 0) {
    g_array_remove_range (array, 0, start_idx);
  }

  for (i = 0; i < array->len; i++) {
    guint idx = array->len - 1 - i;
    StatsPacket *pkt = &g_array_index (array, StatsPacket, idx);
    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts)) {
      GstClockTimeDiff offset = GST_CLOCK_DIFF (pkt->local_ts, end_time);
      if (offset >= 0) {
        GST_DEBUG ("Setting last packet in our window to #%u: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt->local_ts));
        end_idx = idx;
        ret = TRUE;
        break;
      }
    }
  }

  /* jump out early if we could not find a window */
  if (!ret) {
    return FALSE;
  }

  /* the amount of packets */
  *packets = end_idx + 1;

  return ret;
}

static gboolean
twcc_stats_ctx_calculate_windowed_stats (TWCCStatsCtx * ctx,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time)
{
  GArray *packets = ctx->packets;
  guint packets_sent = 0;
  guint packets_recv = 0;
  guint packets_recovered = 0;
  guint packets_lost = 0;

  guint i;
  guint bits_sent = 0;
  guint bits_recv = 0;
  GstClockTimeDiff delta_delta_sum = 0;
  guint delta_delta_count = 0;

  StatsPacket *first_local_pkt = NULL;
  StatsPacket *last_local_pkt = NULL;
  StatsPacket *first_remote_pkt = NULL;
  StatsPacket *last_remote_pkt = NULL;

  GstClockTimeDiff local_duration = 0;
  GstClockTimeDiff remote_duration = 0;

  ctx->packet_loss_pct = 0.0;
  ctx->avg_delta_of_delta = 0;
  ctx->bitrate_sent = 0;
  ctx->bitrate_recv = 0;
  ctx->recovery_pct = -1.0;

  gboolean ret =
      _get_stats_packets_window (packets, start_time, end_time, &packets_sent);
  if (!ret || packets_sent < 2) {
    GST_INFO ("Not enough packets to fill our window yet!");
    return FALSE;
  }

  for (i = 0; i < packets_sent; i++) {
    StatsPacket *pkt = &g_array_index (packets, StatsPacket, i);
    GST_LOG ("%u: pkt #%u, pt: %u, size: %u, arrived: %s",
        i, pkt->seqnum, pkt->pt, pkt->size * 8,
        GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) ? "YES :)" : "NO :(");

    if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts)) {
      /* don't count the bits for the first packet in the window */
      if (first_local_pkt == NULL) {
        first_local_pkt = pkt;
      } else {
        bits_sent += pkt->size * 8;
      }
      last_local_pkt = pkt;
    }

    if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts)) {
      /* don't count the bits for the first packet in the window */
      if (first_remote_pkt == NULL) {
        first_remote_pkt = pkt;
      } else {
        bits_recv += pkt->size * 8;
      }
      last_remote_pkt = pkt;
      packets_recv++;
    } else {
      GST_LOG ("Packet #%u is lost, recovered=%u", pkt->seqnum, pkt->recovered);
      packets_lost++;
      if (pkt->recovered) {
        packets_recovered++;
      }
    }

    if (GST_CLOCK_STIME_IS_VALID (pkt->delta_delta)) {
      delta_delta_sum += pkt->delta_delta;
      delta_delta_count++;
    }
  }

  ctx->packets_sent = packets_sent;
  ctx->packets_recv = packets_recv;

  if (first_local_pkt && last_local_pkt) {
    local_duration =
        GST_CLOCK_DIFF (first_local_pkt->local_ts, last_local_pkt->local_ts);
  }
  if (first_remote_pkt && last_remote_pkt) {
    remote_duration =
        GST_CLOCK_DIFF (first_remote_pkt->remote_ts,
        last_remote_pkt->remote_ts);
  }

  if (packets_sent)
    ctx->packet_loss_pct = (packets_lost * 100) / (gfloat) packets_sent;

  if (packets_lost) {
    ctx->recovery_pct = (packets_recovered * 100) / (gfloat) packets_lost;
  }

  if (delta_delta_count) {
    ctx->avg_delta_of_delta = delta_delta_sum / delta_delta_count;
  }

  if (local_duration > 0) {
    ctx->bitrate_sent =
        gst_util_uint64_scale (bits_sent, GST_SECOND, local_duration);
  }
  if (remote_duration > 0) {
    ctx->bitrate_recv =
        gst_util_uint64_scale (bits_recv, GST_SECOND, remote_duration);
  }

  GST_DEBUG ("Got stats: bits_sent: %u, bits_recv: %u, packets_sent = %u, "
      "packets_recv: %u, packetlost_pct = %f, recovery_pct = %f"
      "sent_bitrate = %u, " "recv_bitrate = %u, delta-delta-avg = %"
      GST_STIME_FORMAT, bits_sent, bits_recv, packets_sent,
      packets_recv, ctx->packet_loss_pct, ctx->recovery_pct,
      ctx->bitrate_sent, ctx->bitrate_recv,
      GST_STIME_ARGS (ctx->avg_delta_of_delta));

  return TRUE;
}

static GstStructure *
twcc_stats_ctx_get_structure (TWCCStatsCtx * ctx)
{
  return gst_structure_new ("RTPTWCCStats",
      "packets-sent", G_TYPE_UINT, ctx->packets_sent,
      "packets-recv", G_TYPE_UINT, ctx->packets_recv,
      "bitrate-sent", G_TYPE_UINT, ctx->bitrate_sent,
      "bitrate-recv", G_TYPE_UINT, ctx->bitrate_recv,
      "packet-loss-pct", G_TYPE_DOUBLE, ctx->packet_loss_pct,
      "recovery-pct", G_TYPE_DOUBLE, ctx->recovery_pct,
      "avg-delta-of-delta", G_TYPE_INT64, ctx->avg_delta_of_delta, NULL);
}


static void
_update_stats_for_packet_idx (GArray * array, gint packet_idx)
{
  StatsPacket *pkt;
  StatsPacket *prev;

  /* we need at least 2 packets to do any stats */
  if (array->len < 2) {
    GST_DEBUG ("Not yet 2 packets in stats");
    return;
  }

  /* we can't do stats on the first packet */
  if (packet_idx == 0) {
    GST_DEBUG ("First packet can't have stats calculated");
    return;
  }

  /* packet_idx must be inside the array */
  if (array->len <= packet_idx) {
    GST_DEBUG ("Packet idx %u is outside of array!", packet_idx);
    return;
  }

  pkt = &g_array_index (array, StatsPacket, packet_idx);
  prev = &g_array_index (array, StatsPacket, packet_idx - 1);

  if (GST_CLOCK_TIME_IS_VALID (pkt->local_ts) &&
      GST_CLOCK_TIME_IS_VALID (prev->local_ts)) {
    pkt->local_delta = GST_CLOCK_DIFF (prev->local_ts, pkt->local_ts);
    GST_LOG ("Calculated local_delta for packet #%u to %" GST_STIME_FORMAT,
        pkt->seqnum, GST_STIME_ARGS (pkt->local_delta));
  }

  if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) &&
      GST_CLOCK_TIME_IS_VALID (prev->remote_ts)) {
    pkt->remote_delta = GST_CLOCK_DIFF (prev->remote_ts, pkt->remote_ts);
    GST_LOG ("Calculated remote_delta for packet #%u to %" GST_STIME_FORMAT,
        pkt->seqnum, GST_STIME_ARGS (pkt->remote_delta));
  }

  if (GST_CLOCK_STIME_IS_VALID (pkt->local_delta) &&
      GST_CLOCK_STIME_IS_VALID (pkt->remote_delta)) {
    pkt->delta_delta = pkt->remote_delta - pkt->local_delta;
    GST_LOG ("Calculated delta-of-delta for packet #%u to %" GST_STIME_FORMAT,
        pkt->seqnum, GST_STIME_ARGS (pkt->delta_delta));
  }
}

static gint
_get_packet_idx_for_seqnum (GArray * array, guint16 seqnum)
{
  guint i;

  for (i = 0; i < array->len; i++) {
    StatsPacket *pkt = &g_array_index (array, StatsPacket, i);
    if (pkt->seqnum == seqnum) {
      return (gint) i;
    }
  }

  return -1;
}

/* assumes all seqnum are in order */
static gint
_get_packet_idx_for_seqnum_fast (GArray * array, guint16 seqnum)
{
  StatsPacket *first;
  guint16 idx;

  if (array->len == 0)
    return -1;

  first = &g_array_index (array, StatsPacket, 0);
  idx = gst_rtp_buffer_compare_seqnum (first->seqnum, seqnum);

  if (idx < array->len) {
    StatsPacket *found = &g_array_index (array, StatsPacket, idx);
    if (found->seqnum == seqnum) {
      return (gint) idx;
    }
  }

  return -1;
}

static gint
twcc_stats_ctx_get_packet_idx_for_seqnum (TWCCStatsCtx * ctx, guint16 seqnum)
{
  gint ret;

  /* assumes all packets seqnum are in order */
  ret = _get_packet_idx_for_seqnum_fast (ctx->packets, seqnum);
  if (ret != -1)
    return ret;

  return _get_packet_idx_for_seqnum (ctx->packets, seqnum);
}

static StatsPacket *
twcc_stats_ctx_get_packet_for_seqnum (TWCCStatsCtx * ctx, guint16 seqnum)
{
  StatsPacket *ret = NULL;
  gint idx = twcc_stats_ctx_get_packet_idx_for_seqnum (ctx, seqnum);
  if (idx != -1)
    ret = &g_array_index (ctx->packets, StatsPacket, idx);

  return ret;
}

static void
twcc_stats_ctx_add_packet (TWCCStatsCtx * ctx, StatsPacket * pkt)
{
  StatsPacket *last = NULL;
  gint pkt_idx;

  if (ctx->packets->len > 0)
    last = &g_array_index (ctx->packets, StatsPacket, ctx->packets->len - 1);

  /* first a quick check to see if we are going forward in seqnum,
     in which case we simply append */
  if (last == NULL || pkt->seqnum > last->seqnum) {
    GST_DEBUG ("Appending #%u to stats packets", pkt->seqnum);
    g_array_append_val (ctx->packets, *pkt);
    /* and update the stats for this packet */
    _update_stats_for_packet_idx (ctx->packets, ctx->packets->len - 1);
    return;
  }

  /* here we are dealing with a reordered packet, we start by searching for it */
  pkt_idx = twcc_stats_ctx_get_packet_idx_for_seqnum (ctx, pkt->seqnum);
  if (pkt_idx != -1) {
    StatsPacket *existing = &g_array_index (ctx->packets, StatsPacket, pkt_idx);
    /* only update if we don't already have information about this packet, and
       this packet brings something new */
    if (!GST_CLOCK_TIME_IS_VALID (existing->remote_ts) &&
        GST_CLOCK_TIME_IS_VALID (pkt->remote_ts)) {
      GST_DEBUG ("Updating stats packet #%u", pkt->seqnum);
      g_array_index (ctx->packets, StatsPacket, pkt_idx) = *pkt;

      /* now update stats for this packet and the next one along */
      _update_stats_for_packet_idx (ctx->packets, pkt_idx);
      _update_stats_for_packet_idx (ctx->packets, pkt_idx + 1);
    }
    return;
  }
}

/******************************************************/

struct _RTPTWCCManager
{
  GObject object;

  GHashTable *ssrc_to_seqmap;
  GHashTable *pt_to_twcc_ext_id;

  GstClockTime stats_window_size;
  GstClockTime stats_window_delay;
  TWCCStatsCtx *stats_ctx;
  GHashTable *stats_ctx_by_pt;

  guint8 send_ext_id;
  guint8 recv_ext_id;
  guint16 send_seqnum;

  guint mtu;
  guint max_packets_per_rtcp;
  GArray *recv_packets;

  guint64 fb_pkt_count;

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

  RTPTWCCManagerCaps caps_cb;
  gpointer caps_ud;
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
  twcc->pt_to_twcc_ext_id = g_hash_table_new (NULL, NULL);

  twcc->recv_packets = g_array_new (FALSE, FALSE, sizeof (RecvPacket));
  twcc->sent_packets = g_array_new (FALSE, FALSE, sizeof (SentPacket));
  twcc->parsed_packets = g_array_new (FALSE, FALSE, sizeof (ParsedPacket));

  twcc->rtcp_buffers = g_queue_new ();

  twcc->stats_ctx = twcc_stats_ctx_new ();
  twcc->stats_ctx_by_pt = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) twcc_stats_ctx_free);

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
  g_hash_table_destroy (twcc->pt_to_twcc_ext_id);

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

  GST_DEBUG_CATEGORY_INIT (rtp_twcc_debug, "rtptwcc", 0, "RTP TWCC Manager");
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
    ctx = twcc_stats_ctx_new ();
    g_hash_table_insert (twcc->stats_ctx_by_pt, GUINT_TO_POINTER (pt), ctx);
  }
  return ctx;
}


static void
_update_stats_with_recovered (RTPTWCCManager * twcc, guint16 seqnum)
{
  TWCCStatsCtx *ctx;
  StatsPacket *pkt =
      twcc_stats_ctx_get_packet_for_seqnum (twcc->stats_ctx, seqnum);

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

static gint32
_lookup_seqnum (RTPTWCCManager * twcc, guint32 ssrc, guint16 seqnum)
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

static void
_add_packet_to_stats (RTPTWCCManager * twcc,
    ParsedPacket * parsed_pkt, SentPacket * sent_pkt)
{
  TWCCStatsCtx *ctx;
  StatsPacket pkt;

  stats_packet_init_from_sent_packet (&pkt, sent_pkt);

  /* add packet to the stats context */
  twcc_stats_ctx_add_packet (twcc->stats_ctx, &pkt);

  /* add packet to the payload specific stats context */
  ctx = _get_ctx_for_pt (twcc, pkt.pt);
  twcc_stats_ctx_add_packet (ctx, &pkt);

  /* extra check to see if we can confirm the arrival of a recovery packet,
     and hence set the "original" packet as recovered */
  if (parsed_pkt->status != RTP_TWCC_PACKET_STATUS_NOT_RECV
      && sent_pkt->rtx_osn != -1) {
    gint32 recovered_seq =
        _lookup_seqnum (twcc, sent_pkt->rtx_ssrc, sent_pkt->rtx_osn);
    if (recovered_seq != -1) {
      GST_LOG ("RTX Packet %u protects seqnum %d", sent_pkt->seqnum,
          recovered_seq);
      _update_stats_with_recovered (twcc, recovered_seq);
    }
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

/*
* Returns a pointer to the twcc extension header data for the given id, or adds
* it, if it is not present.
*
* Note: we want to override the extension value if it is present.
*/
static gpointer
_get_twcc_buffer_ext_data (GstRTPBuffer * rtpbuf, guint8 ext_id)
{
  gpointer data = NULL;
  guint16 tmp = 0;
  gboolean added;

  if (gst_rtp_buffer_get_extension_onebyte_header (rtpbuf, ext_id, 0, &data,
          NULL)) {
    return data;
  }

  added = gst_rtp_buffer_add_extension_onebyte_header (rtpbuf, ext_id, &tmp,
      sizeof (tmp));
  if (added) {
    gst_rtp_buffer_get_extension_onebyte_header (rtpbuf, ext_id, 0, &data,
        NULL);
  }

  return data;
}

static void
_set_twcc_seqnum_data (RTPTWCCManager * twcc, RTPPacketInfo * pinfo,
    GstBuffer * buf, guint8 ext_id)
{
  SentPacket packet;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gpointer data;
  guint16 seqnum;

  if (!gst_rtp_buffer_map (buf, GST_MAP_READWRITE, &rtp)) {
    GST_WARNING ("Couldn't map the buffer %" GST_PTR_FORMAT, buf);
    return;
  }

  data = _get_twcc_buffer_ext_data (&rtp, ext_id);
  if (!data) {
    gst_rtp_buffer_unmap (&rtp);
    return;
  }

  seqnum = twcc->send_seqnum++;
  GST_WRITE_UINT16_BE (data, seqnum);
  sent_packet_init (&packet, seqnum, pinfo, &rtp);
  gst_rtp_buffer_unmap (&rtp);
  g_array_append_val (twcc->sent_packets, packet);
  _prune_old_sent_packets (twcc);
  rtp_twcc_manager_register_seqnum (twcc, pinfo->ssrc, pinfo->seqnum, seqnum);

  gst_buffer_add_tx_feedback_meta (pinfo->data, seqnum,
      GST_TX_FEEDBACK_CAST (twcc));

  GST_LOG ("Send: twcc-seqnum: %u, pt: %u, marker: %d, len: %u, ts: %"
      GST_TIME_FORMAT, seqnum, packet.pt, pinfo->marker, packet.size,
      GST_TIME_ARGS (pinfo->current_time));
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
        GST_LOG ("Missing run of %u, using run-length chunk", pkt->missing_run);
        rtp_twcc_write_run_length_chunk (packet_chunks,
            RTP_TWCC_PACKET_STATUS_NOT_RECV, pkt->missing_run);
        /* for this case we need to set the missing-run to 0, or else
           the status vector chunk will use it and write its own version */
        pkt->missing_run = 0;
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
  gboolean missing_packets = FALSE;

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

    if (pkt->missing_run > 0)
      missing_packets = TRUE;

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

  /* If we have any missing packets in this report, keep the last packet around,
     potentially reporting it several times.
     This is mimicing Chrome WebRTC behavior. */
  if (missing_packets) {
    twcc->recv_packets =
        g_array_remove_range (twcc->recv_packets, 0,
        twcc->recv_packets->len - 1);
  } else {
    g_array_set_size (twcc->recv_packets, 0);
  }
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

static gboolean
_find_seqnum_in_recv_packets (RTPTWCCManager * twcc, guint16 seqnum)
{
  for (guint i = 0; i < twcc->recv_packets->len; i++) {
    RecvPacket *pkt = &g_array_index (twcc->recv_packets, RecvPacket, i);
    if (pkt->seqnum == seqnum)
      return TRUE;
  }
  return FALSE;
}

gboolean
rtp_twcc_manager_recv_packet (RTPTWCCManager * twcc, RTPPacketInfo * pinfo)
{
  gboolean send_feedback = FALSE;
  RecvPacket packet;
  gint32 seqnum;
  gint diff;
  gboolean reordered_packet = FALSE;

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

  if (twcc->recv_packets->len > 0) {
    RecvPacket *last = &g_array_index (twcc->recv_packets, RecvPacket,
        twcc->recv_packets->len - 1);
    diff = gst_rtp_buffer_compare_seqnum (last->seqnum, seqnum);
    if (diff <= 0) {
      /* duplicate check */
      if (_find_seqnum_in_recv_packets (twcc, seqnum)) {
        GST_INFO ("Received duplicate packet (#%u), dropping", seqnum);
        return FALSE;
      }
      /* if not duplicate, it is reordered */
      GST_INFO ("Received a reordered packet (#%u)", seqnum);
      reordered_packet = TRUE;
    }
  }

  /* store the packet for Transport-wide RTCP feedback message */
  recv_packet_init (&packet, seqnum, pinfo);
  g_array_append_val (twcc->recv_packets, packet);

  /* if we received a reordered packet, we need to sort the list */
  if (reordered_packet)
    g_array_sort (twcc->recv_packets, _twcc_seqnum_sort);

  GST_LOG ("Receive: twcc-seqnum: #%u, pt: %u, marker: %d, ts: %"
      GST_TIME_FORMAT, seqnum, pinfo->pt, pinfo->marker,
      GST_TIME_ARGS (packet.ts));

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

  GST_LOG ("considering twcc. now: %" GST_TIME_FORMAT
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

static guint8
get_twcc_ext_id_for_pt (RTPTWCCManager * twcc, guint8 pt)
{
  guint8 twcc_ext_id;
  GstCaps *caps;
  gpointer value;

  value = g_hash_table_lookup (twcc->pt_to_twcc_ext_id, GUINT_TO_POINTER (pt));
  if (value)
    return GPOINTER_TO_UINT (value);

  if (!twcc->caps_cb)
    return 0;

  caps = twcc->caps_cb (pt, twcc->caps_ud);
  if (!caps)
    return 0;

  twcc_ext_id =
      gst_rtp_get_extmap_id_for_attribute (gst_caps_get_structure (caps, 0),
      TWCC_EXTMAP_STR);
  gst_caps_unref (caps);

  g_hash_table_insert (twcc->pt_to_twcc_ext_id, GUINT_TO_POINTER (pt),
      GUINT_TO_POINTER (twcc_ext_id));
  GST_LOG ("Added payload (%u) for twcc send ext-id: %u", pt, twcc_ext_id);

  return twcc_ext_id;
}

void
rtp_twcc_manager_send_packet (RTPTWCCManager * twcc, RTPPacketInfo * pinfo)
{
  guint8 pinfo_twcc_ext_id;

  pinfo_twcc_ext_id = get_twcc_ext_id_for_pt (twcc, pinfo->pt);

  /* save the first valid twcc extid we get from pt */
  if (pinfo_twcc_ext_id > 0 && twcc->send_ext_id == 0) {
    twcc->send_ext_id = pinfo_twcc_ext_id;
    GST_INFO ("TWCC enabled for send using extension id: %u",
        twcc->send_ext_id);
  }

  if (twcc->send_ext_id == 0)
    return;

  /* the packet info twcc_ext_id should match the parsed one */
  if (pinfo_twcc_ext_id != twcc->send_ext_id)
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

  idx = gst_rtp_buffer_compare_seqnum (first->seqnum, seqnum);

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
  GST_LOG ("Adding parsed packet #%u with status %u", seqnum, status);
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
  GST_LOG ("Found run-length: %u from seqnum: #%u with status code: %u",
      run_length, seqnum_offset, status_code);

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

  GST_LOG ("Found status vector for %u from seqnum: #%u with symbol size: %u",
      num_bits, seqnum_offset, symbol_size);

  for (i = 0; i < num_bits; i++) {
    guint8 status_code;
    if (gst_bit_reader_get_bits_uint8 (reader, &status_code, symbol_size))
      _add_parsed_packet (parsed_packets, seqnum_offset + i, status_code);
  }

  return num_bits;
}

/* keep the last 1000 packets... FIXME: do something smarter */
static void
_prune_sent_packets (RTPTWCCManager * twcc)
{
  if (twcc->sent_packets->len > 1000) {
    g_array_remove_range (twcc->sent_packets, 0,
        twcc->sent_packets->len - 1000);
  }
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
    goto done;
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
_add_parsed_packet_to_value_array (GValueArray * array, ParsedPacket * pkt)
{
  _append_structure_to_value_array (array,
      gst_structure_new ("RTPTWCCPacket",
          "seqnum", G_TYPE_UINT, pkt->seqnum,
          "remote-ts", G_TYPE_UINT64, pkt->remote_ts,
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
    GST_LOG ("Started parse for chunk-type: %u parsed packets: (%u/%u)",
        chunk_type, packets_parsed, packet_count);

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
      _add_parsed_packet_to_value_array (array, pkt);
    }

    if (first_sent_pkt) {
      SentPacket *found = NULL;
      guint16 sent_idx =
          gst_rtp_buffer_compare_seqnum (first_sent_pkt->seqnum, pkt->seqnum);
      if (sent_idx < twcc->sent_packets->len)
        found = &g_array_index (twcc->sent_packets, SentPacket, sent_idx);
      if (found && found->seqnum == pkt->seqnum) {
        found->remote_ts = pkt->remote_ts;

        GST_LOG ("matching pkt: #%u with local_ts: %" GST_TIME_FORMAT
            " size: %u, remote-ts: %" GST_TIME_FORMAT, pkt->seqnum,
            GST_TIME_ARGS (found->local_ts),
            found->size * 8, GST_TIME_ARGS (pkt->remote_ts));

        _add_packet_to_stats (twcc, pkt, found);

        /* calculate the round-trip time */
        rtt = GST_CLOCK_DIFF (found->local_ts, current_time);

      }
    }
  }

  if (GST_CLOCK_STIME_IS_VALID (rtt))
    twcc->avg_rtt = WEIGHT (rtt, twcc->avg_rtt, 0.1);
  twcc->last_report_time = current_time;

  _prune_sent_packets (twcc);

  _structure_take_value_array (ret, "packets", array);

  return ret;
}

GstStructure *
rtp_twcc_manager_get_windowed_stats (RTPTWCCManager * twcc)
{
  GstStructure *ret;
  GValueArray *array;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  GstClockTimeDiff start_time;
  GstClockTimeDiff end_time;

  GstClockTime last_ts = twcc_stats_ctx_get_last_local_ts (twcc->stats_ctx);
  if (!GST_CLOCK_TIME_IS_VALID (last_ts))
    return twcc_stats_ctx_get_structure (twcc->stats_ctx);;

  array = g_value_array_new (0);
  end_time = GST_CLOCK_DIFF (twcc->stats_window_delay, last_ts);
  start_time = end_time - twcc->stats_window_size;

  GST_DEBUG_OBJECT (twcc,
      "Calculating windowed stats for the window %" GST_STIME_FORMAT
      " starting from %" GST_STIME_FORMAT " to: %" GST_STIME_FORMAT,
      GST_STIME_ARGS (twcc->stats_window_size), GST_STIME_ARGS (start_time),
      GST_STIME_ARGS (end_time));

  twcc_stats_ctx_calculate_windowed_stats (twcc->stats_ctx, start_time,
      end_time);
  ret = twcc_stats_ctx_get_structure (twcc->stats_ctx);
  GST_LOG ("Full stats: %" GST_PTR_FORMAT, ret);

  g_hash_table_iter_init (&iter, twcc->stats_ctx_by_pt);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStructure *s;
    guint pt = GPOINTER_TO_UINT (key);
    TWCCStatsCtx *ctx = value;
    twcc_stats_ctx_calculate_windowed_stats (ctx, start_time, end_time);
    s = twcc_stats_ctx_get_structure (ctx);
    gst_structure_set (s, "pt", G_TYPE_UINT, pt, NULL);
    _append_structure_to_value_array (array, s);
    GST_LOG ("Stats for pt %u: %" GST_PTR_FORMAT, pt, s);
  }

  _structure_take_value_array (ret, "payload-stats", array);

  return ret;
}

void
rtp_twcc_manager_set_callback (RTPTWCCManager * twcc, RTPTWCCManagerCaps cb,
    gpointer user_data)
{
  twcc->caps_cb = cb;
  twcc->caps_ud = user_data;
}

void
rtp_twcc_manager_set_stats_window_size (RTPTWCCManager * twcc,
    GstClockTime size)
{
  GST_INFO_OBJECT (twcc, "Setting stats window size to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (size));
  twcc->stats_window_size = size;
}

GstClockTime
rtp_twcc_manager_get_stats_window_size (RTPTWCCManager * twcc)
{
  return twcc->stats_window_size;
}

void
rtp_twcc_manager_set_stats_window_delay (RTPTWCCManager * twcc,
    GstClockTime delay)
{
  GST_INFO_OBJECT (twcc, "Setting stats window delay to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (delay));
  twcc->stats_window_delay = delay;
}

GstClockTime
rtp_twcc_manager_get_stats_window_delay (RTPTWCCManager * twcc)
{
  return twcc->stats_window_delay;
}
