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
#include <gst/rtp/gstrtprepairmeta.h>
#include <gst/base/gstbitreader.h>
#include <gst/base/gstbitwriter.h>
#include <gst/net/gsttxfeedback.h>
#include <gst/base/gstqueuearray.h>

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

#define PACKETS_HIST_DUR (10 * GST_SECOND)
/* How many packets should fit into the packets history by default.
   Estimated bundle throughput is up to 150 per packets at maximum in average
   circumstances. */
#define PACKETS_HIST_LEN_DEFAULT (300 * PACKETS_HIST_DUR / GST_SECOND)

#define MAX_STATS_PACKETS 1000

typedef enum {
  RTP_TWCC_FECBLOCK_PKT_UNKNOWN,
  RTP_TWCC_FECBLOCK_PKT_RECEIVED,
  RTP_TWCC_FECBLOCK_PKT_RECOVERED,
  RTP_TWCC_FECBLOCK_PKT_LOST
} TWCCPktState;

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
  RTPTWCCPacketStatus status;
  guint16 seqnum;
  GstClockTime remote_ts;
} ParsedPacket;

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

struct _RTPTWCCManager
{
  GObject object;
  GMutex recv_lock;
  GMutex send_lock;

  GHashTable *ssrc_to_seqmap;
  GHashTable *pt_to_twcc_ext_id;

  TWCCStatsCtx *stats_ctx;
  /* The first packet in stats_ctx seqnum, valid even if there is a gap in
   stats_ctx caused feedback packet loss
   */
  gint32 stats_ctx_first_seqnum;
  GHashTable *stats_ctx_by_pt;

  /* In order to keep RingBuffer sizes under control, we assert
      that the old packets we remove from the queues are older than statistics
      window we use.
   */
  GstClockTime prev_stat_window_beginning;

  guint8 send_ext_id;
  guint8 recv_ext_id;
  guint16 send_seqnum;

  guint mtu;
  guint max_packets_per_rtcp;
  GArray *recv_packets;

  guint64 fb_pkt_count;

  /* Array of SentPackets struct */
  GstQueueArray *sent_packets;
  /* Array of GArrays with pointers to SentPackets structs from sent_packets 
    to which twcc feedbacks were received and are waiting to be processed by
    statistics thread.
  */
  GstQueueArray *sent_packets_feedbacks;
  GMutex sent_packets_feedback_lock;
  
  gsize sent_packets_size;
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

  /* Redundancy bookkeeping */
  GHashTable * redund_2_redblocks;
  GHashTable * seqnum_2_redblocks;

  /* The last seqnum which twcc feedback was processed by statistics thread */
  gint32 last_processed_sent_seqnum;
};

/******************************************************************************/
/* Redundancy book keeping subpart
  "Was a certain packet recovered on the receiver side?"
  
  * Organizes sent data packets and redundant packets into blocks
  * Keeps track of redundant packets reception such as RTX and FEC packets
  * Maps all packets to blocks and vice versa
  * Is used to calculate redundancy statistics
 */

static SentPacket * _find_stats_sentpacket (RTPTWCCManager * twcc, guint16 seqnum);
static SentPacket * _find_sentpacket (RTPTWCCManager * twcc, guint16 seqnum);

typedef GArray* RedBlockKey;

typedef struct 
{
  GArray *seqs;
  GArray *states;

  GArray * fec_seqs;
  GArray * fec_states;

  gsize num_redundant_packets;
} RedBlock;

static guint _redund_hash (gconstpointer key);
static gboolean _redund_equal (gconstpointer a, gconstpointer b);

static RedBlock *
_redblock_new(GArray* seq, guint16 fec_seq,
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

static void
_redblock_free(RedBlock *block)
{
  g_array_unref (block->seqs);
  g_array_free (block->states, TRUE);
  g_array_free (block->fec_seqs, TRUE);
  g_array_free (block->fec_states, TRUE);
  g_free (block);
}

static RedBlockKey
_redblock_key_new (GArray * seqs)
{
  return g_array_ref (seqs);
}

static void
_redblock_key_free (RedBlockKey key)
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
static gsize
_redblock_reconsider (RTPTWCCManager * twcc, RedBlock * block)
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
    SentPacket *pkt = _find_stats_sentpacket (twcc,
        g_array_index (block->seqs, guint16, 0));
    
    if (pkt && pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      for (gsize i = 0; i < block->fec_seqs->len; ++i) {
        SentPacket *pkt = _find_stats_sentpacket (twcc,
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
    SentPacket *pkt = _find_stats_sentpacket (twcc,
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
    SentPacket *pkt = _find_stats_sentpacket (twcc,
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
      SentPacket *pkt = _find_stats_sentpacket (twcc,
          g_array_index (block->seqs, guint16, i));
      if (pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
        pkt->state = RTP_TWCC_FECBLOCK_PKT_RECOVERED;
        nrecovered++;
      }
    }
    for (gsize i = 0; i < block->fec_seqs->len; ++i) {
      SentPacket *pkt = _find_stats_sentpacket (twcc,
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
    GST_DEBUG ("Not enough starts to do a window");
    return FALSE;
  }

  for (i = 0; i < array_length; i++) {
    SentPacket *pkt = ((StatsPktPtr*)gst_queue_array_peek_nth_struct (array, i))
        ->sentpkt;
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
    SentPacket *pkt = ((StatsPktPtr*)gst_queue_array_peek_nth_struct (array, idx))
        ->sentpkt;
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

static void _rm_last_stats_pkt (RTPTWCCManager * twcc);

static gboolean
twcc_stats_ctx_calculate_windowed_stats (RTPTWCCManager * twcc, TWCCStatsCtx * ctx,
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
      ((StatsPktPtr*)gst_queue_array_peek_nth_struct (packets, start_idx))
        ->sentpkt->pt,
      ((StatsPktPtr*)gst_queue_array_peek_nth_struct (packets, start_idx))
        ->sentpkt->seqnum,
      ((StatsPktPtr*)gst_queue_array_peek_nth_struct (packets,
          packets_sent + start_idx - 1))->sentpkt->seqnum);
  }

  for (i = 0; i < packets_sent; i++) {
    SentPacket *prev = NULL;
    if (i + start_idx >= 1)
      prev = ((StatsPktPtr*)gst_queue_array_peek_nth_struct (packets,
          i + start_idx))->sentpkt;

    SentPacket *pkt = ((StatsPktPtr*)gst_queue_array_peek_nth_struct (packets,
      i + start_idx))->sentpkt;
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
      GST_LOG ("Packet #%u is lost and recovered");
      packets_lost++;
      packets_recovered++;
    } else if(pkt->state == RTP_TWCC_FECBLOCK_PKT_LOST) {
      GST_LOG ("Packet #%u is lost");
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
_idx_sentpacket (RTPTWCCManager * twcc, guint16 seqnum)
{
  const gint idx = gst_rtp_buffer_compare_seqnum (
      (guint16)twcc->stats_ctx_first_seqnum, seqnum);
  if (twcc->stats_ctx_first_seqnum >= 0 && idx >= 0) {
    return idx;
  } else {
    return -1;
  }
}

static SentPacket *
_find_stats_sentpacket (RTPTWCCManager * twcc, guint16 seqnum)
{
  gint idx = _idx_sentpacket (twcc, seqnum);
  SentPacket * res = NULL;
  if (idx >= 0 && idx < gst_queue_array_get_length (twcc->stats_ctx->pt_packets)) {
    res = ((StatsPktPtr*)gst_queue_array_peek_nth_struct (twcc->stats_ctx->pt_packets, idx))
        ->sentpkt;
  }

  return res;
}

static TWCCStatsCtx *
_get_ctx_for_pt (RTPTWCCManager * twcc, guint pt);

static void
twcc_stats_ctx_add_packet (RTPTWCCManager * twcc, SentPacket * pkt)
{
  if (!pkt) {
    return;
  } else if (gst_queue_array_is_empty (twcc->stats_ctx->pt_packets)) {
    gst_queue_array_push_tail_struct (twcc->stats_ctx->pt_packets,
        (StatsPktPtr*)&pkt);
    TWCCStatsCtx *ctx = _get_ctx_for_pt (twcc, pkt->pt);
    gst_queue_array_push_tail_struct (ctx->pt_packets, (StatsPktPtr*)&pkt);
    twcc->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = pkt;
    twcc->stats_ctx_first_seqnum = pkt->seqnum;
  } else {
    const gint idx = _idx_sentpacket (twcc, pkt->seqnum);
    GstQueueArray * main_array = twcc->stats_ctx->pt_packets;
    if (idx < 0) {
      GST_WARNING ("Packet #%u is too old for stats, dropping, latest pkt is #%u",
        pkt->seqnum, twcc->stats_ctx_first_seqnum);
      return;
    } else if (idx >= gst_queue_array_get_length (main_array)) {
      const gsize n2push = idx - gst_queue_array_get_length (main_array);
      /* if n2push > 0 means that the last twcc feedback packet[s] is lost,
       */
      for (gsize i = 0; i < n2push; i++) {
        gst_queue_array_push_tail_struct (main_array, 
            &null_statspktptr);
      }
      gst_queue_array_push_tail_struct (main_array, (StatsPktPtr*)&pkt);
      TWCCStatsCtx *ctx = _get_ctx_for_pt (twcc, pkt->pt);
      gst_queue_array_push_tail_struct (ctx->pt_packets, (StatsPktPtr*)&pkt);
      twcc->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = pkt;
    } else {
      /* TWCC packets reordered -- do nothing */
      GST_WARNING ("Packet #%u is out of order, not going to stats", pkt->seqnum);
    }
  }
}

/******************************************************/

static SentPacket *
_find_sentpacket (RTPTWCCManager * twcc, guint16 seqnum)
{
  if (gst_queue_array_is_empty (twcc->sent_packets) == TRUE) {
    return NULL;
  }

  g_mutex_lock (&twcc->send_lock);

  SentPacket * first = gst_queue_array_peek_head_struct (twcc->sent_packets);
  SentPacket * result;

  const gint idx = gst_rtp_buffer_compare_seqnum (first->seqnum, seqnum);
  if (idx < gst_queue_array_get_length (twcc->sent_packets) && idx >= 0) {
    result = (SentPacket*)
        gst_queue_array_peek_nth_struct (twcc->sent_packets, idx);
  } else {
    result = NULL;
  }

  g_mutex_unlock (&twcc->send_lock);

  if (result && result->seqnum == seqnum) {
    return result;
  }

  return NULL;
}

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
  twcc->sent_packets_size = PACKETS_HIST_LEN_DEFAULT;
  twcc->sent_packets = gst_queue_array_new_for_struct (sizeof(SentPacket), 
      twcc->sent_packets_size);
  gst_queue_array_set_clear_func (twcc->sent_packets, (GDestroyNotify)_free_sentpacket);
  twcc->sent_packets_feedbacks = gst_queue_array_new (60);
  gst_queue_array_set_clear_func (twcc->sent_packets_feedbacks,
      (GDestroyNotify)g_array_unref);
  g_mutex_init (&twcc->sent_packets_feedback_lock);
  
  twcc->parsed_packets = g_array_new (FALSE, FALSE, sizeof (ParsedPacket));
  g_mutex_init (&twcc->recv_lock);
  g_mutex_init (&twcc->send_lock);

  twcc->rtcp_buffers = g_queue_new ();

  twcc->stats_ctx = twcc_stats_ctx_new ();
  twcc->stats_ctx_first_seqnum = -1;
  twcc->stats_ctx_by_pt = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) twcc_stats_ctx_free);

  twcc->prev_stat_window_beginning = GST_CLOCK_TIME_NONE;

  twcc->recv_media_ssrc = -1;
  twcc->recv_sender_ssrc = -1;

  twcc->first_fci_parse = TRUE;

  twcc->feedback_interval = GST_CLOCK_TIME_NONE;
  twcc->next_feedback_send_time = GST_CLOCK_TIME_NONE;
  twcc->last_report_time = GST_CLOCK_TIME_NONE;

  twcc->redund_2_redblocks = g_hash_table_new_full (_redund_hash, _redund_equal,
      (GDestroyNotify)_redblock_key_free, (GDestroyNotify)_redblock_free);
  twcc->seqnum_2_redblocks = g_hash_table_new_full (g_direct_hash, 
      g_direct_equal, NULL, NULL);

  twcc->last_processed_sent_seqnum = -1;
}

static void
rtp_twcc_manager_finalize (GObject * object)
{
  RTPTWCCManager *twcc = RTP_TWCC_MANAGER_CAST (object);

  g_hash_table_destroy (twcc->ssrc_to_seqmap);
  g_hash_table_destroy (twcc->pt_to_twcc_ext_id);

  g_array_unref (twcc->recv_packets);
  gst_queue_array_free (twcc->sent_packets);
  gst_queue_array_free (twcc->sent_packets_feedbacks);
  g_array_unref (twcc->parsed_packets);
  g_queue_free_full (twcc->rtcp_buffers, (GDestroyNotify) gst_buffer_unref);
  g_mutex_clear (&twcc->recv_lock);
  g_mutex_clear (&twcc->send_lock);
  g_mutex_clear (&twcc->sent_packets_feedback_lock);

  g_hash_table_destroy (twcc->stats_ctx_by_pt);
  g_hash_table_destroy (twcc->seqnum_2_redblocks);
  g_hash_table_destroy (twcc->redund_2_redblocks);
  
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
    ctx->last_pkt_fb = NULL;
  }
  return ctx;
}

static void
_rm_last_packet_from_stats_arrays (RTPTWCCManager * twcc)
{
  SentPacket * head = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        twcc->stats_ctx->pt_packets))->sentpkt;
  if (head) {
    TWCCStatsCtx * ctx = _get_ctx_for_pt (twcc, head->pt);
    SentPacket * ctx_pkt = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        ctx->pt_packets))->sentpkt;
    if (!ctx_pkt || ctx_pkt->seqnum != head->seqnum) {
      GST_WARNING ("Attempting to remove packet from pt stats context "
      "which seqnum does not match the main stats context seqnum, "
          "main: #%u, pt: %u, context packet: #%u, pt: %u",
            head->seqnum, head->pt, ctx_pkt ? ctx_pkt->seqnum : -1, ctx_pkt ? ctx_pkt->pt : -1);
      g_assert_not_reached ();
    }
    if (ctx->last_pkt_fb == head) {
      twcc->stats_ctx->last_pkt_fb = ctx->last_pkt_fb = NULL;
    }
    gst_queue_array_pop_head_struct (ctx->pt_packets);
    GST_LOG ("Removing packet #%u from stats context, ts: %" GST_STIME_FORMAT,
        head->seqnum, head->local_ts);
  }
  gst_queue_array_pop_head_struct (twcc->stats_ctx->pt_packets);
  twcc->stats_ctx_first_seqnum++;
}

static void
_rm_last_stats_pkt (RTPTWCCManager * twcc)
{
  SentPacket * head = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        twcc->stats_ctx->pt_packets))->sentpkt;
  /* If this packet maps to a block in hash tables -- remove every links 
  leading to this block as well as this packet: as we will remove this packet
  from the context, we will not be able to use this block anyways. */
  RedBlock * block = NULL;
  if (head && g_hash_table_lookup_extended (twcc->seqnum_2_redblocks,
      GUINT_TO_POINTER(head->seqnum), NULL, (gpointer *)&block)) {
        RedBlockKey key = _redblock_key_new (block->seqs);
        for (gsize i = 0; i < block->seqs->len; i++) {
          g_hash_table_remove (twcc->seqnum_2_redblocks,
              GUINT_TO_POINTER(g_array_index (block->seqs, guint16, i)));
        }
        for (gsize i = 0; i < block->fec_seqs->len; i++) {
          g_hash_table_remove (twcc->seqnum_2_redblocks,
              GUINT_TO_POINTER(g_array_index (block->fec_seqs, guint16, i)));
        }
        g_hash_table_remove (twcc->redund_2_redblocks, key);
        _redblock_key_free (key);
  }
  _rm_last_packet_from_stats_arrays (twcc);
}

static gint32
_lookup_seqnum (RTPTWCCManager * twcc, guint32 ssrc, guint16 seqnum)
{
  gint32 ret = -1;

  GHashTable *seq_to_twcc =
      g_hash_table_lookup (twcc->ssrc_to_seqmap, GUINT_TO_POINTER (ssrc));
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

/**
  * Set TWCC seqnum and remember the packet for statistics.
  * 
  * Fill in SentPacket structure with the seqnum and the packet info,
  * and add it to the sent_packets queue, which is used by the statistics thread
  *
  * NB: protect_seqnum is a reference of the GstBuffer's meta!
  *
  * NB: at the moment protect_seqnum contains internal seqnum,
     they will be replaced with twcc seqnums in-place in statistics thread!
  */
static void
_set_twcc_seqnum_data (RTPTWCCManager * twcc, RTPPacketInfo * pinfo,
    GstBuffer * buf, guint8 ext_id)
{
  SentPacket packet;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  gpointer data;
  guint16 seqnum;
  GArray *protect_seqnums_array = NULL;
  guint32 protect_ssrc = 0;
  gint redundant_pkt_idx = -1;
  gint redundant_pkt_num = -1;

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

  /* In oreder to be able to map rtp seqnum to twcc seqnum in future, we
    store it in certain hash tables (e.g. it might be needed to process
    received feedback on a FEC packet) */
  rtp_twcc_manager_register_seqnum (twcc, pinfo->ssrc, pinfo->seqnum,
          seqnum);

  /* If this packet is RTX/FEC packet, keep track of its meta */
  GstRTPRepairMeta *repair_meta = NULL;
  if (repair_meta = gst_buffer_get_rtp_repair_meta (buf)) {
    protect_ssrc = repair_meta->ssrc;
    protect_seqnums_array = g_array_ref (repair_meta->seqnums);
    redundant_pkt_idx = repair_meta->idx_red_packets;
    redundant_pkt_num = repair_meta->num_red_packets;
  }
  
  sent_packet_init (&packet, seqnum, pinfo, &rtp, 
      redundant_pkt_idx, redundant_pkt_num,
      protect_ssrc, protect_seqnums_array);

  for (guint i = 0; protect_seqnums_array && i < protect_seqnums_array->len; i++) {
    const guint16 prot_seqnum_ = g_array_index (protect_seqnums_array, guint16, i);
    GST_DEBUG_OBJECT(twcc, "%u protects seqnum: %u", seqnum, prot_seqnum_);
  }
  gst_rtp_buffer_unmap (&rtp);

  {
    g_mutex_lock (&twcc->send_lock);
    if (gst_queue_array_get_length(twcc->sent_packets) >= twcc->sent_packets_size) {
      /* It could mean that statistics was not called   at all, asumming that
        the packet was not referenced anywhere else, we can drop it.
       */
      GstClockTime pkt_ts = 
          ((SentPacket*)gst_queue_array_peek_head_struct (twcc->sent_packets))
            ->local_ts;
      if (GST_CLOCK_TIME_IS_VALID(twcc->prev_stat_window_beginning) &&
          GST_CLOCK_DIFF (pkt_ts, twcc->prev_stat_window_beginning) 
              < 0) {
          GST_WARNING_OBJECT (twcc, "sent_packets FIFO overflows, dropping");
          g_assert_not_reached ();
      } else if (GST_CLOCK_TIME_IS_VALID(twcc->prev_stat_window_beginning) &&
        GST_CLOCK_DIFF (pkt_ts, twcc->prev_stat_window_beginning)
          < GST_MSECOND * 250) {
          GST_WARNING_OBJECT (twcc, "Risk of"
            " underrun of sent_packets FIFO");
      }
      gst_queue_array_pop_head_struct (twcc->sent_packets);
    }
    gst_queue_array_push_tail_struct (twcc->sent_packets, &packet);
    g_mutex_unlock (&twcc->send_lock);
  }

  gst_buffer_add_tx_feedback_meta (pinfo->data, seqnum,
      GST_TX_FEEDBACK_CAST (twcc));

  GST_DEBUG_OBJECT
      (twcc, "Send: twcc-seqnum: %u, seqnum: %u, pt: %u, marker: %d, "
      "redundant_idx: %d, redundant_num: %d, protected_seqnums: %u,"
      "size: %u, ts: %"
      GST_TIME_FORMAT, packet.seqnum, pinfo->seqnum, packet.pt, pinfo->marker,
      packet.redundant_idx, packet.redundant_num,
      packet.protects_seqnums ? packet.protects_seqnums->len : 0,
      packet.size, GST_TIME_ARGS (pinfo->current_time));
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

/* must be called with recv_lock */
static void
rtp_twcc_manager_add_fci_unlocked (RTPTWCCManager * twcc,
    GstRTCPPacket * packet)
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
    if (i == 0) {
      pkt->missing_run = 0;
    } else {
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

/* must be called with the recv_lock */
static void
rtp_twcc_manager_create_feedback_unlocked (RTPTWCCManager * twcc)
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

  rtp_twcc_manager_add_fci_unlocked (twcc, &packet);

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

  g_mutex_lock (&twcc->recv_lock);

  /* if this packet would exceed the capacity of our MTU, we create a feedback
     with the current packets, and start over with this one */
  if (_exceeds_max_packets (twcc, seqnum)) {
    GST_INFO ("twcc-seqnum: %u would overflow max packets: %u, create feedback"
        " with current packets", seqnum, twcc->max_packets_per_rtcp);
    rtp_twcc_manager_create_feedback_unlocked (twcc);
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
        g_mutex_unlock (&twcc->recv_lock);
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
    rtp_twcc_manager_create_feedback_unlocked (twcc);
    send_feedback = TRUE;

    twcc->packet_count_no_marker = 0;
  }

  g_mutex_unlock (&twcc->recv_lock);

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
    if (twcc->recv_packets->len > 0) {
      g_mutex_lock (&twcc->recv_lock);
      rtp_twcc_manager_create_feedback_unlocked (twcc);
      g_mutex_unlock (&twcc->recv_lock);
    }
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
  const guint16 seqnum = (guint16) buffer_id;
  SentPacket *pkt = _find_sentpacket (twcc, seqnum);

  if (pkt) {
    pkt->socket_ts = ts;
    GST_LOG ("packet #%u, setting socket-ts %" GST_TIME_FORMAT,
        seqnum, GST_TIME_ARGS (ts));
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
  guint16 run_length = 0;
  guint8 status_code = 0;
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
  GstClockTimeDiff rtt = GST_CLOCK_STIME_NONE;
  SentPacket * found;

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

  ts_rounded = base_time;
  GArray * pkt_2_stats = g_array_sized_new (FALSE, FALSE, 
      sizeof (SentPacket*), twcc->parsed_packets->len);
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

      GST_DEBUG_OBJECT ( twcc, "pkt: #%u, remote_ts: %" GST_TIME_FORMAT
          " delta_ts: %" GST_STIME_FORMAT
          " status: %u", pkt->seqnum,
          GST_TIME_ARGS (pkt->remote_ts), GST_STIME_ARGS (delta_ts),
          pkt->status);
      _add_parsed_packet_to_value_array (array, pkt);
    } else {
      GST_DEBUG_OBJECT ( twcc, "pkt: #%u, remote_ts: 0 delta_ts: 0 status: %u",
          pkt->seqnum, pkt->status);
    }
    /* Do not process feedback on packets we have got feedback previously */
    if (!!(found = _find_sentpacket (twcc, pkt->seqnum)) 
        && (found->state == RTP_TWCC_FECBLOCK_PKT_UNKNOWN
          || found->state == RTP_TWCC_FECBLOCK_PKT_LOST)
        ) {
      found->remote_ts = pkt->remote_ts;
      found->state = pkt->status == RTP_TWCC_PACKET_STATUS_NOT_RECV
          ? RTP_TWCC_FECBLOCK_PKT_LOST : RTP_TWCC_FECBLOCK_PKT_RECEIVED;
      g_array_append_vals (pkt_2_stats, &found, 1);
      GST_LOG ("matching pkt: #%u with local_ts: %" GST_TIME_FORMAT
          " size: %u, remote-ts: %" GST_TIME_FORMAT, pkt->seqnum,
          GST_TIME_ARGS (found->local_ts),
          found->size * 8, GST_TIME_ARGS (pkt->remote_ts));

      /* calculate the round-trip time */
      rtt = GST_CLOCK_DIFF (found->local_ts, current_time);
    }
  }
  {
    g_mutex_lock (&twcc->sent_packets_feedback_lock);
    if (gst_queue_array_get_length (twcc->sent_packets_feedbacks) == 60) {
      /*  Valid prev_stat_window_beginning value means statistics are being
          requested, and as sent_packets_feedbacks FIFO is overflow lead to
          data leakage.
      */
      if (GST_CLOCK_TIME_IS_VALID(twcc->prev_stat_window_beginning)) {
        GST_WARNING_OBJECT (twcc, "sent_packets_feedbacks is overflown");
      }
      gst_queue_array_pop_head (twcc->sent_packets_feedbacks);
    }
    gst_queue_array_push_tail (twcc->sent_packets_feedbacks, pkt_2_stats);
    g_mutex_unlock (&twcc->sent_packets_feedback_lock);
  }

  if (GST_CLOCK_STIME_IS_VALID (rtt))
    twcc->avg_rtt = WEIGHT (rtt, twcc->avg_rtt, 0.1);
  twcc->last_report_time = current_time;

  _structure_take_value_array (ret, "packets", array);

  return ret;
}

/* Once we've got feedback on a packet, we need to account it in the internal
  structures. */
*/
static void
_prtocess_pkt_feedback (SentPacket * pkt, RTPTWCCManager * twcc)
{
  if (pkt->stats_processed) {
    /* This packet was already added to stats structures, but we've got 
        one more feedback for it
      */
    RedBlock * block;
    if (g_hash_table_lookup_extended (twcc->seqnum_2_redblocks,
        GUINT_TO_POINTER(pkt->seqnum), NULL, (gpointer *)&block)) {
      const gsize packets_recovered = _redblock_reconsider (twcc, block);
      if (packets_recovered > 0) {
        GST_LOG ("Reconsider block because of packet #%u, "
        "recovered %lu pckt", pkt->seqnum, packets_recovered);
      }
    }
    return;
  }
  pkt->stats_processed = TRUE;
  GST_LOG ("Processing #%u packet in stats, state: %s", pkt->seqnum,
    _pkt_state_s (pkt->state));

  twcc_stats_ctx_add_packet (twcc, pkt);

  /* This is either RTX or FEC packet */
  if (pkt->protects_seqnums && pkt->protects_seqnums->len > 0) {
    /* We are expecting non-twcc seqnums in the buffer's meta here, so
      change them to twcc seqnums. */

    if (pkt->redundant_idx < 0 || pkt->redundant_num <= 0
        || pkt->redundant_idx >= pkt->redundant_num) {
          GST_ERROR ("Invalid FEC packet: idx: %d, num: %d",
              pkt->redundant_idx, pkt->redundant_num);
          g_assert_not_reached ();
    }
    
    for (gsize i = 0; i < pkt->protects_seqnums->len; i++) {
      const guint16 prot_seqnum = g_array_index (pkt->protects_seqnums,
          guint16, i);
      gint32 twcc_seqnum = _lookup_seqnum (twcc, pkt->protects_ssrc,
          prot_seqnum);
      if (twcc_seqnum != -1) {
        g_array_index (pkt->protects_seqnums, guint16, i) 
            = (guint16)twcc_seqnum;
      }
      GST_LOG ("FEC sn: #%u covers twcc sn: #%u, orig sn: %u",
          pkt->seqnum, twcc_seqnum, prot_seqnum);
    }
  
    /* Check if this packet covers the same block that was already added. */
    RedBlockKey key = _redblock_key_new (pkt->protects_seqnums);
    RedBlock * block = NULL;
    if (g_hash_table_lookup_extended (twcc->redund_2_redblocks, key, NULL,
        (gpointer*)&block)) {
      /* Add redundant packet to the existent block */
      if (block->fec_seqs->len != pkt->redundant_num
          || block->fec_states->len != pkt->redundant_num
          || g_array_index (block->fec_seqs, guint16, (gsize)pkt->redundant_idx) != 0
          || g_array_index (block->fec_states, TWCCPktState, (gsize)pkt->redundant_idx) != RTP_TWCC_FECBLOCK_PKT_UNKNOWN) {

        GST_WARNING_OBJECT (twcc, "Got contradictory FEC block: "
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
      g_hash_table_insert (twcc->seqnum_2_redblocks, 
          GUINT_TO_POINTER(pkt->seqnum), block);
    } else {
      /* Add every data packet into seqnum_2_redblocks  */
      block = _redblock_new (pkt->protects_seqnums, pkt->seqnum,
          pkt->redundant_idx, pkt->redundant_num);
      g_array_index (block->fec_seqs, guint16, (gsize)pkt->redundant_idx) 
              = pkt->seqnum;
      g_array_index (block->fec_states, TWCCPktState,
              (gsize)pkt->redundant_idx) = pkt->state;
      g_hash_table_insert (twcc->redund_2_redblocks, key, block);
      /* Link this seqnum to the block in order to be able to 
        release the block once this packet leave its lifetime */
      g_hash_table_insert (twcc->seqnum_2_redblocks, 
          GUINT_TO_POINTER(pkt->seqnum), block);
      for (gsize i = 0; i < pkt->protects_seqnums->len; ++i) {
        const guint64 data_key = g_array_index (pkt->protects_seqnums,
            guint16, i);
        RedBlock * data_block = NULL;
        if (!g_hash_table_lookup_extended (twcc->seqnum_2_redblocks, 
            GUINT_TO_POINTER(data_key), NULL, (gpointer*)&data_block)) {
          
          g_hash_table_insert (twcc->seqnum_2_redblocks, 
              GUINT_TO_POINTER(data_key), block);
        } else if (block != data_block) {
          /* Overlapped blocks are not supported yet */
          GST_WARNING_OBJECT (twcc, "Data packet %ld covered by two blocks",
              data_key);
          g_hash_table_replace (twcc->seqnum_2_redblocks, 
              GUINT_TO_POINTER(data_key), block);
        }
      }
    }
    const gsize packets_recovered = _redblock_reconsider (twcc, block);
    GST_LOG ("Reconsider block because of packet #%u, recovered %lu pckt",
        pkt->seqnum, packets_recovered);
  /* Neither RTX nor FEC  */
  } else {
    RedBlock * block;
    if (g_hash_table_lookup_extended (twcc->seqnum_2_redblocks,
        GUINT_TO_POINTER(pkt->seqnum), NULL, (gpointer *)&block)) {

      for (gsize i = 0; i < block->seqs->len; ++i) {
        if (g_array_index (block->seqs, guint16, i) == pkt->seqnum) {
          g_array_index (block->states, TWCCPktState, i) = 
            _better_pkt_state (g_array_index (block->states, TWCCPktState, i),
                               pkt->state);
          break;
        }
      }
      const gsize packets_recovered = _redblock_reconsider (twcc, block);
      GST_LOG ("Reconsider block because of packet #%u, "
      "recovered %lu pckt", pkt->seqnum, packets_recovered);
    }
  }
}

GstStructure *
rtp_twcc_manager_get_windowed_stats (RTPTWCCManager * twcc,
    GstClockTime stats_window_size, GstClockTime stats_window_delay)
{
  GstStructure *ret;
  GValueArray *array;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  GstClockTimeDiff start_time;
  GstClockTimeDiff end_time;

  while (!gst_queue_array_is_empty(twcc->sent_packets_feedbacks)) {
    g_mutex_lock (&twcc->sent_packets_feedback_lock);
    GArray * psentpkts = gst_queue_array_pop_head (twcc->sent_packets_feedbacks);
    g_mutex_unlock (&twcc->sent_packets_feedback_lock);
    for (gsize i  = 0; i < psentpkts->len; i++) {
      SentPacket * pkt = (SentPacket*)g_array_index (psentpkts, SentPacket*, i);
      if (!pkt) {
        continue;
      }
      prtocess_pkt_feedback (pkt, twcc);
    } /* for array */
    g_array_unref (psentpkts);
  } /* while fifo of arrays */

  GstClockTime last_ts = twcc_stats_ctx_get_last_local_ts (twcc->stats_ctx);
  if (!GST_CLOCK_TIME_IS_VALID (last_ts))
    return twcc_stats_ctx_get_structure (twcc->stats_ctx);

  /* Prune old packets in stats */
  gint last_seqnum_to_free = -1;
  /* First remove all them from stats structures, and then from sent_packets
    queue at once so as not to lock sent_packets for longer then necessary
  */
  while (!gst_queue_array_is_empty (twcc->stats_ctx->pt_packets)) {
    SentPacket * pkt = ((StatsPktPtr*)gst_queue_array_peek_head_struct (
        twcc->stats_ctx->pt_packets))->sentpkt;
    if (gst_queue_array_get_length (twcc->stats_ctx->pt_packets) 
      >= MAX_STATS_PACKETS
      || (pkt && GST_CLOCK_DIFF (pkt->local_ts, last_ts) > PACKETS_HIST_DUR)) {
      if (pkt) {
        if (last_seqnum_to_free >= 0 
            && gst_rtp_buffer_compare_seqnum (pkt->seqnum, last_seqnum_to_free)
              >= 0) {
          GST_WARNING_OBJECT (twcc, "Seqnum reorder in stats pkts");
          g_assert_not_reached ();
        }
        last_seqnum_to_free = pkt->seqnum;
      }
      _rm_last_stats_pkt (twcc);
    } else {
      break;
    }
  }
  /* Remove old packets from sent_packets queue */
  if (last_seqnum_to_free >= 0) {
    g_mutex_lock (&twcc->send_lock);
    while (!gst_queue_array_is_empty (twcc->sent_packets)) {
      SentPacket * pkt = gst_queue_array_peek_head_struct (twcc->sent_packets);
      GST_LOG_OBJECT (twcc, "Freeing sent packet #%u", pkt->seqnum);
      if (gst_rtp_buffer_compare_seqnum (pkt->seqnum, last_seqnum_to_free)
          >= 0) {
        _free_sentpacket (pkt);
        gst_queue_array_pop_head (twcc->sent_packets);
      } else {
        break;
      }
    }
    g_mutex_unlock (&twcc->send_lock);
  }

  array = g_value_array_new (0);
  end_time = GST_CLOCK_DIFF (stats_window_delay, last_ts);
  start_time = end_time - stats_window_size;

  GST_DEBUG_OBJECT (twcc,
      "Calculating windowed stats for the window %" GST_STIME_FORMAT
      " starting from %" GST_STIME_FORMAT " to: %" GST_STIME_FORMAT " overall packets: %u",
      GST_STIME_ARGS (stats_window_size), GST_STIME_ARGS (start_time),
      GST_STIME_ARGS (end_time),
      gst_queue_array_get_length (twcc->stats_ctx->pt_packets));

  if (!GST_CLOCK_TIME_IS_VALID(twcc->prev_stat_window_beginning) ||
      GST_CLOCK_DIFF (twcc->prev_stat_window_beginning, start_time) > 0) {
        twcc->prev_stat_window_beginning = start_time;
  }

  twcc_stats_ctx_calculate_windowed_stats (twcc, twcc->stats_ctx, start_time,
      end_time);
  ret = twcc_stats_ctx_get_structure (twcc->stats_ctx);
  GST_LOG ("Full stats: %" GST_PTR_FORMAT, ret);

  g_hash_table_iter_init (&iter, twcc->stats_ctx_by_pt);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    GstStructure *s;
    guint pt = GPOINTER_TO_UINT (key);
    TWCCStatsCtx *ctx = value;
    twcc_stats_ctx_calculate_windowed_stats (twcc, ctx, start_time, end_time);
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