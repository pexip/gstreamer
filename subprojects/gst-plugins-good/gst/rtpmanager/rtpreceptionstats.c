/* GStreamer
 * Copyright (C)  2019 Pexip (http://pexip.com/)
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

#include "rtpreceptionstats.h"
#include <gst/gst.h>

typedef struct
{
  guint32 ssrc;
  guint16 *seq;
  gsize seq_len;
} BlockKey;

typedef struct 
{
  GArray *seqs;
  GArray *states;
  guint32 ssrc;

  guint32 fec_ssrc;
  GArray * fec_seqs;
  GArray * fec_states;

  gsize num_redundant_packets;
} Block;

struct _RTPReceptionStats
{
  GHashTable * redund_2_blocks;
  GHashTable * seqnum_2_blocks;

  RTPReceptionStatsRecoverCB recover_cb;
  void * recover_cb_data;
};

static guint block_2_key(guint32 ssrc, guint16 *seq, gsize seq_len);
static guint64 _seqnum_2_key(guint32 ssrc, guint16 seq);
static guint _redund_hash (gconstpointer key);
static gboolean _redund_equal (gconstpointer a, gconstpointer b);

static RTPReceptionPktState 
_update_block (RTPReceptionStats *stats, Block * block);

static Block *
_add_redundant_packet (RTPReceptionStats *stats, guint32 ssrc, guint16 *seq, 
    gsize seq_len, guint32 fec_ssrc, guint16 fec_seq);

static void _block_free(Block *block);
static const BlockKey * _block_key_new (guint32 ssrc, guint16 *seq, gsize seq_len);
static void _block_key_free (BlockKey * key);

/******************************************************************************/

RTPReceptionStats * rtp_reception_stats_new(RTPReceptionStatsRecoverCB recover_cb, 
    void * recover_cb_data)
{
  RTPReceptionStats * res = g_malloc (sizeof (RTPReceptionStats));
  res->redund_2_blocks = g_hash_table_new_full (_redund_hash, _redund_equal,
      (GDestroyNotify)_block_key_free, (GDestroyNotify)_block_free);
  res->seqnum_2_blocks = g_hash_table_new_full (g_direct_hash, 
      g_direct_equal, NULL, NULL);

  res->recover_cb = recover_cb;
  res->recover_cb_data = recover_cb_data;

  return res;
}

void rtp_reception_stats_free(RTPReceptionStats *stats)
{
  g_hash_table_destroy (stats->seqnum_2_blocks);
  g_hash_table_destroy (stats->redund_2_blocks);
  g_free (stats);
}


void rtp_reception_stats_add_redundant_packet(RTPReceptionStats *stats,
   guint32 ssrc, guint16 * seq, gsize seq_len, guint32 fec_ssrc, guint16 fec_seq)
{
  Block * block = _add_redundant_packet (stats, ssrc, seq, seq_len, 
      fec_ssrc, fec_seq);
  for (gsize i = 0; i < seq_len; ++i) {
    guint64 key = _seqnum_2_key(ssrc, seq[i]);
    if (!g_hash_table_contains(stats->seqnum_2_blocks, GUINT_TO_POINTER(key))) {
      g_hash_table_insert(stats->seqnum_2_blocks, GUINT_TO_POINTER(key), block);
    }
  }
  // add fec packets to the index
  guint64 fec_key = _seqnum_2_key(fec_ssrc, fec_seq);
  if (g_hash_table_contains(stats->seqnum_2_blocks, GUINT_TO_POINTER(fec_key))) {
    GST_ERROR ("The FEC packet is already exists in the store, "
      "seqnum: %u, ssrc: %u", fec_seq, fec_ssrc);
    g_assert_not_reached ();
  }
  g_hash_table_insert(stats->seqnum_2_blocks, GUINT_TO_POINTER(fec_key), block);
}

void rtp_reception_stats_update_reception(RTPReceptionStats *stats,
    guint32 ssrc, guint16 seq, gboolean received)
{
  const guint64 key = _seqnum_2_key(ssrc, seq);
  Block * block;
  if (!g_hash_table_lookup_extended (stats->seqnum_2_blocks,
      GUINT_TO_POINTER (key), NULL, (gpointer *)&block)) {
  } else {
    gsize idx = 0;
    // Try to find it among the data packets.
    for (idx = 0; idx < block->seqs->len; idx++) {
      if (g_array_index (block->seqs, guint16, idx) == seq) {
        break;
      }
    }
    if (idx < block->seqs->len) {
      g_array_index (block->states, RTPReceptionPktState, idx) = 
        received ? RTP_RECEPTION_PKT_RECEIVED : RTP_RECEPTION_PKT_LOST;
    // There is no such packet in the data packets, try to find in FEC packets
    } else {
      for (idx = 0; idx < block->fec_seqs->len; idx++) {
       if (g_array_index (block->fec_seqs, guint16, idx) == seq) {
          break;
        }
      }
      if (idx == block->fec_seqs->len) {
        g_assert_not_reached ();
      }
      g_array_index (block->fec_states, RTPReceptionPktState, idx) = 
        received ? RTP_RECEPTION_PKT_RECEIVED : RTP_RECEPTION_PKT_LOST;
    }
    _update_block (stats, block);
  }
}

RTPReceptionPktState rtp_reception_stats_get_reception(RTPReceptionStats *stats,
  guint32 ssrc, guint16 seq)
{
  const guint64 key = _seqnum_2_key(ssrc, seq);
  if (!g_hash_table_contains (stats->seqnum_2_blocks, GUINT_TO_POINTER(key))) {
    GST_WARNING ("Requested status of the data packet which was not yet covered with"
    " any FEC block, seqnum: %u, ssrc: %u", seq, ssrc);
  } else {
    Block * block = g_hash_table_lookup (stats->seqnum_2_blocks,
        GUINT_TO_POINTER(key));
    RTPReceptionPktState state = RTP_RECEPTION_PKT_UNKNOWN;
    gsize idx = 0;
    for (idx = 0; idx < block->seqs->len; idx++) {
      if (g_array_index (block->seqs, guint16, idx) == seq) {
        break;
      }
    }
    // This is a data packet, check the state in the data packets array.
    if (idx < block->seqs->len) {
      state = g_array_index (block->states, RTPReceptionPktState, idx);
    // This is a FEC packet, check the state in the FEC packets array.
    } else {
      for (idx = 0; idx < block->fec_seqs->len; idx++) {
        if (g_array_index (block->fec_seqs, guint16, idx) == seq) {
          break;
        }
      }
      if (idx == block->fec_seqs->len) {
        g_assert_not_reached ();
      }
      state = g_array_index (block->fec_states, RTPReceptionPktState, idx);
    }
    // If the packet was lost, try to see if it could be recovered from others.
    if (state == RTP_RECEPTION_PKT_LOST) {
      return _update_block (stats, block);
    }

    return state;
  }
  return RTP_RECEPTION_PKT_UNKNOWN;
}

/******************************************************************************/

static guint block_2_key(guint32 ssrc, guint16 *seq, gsize seq_len)
{
  guint32 key = ssrc;
  for (gsize i = 0; i < seq_len; i++) {
    key ^= seq[i];
  }
  return key;
}

static guint _redund_hash (gconstpointer key)
{
  BlockKey *bk = (BlockKey *)key;
  return block_2_key(bk->ssrc, bk->seq, bk->seq_len);
}

static gboolean _redund_equal (gconstpointer a, gconstpointer b)
{
  BlockKey *bk1 = (BlockKey *)a;
  BlockKey *bk2 = (BlockKey *)b;
  return bk1->ssrc == bk2->ssrc && bk1->seq_len == bk2->seq_len &&
    memcmp(bk1->seq, bk2->seq, bk1->seq_len * sizeof(guint16)) == 0;
}

static guint64 _seqnum_2_key(guint32 ssrc, guint16 seq)
{
  return ((guint64)ssrc) | ((guint64)seq << 32);
}

static Block *
_find_block_in_list(GList *list, guint32 ssrc, guint16 *seq, gsize seq_len)
{
  for (GList *l = list; l; l = l->next)
  {
    Block *block = l->data;
    if (block->ssrc == ssrc && memcmp(seq, block->seqs->data, seq_len * sizeof(guint16)) == 0)
      return block;
  }
  return NULL;
}

static RTPReceptionPktState 
_update_block (RTPReceptionStats *stats, Block * block)
{

  gsize loss_count = 0;
  for (gsize i = 0; i < block->seqs->len; i++) {
    RTPReceptionPktState cur_state = 
      g_array_index (block->states, RTPReceptionPktState, i);
    // If we don't know the state of any packet in the block, let's
    // return unknown.
    if (cur_state == RTP_RECEPTION_PKT_UNKNOWN) {
      GST_WARNING ("Unknown state of the packet, ssrc: %u, seq: %u", block->ssrc,
          g_array_index (block->seqs, guint16, i));
      return RTP_RECEPTION_PKT_UNKNOWN;
    } else if (cur_state == RTP_RECEPTION_PKT_LOST) {
      loss_count++;
    }
  }
  for (gsize i = 0; i < block->fec_seqs->len; i++) {
    RTPReceptionPktState cur_state = 
      g_array_index (block->fec_states, RTPReceptionPktState, i);
    if (cur_state == RTP_RECEPTION_PKT_UNKNOWN) {
      return RTP_RECEPTION_PKT_UNKNOWN;
    } else if (cur_state == RTP_RECEPTION_PKT_LOST) {
      loss_count++;
    }
  }
  if (loss_count <= block->fec_seqs->len) {
    // All the packets in this block could be recovered.
    for (gsize i = 0; i < block->seqs->len; i++) {
      if (g_array_index (block->states, RTPReceptionPktState, i) == 
          RTP_RECEPTION_PKT_LOST) {
        if (stats->recover_cb) {
          stats->recover_cb (stats->recover_cb_data, 
              g_array_index (block->seqs, guint16, i));
        }
        g_array_index (block->states, RTPReceptionPktState, i) =
          RTP_RECEPTION_PKT_RECOVERED;
      }
    }
    for (gsize i = 0; i < block->fec_seqs->len; i++) {
      if (g_array_index (block->fec_states, RTPReceptionPktState, i) == 
          RTP_RECEPTION_PKT_LOST) {
        if (stats->recover_cb) {
          stats->recover_cb (stats->recover_cb_data,
              g_array_index (block->seqs, guint16, i));
        }

        g_array_index (block->fec_states, RTPReceptionPktState, i) =
          RTP_RECEPTION_PKT_RECOVERED;
      }
    }
    return RTP_RECEPTION_PKT_RECOVERED;
  } else {
    return RTP_RECEPTION_PKT_LOST;
  }

}

static Block *
_block_new(guint32 ssrc, guint16 *seq, gsize seq_len)
{
  Block *block = g_malloc (sizeof (Block));
  block->seqs = g_array_new (FALSE, FALSE, sizeof (guint16));
  g_array_set_size (block->seqs, seq_len);
  block->states = g_array_new (FALSE, FALSE, sizeof (RTPReceptionPktState));
  g_array_set_size (block->states, seq_len);
  for (gsize i = 0; i < seq_len; i++)
  {
    g_array_index (block->seqs, guint16, i) = seq[i];
    g_array_index (block->states, RTPReceptionPktState, i) =
      RTP_RECEPTION_PKT_UNKNOWN;
  }
  block->ssrc = ssrc;
  block->num_redundant_packets = 1;

  block->fec_seqs = g_array_new (FALSE, FALSE, sizeof (guint16));
  block->fec_states = g_array_new (FALSE, FALSE, sizeof (RTPReceptionPktState));
  return block;
}

static void
_block_free(Block *block)
{
  g_array_free (block->seqs, TRUE);
  g_array_free (block->states, TRUE);
  g_array_free (block->fec_seqs, TRUE);
  g_array_free (block->fec_states, TRUE);
  g_free (block);
}

static const BlockKey *
_block_key_new (guint32 ssrc, guint16 *seq, gsize seq_len)
{
  BlockKey *key = g_malloc0 (sizeof(BlockKey));
  key->seq = g_malloc0 (seq_len * sizeof (guint16));
  memcpy (key->seq, seq, seq_len * sizeof (guint16));
  key->seq_len = seq_len;
  key->ssrc = ssrc;

  return key;
}

static void
_block_key_free (BlockKey * key)
{
  g_free (key->seq);
  g_free (key);
}

static Block *
_add_redundant_packet (RTPReceptionStats *stats, guint32 ssrc, guint16 *seq, 
    gsize seq_len, guint32 fec_ssrc, guint16 fec_seq)
{
  const RTPReceptionPktState unknown_state = RTP_RECEPTION_PKT_UNKNOWN;
  const BlockKey * key = _block_key_new (ssrc, seq, seq_len);
  
  Block * block = NULL;
  if (g_hash_table_lookup_extended(stats->redund_2_blocks,
      key, NULL, (gpointer *)&block)) {
        block->num_redundant_packets++;
        if (block->fec_ssrc != fec_ssrc) {
          GST_ERROR ("The FEC SSRC is different from the one already stored, "
            "old: %u, new: %u", block->fec_ssrc, fec_ssrc);
          g_assert_not_reached ();
        }
        g_array_append_val (block->fec_seqs, fec_seq);
        g_array_append_val (block->fec_states, unknown_state);
        _block_key_free (key);
  // No such key in the hash table
  } else {
    block = _block_new (ssrc, seq, seq_len);
    block->fec_ssrc = fec_ssrc;
    g_array_append_val (block->fec_seqs, fec_seq);
    g_array_append_val (block->fec_states, unknown_state);
    g_hash_table_insert(stats->redund_2_blocks, key, block);
  }

  return block;
}
