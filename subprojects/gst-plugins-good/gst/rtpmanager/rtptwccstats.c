#include "rtptwccstats.h"
#include <gst/rtp/gstrtprepairmeta.h>
#include <gst/base/gstqueuearray.h>

#define WEIGHT(a, b, w) (((a) * (w)) + ((b) * (1.0 - (w))))

#define PACKETS_HIST_DUR (10 * GST_SECOND)
/* How many packets should fit into the packets history by default.
   Estimated bundle throughput is up to 150 per packets at maximum in average
   circumstances. */
#define PACKETS_HIST_LEN_DEFAULT (300 * PACKETS_HIST_DUR / GST_SECOND)

#define MAX_STATS_PACKETS 1000

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
  gint redundant_idx; /* if it's redudndant packet -- series number in a block,
                        -1 otherwise */
  gint redundant_num; /* if it'r a redundant packet -- how many packets are 
                          in the block, -1 otherwise */
  guint32 protects_ssrc; /* for redundant packets: SSRC of the data stream */

  /* For redundant packets: seqnums of the packets being protected 
   * by this packet. 
   * IMPORTANT: Once the packet is checked in before transmission, this array
   * contains rtp seqnums. After receiving a feedback on the packet, the array
   * is converted to TWCC seqnums. This is done to shift some work to the 
   * get_windowed_stats function, which should be less time-critical.
   */
  GArray * protects_seqnums;
  gboolean stats_processed;

  TWCCPktState state;
  gint update_stats;
} SentPacket;

typedef struct
{
  SentPacket * sentpkt;
} StatsPktPtr;

static StatsPktPtr null_statspktptr = {.sentpkt=NULL};

typedef struct
{
  GstQueueArray *pt_packets;
  SentPacket *last_pkt_fb;
  gint64 new_packets_idx;

  /* windowed stats */
  guint packets_sent;
  guint packets_recv;
  guint bitrate_sent;
  guint bitrate_recv;
  gdouble packet_loss_pct;
  gdouble recovery_pct;
  gint64 avg_delta_of_delta;
  gdouble delta_of_delta_growth;
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

  GstQueueArray *sent_packets;
  gsize sent_packets_size;

  /* Ring Buffer of pointers to SentPacket struct from sent_packets
     to which we've got feedbacks, but not processed during statistics */
  GstQueueArray *sent_packets_feedbacks;

  /* Redundancy bookkeeping */
  GHashTable * redund_2_redblocks;
  GHashTable * seqnum_2_redblocks;

  GstClockTimeDiff avg_rtt;
  GstClockTimeDiff rtt;
};

/******************************************************************************/
typedef GArray* RedBlockKey;

typedef struct 
{
  GArray *seqs;
  GArray *states;

  GArray * fec_seqs;
  GArray * fec_states;

  gsize num_redundant_packets;
} RedBlock;

static RedBlock *_redblock_new(GArray* seq, guint16 fec_seq,
    guint16 idx_redundant_packets, guint16 num_redundant_packets);
static void _redblock_free(RedBlock *block);
static RedBlockKey _redblock_key_new (GArray * seqs);
static void _redblock_key_free (RedBlockKey key);
static guint redblock_2_key(GArray * seq);
static guint _redund_hash (gconstpointer key);
static gboolean _redund_equal (gconstpointer a, gconstpointer b);
static gsize _redblock_reconsider (TWCCStatsManager * statsman, RedBlock * block);


static TWCCStatsCtx *_get_ctx_for_pt (TWCCStatsManager *statsman, guint pt);

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
  switch (state){
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

static StatsPktPtr*
_sent_pkt_get (GstQueueArray* pkt_array, guint idx)
{
  return (StatsPktPtr*)
    gst_queue_array_peek_nth_struct (pkt_array, idx);
}

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

static void
_sent_pkt_keep_length (TWCCStatsManager *statsman, gsize max_len,
    SentPacket* new_packet)
{
  if (gst_queue_array_get_length(statsman->sent_packets) >= max_len) {
    /* It could mean that statistics was not called at all, asumming that
      the oldest packet was not referenced anywhere else, we can drop it.
      */
    SentPacket * head = (SentPacket*)gst_queue_array_peek_head_struct (statsman->sent_packets);
    GstClockTime pkt_ts = head->local_ts;
    if (GST_CLOCK_TIME_IS_VALID(statsman->prev_stat_window_beginning) &&
        GST_CLOCK_DIFF (pkt_ts, statsman->prev_stat_window_beginning) 
            < 0) {
        GST_WARNING_OBJECT (statsman->parent, "sent_packets FIFO overflows, dropping");
        g_assert_not_reached ();
    } else if (GST_CLOCK_TIME_IS_VALID(statsman->prev_stat_window_beginning) &&
      GST_CLOCK_DIFF (pkt_ts, statsman->prev_stat_window_beginning)
        < GST_MSECOND * 250) {
        GST_WARNING_OBJECT (statsman->parent, "Risk of"
          " underrun of sent_packets FIFO");
    }
    GST_LOG_OBJECT (statsman->parent, "Keeping sent_packets FIFO length: %u, dropping packet #%u",
        max_len, head->seqnum);
    gst_queue_array_pop_head_struct (statsman->sent_packets);
  }
  gst_queue_array_push_tail_struct (statsman->sent_packets, new_packet);
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
  GST_LOG_OBJECT (statsman->parent, "Registering OSN: %u to statsman-twcc_seqnum: %u with ssrc: %u", seqnum,
      twcc_seqnum, ssrc);
}

static void
_sent_packet_init (SentPacket * packet, guint16 seqnum, RTPPacketInfo * pinfo,
    GstRTPBuffer * rtp, gint redundant_idx, gint redundant_num,
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
_free_sentpacket (SentPacket * pkt)
{
  if (pkt->protects_seqnums) {
    g_array_unref (pkt->protects_seqnums);
  }
}

static TWCCStatsCtx *
twcc_stats_ctx_new (void)
{
  TWCCStatsCtx *ctx = g_new0 (TWCCStatsCtx, 1);

  ctx->pt_packets = gst_queue_array_new_for_struct (sizeof(StatsPktPtr), 
      MAX_STATS_PACKETS);
  ctx->last_pkt_fb = NULL;

  return ctx;
}

static void
twcc_stats_ctx_free (TWCCStatsCtx * ctx)
{
  gst_queue_array_free (ctx->pt_packets);
  g_free (ctx);
}

static GstClockTime
_pkt_stats_ts (SentPacket * pkt)
{
  return GST_CLOCK_TIME_IS_VALID (pkt->socket_ts) 
      ? pkt->socket_ts 
      : pkt->local_ts;
}

static GstClockTime
twcc_stats_ctx_get_last_local_ts (TWCCStatsCtx * ctx)
{
  GstClockTime ret = GST_CLOCK_TIME_NONE;
  SentPacket *pkt = ctx->last_pkt_fb;
  if (pkt) {
    ret = _pkt_stats_ts (pkt);
  }
  return ret;
}

static gboolean
_get_stats_packets_window (GstQueueArray * array,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time,
    guint * start_idx, guint * num_packets)
{
  gboolean ret = FALSE;
  guint end_idx = 0;
  guint i;
  const guint array_length = gst_queue_array_get_length (array); 

  if (array_length < 2) {
    GST_DEBUG ("Not enough stats to do a window");
    return FALSE;
  }

  for (i = 0; i < array_length; i++) {
    SentPacket *pkt = _sent_pkt_get (array, i)->sentpkt;
    if (!pkt) {
      continue;
    }
    /* Do not process packets that were not reported about in feedbacks
      yet. */
    if (pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      continue;
    }
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
    if (!pkt) {
      continue;
    }
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

static void _rm_last_stats_pkt (TWCCStatsManager * ctx);

static gboolean
twcc_stats_ctx_calculate_windowed_stats (TWCCStatsCtx * ctx,
    GstClockTimeDiff start_time, GstClockTimeDiff end_time)
{
  GstQueueArray *packets = ctx->pt_packets;
  guint start_idx;
  guint packets_sent = 0;
  guint packets_recv = 0;
  guint packets_recovered = 0;
  guint packets_lost = 0;

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

  ctx->packet_loss_pct = 0.0;
  ctx->avg_delta_of_delta = 0;
  ctx->delta_of_delta_growth = 0.0;
  ctx->bitrate_sent = 0;
  ctx->bitrate_recv = 0;
  ctx->recovery_pct = -1.0;

  gboolean ret =
      _get_stats_packets_window (packets, start_time, end_time, &start_idx,
      &packets_sent);
  if (!ret || packets_sent < 2) {
    GST_INFO ("Not enough packets to fill our window yet!");
    return FALSE;
  } else {
    GST_DEBUG ("Stats window: %u packets, pt: %d, %d->%d", packets_sent,
      _sent_pkt_get (packets, start_idx) ->sentpkt->pt,
      _sent_pkt_get (packets, start_idx)->sentpkt->seqnum,
      _sent_pkt_get (packets, packets_sent + start_idx - 1)->sentpkt->seqnum);
  }

  for (i = 0; i < packets_sent; i++) {
    SentPacket *prev = NULL;
    if (i + start_idx >= 1)
      prev = _sent_pkt_get (packets, i + start_idx)->sentpkt;

    SentPacket *pkt = _sent_pkt_get(packets, i + start_idx)->sentpkt;
    if (!pkt) {
      continue;
    }
    GST_LOG ("STATS WINDOW: %u/%u: pkt #%u, pt: %u, size: %u, arrived: %s, "
        "local-ts: %" GST_TIME_FORMAT ", remote-ts %" GST_TIME_FORMAT,
        i + 1, packets_sent, pkt->seqnum, pkt->pt, pkt->size * 8,
        GST_CLOCK_TIME_IS_VALID (pkt->remote_ts) ? "YES" : "NO",
        GST_TIME_ARGS (_pkt_stats_ts (pkt)), GST_TIME_ARGS (pkt->remote_ts));

    if (GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (pkt))) {
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
      packets_recv++;
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      GST_LOG ("Packet #%u is lost and recovered", pkt->seqnum);
      packets_lost++;
      packets_recovered++;
    } else if(pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      GST_LOG ("Packet #%u is lost", pkt->seqnum);
      packets_lost++;
    }

    if (!prev) {
      continue;
    }

    GstClockTimeDiff local_delta = GST_CLOCK_STIME_NONE;
    GstClockTimeDiff remote_delta = GST_CLOCK_STIME_NONE;
    GstClockTimeDiff delta_delta = GST_CLOCK_STIME_NONE;

    if (GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (pkt)) &&
      GST_CLOCK_TIME_IS_VALID (_pkt_stats_ts (prev))) {
        local_delta = GST_CLOCK_DIFF (_pkt_stats_ts (prev),
            _pkt_stats_ts (pkt));
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
      if (i < packets_sent / 2) {
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
    ctx->packet_loss_pct = (packets_lost * 100) / (gfloat) packets_sent;

  if (packets_lost) {
    ctx->recovery_pct = (packets_recovered * 100) / (gfloat) packets_lost;
    ctx->recovery_pct = MIN(ctx->recovery_pct, 100);
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

  GST_INFO ("Got stats: bits_sent: %u, bits_recv: %u, packets_sent = %u, "
      "packets_recv: %u, packetlost_pct = %lf, recovery_pct = %lf, "
      "recovered: %u, local_duration=%" GST_TIME_FORMAT ", "
      "remote_duration=%" GST_TIME_FORMAT ", " 
      "sent_bitrate = %u, " "recv_bitrate = %u, delta-delta-avg = %"
      GST_STIME_FORMAT ", delta-delta-growth=%lf", bits_sent, bits_recv,
      packets_sent, packets_recv, ctx->packet_loss_pct, ctx->recovery_pct,
      packets_recovered,
      GST_TIME_ARGS (local_duration), GST_TIME_ARGS (remote_duration),
      ctx->bitrate_sent, ctx->bitrate_recv,
      GST_STIME_ARGS (ctx->avg_delta_of_delta), ctx->delta_of_delta_growth);

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
      "avg-delta-of-delta", G_TYPE_INT64, ctx->avg_delta_of_delta,
      "delta-of-delta-growth", G_TYPE_DOUBLE, ctx->delta_of_delta_growth, NULL);
}

static gint
_idx_sentpacket (TWCCStatsManager * statsman, guint16 seqnum)
{
  const gint idx = gst_rtp_buffer_compare_seqnum (
      (guint16)statsman->stats_ctx_first_seqnum, seqnum);
  if (statsman->stats_ctx_first_seqnum >= 0 && idx >= 0) {
    return idx;
  } else {
    return -1;
  }
}

static SentPacket *
_find_stats_sentpacket (TWCCStatsManager * statsman, guint16 seqnum)
{
  gint idx = _idx_sentpacket (statsman, seqnum);
  SentPacket * res = NULL;
  if (idx >= 0 
      && idx < gst_queue_array_get_length (statsman->stats_ctx->pt_packets)) {
    res = _sent_pkt_get (statsman->stats_ctx->pt_packets, idx)->sentpkt;
  }

  return res;
}

static TWCCStatsCtx *
_get_ctx_for_pt (TWCCStatsManager *statsman, guint pt);

static void
twcc_stats_ctx_add_packet (TWCCStatsManager *statsman, SentPacket * pkt)
{
  if (!pkt) {
    return;
  } else if (gst_queue_array_is_empty (statsman->stats_ctx->pt_packets)) {
    gst_queue_array_push_tail_struct (statsman->stats_ctx->pt_packets,
        (StatsPktPtr*)&pkt);
    TWCCStatsCtx *ctx = _get_ctx_for_pt (statsman, pkt->pt);
    gst_queue_array_push_tail_struct (ctx->pt_packets, (StatsPktPtr*)&pkt);
    statsman->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = pkt;
    statsman->stats_ctx_first_seqnum = pkt->seqnum;
  } else {
    const gint idx = _idx_sentpacket (statsman, pkt->seqnum);
    GstQueueArray * main_array = statsman->stats_ctx->pt_packets;
    if (idx < 0) {
      GST_WARNING_OBJECT (statsman->parent, "Packet #%u is too old for stats, dropping, latest pkt is #%u",
        pkt->seqnum, statsman->stats_ctx_first_seqnum);
      return;
    } else if (idx >= gst_queue_array_get_length (main_array)) {
      const gsize n2push = idx - gst_queue_array_get_length (main_array);
      /* if n2push > 0 means that the last statsman feedback packet[s] is lost,
       */
      for (gsize i = 0; i < n2push; i++) {
        gst_queue_array_push_tail_struct (main_array, 
            &null_statspktptr);
      }
      gst_queue_array_push_tail_struct (main_array, (StatsPktPtr*)&pkt);
      TWCCStatsCtx *ctx = _get_ctx_for_pt (statsman, pkt->pt);
      gst_queue_array_push_tail_struct (ctx->pt_packets, (StatsPktPtr*)&pkt);
      statsman->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = pkt;
    } else {
      /* TWCC packets reordered -- do nothing */
      GST_WARNING_OBJECT (statsman->parent, "Packet #%u is out of order, not going to stats", pkt->seqnum);
    }
  }
}

/******************************************************************************/
/* Redundancy book keeping subpart
  "Was a certain packet recovered on the receiver side?"
  
  * Organizes sent data packets and redundant packets into blocks
  * Keeps track of redundant packets reception such as RTX and FEC packets
  * Maps all packets to blocks and vice versa
  * Is used to calculate redundancy statistics
 */

static RedBlock * _redblock_new(GArray* seq, guint16 fec_seq,
    guint16 idx_redundant_packets, guint16 num_redundant_packets)
{
  RedBlock *block = g_malloc (sizeof (RedBlock));
  block->seqs = g_array_ref (seq);
  block->states = g_array_new (FALSE, FALSE, sizeof (TWCCPktState));
  g_array_set_size (block->states, seq->len);
  for (gsize i = 0; i < seq->len; i++)
  {
    g_array_index (block->states, TWCCPktState, i) =
      RTP_TWCC_FECBLOCK_PKT_UNKNOWN;
  }
  block->num_redundant_packets = num_redundant_packets;

  block->fec_seqs = g_array_new (FALSE, FALSE, sizeof (guint16));
  if (num_redundant_packets < 1 || idx_redundant_packets >= num_redundant_packets) {
    GST_ERROR ("Incorrect redundant packet index or number: %hu/%hu",
        idx_redundant_packets, num_redundant_packets);
    g_assert_not_reached ();
  }
  g_array_set_size (block->fec_seqs, num_redundant_packets);
  block->fec_states = g_array_new (FALSE, FALSE, sizeof (TWCCPktState));
  g_array_set_size (block->fec_states, num_redundant_packets);
  for (guint16 i = 0; i < num_redundant_packets; i++)
  {
    g_array_index (block->fec_states, TWCCPktState, i) 
      = RTP_TWCC_FECBLOCK_PKT_UNKNOWN;
    g_array_index (block->fec_seqs, guint16, i) = 0;
  }
  g_array_index (block->fec_seqs, guint16, idx_redundant_packets) 
    = fec_seq;
  return block;
}

static void _redblock_free(RedBlock *block) {
  g_array_unref (block->seqs);
  g_array_free (block->states, TRUE);
  g_array_free (block->fec_seqs, TRUE);
  g_array_free (block->fec_states, TRUE);
  g_free (block);
}

static RedBlockKey _redblock_key_new (GArray * seqs)
{
  return g_array_ref (seqs);
}

static void _redblock_key_free (RedBlockKey key)
{
  g_array_unref (key);
}

static guint redblock_2_key(GArray * seq)
{
  guint32 key = 0;
  gsize i = 0;
  /* In reality seq contains guint16, but we treat it as 32bits ints till 
  we can */
  for (; i < seq->len / 2; i += 2) {
    key ^= g_array_index(seq, guint32, i / 2);
  }
  for (; i < seq->len; i++) {
    key ^= g_array_index(seq, guint16, i);
  }
  return key;
}

static guint _redund_hash (gconstpointer key)
{
  RedBlockKey bk = (RedBlockKey)key;
  return redblock_2_key(bk);
}

static gboolean _redund_equal (gconstpointer a, gconstpointer b)
{
  RedBlockKey bk1 = (RedBlockKey)a;
  RedBlockKey bk2 = (RedBlockKey)b;
  return bk1->len == bk2->len &&
    memcmp(bk1->data, bk2->data, bk1->len * sizeof(guint16))
    == 0;
}

/* Check if the block could be recovered:
    * all packets have known states
    * number of lost packets is less than redundant packets were originally sent

  Returns the number of recoverd packets
*/
static gsize _redblock_reconsider (TWCCStatsManager * statsman, RedBlock * block)
{
  gsize nreceived = 0;
  gboolean recovered = FALSE;
  gboolean unknowns = FALSE;
  gsize nrecovered = 0;
  gsize lost = 0;

  gchar states_media[48];
  gchar states_fec[16];

  /* Special case for RTX: lost RTX introduces extra complexity which 
    is easier to handle separately
  */
  if (block->seqs->len == 1) {
    SentPacket *pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->seqs, guint16, 0));
    
    if (pkt && pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      for (gsize i = 0; i < block->fec_seqs->len; ++i) {
        SentPacket *pkt = _find_stats_sentpacket (statsman,
            g_array_index (block->fec_seqs, guint16, i));
        if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
          nrecovered++;
          break;
        }
      }
      if (nrecovered == 1) {
        pkt->state = RTP_TWCC_FECBLOCK_PKT_RECOVERED;
      }
    }

    return nrecovered;
  }
  
  /* Walk through all the packets and check if the block could be recovered */
  for (gsize i = 0; i < block->seqs->len; ++i) {
    SentPacket *pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->seqs, guint16, i));
    if (!pkt || pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      unknowns = TRUE;
      if (i < G_N_ELEMENTS(states_media)) states_media[i] = 'U'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
      nreceived++;
      if (i < G_N_ELEMENTS(states_media)) states_media[i] = '+'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      recovered = TRUE;
      if (i < G_N_ELEMENTS(states_media)) states_media[i] = 'R'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      lost++;
      if (i < G_N_ELEMENTS(states_media)) states_media[i] = '-'; 
    }

    if (pkt) {
      pkt->update_stats = FALSE;
    }
  }
  states_media[block->seqs->len] = '\0';

  /* Walk through all fec packets */
  for (gsize i = 0; i < block->fec_seqs->len; ++i) {
    if (g_array_index (block->fec_states, TWCCPktState, i) 
        == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      unknowns = TRUE;
      if (i < G_N_ELEMENTS(states_fec)) states_fec[i] = 'U'; 
      continue;
    }
    SentPacket *pkt = _find_stats_sentpacket (statsman,
        g_array_index (block->fec_seqs, guint16, i));
    if (!pkt || pkt->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {
      unknowns = TRUE;
      if (i < G_N_ELEMENTS(states_fec)) states_fec[i] = 'U'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECEIVED) {
      nreceived++;
      if (i < G_N_ELEMENTS(states_fec)) states_fec[i] = '+'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_RECOVERED) {
      recovered = TRUE;
      if (i < G_N_ELEMENTS(states_fec)) states_fec[i] = 'R'; 
    } else if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      lost++;
      if (i < G_N_ELEMENTS(states_fec)) states_fec[i] = '-'; 
    }

    if (pkt) {
      pkt->update_stats = FALSE;
    }
  }
  states_fec[block->fec_seqs->len] = '\0';


  /* We have packet[s] that was not reported about in feedbacks yet */
  if (unknowns) {
    GST_INFO ("Media: %s; FEC: %s", states_media, states_fec);
    GST_INFO ("The FEC block has unknown packets");
    return 0;
  }

  /* Error: it's not possible to recover a part of a block */
  if ((lost + nreceived != block->seqs->len + block->fec_seqs->len)
      || (lost > 0 && recovered)) {
    GST_ERROR ("The FEC block is partly recovered, abort: %lu lost, %lu/%lu received",
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
_get_ctx_for_pt (TWCCStatsManager * statsman, guint pt)
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
_rm_last_packet_from_stats_arrays (TWCCStatsManager *statsman)
{
  SentPacket * head = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        statsman->stats_ctx->pt_packets))->sentpkt;
  if (head) {
    TWCCStatsCtx * ctx = _get_ctx_for_pt (statsman, head->pt);
    SentPacket * ctx_pkt = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        ctx->pt_packets))->sentpkt;
    if (!ctx_pkt || ctx_pkt->seqnum != head->seqnum) {
      GST_WARNING_OBJECT (statsman->parent, "Attempting to remove packet from pt stats context "
      "which seqnum does not match the main stats context seqnum, "
          "main: #%u, pt: %u, context packet: #%u, pt: %u",
            head->seqnum, head->pt, 
            ctx_pkt ? ctx_pkt->seqnum : -1, ctx_pkt ? ctx_pkt->pt : -1);
      g_assert_not_reached ();
    }
    if (ctx->last_pkt_fb == head) {
      statsman->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = NULL;
    }
    gst_queue_array_pop_head_struct (ctx->pt_packets);
    GST_LOG_OBJECT (statsman->parent, "Removing packet #%u from stats context, ts: %" GST_STIME_FORMAT,
        head->seqnum, head->local_ts);
  }
  gst_queue_array_pop_head_struct (statsman->stats_ctx->pt_packets);
  statsman->stats_ctx_first_seqnum++;
}

static void
_rm_last_stats_pkt (TWCCStatsManager * ctx)
{
  SentPacket * head = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        ctx->stats_ctx->pt_packets))->sentpkt;
  /* If this packet maps to a block in hash tables -- remove every links 
  leading to this block as well as this packet: as we will remove this packet
  from the context, we will not be able to use this block anyways. */
  RedBlock * block = NULL;
  if (head && g_hash_table_lookup_extended (ctx->seqnum_2_redblocks,
      GUINT_TO_POINTER(head->seqnum), NULL, (gpointer *)&block)) {
        RedBlockKey key = _redblock_key_new (block->seqs);
        for (gsize i = 0; i < block->seqs->len; i++) {
          g_hash_table_remove (ctx->seqnum_2_redblocks,
              GUINT_TO_POINTER(g_array_index (block->seqs, guint16, i)));
        }
        for (gsize i = 0; i < block->fec_seqs->len; i++) {
          g_hash_table_remove (ctx->seqnum_2_redblocks,
              GUINT_TO_POINTER(g_array_index (block->fec_seqs, guint16, i)));
        }
        g_hash_table_remove (ctx->redund_2_redblocks, key);
        _redblock_key_free (key);
  }
  _rm_last_packet_from_stats_arrays (ctx);
}

static gint32
_lookup_seqnum (TWCCStatsManager *statsman, guint32 ssrc, guint16 seqnum)
{
  gint32 ret = -1;

  GHashTable *seq_to_twcc =
      g_hash_table_lookup (statsman->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
  if (seq_to_twcc) {
    if (g_hash_table_lookup_extended (seq_to_twcc, GUINT_TO_POINTER (seqnum),
        NULL, (gpointer *)&ret)) {
          return ret;
    } else {
      return -1;
    }
  }
  return ret;
}

static SentPacket *
_find_sentpacket (TWCCStatsManager * statsman, guint16 seqnum)
{
  if (gst_queue_array_is_empty (statsman->sent_packets) == TRUE) {
    return NULL;
  }

  SentPacket * first = gst_queue_array_peek_head_struct (statsman->sent_packets);
  SentPacket * result;

  const gint idx = gst_rtp_buffer_compare_seqnum (first->seqnum, seqnum);
  if (idx < gst_queue_array_get_length (statsman->sent_packets) && idx >= 0) {
    result = (SentPacket*)
        gst_queue_array_peek_nth_struct (statsman->sent_packets, idx);
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
_process_pkt_feedback (SentPacket * pkt, TWCCStatsManager * statsman)
{
  if (pkt->stats_processed) {
    /* This packet was already added to stats structures, but we've got 
        one more feedback for it
      */
    RedBlock * block;
    if (g_hash_table_lookup_extended (statsman->seqnum_2_redblocks,
        GUINT_TO_POINTER(pkt->seqnum), NULL, (gpointer *)&block)) {
      const gsize packets_recovered = _redblock_reconsider (statsman, block);
      if (packets_recovered > 0) {
        GST_LOG_OBJECT (statsman->parent, "Reconsider block because of packet #%u, "
        "recovered %lu pckt", pkt->seqnum, packets_recovered);
      }
    }
    return;
  }
  pkt->stats_processed = TRUE;
  GST_LOG_OBJECT (statsman->parent, "Processing #%u packet in stats, state: %s", pkt->seqnum,
    _pkt_state_s (pkt->state));

  twcc_stats_ctx_add_packet (statsman, pkt);

  /* This is either RTX or FEC packet */
  if (pkt->protects_seqnums && pkt->protects_seqnums->len > 0) {
    /* We are expecting non-twcc seqnums in the buffer's meta here, so
      change them to twcc seqnums. */

    if (pkt->redundant_idx < 0 || pkt->redundant_num <= 0
        || pkt->redundant_idx >= pkt->redundant_num) {
          GST_ERROR_OBJECT (statsman->parent, "Invalid FEC packet: idx: %d, num: %d",
              pkt->redundant_idx, pkt->redundant_num);
          g_assert_not_reached ();
    }
    
    for (gsize i = 0; i < pkt->protects_seqnums->len; i++) {
      const guint16 prot_seqnum = g_array_index (pkt->protects_seqnums,
          guint16, i);
      gint32 twcc_seqnum = _lookup_seqnum (statsman, pkt->protects_ssrc,
          prot_seqnum);
      if (twcc_seqnum != -1) {
        g_array_index (pkt->protects_seqnums, guint16, i) 
            = (guint16)twcc_seqnum;
      }
      GST_LOG_OBJECT (statsman->parent, "FEC sn: #%u covers twcc sn: #%u, orig sn: %u",
          pkt->seqnum, twcc_seqnum, prot_seqnum);
    }
  
    /* Check if this packet covers the same block that was already added. */
    RedBlockKey key = _redblock_key_new (pkt->protects_seqnums);
    RedBlock * block = NULL;
    if (g_hash_table_lookup_extended (statsman->redund_2_redblocks, key, NULL,
        (gpointer*)&block)) {
      /* Add redundant packet to the existent block */
      if (block->fec_seqs->len != pkt->redundant_num
          || block->fec_states->len != pkt->redundant_num
          || g_array_index (block->fec_seqs, guint16, (gsize)pkt->redundant_idx) != 0
          || g_array_index (block->fec_states, TWCCPktState, (gsize)pkt->redundant_idx) != RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {

        GST_WARNING_OBJECT (statsman->parent, "Got contradictory FEC block: "
            "seqs: %u, states: %u, redundant_num: %d, redundant_idx: %d",
            block->fec_seqs->len, block->fec_states->len, pkt->redundant_num, pkt->redundant_idx);
        _redblock_key_free (key);
        return;
      }
      g_array_index (block->fec_seqs, guint16, (gsize)pkt->redundant_idx) 
          = pkt->seqnum;
      g_array_index (block->fec_states, TWCCPktState,
          (gsize)pkt->redundant_idx) = pkt->state;
      
      _redblock_key_free (key);

      /* Link this seqnum to the block in order to be able to 
      release the block once this packet leave its lifetime */
      g_hash_table_insert (statsman->seqnum_2_redblocks, 
          GUINT_TO_POINTER(pkt->seqnum), block);
    } else {
      /* Add every data packet into seqnum_2_redblocks  */
      block = _redblock_new (pkt->protects_seqnums, pkt->seqnum,
          pkt->redundant_idx, pkt->redundant_num);
      g_array_index (block->fec_seqs, guint16, (gsize)pkt->redundant_idx) 
              = pkt->seqnum;
      g_array_index (block->fec_states, TWCCPktState,
              (gsize)pkt->redundant_idx) = pkt->state;
      g_hash_table_insert (statsman->redund_2_redblocks, key, block);
      /* Link this seqnum to the block in order to be able to 
        release the block once this packet leave its lifetime */
      g_hash_table_insert (statsman->seqnum_2_redblocks, 
          GUINT_TO_POINTER(pkt->seqnum), block);
      for (gsize i = 0; i < pkt->protects_seqnums->len; ++i) {
        const guint64 data_key = g_array_index (pkt->protects_seqnums,
            guint16, i);
        RedBlock * data_block = NULL;
        if (!g_hash_table_lookup_extended (statsman->seqnum_2_redblocks, 
            GUINT_TO_POINTER(data_key), NULL, (gpointer*)&data_block)) {
          
          g_hash_table_insert (statsman->seqnum_2_redblocks, 
              GUINT_TO_POINTER(data_key), block);
        } else if (block != data_block) {
          /* Overlapped blocks are not supported yet */
          GST_WARNING_OBJECT (statsman->parent, "Data packet %ld covered by two blocks",
              data_key);
          g_hash_table_replace (statsman->seqnum_2_redblocks, 
              GUINT_TO_POINTER(data_key), block);
        }
      }
    }
    const gsize packets_recovered = _redblock_reconsider (statsman, block);
    GST_LOG_OBJECT (statsman->parent, "Reconsider block because of packet #%u, recovered %lu pckt",
        pkt->seqnum, packets_recovered);
  /* Neither RTX nor FEC  */
  } else {
    RedBlock * block;
    if (g_hash_table_lookup_extended (statsman->seqnum_2_redblocks,
        GUINT_TO_POINTER(pkt->seqnum), NULL, (gpointer *)&block)) {

      for (gsize i = 0; i < block->seqs->len; ++i) {
        if (g_array_index (block->seqs, guint16, i) == pkt->seqnum) {
          g_array_index (block->states, TWCCPktState, i) = 
            _better_pkt_state (g_array_index (block->states, TWCCPktState, i),
                               pkt->state);
          break;
        }
      }
      const gsize packets_recovered = _redblock_reconsider (statsman, block);
      GST_LOG_OBJECT (statsman->parent, "Reconsider block because of packet #%u, "
      "recovered %lu pckt", pkt->seqnum, packets_recovered);
    }
  }
}

/******************************************************************************/

TWCCStatsManager *
rtp_twcc_stats_manager_new (GObject *parent)
{
  TWCCStatsManager *statsman = g_new0 (TWCCStatsManager, 1);
  statsman->parent = parent;
  g_object_ref (parent);

  statsman->stats_ctx = twcc_stats_ctx_new ();
  statsman->ssrc_to_seqmap = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_hash_table_destroy);
  statsman->sent_packets = gst_queue_array_new_for_struct (sizeof(SentPacket), 
      PACKETS_HIST_LEN_DEFAULT);
  statsman->sent_packets_size = PACKETS_HIST_LEN_DEFAULT;
  gst_queue_array_set_clear_func (statsman->sent_packets, (GDestroyNotify)_free_sentpacket);
  statsman->sent_packets_feedbacks = gst_queue_array_new (300);
  statsman->stats_ctx_first_seqnum = -1;
  statsman->stats_ctx_by_pt = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) twcc_stats_ctx_free);

  statsman->prev_stat_window_beginning = GST_CLOCK_TIME_NONE;

  statsman->redund_2_redblocks = g_hash_table_new_full (_redund_hash,
      _redund_equal, (GDestroyNotify)_redblock_key_free,
      (GDestroyNotify)_redblock_free);
  statsman->seqnum_2_redblocks = g_hash_table_new_full (g_direct_hash, 
      g_direct_equal, NULL, NULL);

  return statsman;
}

void
rtp_twcc_stats_manager_free (TWCCStatsManager *statsman)
{
  g_object_ref (statsman->parent);
  g_hash_table_destroy (statsman->ssrc_to_seqmap);
  gst_queue_array_free (statsman->sent_packets);
  gst_queue_array_free (statsman->sent_packets_feedbacks);
  g_hash_table_destroy (statsman->stats_ctx_by_pt);
  g_hash_table_destroy (statsman->redund_2_redblocks);
  g_hash_table_destroy (statsman->seqnum_2_redblocks);
  twcc_stats_ctx_free (statsman->stats_ctx);
}

void rtp_twcc_stats_sent_pkt (TWCCStatsManager *statsman,
    RTPPacketInfo * pinfo, GstRTPBuffer *rtp, guint16 twcc_seqnum)
{
  GstBuffer * buf = rtp->buffer;
  SentPacket packet;
  GArray *protect_seqnums_array = NULL;
  guint32 protect_ssrc = 0;
  gint redundant_pkt_idx = -1;
  gint redundant_pkt_num = -1;

  /* In oreder to be able to map rtp seqnum to twcc seqnum in future, we
    store it in certain hash tables (e.g. it might be needed to process
    received feedback on a FEC packet) */
  _register_seqnum (statsman, pinfo->ssrc,
      pinfo->seqnum, twcc_seqnum);

  /* If this packet is RTX/FEC packet, keep track of its meta */
  GstRTPRepairMeta *repair_meta = NULL;
  if ((repair_meta = gst_buffer_get_rtp_repair_meta (buf)) != NULL) {
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
  _sent_pkt_keep_length (statsman, statsman->sent_packets_size, &packet);

  for (guint i = 0; protect_seqnums_array && i < protect_seqnums_array->len; i++) {
    const guint16 prot_seqnum_ = g_array_index (protect_seqnums_array, guint16, i);
    GST_DEBUG_OBJECT (statsman->parent, "%u protects seqnum: %u", twcc_seqnum, prot_seqnum_);
  }

  GST_DEBUG_OBJECT
    (statsman->parent, "Send: twcc-seqnum: %u, seqnum: %u, pt: %u, marker: %d, "
    "redundant_idx: %d, redundant_num: %d, protected_seqnums: %u,"
    "size: %u, ts: %"
    GST_TIME_FORMAT, packet.seqnum, pinfo->seqnum, packet.pt, pinfo->marker,
    packet.redundant_idx, packet.redundant_num,
    packet.protects_seqnums ? packet.protects_seqnums->len : 0,
    packet.size, GST_TIME_ARGS (pinfo->current_time));
}

void rtp_twcc_stats_set_sock_ts (TWCCStatsManager *statsman,
    guint16 seqnum, GstClockTime sock_ts)
{
  SentPacket * pkt = _find_sentpacket (statsman, seqnum);
  if (pkt) {
    pkt->socket_ts = sock_ts;
    GST_LOG_OBJECT (statsman->parent, "packet #%u, setting socket-ts %" GST_TIME_FORMAT,
        seqnum, GST_TIME_ARGS (sock_ts));
  } else {
    GST_WARNING_OBJECT (statsman->parent, "Unable to update send-time for twcc-seqnum #%u", seqnum);
  }
}

void rtp_twcc_manager_tx_start_feedback (TWCCStatsManager *statsman)
{
  statsman->rtt = GST_CLOCK_TIME_NONE;
}

void rtp_twcc_stats_pkt_feedback (TWCCStatsManager *statsman,
    guint16 seqnum, GstClockTime remote_ts, GstClockTime current_time, 
    TWCCPktState status)
{
  SentPacket * found;
  if (!!(found = _find_sentpacket (statsman, seqnum))) {
    /* Do not process feedback on packets we have got feedback previously */
    if (found->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN
      || found->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      found->remote_ts = remote_ts;
      found->state = status;
      gst_queue_array_push_tail (statsman->sent_packets_feedbacks, found);
      GST_LOG_OBJECT (statsman->parent, "matching pkt: #%u with local_ts: %" GST_TIME_FORMAT
          " size: %u, remote-ts: %" GST_TIME_FORMAT, seqnum,
          GST_TIME_ARGS (found->local_ts),
          found->size * 8, GST_TIME_ARGS (remote_ts));

      /* calculate the round-trip time */
      statsman->rtt = GST_CLOCK_DIFF (found->local_ts, current_time);
    } else {
      /* We've got feed back on the packet that was covered with the previous TWCC report.
        Receiver could send two feedbacks on a single packet on purpose, 
        so we just ignore it. */
      GST_LOG_OBJECT (statsman->parent, "Rejecting second feedback on a packet #%u", seqnum);
    }
  } else {
    GST_WARNING_OBJECT (statsman->parent, "Feedback on unknown packet #%u", seqnum);
  }
}

void rtp_twcc_manager_tx_end_feedback (TWCCStatsManager *statsman)
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

  while (!gst_queue_array_is_empty(statsman->sent_packets_feedbacks)) {
    SentPacket * pkt = (SentPacket*)gst_queue_array_pop_head (statsman->sent_packets_feedbacks);
    if (!pkt) {
        continue;
    }
    _process_pkt_feedback (pkt, statsman);
  } /* while fifo of arrays */

  GstClockTime last_ts = twcc_stats_ctx_get_last_local_ts (statsman->stats_ctx);
  if (!GST_CLOCK_TIME_IS_VALID (last_ts))
    return twcc_stats_ctx_get_structure (statsman->stats_ctx);

  /* Prune old packets in stats */
  gint last_seqnum_to_free = -1;
  /* First remove all them from stats structures, and then from sent_packets
    queue at once so as not to lock sent_packets for longer then necessary
  */
  while (!gst_queue_array_is_empty (statsman->stats_ctx->pt_packets)) {
    SentPacket * pkt = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        statsman->stats_ctx->pt_packets))->sentpkt;
    if (gst_queue_array_get_length (statsman->stats_ctx->pt_packets) 
      >= MAX_STATS_PACKETS
      || (pkt && GST_CLOCK_DIFF (pkt->local_ts, last_ts) > PACKETS_HIST_DUR)) {
      if (pkt) {
        if (last_seqnum_to_free >= 0 
            && gst_rtp_buffer_compare_seqnum (pkt->seqnum, last_seqnum_to_free)
              >= 0) {
          GST_WARNING_OBJECT (statsman->parent, "Seqnum reorder in stats pkts");
          g_assert_not_reached ();
        }
        last_seqnum_to_free = pkt->seqnum;
      }
      _rm_last_stats_pkt (statsman);
    } else {
      break;
    }
  }
  /* Remove old packets from sent_packets queue */
  if (last_seqnum_to_free >= 0) {
    while (!gst_queue_array_is_empty (statsman->sent_packets)) {
      SentPacket * pkt = gst_queue_array_peek_head_struct (statsman->sent_packets);
      GST_LOG_OBJECT (statsman->parent, "Freeing sent packet #%u", pkt->seqnum);
      if (gst_rtp_buffer_compare_seqnum (pkt->seqnum, last_seqnum_to_free)
          >= 0) {
        _free_sentpacket (pkt);
        gst_queue_array_pop_head (statsman->sent_packets);
      } else {
        break;
      }
    }
  }

  array = g_value_array_new (0);
  end_time = GST_CLOCK_DIFF (stats_window_delay, last_ts);
  start_time = end_time - stats_window_size;

  GST_DEBUG_OBJECT (statsman->parent,
      "Calculating windowed stats for the window %" GST_STIME_FORMAT
      " starting from %" GST_STIME_FORMAT " to: %" GST_STIME_FORMAT " overall packets: %u",
      GST_STIME_ARGS (stats_window_size), GST_STIME_ARGS (start_time),
      GST_STIME_ARGS (end_time),
      gst_queue_array_get_length (statsman->stats_ctx->pt_packets));

  if (!GST_CLOCK_TIME_IS_VALID(statsman->prev_stat_window_beginning) ||
      GST_CLOCK_DIFF (statsman->prev_stat_window_beginning, start_time) > 0) {
        statsman->prev_stat_window_beginning = start_time;
  }

  twcc_stats_ctx_calculate_windowed_stats (statsman->stats_ctx, start_time,
      end_time);
  ret = twcc_stats_ctx_get_structure (statsman->stats_ctx);
  GST_LOG_OBJECT (statsman->parent, "Full stats: %" GST_PTR_FORMAT, ret);

  g_hash_table_iter_init (&iter, statsman->stats_ctx_by_pt);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStructure *s;
    guint pt = GPOINTER_TO_UINT (key);
    TWCCStatsCtx *ctx = value;
    twcc_stats_ctx_calculate_windowed_stats (ctx, start_time, end_time);
    s = twcc_stats_ctx_get_structure (ctx);
    gst_structure_set (s, "pt", G_TYPE_UINT, pt, NULL);
    _append_structure_to_value_array (array, s);
    GST_LOG_OBJECT (statsman->parent, "Stats for pt %u: %" GST_PTR_FORMAT, pt, s);
  }

  _structure_take_value_array (ret, "payload-stats", array);

  return ret;
}

