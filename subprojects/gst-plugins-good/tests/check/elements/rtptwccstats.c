/* GStreamer
 *
 * Copyright (C) 2026 Pexip (http://pexip.com/)
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

#include <gst/check/gstcheck.h>
#include "gst/rtpmanager/rtptwccstats.h"

/* Decoded statuses only: no RTCP parser, network, threads or timing oracle.
 * The fixed history fits below capacity and spans the sequence-number wrap. */
#define HISTORY_SIZE 16384
#define BASE_SEQNUM 60000
#define PACKET_INTERVAL (100 * GST_USECOND)
#define STATS_WINDOW (4 * GST_SECOND)

GST_DEBUG_CATEGORY (rtp_twcc_debug);

static TWCCStatsManager *statsman;
static GObject *parent;

#ifndef GST_DISABLE_GST_DEBUG
static guint unknown_warnings;
static guint unexpected_warnings;
static GstDebugLevel saved_threshold;
static guint default_log_functions;

static void
count_warnings (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  if (category == rtp_twcc_debug && level == GST_LEVEL_WARNING &&
      g_strcmp0 (function, "rtp_twcc_stats_pkt_feedback") == 0 &&
      g_str_has_prefix (gst_debug_message_get (message),
          "Feedback on unknown packet #")) {
    unknown_warnings++;
    return;
  }

  if (category == rtp_twcc_debug && level <= GST_LEVEL_WARNING)
    unexpected_warnings++;
  gst_debug_log_default (category, level, file, function, line, object, message,
      user_data);
}
#endif

static void
setup (void)
{
  parent = g_object_new (G_TYPE_OBJECT, NULL);
  statsman = rtp_twcc_stats_manager_new (parent);
#ifndef GST_DISABLE_GST_DEBUG
  unknown_warnings = 0;
  unexpected_warnings = 0;
  saved_threshold = gst_debug_category_get_threshold (rtp_twcc_debug);
  gst_debug_category_set_threshold (rtp_twcc_debug, GST_LEVEL_WARNING);
  /* Count expected diagnostics without printing thousands of lines. */
  default_log_functions = gst_debug_remove_log_function (gst_debug_log_default);
  gst_debug_add_log_function (count_warnings, NULL, NULL);
#endif
}

static void
teardown (void)
{
  rtp_twcc_stats_manager_free (statsman);
  g_object_unref (parent);
#ifndef GST_DISABLE_GST_DEBUG
  gst_debug_remove_log_function (count_warnings);
  for (guint i = 0; i < default_log_functions; i++)
    gst_debug_add_log_function (gst_debug_log_default, NULL, NULL);
  gst_debug_category_set_threshold (rtp_twcc_debug, saved_threshold);
  fail_unless_equals_int (unexpected_warnings, 0);
#endif
}

static void
check_unknown_warnings (guint expected G_GNUC_UNUSED)
{
#ifndef GST_DISABLE_GST_DEBUG
  fail_unless_equals_int (unknown_warnings, expected);
#endif
}

static void
send_packets (guint first, guint count, GstClockTime start_time)
{
  GstBuffer *buffer = gst_rtp_buffer_new_allocate (100, 0, 0);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  RTPPacketInfo pinfo = { 0 };

  fail_unless (gst_rtp_buffer_map (buffer, GST_MAP_READWRITE, &rtp));
  pinfo.ssrc = 1234;
  pinfo.bytes = gst_buffer_get_size (buffer);
  gst_rtp_buffer_set_ssrc (&rtp, pinfo.ssrc);
  for (guint i = first; i < first + count; i++) {
    pinfo.seqnum = (guint16) (BASE_SEQNUM + i);
    pinfo.current_time = start_time + (i - first) * PACKET_INTERVAL;
    gst_rtp_buffer_set_seq (&rtp, pinfo.seqnum);
    gst_rtp_buffer_set_timestamp (&rtp, i * 90);
    gst_rtp_buffer_set_payload_type (&rtp, 96 + (i / 2) % 2);
    rtp_twcc_stats_sent_pkt (statsman, &pinfo, &rtp, pinfo.seqnum);
  }
  gst_rtp_buffer_unmap (&rtp);
  gst_buffer_unref (buffer);
}

static void
feedback_packet (guint index, TWCCPktState status)
{
  GstClockTime remote_ts = status == RTP_TWCC_FECBLOCK_PKT_RECEIVED ?
      30 * GST_SECOND + index * PACKET_INTERVAL : GST_CLOCK_TIME_NONE;

  rtp_twcc_stats_pkt_feedback (statsman, (guint16) (BASE_SEQNUM + index),
      remote_ts, 40 * GST_SECOND, status);
}

static void
feedback_range (guint first, guint count, TWCCPktState status)
{
  rtp_twcc_manager_tx_start_feedback (statsman);
  for (guint i = first; i < first + count; i++)
    feedback_packet (i, status);
  rtp_twcc_manager_tx_end_feedback (statsman);
}

static void
check_counts (const GstStructure * stats, guint sent, guint received)
{
  guint packets_sent, packets_recv;
  gdouble loss, recovery;
  gint64 delta;

  fail_unless (gst_structure_get (stats,
          "packets-sent", G_TYPE_UINT, &packets_sent,
          "packets-recv", G_TYPE_UINT, &packets_recv,
          "packet-loss-pct", G_TYPE_DOUBLE, &loss,
          "recovery-pct", G_TYPE_DOUBLE, &recovery,
          "avg-delta-of-delta", G_TYPE_INT64, &delta, NULL));
  fail_unless_equals_int (packets_sent, sent);
  fail_unless_equals_int (packets_recv, received);
  fail_unless (fabs (loss - (sent - received) * 100.0 / sent) < 0.00001,
      "Unexpected loss percentage: %g", loss);
  fail_unless_equals_float (recovery, sent == received ? -1.0 : 0.0);
  fail_unless_equals_int64 (delta, 0);
}

static GstStructure *
check_stats (guint sent, guint received)
{
  GstStructure *stats = rtp_twcc_stats_do_stats (statsman, STATS_WINDOW, 0);
  const GValue *value;
  GValueArray *payload_stats;
  guint seen = 0;

  fail_unless (stats != NULL);
  check_counts (stats, sent, received);
  value = gst_structure_get_value (stats, "payload-stats");
  fail_unless (value != NULL);
  payload_stats = g_value_get_boxed (value);
  fail_unless_equals_int (payload_stats->n_values, 2);
  for (guint i = 0; i < payload_stats->n_values; i++) {
    const GstStructure *pt_stats =
        gst_value_get_structure (g_value_array_get_nth (payload_stats, i));
    guint pt;

    fail_unless (gst_structure_get_uint (pt_stats, "pt", &pt));
    fail_unless (pt == 96 || pt == 97);
    fail_if (seen & (1 << (pt - 96)));
    seen |= 1 << (pt - 96);
    check_counts (pt_stats, sent / 2, received / 2);
  }
  fail_unless_equals_int (seen, 3);
  return stats;
}

GST_START_TEST (test_large_decoded_feedback_overlap)
{
  GstStructure *before, *after;

  send_packets (0, HISTORY_SIZE, GST_SECOND);
  feedback_range (0, HISTORY_SIZE, RTP_TWCC_FECBLOCK_PKT_LOST);
  before = check_stats (HISTORY_SIZE, 0);

  feedback_range (0, HISTORY_SIZE, RTP_TWCC_FECBLOCK_PKT_LOST);
  after = check_stats (HISTORY_SIZE, 0);
  fail_unless (gst_structure_is_equal (before, after));
  gst_structure_free (before);
  gst_structure_free (after);

  feedback_range (HISTORY_SIZE / 2, HISTORY_SIZE / 2,
      RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  after = check_stats (HISTORY_SIZE, HISTORY_SIZE / 2);
  gst_structure_free (after);

  /* Overlap upgrades only the newly received quarter. */
  feedback_range (HISTORY_SIZE / 4, HISTORY_SIZE / 2,
      RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  before = check_stats (HISTORY_SIZE, HISTORY_SIZE * 3 / 4);
  feedback_range (0, HISTORY_SIZE, RTP_TWCC_FECBLOCK_PKT_LOST);
  after = check_stats (HISTORY_SIZE, HISTORY_SIZE * 3 / 4);
  fail_unless (gst_structure_is_equal (before, after));
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), HISTORY_SIZE);
  check_unknown_warnings (0);
  gst_structure_free (before);
  gst_structure_free (after);
}

GST_END_TEST;

GST_START_TEST (test_large_decoded_feedback_expired_history)
{
  const guint retained = HISTORY_SIZE / 2;
  GstStructure *before, *after;

  send_packets (0, retained, GST_SECOND);
  send_packets (retained, retained, 20 * GST_SECOND);
  feedback_range (0, HISTORY_SIZE, RTP_TWCC_FECBLOCK_PKT_LOST);
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), HISTORY_SIZE);
  /* Statistics drains pending feedback before aging the first half out. */
  before = check_stats (retained, 0);
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), retained);
  gst_structure_free (before);

  rtp_twcc_manager_tx_start_feedback (statsman);
  for (guint i = 0; i < HISTORY_SIZE; i++)
    feedback_packet (i, i % 2 ? RTP_TWCC_FECBLOCK_PKT_RECEIVED :
        RTP_TWCC_FECBLOCK_PKT_LOST);
  rtp_twcc_manager_tx_end_feedback (statsman);
  before = check_stats (retained, retained / 2);
  check_unknown_warnings (retained);

  /* Never-sent statuses must not recreate history or change retained stats. */
  feedback_range (HISTORY_SIZE, 16, RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  after = check_stats (retained, retained / 2);
  fail_unless (gst_structure_is_equal (before, after));
  check_unknown_warnings (retained + 16);
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), retained);
  gst_structure_free (before);
  gst_structure_free (after);

  send_packets (HISTORY_SIZE, 4,
      20 * GST_SECOND + retained * PACKET_INTERVAL);
  feedback_range (HISTORY_SIZE, 4, RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  after = check_stats (retained + 4, retained / 2 + 4);
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), retained + 4);
  check_unknown_warnings (retained + 16);
  gst_structure_free (after);
}

GST_END_TEST;

GST_START_TEST (test_large_decoded_feedback_inferred_gap)
{
  GstStructure *before, *after;

  send_packets (0, HISTORY_SIZE, GST_SECOND);
  rtp_twcc_stats_check_for_lost_packets (statsman, BASE_SEQNUM, 2, 255);
  feedback_range (0, 2, RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  /* Consecutive feedback numbers wrap while covering a large missing range. */
  rtp_twcc_stats_check_for_lost_packets (statsman,
      (guint16) (BASE_SEQNUM + HISTORY_SIZE - 2), 2, 0);
  feedback_range (HISTORY_SIZE - 2, 2, RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  before = check_stats (HISTORY_SIZE, 4);

  rtp_twcc_stats_check_for_lost_packets (statsman, BASE_SEQNUM, 2, 255);
  feedback_range (0, 2, RTP_TWCC_FECBLOCK_PKT_RECEIVED);
  after = check_stats (HISTORY_SIZE, 4);
  fail_unless (gst_structure_is_equal (before, after));
  fail_unless_equals_int (rtp_twcc_stats_queue_len (statsman), HISTORY_SIZE);
  check_unknown_warnings (0);
  gst_structure_free (before);
  gst_structure_free (after);
}

GST_END_TEST;

static Suite *
rtptwccstats_suite (void)
{
  Suite *s = suite_create ("rtptwccstats");
  TCase *tc = tcase_create ("decoded-feedback");

  GST_DEBUG_CATEGORY_INIT (rtp_twcc_debug, "rtptwccstats-test", 0,
      "TWCC decoded feedback accounting");
  suite_add_tcase (s, tc);
  tcase_add_checked_fixture (tc, setup, teardown);
  tcase_add_test (tc, test_large_decoded_feedback_overlap);
  tcase_add_test (tc, test_large_decoded_feedback_expired_history);
  tcase_add_test (tc, test_large_decoded_feedback_inferred_gap);
  return s;
}

GST_CHECK_MAIN (rtptwccstats);
