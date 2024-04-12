/* GStreamer
 * Copyright (C) 2024 Pexip (http://pexip.com/)
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

#ifndef __RTP_RECEPRION_STATS_H__
#define __RTP_RECEPRION_STATS_H__

#include <glib.h>

typedef enum {
  RTP_RECEPTION_PKT_UNKNOWN,
  RTP_RECEPTION_PKT_RECEIVED,
  RTP_RECEPTION_PKT_RECOVERED,
  RTP_RECEPTION_PKT_LOST
} RTPReceptionPktState;

struct _RTPReceptionStats;
typedef struct _RTPReceptionStats RTPReceptionStats;

/* Callback that been called every time a packet is detected to be recovered. */
typedef void (*RTPReceptionStatsRecoverCB)(void*, guint16);

RTPReceptionStats * rtp_reception_stats_new (
  RTPReceptionStatsRecoverCB recover_cb, void * recover_cb_data);
void rtp_reception_stats_free(RTPReceptionStats *stats);

void rtp_reception_stats_add_redundant_packet(RTPReceptionStats *stats,
   guint32 ssrc, guint16 * seq, gsize seq_len, guint32 fec_ssrc, guint16 fec_seq);

void rtp_reception_stats_update_reception(RTPReceptionStats *stats,
    guint32 ssrc, guint16 seq, gboolean received);

RTPReceptionPktState rtp_reception_stats_get_reception(RTPReceptionStats *stats,
    guint32 ssrc, guint16 seq);

#endif /* __RTP_RECEPRION_STATS_H__ */
