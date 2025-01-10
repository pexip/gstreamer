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

#ifndef __RTP_TWCC_STATS_H__
#define __RTP_TWCC_STATS_H__

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include "rtpstats.h"

typedef enum {
  RTP_TWCC_FECBLOCK_PKT_UNKNOWN,
  RTP_TWCC_FECBLOCK_PKT_RECEIVED,
  RTP_TWCC_FECBLOCK_PKT_RECOVERED,
  RTP_TWCC_FECBLOCK_PKT_LOST
} TWCCPktState;

struct _TWCCStatsManager;
typedef struct _TWCCStatsManager TWCCStatsManager;

TWCCStatsManager *rtp_twcc_stats_manager_new (GObject *parent);
void rtp_twcc_stats_manager_free (TWCCStatsManager *stats_manager);

void rtp_twcc_stats_sent_pkt (TWCCStatsManager *stats_manager,
    RTPPacketInfo * pinfo, GstRTPBuffer *rtp, guint16 twcc_seqnum);

void rtp_twcc_stats_set_sock_ts (TWCCStatsManager *stats_manager,
    guint16 seqnum, GstClockTime sock_ts);

void rtp_twcc_manager_tx_start_feedback (TWCCStatsManager *stats_manager);
void rtp_twcc_stats_pkt_feedback (TWCCStatsManager *stats_manager,
    guint16 seqnum, GstClockTime remote_ts, GstClockTime current_time,
    TWCCPktState status);
void rtp_twcc_manager_tx_end_feedback (TWCCStatsManager *stats_manager);

GstStructure *rtp_twcc_stats_do_stats (TWCCStatsManager *stats_manager,
    GstClockTime stats_window_size, GstClockTime stats_window_delay);

#endif /* __RTP_TWCC_STATS_H__ */