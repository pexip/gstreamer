/* GStreamer
 * Copyright (C)  2025 Pexip (http://pexip.com/)
 *   @author: Mikhail Baranov <mikhail.baranov@pexip.com>
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

#include "rtptwccstats.h"
#include <gst/rtp/gstrtprepairmeta.h>
#include <gst/gstvecdeque.h>

#define WEIGHT(a, b, w) (((a) * (w)) + ((b) * (1.0 - (w))))

#define MAX_STATS_PACKETS 30000
#define PACKETS_HIST_DUR (10 * GST_SECOND)
/* How many packets should fit into the packets history by default.
   Estimated bundle throughput is up to 150 per packets at maximum in average
   circumstances. */
#define PACKETS_HIST_LEN_DEFAULT MAX_STATS_PACKETS

GST_DEBUG_CATEGORY_EXTERN (rtp_twcc_debug);
#define GST_CAT_DEFAULT rtp_twcc_debug

typedef struct
{
  GstClockTime local_ts;
  GstClockTime socket_ts;
  GstClockTime remote_ts;
  guint16 seqnum;
  guint16 orig_seqnum;
  guint32 ssrc;
  guint8 pt;
  guint size;
  gboolean lost;
  gint redundant_idx;           /* if it's redudndant packet -- series number in a block,
                                   -1 otherwise */
  gint redundant_num;           /* if it'r a redundant packet -- how many packets are 
                                   in the block, -1 otherwise */
  guint32 protects_ssrc;        /* for redundant packets: SSRC of the data stream */

  /* For redundant packets: seqnums of the packets being protected 
   * by this packet. 
   * IMPORTANT: Once the packet is checked in before transmission, this array
   * contains rtp seqnums. After receiving a feedback on the packet, the array
   * is converted to TWCC seqnums. This is done to shift some work to the 
   * get_windowed_stats function, which should be less time-critical.
   */
  GArray *protects_seqnums;
  gboolean stats_processed;

  TWCCPktState state;
} SentPacket;

typedef struct
{
  SentPacket *sentpkt;
} StatsPktPtr;

static StatsPktPtr null_statspktptr = {.sentpkt = NULL };

typedef struct
{
  GstVecDeque *pt_packets;
  SentPacket *last_pkt_fb;      /* Latest packet on which we have received feedback */

  /* windowed stats */
  guint packets_sent;
  guint packets_recv;
  guint bitrate_sent;
  guint bitrate_recv;
  gdouble packet_loss_pct;
  gdouble recovery_pct;
  gint64 avg_delta_of_delta;
  gdouble delta_of_delta_growth;
  gdouble queueing_slope;
} TWCCStatsCtx;

struct _TWCCStatsManager
{
  GObject *parent;

  TWCCStatsCtx *stats_ctx;
  /* The first packet in stats_ctx seqnum, valid even if there is a gap in
     stats_ctx caused feedback packet loss
   */
  gint32 stats_ctx_first_seqnum;
  GHashTable *stats_ctx_by_pt;
  GHashTable *ssrc_to_seqmap;

  /* In order to keep RingBuffer sizes under control, we assert
     that the old packets we remove from the queues are older than statistics
     window we use.
   */
  GstClockTime prev_stat_window_beginning;

  GstVecDeque *sent_packets;
  gsize sent_packets_size;

  /* Ring Buffer of pointers to SentPacket struct from sent_packets
     to which we've got feedbacks, but not processed during statistics */
  GstVecDeque *sent_packets_feedbacks;

  /* Redundancy bookkeeping */
  GHashTable *redund_2_redblocks;
  GHashTable *seqnum_2_redblocks;

  gboolean first_fci_parse;
  guint16 expected_parsed_seqnum;
  guint8 expected_parsed_fb_pkt_count;

  GstClockTimeDiff avg_rtt;
  GstClockTimeDiff rtt;
};

static SentPacket *_find_sentpacket (TWCCStatsManager * statsman,
    guint16 seqnum);

/******************************************************************************/
typedef GArray *RedBlockKey;

typedef struct
{
  GArray *seqs;
  GArray *states;

  GArray *fec_seqs;
  GArray *fec_states;

  gsize num_redundant_packets;
} RedBlock;

static RedBlock *_redblock_new (GArray * seq, guint16 fec_seq,
    guint16 idx_redundant_packets, guint16 num_redundant_packets);
static void _redblock_free (RedBlock * block);
static RedBlockKey _redblock_key_new (GArray * seqs);
static void _redblock_key_free (RedBlockKey key);
static guint redblock_2_key (GArray * seq);
static guint _redund_hash (gconstpointer key);
static gboolean _redund_equal (gconstpointer a, gconstpointer b);
static gsize _redblock_reconsider (TWCCStatsManager * statsman,
    RedBlock * block);


static TWCCStatsCtx *_get_ctx_for_pt (TWCCStatsManager * statsman, guint pt);

/******************************************************************************/
/* Pick more definitive pkt state */
static TWCCPktState
_better_pkt_state (TWCCPktState state1, TWCCPktState state2)
{
  if (state1 == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
    return state2;
  } else if (state2 == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
    return state1;
  } else if (state1 == RTP_TWCC_FECBLOCK_PKT_LOST) {
    return state2;
  } else if (state2 == RTP_TWCC_FECBLOCK_PKT_LOST) {
    return state1;
  } else if (state1 == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
    return state2;
  } else if (state2 == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
    return state1;
  } else {
    return state1;
  }
}

static const gchar *
_pkt_state_s (TWCCPktState state)
{
  switch (state) {
    case RTP_TWCC_FECBLOCK_PKT_UNKNOWN:
      return "UNKNOWN";
    case RTP_TWCC_FECBLOCK_PKT_RECEIVED:
      return "RECEIVED";
    case RTP_TWCC_FECBLOCK_PKT_RECOVERED:
      return "RECOVERED";
    case RTP_TWCC_FECBLOCK_PKT_LOST:
      return "LOST";
    default:
      return "INVALID";
  }
}

static StatsPktPtr *
_sent_pkt_get (GstVecDeque *pkt_array, guint idx)
{
  return (StatsPktPtr *)
      gst_vec_deque_peek_nth_struct (pkt_array, idx);
}

static void
_append_structure_to_value_array (GValueArray *array, GstStructure *s)
{
  GValue *val;
  g_value_array_append (array, NULL);
  val = g_value_array_get_nth (array, array->n_values - 1);
  g_value_init (val, GST_TYPE_STRUCTURE);
  g_value_take_boxed (val, s);
}

static void
_structure_take_value_array (GstStructure *s,
    const gchar *field_name, GValueArray *array)
{
  GValue value = G_VALUE_INIT;
  g_value_init (&value, G_TYPE_VALUE_ARRAY);
  g_value_take_boxed (&value, array);
  gst_structure_take_value (s, field_name, &value);
  g_value_unset (&value);
}

static void
_register_seqnum (TWCCStatsManager *statsman,
    guint32 ssrc, guint16 seqnum, guint16 twcc_seqnum)
{
  GHashTable *seq_to_twcc =
      g_hash_table_lookup (statsman->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
  if (!seq_to_twcc) {
    seq_to_twcc = g_hash_table_new (NULL, NULL);
    g_hash_table_insert (statsman->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc),
        seq_to_twcc);
  }
  g_hash_table_insert (seq_to_twcc, GUINT_TO_POINTER (seqnum),
      GUINT_TO_POINTER (twcc_seqnum));
  GST_LOG_OBJECT (statsman->parent,
      "Registering OSN: %u to statsman-twcc_seqnum: %u with ssrc: %u", seqnum,
      twcc_seqnum, ssrc);
}

static void
_sent_packet_init (SentPacket *packet, guint16 seqnum, RTPPacketInfo *pinfo,
    GstRTPBuffer *rtp, gint redundant_idx, gint redundant_num,
    guint32 protect_ssrc, GArray *protect_seqnums_array)
{
  packet->seqnum = seqnum;
  packet->orig_seqnum = gst_rtp_buffer_get_seq (rtp);
  packet->ssrc = gst_rtp_buffer_get_ssrc (rtp);
  packet->local_ts = pinfo->current_time;
  packet->size = pinfo->bytes + 12;     /* the reported wireshark size */
  packet->pt = gst_rtp_buffer_get_payload_type (rtp);
  packet->remote_ts = GST_CLOCK_TIME_NONE;
  packet->socket_ts = GST_CLOCK_TIME_NONE;
  packet->lost = FALSE;
  packet->state = RTP_TWCC_FECBLOCK_PKT_UNKNOWN;
  packet->redundant_idx = redundant_idx;
  packet->redundant_num = redundant_num;
  packet->protects_ssrc = protect_ssrc;
  packet->protects_seqnums = protect_seqnums_array;
  packet->stats_processed = FALSE;
}

static void
_free_sentpacket (SentPacket *pkt)
{
  if (pkt && pkt->protects_seqnums) {
    g_array_unref (pkt->protects_seqnums);
  }
}

static void _rm_redundancy_links_pkt (TWCCStatsManager * ctx, SentPacket * pkt);

static GstClockTime
_pkt_stats_ts (SentPacket *pkt)
{
  if (!pkt) {
    return GST_CLOCK_TIME_NONE;
  } else {
    return GST_CLOCK_TIME_IS_VALID (pkt->socket_ts)
        ? pkt->socket_ts : pkt->local_ts;
  }
}

static void
_rm_pkt_stats (TWCCStatsManager *statsman, SentPacket *pkt)
{
  _rm_redundancy_links_pkt (statsman, pkt);
  GST_LOG_OBJECT (statsman->parent,
      "Removing #%u from history, main ctx length: %d, pkt ts: %"
      GST_TIME_FORMAT, pkt->seqnum,
      gst_vec_deque_get_length (statsman->stats_ctx->pt_packets),
      GST_TIME_ARGS (_pkt_stats_ts (pkt)));

  /* First, remove packet from pt-specific context */
  TWCCStatsCtx *ctx = _get_ctx_for_pt (statsman, pkt->pt);
  SentPacket *ctx_pkt =
      ((StatsPktPtr *) gst_vec_deque_pop_head_struct (ctx->
          pt_packets))->sentpkt;
  if (ctx_pkt != pkt) {
    GST_ERROR_OBJECT (statsman->parent,
        "Removedpkt: %p, ctx_pkt: %p", pkt, ctx_pkt);
    GST_ERROR_OBJECT (statsman->parent,
        "Removed pkt #%u != head of stats ctx: #%u, pkt: %p, ctx_pkt: %p",
        pkt->seqnum, ctx_pkt->seqnum, pkt, ctx_pkt);
    g_assert_not_reached ();
  }
  if (ctx->last_pkt_fb == ctx_pkt) {
    ctx->last_pkt_fb = NULL;
  }

  ctx_pkt =
      ((StatsPktPtr *) gst_vec_deque_pop_head_struct (statsman->
          stats_ctx->pt_packets))->sentpkt;
  if (ctx_pkt != pkt) {
    GST_ERROR_OBJECT (statsman->parent,
        "Removed pkt #%u != head of main stats ctx: #%u", pkt->seqnum,
        ctx_pkt->seqnum);
    g_assert_not_reached ();
  }
  if (statsman->stats_ctx->last_pkt_fb == pkt) {
    g_assert (gst_vec_deque_is_empty (statsman->stats_ctx));
    statsman->stats_ctx->last_pkt_fb = NULL;
  }
  statsman->stats_ctx_first_seqnum =
      (guint16) (statsman->stats_ctx_first_seqnum + 1);
}

static gboolean
_keep_history_length (TWCCStatsManager *statsman, gsize max_len,
    GstClockTime cur_time, GstClockTime max_history_duration)
{
  if (gst_vec_deque_is_empty (statsman->sent_packets)) {
    return FALSE;
  }

  SentPacket *head = gst_vec_deque_peek_head_struct (statsman->sent_packets);
  GstClockTime pkt_ts = _pkt_stats_ts (head);
  const gboolean too_long_sent_pkts =
      gst_vec_deque_get_length (statsman->sent_packets) >= max_len;
  const gboolean too_long_main_ctx =
      gst_vec_deque_get_length (statsman->stats_ctx->pt_packets) >
      MAX_STATS_PACKETS;
  const gboolean too_old_pkt = GST_CLOCK_TIME_IS_VALID (cur_time)
      && GST_CLOCK_TIME_IS_VALID (max_history_duration)
      && GST_CLOCK_DIFF (pkt_ts, cur_time) > max_history_duration;

  if (too_long_sent_pkts || too_long_main_ctx || too_old_pkt) {
    /* It could mean that statistics was not called at all, asumming that
       the oldest packet was not referenced anywhere else, we can drop it.
     */
    GST_LOG_OBJECT (statsman->parent,
        "Remove pkt from history #%u, cause: \"%s\", len: %ld, head_ts: %"
        GST_TIME_FORMAT ", cur_ts: %" GST_TIME_FORMAT,
        head->seqnum,
        too_long_sent_pkts ? "len(sent_pkts)" :
        too_long_main_ctx ? "duration(stats_ctx)" :
        too_old_pkt ? "old(head_pkt)" : "unknown",
        gst_vec_deque_get_length (statsman->sent_packets),
        GST_TIME_ARGS (pkt_ts), GST_TIME_ARGS (cur_time));
    if (GST_CLOCK_TIME_IS_VALID (statsman->prev_stat_window_beginning) &&
        GST_CLOCK_DIFF (pkt_ts, statsman->prev_stat_window_beginning)
        < 0) {
      GST_WARNING_OBJECT (statsman->parent,
          "sent_packets FIFO overflows, dropping");
    } else if (GST_CLOCK_TIME_IS_VALID (statsman->prev_stat_window_beginning) &&
        GST_CLOCK_DIFF (pkt_ts, statsman->prev_stat_window_beginning)
        < GST_MSECOND * 1500) {
      GST_WARNING_OBJECT (statsman->parent, "Risk of"
          " underrun of sent_packets FIFO");
    }
    gst_vec_deque_pop_head_struct (statsman->sent_packets);
    _rm_pkt_stats (statsman, head);
    _free_sentpacket (head);
    return TRUE;
  } else {
    return FALSE;
  }
}

static SentPacket *
_sent_pkt_keep_length (TWCCStatsManager *statsman, gsize max_len,
    SentPacket *new_packet)
{
  _keep_history_length (statsman, max_len, GST_CLOCK_TIME_NONE,
      GST_CLOCK_TIME_NONE);
  gst_vec_deque_push_tail_struct (statsman->sent_packets, new_packet);
  return (SentPacket *) gst_vec_deque_peek_tail_struct (statsman->sent_packets);
}

static TWCCStatsCtx *
twcc_stats_ctx_new (void)
{
  TWCCStatsCtx *ctx = g_new0 (TWCCStatsCtx, 1);

  ctx->pt_packets = gst_vec_deque_new_for_struct (sizeof (StatsPktPtr),
      MAX_STATS_PACKETS);
  ctx->last_pkt_fb = NULL;

  return ctx;
}

static void
twcc_stats_ctx_free (TWCCStatsCtx *ctx)
{
  gst_vec_deque_free (ctx->pt_packets);
  g_free (ctx);
}

static GstClockTime
twcc_stats_ctx_get_last_local_ts (TWCCStatsCtx *ctx)
{
  GstClockTime ret = GST_CLOCK_TIME_NONE;
  SentPacket *pkt = ctx->last_pkt_fb;
  if (pkt) {
    ret = _pkt_stats_ts (pkt);
  }
  return ret;
}

static gboolean
_get_stats_packets_window (GstVecDeque *array,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time,
    guint *start_idx, guint *num_packets)
{
  gboolean ret = FALSE;
  guint end_idx = 0;
  guint i;
  const guint array_length = gst_vec_deque_get_length (array);

  if (array_length < 2) {
    GST_DEBUG ("Not enough stats to do a window");
    return FALSE;
  }

  for (i = 0; i < array_length; i++) {
    SentPacket *pkt = _sent_pkt_get (array, i)->sentpkt;
    const GstClockTime pkt_ts = _pkt_stats_ts (pkt);
    if (GST_CLOCK_TIME_IS_VALID (pkt_ts)) {
      GstClockTimeDiff offset = GST_CLOCK_DIFF (pkt_ts, start_time);
      *start_idx = i;
      /* positive number here means it is older than our start time */
      if (offset > 0) {
        GST_LOG ("Packet #%u is too old: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt_ts));
      } else {
        GST_LOG ("Setting first packet in our window to #%u: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt_ts));
        ret = TRUE;
        break;
      }
    }
  }

  /* jump out early if we could not find a start_idx */
  if (!ret) {
    return FALSE;
  }

  ret = FALSE;
  for (i = 0; i < array_length - *start_idx - 1; i++) {
    guint idx = array_length - 1 - i;
    SentPacket *pkt = _sent_pkt_get (array, idx)->sentpkt;
    const GstClockTime pkt_ts = _pkt_stats_ts (pkt);
    if (pkt_ts) {
      GstClockTimeDiff offset = GST_CLOCK_DIFF (pkt_ts, end_time);
      if (offset >= 0) {
        GST_LOG ("Setting last packet in our window to #%u: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt_ts));
        end_idx = idx;
        ret = TRUE;
        break;
      } else {
        GST_LOG ("Packet #%u is too new: %"
            GST_TIME_FORMAT, pkt->seqnum, GST_TIME_ARGS (pkt_ts));
      }
    }
  }

  /* jump out early if we could not find a window */
  if (!ret) {
    return FALSE;
  }

  *num_packets = end_idx - *start_idx + 1;

  return ret;
}

/* Does linear regression in order to get the slope the two functions */
static gfloat
_get_slope (GArray *x, GArray *y)
{
  gfloat sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
  guint n = x->len;
  guint i;

  for (i = 0; i < n; i++) {
    gfloat xi = g_array_index (x, gfloat, i);
    gfloat yi = g_array_index (y, gfloat, i);
    sum_x += xi;
    sum_y += yi;
    sum_xy += xi * yi;
    sum_x2 += xi * xi;
  }

  return (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
}

typedef struct
{
  gsize n;
  gdouble mean_x;               /* Mean of x */
  gdouble mean_y;               /* Mean of y */
  gdouble Sxy;                  /* Sum of (xi - mean_x) * (yi - mean_y) */
  gdouble Sxx;                  /* Sum of (xi - mean_x)² */
} LinearRegression;

/* Initialize the structure */
static void
_linear_init (LinearRegression *l)
{
  l->n = 0;
  l->mean_x = 0.0;
  l->mean_y = 0.0;
  l->Sxy = 0.0;
  l->Sxx = 0.0;
}

/* Update with a new (x, y) point */
static void
_linear_update (LinearRegression *l, gdouble x, gdouble y)
{
  l->n += 1;
  const gdouble delta_x = x - l->mean_x;
  const gdouble delta_y = y - l->mean_y;

  /* Update means */
  l->mean_x += delta_x / l->n;
  l->mean_y += delta_y / l->n;

  /* Update sums */
  l->Sxx += delta_x * (x - l->mean_x);  /* Uses new mean_x */
  l->Sxy += delta_x * (y - l->mean_y);  /* Uses new mean_y */
}

/* Compute slope and intercept */
static void
_linear_compute (LinearRegression *l, gdouble *slope, gdouble *intercept)
{
  if (l->n < 2) {
    *slope = 0.0;
    if (intercept) {
      *intercept = 0.0;
    }
    return;
  }
  *slope = l->Sxy / l->Sxx;
  if (intercept) {
    *intercept = l->mean_y - (*slope) * l->mean_x;
  }
}

static gboolean
twcc_stats_ctx_calculate_windowed_stats (TWCCStatsCtx *ctx,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time, gint pt)
{
  GstVecDeque *packets = ctx->pt_packets;
  guint start_idx;
  guint packets_sent = 0;
  guint packets_overall = 0;
  guint packets_recv = 0;
  guint packets_recovered = 0;
  guint packets_lost = 0;
  guint packets_unknown = 0;

  guint i;
  guint bits_sent = 0;
  guint bits_recv = 0;

  GstClockTimeDiff delta_delta_sum = 0;
  guint delta_delta_count = 0;
  GstClockTimeDiff first_delta_delta_sum = 0;
  guint first_delta_delta_count = 0;
  GstClockTimeDiff last_delta_delta_sum = 0;
  guint last_delta_delta_count = 0;

  SentPacket *first_local_pkt = NULL;
  SentPacket *last_local_pkt = NULL;
  SentPacket *first_remote_pkt = NULL;
  SentPacket *last_remote_pkt = NULL;

  GstClockTimeDiff local_duration = 0;
  GstClockTimeDiff remote_duration = 0;

  LinearRegression dod_regression;
  _linear_init (&dod_regression);

  ctx->packets_sent = 0;
  ctx->packets_recv = 0;
  ctx->packet_loss_pct = 0.0;
  ctx->avg_delta_of_delta = 0;
  ctx->delta_of_delta_growth = 0.0;
  ctx->bitrate_sent = 0;
  ctx->bitrate_recv = 0;
  ctx->recovery_pct = -1.0;
  ctx->queueing_slope = 0.;

  gboolean ret =
      _get_stats_packets_window (packets, start_time, end_time, &start_idx,
      &packets_overall);
  if (!ret || packets_overall < 2) {
    GST_INFO ("Not enough packets to fill our window yet!");
    return FALSE;
  } else {
    GST_DEBUG ("Stats window: %u packets, pt: %d, %d->%d", packets_overall,
        pt,
        _sent_pkt_get (packets, start_idx)->sentpkt->seqnum,
        _sent_pkt_get (packets,
            packets_overall + start_idx - 1)->sentpkt->seqnum);
  }

  for (i = 0; i < packets_overall; i++) {
    SentPacket *prev = NULL;
    if (i + start_idx >= 1)
      prev = _sent_pkt_get (packets, i + start_idx)->sentpkt;

    SentPacket *pkt = _sent_pkt_get (packets, i + start_idx)->sentpkt;
    if (!pkt) {
      continue;
    }
    GST_LOG ("STATS WINDOW: %u/%u: pkt #%u, pt: %u, size: %u, arrived: %s, "
        "local-ts: %" GST_TIME_FORMAT ", remote-ts %" GST_TIME_FORMAT,
        i + 1, packets_overall, pkt->seqnum, pkt->pt, pkt->size * 8,
        GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) ? "YES" : "NO",
        GST_TIME_ARGS (_pkt_stats_ts (pkt)), GST_TIME_ARGS (pkt->remote_ts));

    if (GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (pkt))
        && pkt->state != RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      /* don't count the bits for the first packet in the window */
      if (first_local_pkt == NULL) {
        first_local_pkt = pkt;
      } else {
        bits_sent += pkt->size * 8;
      }
      last_local_pkt = pkt;
    }

    if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
      /* don't count the bits for the first packet in the window */
      if (first_remote_pkt == NULL) {
        first_remote_pkt = pkt;
      } else {
        bits_recv += pkt->size * 8;
      }
      last_remote_pkt = pkt;
      packets_sent++;
      packets_recv++;
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      GST_LOG ("Packet #%u is lost and recovered", pkt->seqnum);
      packets_sent++;
      packets_lost++;
      packets_recovered++;
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      GST_LOG ("Packet #%u is lost", pkt->seqnum);
      packets_sent++;
      packets_lost++;
    } else {
      GST_LOG ("Packet #%u status is unknown", pkt->seqnum);
      packets_unknown++;
    }

    if (!prev || prev->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      continue;
    }

    GstClockTimeDiff local_delta = GST_CLOCK_STIME_NONE;
    GstClockTimeDiff remote_delta = GST_CLOCK_STIME_NONE;
    GstClockTimeDiff delta_delta = GST_CLOCK_STIME_NONE;

    if (GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (pkt)) &&
        GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (prev))) {
      local_delta = GST_CLOCK_DIFF (_pkt_stats_ts (prev), _pkt_stats_ts (pkt));
    }

    if (GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) &&
        GST_CLOCK_TIME_IS_VALID (prev->remote_ts)) {
      remote_delta = GST_CLOCK_DIFF (prev->remote_ts, pkt->remote_ts);
    }

    if (GST_CLOCK_STIME_IS_VALID (local_delta) &&
        GST_CLOCK_STIME_IS_VALID (remote_delta)) {
      delta_delta = remote_delta - local_delta;

      delta_delta_sum += delta_delta;
      delta_delta_count++;
      _linear_update (&dod_regression,
          (gdouble) (_pkt_stats_ts (pkt) - _pkt_stats_ts (first_local_pkt)),
          (gdouble) delta_delta_sum);
      if (i < packets_overall / 2) {
        first_delta_delta_sum += delta_delta;
        first_delta_delta_count++;
      } else {
        last_delta_delta_sum += delta_delta;
        last_delta_delta_count++;
      }
    }
  }

  ctx->packets_sent = packets_sent;
  ctx->packets_recv = packets_recv;

  if (first_local_pkt && last_local_pkt) {
    local_duration =
        GST_CLOCK_DIFF (_pkt_stats_ts (first_local_pkt),
        _pkt_stats_ts (last_local_pkt));
  }
  if (first_remote_pkt && last_remote_pkt) {
    remote_duration =
        GST_CLOCK_DIFF (first_remote_pkt->remote_ts,
        last_remote_pkt->remote_ts);
  }

  if (packets_sent)
    ctx->packet_loss_pct = (packets_lost * 100)
        / (gfloat) packets_sent;

  if (packets_lost) {
    ctx->recovery_pct = (packets_recovered * 100) / (gfloat) packets_lost;
    ctx->recovery_pct = MIN (ctx->recovery_pct, 100);
  }

  if (delta_delta_count) {
    ctx->avg_delta_of_delta = delta_delta_sum / delta_delta_count;
  }

  if (first_delta_delta_count && last_delta_delta_count) {
    GstClockTimeDiff first_avg =
        first_delta_delta_sum / first_delta_delta_count;
    GstClockTimeDiff last_avg = last_delta_delta_sum / last_delta_delta_count;

    /* filter out very small numbers */
    first_avg = MAX (first_avg, 100 * GST_USECOND);
    last_avg = MAX (last_avg, 100 * GST_USECOND);
    ctx->delta_of_delta_growth = (double) last_avg / (double) first_avg;
  }

  if (local_duration > 0) {
    ctx->bitrate_sent =
        gst_util_uint64_scale (bits_sent, GST_SECOND, local_duration);
  }
  if (remote_duration > 0) {
    ctx->bitrate_recv =
        gst_util_uint64_scale (bits_recv, GST_SECOND, remote_duration);
  }

  _linear_compute (&dod_regression, &ctx->queueing_slope, NULL);

  GST_INFO ("Got stats: bits_sent: %u, bits_recv: %u, packets_sent = %u, "
      "packets_recv: %u, packetlost_pct = %lf, recovery_pct = %lf, "
      "recovered: %u, packets unknown: %u",
      bits_sent, bits_recv, packets_sent, packets_recv, ctx->packet_loss_pct,
      ctx->recovery_pct, packets_recovered, packets_unknown);
  GST_INFO ("local_duration=%" GST_STIME_FORMAT ", "
      "remote_duration=%" GST_STIME_FORMAT ", "
      "sent_bitrate = %u, " "recv_bitrate = %u, delta-delta-avg = %"
      GST_STIME_FORMAT ", delta-delta-growth=%lf, queueing-slope=%lf",
      GST_STIME_ARGS (local_duration),
      GST_STIME_ARGS (remote_duration), ctx->bitrate_sent, ctx->bitrate_recv,
      GST_STIME_ARGS (ctx->avg_delta_of_delta), ctx->delta_of_delta_growth,
      ctx->queueing_slope);
  return TRUE;
}

static GstStructure *
twcc_stats_ctx_get_structure (TWCCStatsCtx *ctx)
{
  return gst_structure_new ("RTPTWCCStats",
      "packets-sent", G_TYPE_UINT, ctx->packets_sent,
      "packets-recv", G_TYPE_UINT, ctx->packets_recv,
      "bitrate-sent", G_TYPE_UINT, ctx->bitrate_sent,
      "bitrate-recv", G_TYPE_UINT, ctx->bitrate_recv,
      "packet-loss-pct", G_TYPE_DOUBLE, ctx->packet_loss_pct,
      "recovery-pct", G_TYPE_DOUBLE, ctx->recovery_pct,
      "avg-delta-of-delta", G_TYPE_INT64, ctx->avg_delta_of_delta,
      "delta-of-delta-growth", G_TYPE_DOUBLE, ctx->delta_of_delta_growth,
      "queueing-slope", G_TYPE_DOUBLE, ctx->queueing_slope, NULL);
}

static gint
_idx_sentpacket (TWCCStatsManager *statsman, guint16 seqnum)
{
  const gint idx = gst_rtp_buffer_compare_seqnum (
      (guint16) statsman->stats_ctx_first_seqnum, seqnum);
  if (statsman->stats_ctx_first_seqnum >= 0 && idx >= 0) {
    return idx;
  } else {
    return -1;
  }
}

static SentPacket *
_find_stats_sentpacket (TWCCStatsManager *statsman, guint16 seqnum)
{
  gint idx = _idx_sentpacket (statsman, seqnum);
  SentPacket *res = NULL;
  if (idx >= 0
      && idx < gst_vec_deque_get_length (statsman->stats_ctx->pt_packets)) {
    res = _sent_pkt_get (statsman->stats_ctx->pt_packets, idx)->sentpkt;
  }

  return res;
}

static TWCCStatsCtx *_get_ctx_for_pt (TWCCStatsManager * statsman, guint pt);

static void
twcc_stats_ctx_add_packet (TWCCStatsManager *statsman, SentPacket *pkt)
{
  if (!pkt) {
    return;
  } else if (gst_vec_deque_is_empty (statsman->stats_ctx->pt_packets)) {
    statsman->stats_ctx_first_seqnum = pkt->seqnum;
  }
  gst_vec_deque_push_tail_struct (statsman->stats_ctx->pt_packets,
      (StatsPktPtr *) & pkt);
  TWCCStatsCtx *ctx = _get_ctx_for_pt (statsman, pkt->pt);
  gst_vec_deque_push_tail_struct (ctx->pt_packets, (StatsPktPtr *) & pkt);
  SentPacket *ctx_pkt =
      ((StatsPktPtr *) gst_vec_deque_peek_head_struct (ctx->
          pt_packets))->sentpkt;
  GST_LOG_OBJECT (statsman->parent,
      "adding pkt to context #%u, pt %u, pkt: %p, ctx_pkt: %p", pkt->seqnum,
      pkt->pt, pkt, ctx_pkt);
}

/******************************************************************************/
/* Redundancy book keeping subpart
  "Was a certain packet recovered on the receiver side?"
  
  * Organizes sent data packets and redundant packets into blocks
  * Keeps track of redundant packets reception such as RTX and FEC packets
  * Maps all packets to blocks and vice versa
  * Is used to calculate redundancy statistics
 */

static RedBlock *
_redblock_new (GArray *seq, guint16 fec_seq,
    guint16 idx_redundant_packets, guint16 num_redundant_packets)
{
  RedBlock *block = g_malloc0 (sizeof (RedBlock));
  block->seqs = g_array_ref (seq);
  block->states = g_array_new (FALSE, FALSE, sizeof (TWCCPktState));
  g_array_set_size (block->states, seq->len);
  for (gsize i = 0; i < seq->len; i++) {
    g_array_index (block->states, TWCCPktState, i) =
        RTP_TWCC_FECBLOCK_PKT_UNKNOWN;
  }
  block->num_redundant_packets = num_redundant_packets;

  block->fec_seqs = g_array_new (FALSE, FALSE, sizeof (guint16));
  if (num_redundant_packets < 1
      || idx_redundant_packets >= num_redundant_packets) {
    GST_ERROR ("Incorrect redundant packet index or number: %hu/%hu",
        idx_redundant_packets, num_redundant_packets);
    g_assert_not_reached ();
  }
  g_array_set_size (block->fec_seqs, num_redundant_packets);
  block->fec_states = g_array_new (FALSE, FALSE, sizeof (TWCCPktState));
  g_array_set_size (block->fec_states, num_redundant_packets);
  for (guint16 i = 0; i < num_redundant_packets; i++) {
    g_array_index (block->fec_states, TWCCPktState, i)
        = RTP_TWCC_FECBLOCK_PKT_UNKNOWN;
    g_array_index (block->fec_seqs, guint16, i) = 0;
  }
  g_array_index (block->fec_seqs, guint16, idx_redundant_packets)
      = fec_seq;
  return block;
}

static void
_redblock_free (RedBlock *block)
{
  g_array_unref (block->seqs);
  g_array_free (block->states, TRUE);
  g_array_free (block->fec_seqs, TRUE);
  g_array_free (block->fec_states, TRUE);
  g_free (block);
}

static RedBlockKey
_redblock_key_new (GArray *seqs)
{
  return g_array_ref (seqs);
}

static void
_redblock_key_free (RedBlockKey key)
{
  g_array_unref (key);
}

static guint
redblock_2_key (GArray *seq)
{
  guint32 key = 0;
  gsize i = 0;
  /* In reality seq contains guint16, but we treat it as 32bits ints till 
     we can */
  for (; i < seq->len / 2; i += 2) {
    key ^= g_array_index (seq, guint32, i / 2);
  }
  for (; i < seq->len; i++) {
    key ^= g_array_index (seq, guint16, i);
  }
  return key;
}

static guint
_redund_hash (gconstpointer key)
{
  RedBlockKey bk = (RedBlockKey) key;
  return redblock_2_key (bk);
}

static gboolean
_redund_equal (gconstpointer a, gconstpointer b)
{
  RedBlockKey bk1 = (RedBlockKey) a;
  RedBlockKey bk2 = (RedBlockKey) b;
  return bk1->len == bk2->len &&
      memcmp (bk1->data, bk2->data, bk1->len * sizeof (guint16))
      == 0;
}

/* Check if the block could be recovered:
    * all packets have known states
    * number of lost packets is less than redundant packets were originally sent

  Returns the number of recoverd packets
*/
static gsize
_redblock_reconsider (TWCCStatsManager *statsman, RedBlock *block)
{
  gsize nreceived = 0;
  gboolean recovered = FALSE;
  gsize nrecovered = 0;
  gsize lost = 0;

  gchar states_media[48];
  gchar states_fec[16];

  /* Special case for RTX: lost RTX introduces extra complexity which 
     is easier to handle separately
   */
  if (block->seqs->len == 1) {
    SentPacket *media_pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->seqs, guint16, 0));

    if (media_pkt && (media_pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST
            || media_pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN)) {
      for (gsize i = 0; i < block->fec_seqs->len; ++i) {
        SentPacket *redundant_pkt = _find_stats_sentpacket (statsman,
            g_array_index (block->fec_seqs, guint16, i));
        if (redundant_pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
          nrecovered++;
          break;
        }
      }
      if (nrecovered == 1) {
        media_pkt->state = RTP_TWCC_FECBLOCK_PKT_RECOVERED;
      }
    }

    return nrecovered;
  }

  /* Walk through all the packets and check if the block could be recovered */
  for (gsize i = 0; i < block->seqs->len; ++i) {
    SentPacket *pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->seqs, guint16, i));
    if (!pkt || pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      lost++;
      if (i < G_N_ELEMENTS (states_media)) {
        states_media[i] = 'U';
      }
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
      nreceived++;
      if (i < G_N_ELEMENTS (states_media))
        states_media[i] = '+';
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      recovered = TRUE;
      if (i < G_N_ELEMENTS (states_media))
        states_media[i] = 'R';
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      lost++;
      if (i < G_N_ELEMENTS (states_media))
        states_media[i] = '-';
    }
  }
  states_media[block->seqs->len] = '\0';

  /* Walk through all fec packets */
  for (gsize i = 0; i < block->fec_seqs->len; ++i) {
    SentPacket *pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->fec_seqs, guint16, i));
    if (!pkt || pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      lost++;
      if (i < G_N_ELEMENTS (states_fec)) {
        states_fec[i] = 'U';
      }
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
      nreceived++;
      if (i < G_N_ELEMENTS (states_fec))
        states_fec[i] = '+';
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      recovered = TRUE;
      if (i < G_N_ELEMENTS (states_fec))
        states_fec[i] = 'R';
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      lost++;
      if (i < G_N_ELEMENTS (states_fec))
        states_fec[i] = '-';
    }
  }
  states_fec[block->fec_seqs->len] = '\0';

  if ((lost + nreceived > block->seqs->len + block->fec_seqs->len)
      || (lost > 0 && recovered)) {
    GST_ERROR
        ("The FEC block is partly recovered, abort: %lu lost, %lu/%lu received",
        lost, nreceived, block->seqs->len + block->fec_seqs->len);
    g_assert_not_reached ();
  }

  if (lost > 0 && lost <= block->fec_seqs->len) {
    /* We have enough packets to recover the block */
    for (gsize i = 0; i < block->seqs->len; ++i) {
      SentPacket *pkt = _find_stats_sentpacket (statsman,
          g_array_index (block->seqs, guint16, i));
      if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
        pkt->state = RTP_TWCC_FECBLOCK_PKT_RECOVERED;
        nrecovered++;
      }
    }
    for (gsize i = 0; i < block->fec_seqs->len; ++i) {
      SentPacket *pkt = _find_stats_sentpacket (statsman,
          g_array_index (block->fec_seqs, guint16, i));
      if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
        pkt->state = RTP_TWCC_FECBLOCK_PKT_RECOVERED;
        nrecovered++;
      }
    }
  }

  GST_INFO ("Media: %s; FEC: %s; recovered: %lu", states_media, states_fec,
      nrecovered);
  return nrecovered;
}

/******************************************************************************/

static TWCCStatsCtx *
_get_ctx_for_pt (TWCCStatsManager *statsman, guint pt)
{
  TWCCStatsCtx *ctx =
      g_hash_table_lookup (statsman->stats_ctx_by_pt, GUINT_TO_POINTER (pt));
  if (!ctx) {
    ctx = twcc_stats_ctx_new ();
    g_hash_table_insert (statsman->stats_ctx_by_pt, GUINT_TO_POINTER (pt), ctx);
    ctx->last_pkt_fb = NULL;
  }
  return ctx;
}

static void
_rm_redundancy_links_pkt (TWCCStatsManager *ctx, SentPacket *pkt)
{
  /* If this packet maps to a block in hash tables -- remove every links 
     leading to this block as well as this packet: as we will remove this packet
     from the context, we will not be able to use this block anyways. */
  RedBlock *block = NULL;
  if (pkt && g_hash_table_lookup_extended (ctx->seqnum_2_redblocks,
          GUINT_TO_POINTER (pkt->seqnum), NULL, (gpointer *) & block)) {
    RedBlockKey key = _redblock_key_new (block->seqs);
    for (gsize i = 0; i < block->seqs->len; i++) {
      g_hash_table_remove (ctx->seqnum_2_redblocks,
          GUINT_TO_POINTER (g_array_index (block->seqs, guint16, i)));
    }
    for (gsize i = 0; i < block->fec_seqs->len; i++) {
      g_hash_table_remove (ctx->seqnum_2_redblocks,
          GUINT_TO_POINTER (g_array_index (block->fec_seqs, guint16, i)));
    }
    g_hash_table_remove (ctx->redund_2_redblocks, key);
    _redblock_key_free (key);
  }
}

static gint32
_lookup_seqnum (TWCCStatsManager *statsman, guint32 ssrc, guint16 seqnum)
{
  gint32 ret = -1;

  GHashTable *seq_to_twcc =
      g_hash_table_lookup (statsman->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
  if (seq_to_twcc) {
    if (g_hash_table_lookup_extended (seq_to_twcc, GUINT_TO_POINTER (seqnum),
            NULL, (gpointer *) & ret)) {
      return ret;
    } else {
      return -1;
    }
  }
  return ret;
}

static SentPacket *
_find_sentpacket (TWCCStatsManager *statsman, guint16 seqnum)
{
  if (gst_vec_deque_is_empty (statsman->sent_packets) == TRUE) {
    return NULL;
  }

  SentPacket *first = gst_vec_deque_peek_head_struct (statsman->sent_packets);
  SentPacket *result;

  const gint idx = gst_rtp_buffer_compare_seqnum (first->seqnum, seqnum);
  if (idx < gst_vec_deque_get_length (statsman->sent_packets) && idx >= 0) {
    result = (SentPacket *)
        gst_vec_deque_peek_nth_struct (statsman->sent_packets, idx);
  } else {
    result = NULL;
  }

  if (result && result->seqnum == seqnum) {
    return result;
  }

  return NULL;
}

/* Once we've got feedback on a packet, we need to account it in the internal
  structures. */
static void
_process_pkt_feedback (SentPacket *pkt, TWCCStatsManager *statsman)
{
  if (pkt->stats_processed) {
    /* This packet was already added to stats structures, but we've got 
       one more feedback for it
     */
    RedBlock *block;
    if (g_hash_table_lookup_extended (statsman->seqnum_2_redblocks,
            GUINT_TO_POINTER (pkt->seqnum), NULL, (gpointer *) & block)) {
      const gsize packets_recovered = _redblock_reconsider (statsman, block);
      if (packets_recovered > 0) {
        GST_LOG_OBJECT (statsman->parent,
            "Reconsider block because of packet #%u, " "recovered %lu pckt",
            pkt->seqnum, packets_recovered);
      }
    }
    return;
  }
  /* last feedback packet */
  if (!statsman->stats_ctx->last_pkt_fb ||
      gst_rtp_buffer_compare_seqnum (statsman->stats_ctx->last_pkt_fb->seqnum,
          pkt->seqnum) > 0) {
    statsman->stats_ctx->last_pkt_fb = pkt;
  }
  TWCCStatsCtx *ctx = _get_ctx_for_pt (statsman, pkt->pt);
  if (!ctx->last_pkt_fb ||
      gst_rtp_buffer_compare_seqnum (ctx->last_pkt_fb->seqnum,
          pkt->seqnum) > 0) {
    ctx->last_pkt_fb = pkt;
  }

  pkt->stats_processed = TRUE;
  GST_LOG_OBJECT (statsman->parent, "Processing #%u packet in stats, state: %s",
      pkt->seqnum, _pkt_state_s (pkt->state));

  /* This is either RTX or FEC packet */
  if (pkt->protects_seqnums && pkt->protects_seqnums->len > 0) {
    /* We are expecting non-twcc seqnums in the buffer's meta here, so
       change them to twcc seqnums. */

    if (pkt->redundant_idx < 0 || pkt->redundant_num <= 0
        || pkt->redundant_idx >= pkt->redundant_num) {
      GST_ERROR_OBJECT (statsman->parent,
          "Invalid FEC packet: idx: %d, num: %d", pkt->redundant_idx,
          pkt->redundant_num);
      g_assert_not_reached ();
    }

    for (gsize i = 0; i < pkt->protects_seqnums->len; i++) {
      const guint16 prot_seqnum = g_array_index (pkt->protects_seqnums,
          guint16, i);
      gint32 twcc_seqnum = _lookup_seqnum (statsman, pkt->protects_ssrc,
          prot_seqnum);
      if (twcc_seqnum != -1) {
        g_array_index (pkt->protects_seqnums, guint16, i)
            = (guint16) twcc_seqnum;
      }
      GST_LOG_OBJECT (statsman->parent,
          "FEC sn: #%u covers twcc sn: #%u, orig sn: %u", pkt->seqnum,
          twcc_seqnum, prot_seqnum);
    }

    /* Check if this packet covers the same block that was already added. */
    RedBlockKey key = _redblock_key_new (pkt->protects_seqnums);
    RedBlock *block = NULL;
    if (g_hash_table_lookup_extended (statsman->redund_2_redblocks, key, NULL,
            (gpointer *) & block)) {
      /* This is not RTX, check this redundant pkt meta */
      if (pkt->redundant_num > 1 &&
          (block->fec_seqs->len != pkt->redundant_num
              || block->fec_states->len != pkt->redundant_num)) {

        GST_WARNING_OBJECT (statsman->parent, "Got contradictory FEC block: "
            "seqs: %u, states: %u, redundant_num: %d, redundant_idx: %d",
            block->fec_seqs->len, block->fec_states->len, pkt->redundant_num,
            pkt->redundant_idx);
        _redblock_key_free (key);
        return;
        /* This is 2nd or more attempt of RTX */
      } else if (pkt->redundant_num == 1) {
        pkt->redundant_idx = block->fec_seqs->len;
        block->num_redundant_packets++;
        g_array_set_size (block->fec_seqs, block->num_redundant_packets);
        g_array_set_size (block->fec_states, block->num_redundant_packets);

        GST_LOG_OBJECT (statsman->parent, "Adding redundant packet #%u to"
            " an exsiting block on position %u", pkt->seqnum,
            pkt->redundant_idx);
      }
      g_array_index (block->fec_seqs, guint16, (gsize) pkt->redundant_idx)
          = pkt->seqnum;
      g_array_index (block->fec_states, TWCCPktState,
          (gsize) pkt->redundant_idx) = pkt->state;

      _redblock_key_free (key);

      /* Link this seqnum to the block in order to be able to 
         release the block once this packet leave its lifetime */
      g_hash_table_insert (statsman->seqnum_2_redblocks,
          GUINT_TO_POINTER (pkt->seqnum), block);
      /* There is no such block, add a new one */
    } else {
      /* Add every data packet into seqnum_2_redblocks  */
      block = _redblock_new (pkt->protects_seqnums, pkt->seqnum,
          pkt->redundant_idx, pkt->redundant_num);
      g_array_index (block->fec_seqs, guint16, (gsize) pkt->redundant_idx)
          = pkt->seqnum;
      g_array_index (block->fec_states, TWCCPktState,
          (gsize) pkt->redundant_idx) = pkt->state;
      g_hash_table_insert (statsman->redund_2_redblocks, key, block);
      /* Link this seqnum to the block in order to be able to 
         release the block once this packet leave its lifetime */
      g_hash_table_insert (statsman->seqnum_2_redblocks,
          GUINT_TO_POINTER (pkt->seqnum), block);
      for (gsize i = 0; i < pkt->protects_seqnums->len; ++i) {
        const guint64 data_key = g_array_index (pkt->protects_seqnums,
            guint16, i);
        RedBlock *data_block = NULL;
        if (!g_hash_table_lookup_extended (statsman->seqnum_2_redblocks,
                GUINT_TO_POINTER (data_key), NULL, (gpointer *) & data_block)) {

          g_hash_table_insert (statsman->seqnum_2_redblocks,
              GUINT_TO_POINTER (data_key), block);
        } else if (block != data_block) {
          /* Overlapped blocks are not supported yet */
          GST_WARNING_OBJECT (statsman->parent,
              "Data packet %ld covered by two blocks", data_key);
          g_hash_table_replace (statsman->seqnum_2_redblocks,
              GUINT_TO_POINTER (data_key), block);
        }
      }
    }
    const gsize packets_recovered = _redblock_reconsider (statsman, block);
    GST_LOG_OBJECT (statsman->parent,
        "Reconsider block because of packet #%u, recovered %lu pckt",
        pkt->seqnum, packets_recovered);
    /* Neither RTX nor FEC  */
  } else {
    RedBlock *block;
    if (g_hash_table_lookup_extended (statsman->seqnum_2_redblocks,
            GUINT_TO_POINTER (pkt->seqnum), NULL, (gpointer *) & block)) {

      for (gsize i = 0; i < block->seqs->len; ++i) {
        if (g_array_index (block->seqs, guint16, i) == pkt->seqnum) {
          g_array_index (block->states, TWCCPktState, i) =
              _better_pkt_state (g_array_index (block->states, TWCCPktState, i),
              pkt->state);
          break;
        }
      }
      const gsize packets_recovered = _redblock_reconsider (statsman, block);
      GST_LOG_OBJECT (statsman->parent,
          "Reconsider block because of packet #%u, " "recovered %lu pckt",
          pkt->seqnum, packets_recovered);
    }
  }
}

/******************************************************************************/

TWCCStatsManager *
rtp_twcc_stats_manager_new (GObject *parent)
{
  TWCCStatsManager *statsman = g_new0 (TWCCStatsManager, 1);
  statsman->parent = parent;

  statsman->stats_ctx = twcc_stats_ctx_new ();
  statsman->ssrc_to_seqmap = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_hash_table_destroy);
  statsman->sent_packets = gst_vec_deque_new_for_struct (sizeof (SentPacket),
      PACKETS_HIST_LEN_DEFAULT);
  statsman->sent_packets_size = PACKETS_HIST_LEN_DEFAULT;
  gst_vec_deque_set_clear_func (statsman->sent_packets,
      (GDestroyNotify) _free_sentpacket);
  statsman->sent_packets_feedbacks = gst_vec_deque_new (300);
  statsman->stats_ctx_first_seqnum = -1;
  statsman->stats_ctx_by_pt = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) twcc_stats_ctx_free);

  statsman->prev_stat_window_beginning = GST_CLOCK_TIME_NONE;

  statsman->redund_2_redblocks = g_hash_table_new_full (_redund_hash,
      _redund_equal, (GDestroyNotify) _redblock_key_free,
      (GDestroyNotify) _redblock_free);
  statsman->seqnum_2_redblocks = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, NULL);

  statsman->first_fci_parse = TRUE;
  statsman->expected_parsed_seqnum = 0;
  statsman->expected_parsed_fb_pkt_count = 0;

  return statsman;
}

void
rtp_twcc_stats_manager_free (TWCCStatsManager *statsman)
{
  g_hash_table_destroy (statsman->ssrc_to_seqmap);
  gst_vec_deque_free (statsman->sent_packets);
  gst_vec_deque_free (statsman->sent_packets_feedbacks);
  g_hash_table_destroy (statsman->stats_ctx_by_pt);
  g_hash_table_destroy (statsman->redund_2_redblocks);
  g_hash_table_destroy (statsman->seqnum_2_redblocks);
  twcc_stats_ctx_free (statsman->stats_ctx);
  g_free (statsman);
}

void
rtp_twcc_stats_sent_pkt (TWCCStatsManager *statsman,
    RTPPacketInfo *pinfo, GstRTPBuffer *rtp, guint16 twcc_seqnum)
{
  GstBuffer *buf = rtp->buffer;
  SentPacket packet;
  GArray *protect_seqnums_array = NULL;
  guint32 protect_ssrc = 0;
  gint redundant_pkt_idx = -1;
  gint redundant_pkt_num = -1;

  /* In oreder to be able to map rtp seqnum to twcc seqnum in future, we
     store it in certain hash tables (e.g. it might be needed to process
     received feedback on a FEC packet) */
  _register_seqnum (statsman, pinfo->ssrc, pinfo->seqnum, twcc_seqnum);

  /* If this packet is RTX/FEC packet, keep track of its meta */
  GstRTPRepairMeta *repair_meta = NULL;
  if ((repair_meta = gst_rtp_repair_meta_get (buf)) != NULL) {
    protect_ssrc = repair_meta->ssrc;
    protect_seqnums_array = g_array_ref (repair_meta->seqnums);
    redundant_pkt_idx = repair_meta->idx_red_packets;
    redundant_pkt_num = repair_meta->num_red_packets;
  }

  _sent_packet_init (&packet, twcc_seqnum, pinfo, rtp,
      redundant_pkt_idx, redundant_pkt_num,
      protect_ssrc, protect_seqnums_array);
  /* Add packet to the sent_packets ring buffer and
     make sure that it is within max_size, if not shrink by 1 pkt */
  SentPacket *sent_pkt =
      _sent_pkt_keep_length (statsman, statsman->sent_packets_size, &packet);
  twcc_stats_ctx_add_packet (statsman, sent_pkt);

  for (guint i = 0; protect_seqnums_array && i < protect_seqnums_array->len;
      i++) {
    const guint16 prot_seqnum_ =
        g_array_index (protect_seqnums_array, guint16, i);
    GST_DEBUG_OBJECT (statsman->parent, "%u protects seqnum: %u", twcc_seqnum,
        prot_seqnum_);
  }

  GST_DEBUG_OBJECT
      (statsman->parent,
      "Send: twcc-seqnum: %u, seqnum: %u, pt: %u, marker: %d, "
      "redundant_idx: %d, redundant_num: %d, protected_seqnums: %u,"
      "size: %u, ts: %" GST_TIME_FORMAT, packet.seqnum, pinfo->seqnum,
      packet.pt, pinfo->marker, packet.redundant_idx, packet.redundant_num,
      packet.protects_seqnums ? packet.protects_seqnums->len : 0, packet.size,
      GST_TIME_ARGS (pinfo->current_time));
}

void
rtp_twcc_stats_set_sock_ts (TWCCStatsManager *statsman,
    guint16 seqnum, GstClockTime sock_ts)
{
  SentPacket *pkt = _find_sentpacket (statsman, seqnum);
  if (pkt) {
    pkt->socket_ts = sock_ts;
    GST_LOG_OBJECT (statsman->parent,
        "packet #%u, setting socket-ts %" GST_TIME_FORMAT, seqnum,
        GST_TIME_ARGS (sock_ts));
  } else {
    GST_WARNING_OBJECT (statsman->parent,
        "Unable to update send-time for twcc-seqnum #%u", seqnum);
  }
}

void
rtp_twcc_manager_tx_start_feedback (TWCCStatsManager *statsman)
{
  statsman->rtt = GST_CLOCK_TIME_NONE;
}

void
rtp_twcc_stats_pkt_feedback (TWCCStatsManager *statsman,
    guint16 seqnum, GstClockTime remote_ts, GstClockTime current_time,
    TWCCPktState status)
{
  SentPacket *found;
  if (!!(found = _find_sentpacket (statsman, seqnum))) {
    /* Do not process feedback on packets we have got feedback previously */
    if (found->state < status) {
      found->remote_ts = remote_ts;
      found->state = status;
      gst_vec_deque_push_tail (statsman->sent_packets_feedbacks, found);
      GST_LOG_OBJECT (statsman->parent,
          "matching pkt: #%u with local_ts: %" GST_TIME_FORMAT
          " size: %u, remote-ts: %" GST_TIME_FORMAT, seqnum,
          GST_TIME_ARGS (found->local_ts), found->size * 8,
          GST_TIME_ARGS (remote_ts));

      /* calculate the round-trip time */
      statsman->rtt = GST_CLOCK_DIFF (found->local_ts, current_time);
    } else {
      /* We've got feed back on the packet that was covered with the previous TWCC report.
         Receiver could send two feedbacks on a single packet on purpose, 
         so we just ignore it. */
      GST_LOG_OBJECT (statsman->parent,
          "Rejecting second feedback on a packet #%u: current state: %s, "
          "received fb: %s", seqnum, _pkt_state_s(found->state),
          _pkt_state_s(status));
    }
  } else {
    GST_WARNING_OBJECT (statsman->parent, "Feedback on unknown packet #%u",
        seqnum);
  }
}

void
rtp_twcc_manager_tx_end_feedback (TWCCStatsManager *statsman)
{
  if (GST_CLOCK_STIME_IS_VALID (statsman->rtt))
    statsman->avg_rtt = WEIGHT (statsman->rtt, statsman->avg_rtt, 0.1);
}

GstStructure *
rtp_twcc_stats_do_stats (TWCCStatsManager *statsman,
    GstClockTime stats_window_size, GstClockTime stats_window_delay)
{
  GstStructure *ret;
  GValueArray *array;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  GstClockTimeDiff start_time;
  GstClockTimeDiff end_time;

  while (!gst_vec_deque_is_empty (statsman->sent_packets_feedbacks)) {
    SentPacket *pkt = (SentPacket *)
        gst_vec_deque_pop_head (statsman->sent_packets_feedbacks);
    if (!pkt) {
      continue;
    }
    _process_pkt_feedback (pkt, statsman);
  }                             /* while fifo of arrays */

  GstClockTime last_ts = twcc_stats_ctx_get_last_local_ts (statsman->stats_ctx);
  if (!GST_CLOCK_TIME_IS_VALID (last_ts))
    return twcc_stats_ctx_get_structure (statsman->stats_ctx);

  /* Prune old packets in stats */
  while (_keep_history_length (statsman, statsman->sent_packets_size, last_ts,
          PACKETS_HIST_DUR));

  array = g_value_array_new (0);
  end_time = GST_CLOCK_DIFF (stats_window_delay, last_ts);
  start_time = end_time - stats_window_size;

  GST_DEBUG_OBJECT (statsman->parent,
      "Calculating windowed stats for the window %" GST_STIME_FORMAT
      " starting from %" GST_STIME_FORMAT " to: %" GST_STIME_FORMAT
      " overall packets: %u", GST_STIME_ARGS (stats_window_size),
      GST_STIME_ARGS (start_time), GST_STIME_ARGS (end_time),
      gst_vec_deque_get_length (statsman->stats_ctx->pt_packets));

  if (!GST_CLOCK_TIME_IS_VALID (statsman->prev_stat_window_beginning) ||
      GST_CLOCK_DIFF (statsman->prev_stat_window_beginning, start_time) > 0) {
    statsman->prev_stat_window_beginning = start_time;
  }

  twcc_stats_ctx_calculate_windowed_stats (statsman->stats_ctx, start_time,
      end_time, -1);
  ret = twcc_stats_ctx_get_structure (statsman->stats_ctx);
  GST_LOG_OBJECT (statsman->parent, "Full stats: %" GST_PTR_FORMAT, ret);

  g_hash_table_iter_init (&iter, statsman->stats_ctx_by_pt);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStructure *s;
    guint pt = GPOINTER_TO_UINT (key);
    TWCCStatsCtx *ctx = value;
    twcc_stats_ctx_calculate_windowed_stats (ctx, start_time, end_time, pt);
    s = twcc_stats_ctx_get_structure (ctx);
    gst_structure_set (s, "pt", G_TYPE_UINT, pt, NULL);
    _append_structure_to_value_array (array, s);
    GST_LOG_OBJECT (statsman->parent, "Stats for pt %u: %" GST_PTR_FORMAT, pt,
        s);
  }

  _structure_take_value_array (ret, "payload-stats", array);

  return ret;
}

/* If TWCC packets are coming in a row, but there is a gap in between
 * the regions of the packets they are covering, the packets in the gap
 * are lost.
 */
void
rtp_twcc_stats_check_for_lost_packets (TWCCStatsManager *statsman,
    guint16 base_seqnum, guint16 packet_count, guint8 fb_pkt_count)
{
  guint packets_lost;
  gint8 fb_pkt_count_diff;
  guint i;

  /* first packet */
  if (statsman->first_fci_parse) {
    statsman->first_fci_parse = FALSE;
    goto done;
  }

  fb_pkt_count_diff =
      (gint8) (fb_pkt_count - statsman->expected_parsed_fb_pkt_count);

  /* we have gone backwards, don't reset the expectations,
     but process the packet nonetheless */
  if (fb_pkt_count_diff < 0) {
    GST_DEBUG_OBJECT (statsman->parent,
        "feedback packet count going backwards (%u < %u)", fb_pkt_count,
        statsman->expected_parsed_fb_pkt_count);
    return;
  }

  /* we have jumped forwards, reset expectations, but don't trigger
     lost packets in case the missing fb-packet(s) arrive later */
  if (fb_pkt_count_diff > 0) {
    GST_DEBUG_OBJECT (statsman->parent,
        "feedback packet count jumped ahead (%u > %u)", fb_pkt_count,
        statsman->expected_parsed_fb_pkt_count);
    goto done;
  }

  if (base_seqnum < statsman->expected_parsed_seqnum) {
    GST_DEBUG_OBJECT (statsman->parent,
        "twcc seqnum is older than expected  (%u < %u)", base_seqnum,
        statsman->expected_parsed_seqnum);
    goto done;
  }

  packets_lost = base_seqnum - statsman->expected_parsed_seqnum;
  for (i = 0; i < packets_lost; i++) {
    const guint16 seqnum = statsman->expected_parsed_seqnum + i;
    SentPacket *found;
    if (!!(found = _find_sentpacket (statsman, seqnum))) {
      /* Do not process feedback on packets we have got feedback previously */
      if (found->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
        found->state = RTP_TWCC_FECBLOCK_PKT_LOST;
        gst_vec_deque_push_tail (statsman->sent_packets_feedbacks, found);
        GST_LOG_OBJECT (statsman->parent,
            "Processing lost pkt feedback: #%u with local_ts: %" GST_TIME_FORMAT
            " size: %u", seqnum,
            GST_TIME_ARGS (found->local_ts), found->size * 8);

      } else {
        /* We've got feed back on the packet that was covered with the previous TWCC report.
           Receiver could send two feedbacks on a single packet on purpose, 
           so we just ignore it. */
        GST_LOG_OBJECT (statsman->parent,
            "Rejecting second feedback on a packet #%u", seqnum);
      }
    }
  }

done:
  statsman->expected_parsed_seqnum = base_seqnum + packet_count;
  statsman->expected_parsed_fb_pkt_count = fb_pkt_count + 1;
  return;
}

guint
rtp_twcc_stats_queue_len (TWCCStatsManager *stats_manager)
{
  return gst_vec_deque_get_length (stats_manager->sent_packets);
}
