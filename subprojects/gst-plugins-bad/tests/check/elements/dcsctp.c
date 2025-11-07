/*
 * Copyright (c) 2019, Pexip AS
 *  @author: John-Mark Bell <jmb@pexip.com>
 *  @author: Thomas Williams <thomas.williams@pexip.com>
 *  @author: Tulio Beloqui <tulio@pexip.com>
 *  @author: Will Miller <will.miller@pexip.com>
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#include <gst/check/gstcheck.h>
#include <gst/sctp/sctpreceivemeta.h>
#include <gst/sctp/sctpsendmeta.h>

#include "sctpharness.h"

GST_START_TEST (sctp_init)
{
  SctpHarness *h = sctp_harness_new (FALSE);
  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_disconnect)
{
  const guint a = 1, b = 2;
  SctpHarness *h = sctp_harness_new (FALSE);

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  sctp_harness_disconnect_session (h, a);

  fail_unless (sctp_harness_wait_for_association_disestablished (h, a));
  fail_unless (sctp_harness_wait_for_association_disestablished (h, b));

  sctp_harness_reconnect_session (h, a);
  sctp_harness_reconnect_session (h, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_disconnect_unclean)
{
  const guint a = 1, b = 2;
  SctpHarness *h = sctp_harness_new (FALSE);

  sctp_harness_session_new (h, a, TRUE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  sctp_harness_break_network (h, a);

  fail_unless (sctp_harness_wait_for_association_disestablished (h, a));

  sctp_harness_unbreak_network (h, a);
  sctp_harness_reconnect_session (h, a);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_restarted (h, b));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_shutdown)
{
  const guint a = 1, b = 2;
  SctpHarness *h = sctp_harness_new (TRUE);

  sctp_harness_session_new (h, a, TRUE);
  sctp_harness_session_new (h, b, TRUE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  sctp_harness_disconnect_session (h, b);

  fail_unless (sctp_harness_wait_for_association_disestablished (h, a));
  fail_unless (sctp_harness_wait_for_association_disestablished (h, b));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_disconnect_unclean_stream_sock)
{
  const guint a = 1, b = 2;
  SctpHarness *h = sctp_harness_new (TRUE);

  sctp_harness_session_new (h, a, TRUE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  sctp_harness_break_network (h, a);

  fail_unless (sctp_harness_wait_for_association_disestablished (h, a));

  sctp_harness_unbreak_network (h, a);
  sctp_harness_reconnect_session (h, a);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_restarted (h, b));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_stream_create_and_destroy)
{
  const guint a = 1, b = 2;
  const guint sctp_ppid = 51;
  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send, *b_recv;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  // Create send stream 
  a_send = sctp_harness_create_send_stream (h, a, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  // Create and push buffer 
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  gst_harness_push (a_send, buf);

  // Expect recv stream to be created 
  b_recv = sctp_harness_wait_for_stream_created (h, b, a);
  fail_unless (b_recv != NULL);

  // Retrieve buffer from recv stream 
  GstBuffer *recvbuf = gst_harness_pull (b_recv);

  // Ensure it looks sane 
  fail_unless_equals_int ((gint) gst_buffer_get_size (recvbuf), sizeof (data));
  fail_unless_equals_int ((gint) gst_buffer_memcmp (recvbuf, 0, data,
          sizeof (data)), 0);
  GstSctpReceiveMeta *recv_meta = gst_sctp_buffer_get_receive_meta (recvbuf);
  fail_unless_equals_int (recv_meta->ppid, sctp_ppid);

  gst_buffer_unref (recvbuf);

  // Destroy the send stream 
  sctp_harness_destroy_send_stream (h, a, a);
  // Expect the corresponding recv stream to be destroyed 
  fail_unless (sctp_harness_wait_for_stream_destroyed (h, b, a));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_stream_create_and_reset)
{
  const guint a = 1, b = 2;
  const guint sctp_ppid = 51;
  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send, *a_recv;
  GstHarness *b_send, *b_recv;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  // Create send streams 
  a_send = sctp_harness_create_send_stream (h, a, a);
  b_send = sctp_harness_create_send_stream (h, b, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");
  gst_harness_set_src_caps_str (b_send, "application/x-data");

  // Create and push buffer from A 
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  gst_harness_push (a_send, buf);

  // ... and from B 
  buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  gst_harness_push (b_send, buf);

  // Expect recv streams to be created 
  a_recv = sctp_harness_wait_for_stream_created (h, a, a);
  b_recv = sctp_harness_wait_for_stream_created (h, b, a);
  fail_unless (a_recv != NULL);
  fail_unless (b_recv != NULL);

  // Retrieve buffer from recv streams 
  GstBuffer *recvbuf = gst_harness_pull (a_recv);
  gst_buffer_unref (recvbuf);
  recvbuf = gst_harness_pull (b_recv);
  gst_buffer_unref (recvbuf);

  // Send a reset from B 
  sctp_harness_reset_stream (h, b, a);
  // Expect recv streams to be destroyed 
  fail_unless (sctp_harness_wait_for_stream_destroyed (h, b, a));
  fail_unless (sctp_harness_wait_for_stream_destroyed (h, a, b));

  sctp_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (sctp_destroy_receive)
{
  const guint a = 1, b = 2;
  const guint sctp_ppid = 51;
  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  // Create send streams 
  a_send = sctp_harness_create_send_stream (h, a, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  // Create and push buffer from A 
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  gst_harness_push (a_send, buf);

  // teardown the receive first! 
  sctp_harness_session_destroy (h, b);

  sctp_harness_teardown (h);
}

GST_END_TEST;

gboolean run_stress_instances;

static gpointer
stress_encoder_instance_func (G_GNUC_UNUSED gpointer user_data)
{
  while (run_stress_instances) {
    const int id = 51;

    GstElement *sctpenc = gst_element_factory_make ("dcsctpenc", NULL);
    GstClock *systemclock = gst_system_clock_obtain ();
    gst_element_set_clock (sctpenc, systemclock);
    gst_object_unref (systemclock);

    g_object_set (sctpenc,
        "sctp-association-id", id,
        "remote-sctp-port", 5000,
        "use-sock-stream", TRUE, "aggressive-heartbeat", FALSE, NULL);

    gst_element_set_state (sctpenc, GST_STATE_PLAYING);
    g_thread_yield ();
    gst_element_set_state (sctpenc, GST_STATE_NULL);
    gst_object_unref (sctpenc);
  }

  return NULL;
}

static gpointer
stress_decoder_instance_func (G_GNUC_UNUSED gpointer user_data)
{
  while (run_stress_instances) {
    const int id = 51;

    GstElement *sctpdec = gst_element_factory_make ("dcsctpdec", NULL);
    GstClock *systemclock = gst_system_clock_obtain ();
    gst_element_set_clock (sctpdec, systemclock);
    gst_object_unref (systemclock);

    g_object_set (sctpdec,
        "sctp-association-id", id, "local-sctp-port", 5000, NULL);

    gst_element_set_state (sctpdec, GST_STATE_PLAYING);
    g_thread_yield ();
    gst_element_set_state (sctpdec, GST_STATE_NULL);
    gst_object_unref (sctpdec);
  }

  return NULL;
}

GST_START_TEST (sctp_stress_encoder_decoder_instances)
{
  GThread *stress_enc_t;
  GThread *stress_dec_t;

  run_stress_instances = TRUE;

  stress_enc_t =
      g_thread_new ("stress-enc", stress_encoder_instance_func, NULL);
  stress_dec_t =
      g_thread_new ("stress-dec", stress_decoder_instance_func, NULL);

  g_usleep (3 * G_USEC_PER_SEC);

  run_stress_instances = FALSE;

  g_thread_join (stress_enc_t);
  g_thread_join (stress_dec_t);
}

GST_END_TEST;

#define SCTP_STRESS_SESSIONS_MAX 16
#define SCTP_STRESS_SESSIONS_ITS 1000

typedef struct
{
  SctpHarness *h;
  guint a, b;
  guint stream_id;
}
SctpSessionsStressCtx;

static gpointer
sctp_stress_sessions_send_and_disconnect_func (gpointer user_data)
{
  const SctpSessionsStressCtx *ctx = user_data;
  const guint a = ctx->a, b = ctx->b;
  const guint sctp_ppid = 51;

  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  a_send = sctp_harness_create_send_stream (h, a, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  // Create and push buffer from A 
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  gst_harness_push (a_send, buf);

  // disconnect the sender session... 
  sctp_harness_disconnect_session (h, a);

  // let other threads do some work... 
  g_thread_yield ();
  g_thread_yield ();
  g_thread_yield ();

  // teardown hopefully will race with the SCTP thread 
  sctp_harness_teardown (h);

  return NULL;
}

// This test verifies that there is no race condition between teardown and
// closing SCTP sockets
GST_START_TEST (sctp_stress_sessions_send_and_disconnect)
{
  gint i, its, id;
  GThread *t[SCTP_STRESS_SESSIONS_MAX];
  SctpSessionsStressCtx ctx[SCTP_STRESS_SESSIONS_MAX];

  id = 1;
  for (its = 0; its < SCTP_STRESS_SESSIONS_ITS; its++) {
    for (i = 0; i < SCTP_STRESS_SESSIONS_MAX; i++) {
      ctx[i].a = id++;
      ctx[i].b = id++;
      t[i] =
          g_thread_new (NULL, sctp_stress_sessions_send_and_disconnect_func,
          &ctx[i]);
    }

    for (i = 0; i < SCTP_STRESS_SESSIONS_MAX; i++)
      g_thread_join (t[i]);
  }

}

GST_END_TEST;

#define SCTP_STRESS_MANY_STREAMS_MAX 100

static gpointer
sctp_stress_many_streams_func (gpointer user_data)
{
  const SctpSessionsStressCtx *ctx = user_data;
  GstHarness *a_send;
  const guint sctp_ppid = 51;
  const guint max_pushes = 1000;

  // Create send streams 
  a_send = sctp_harness_create_send_stream (ctx->h, ctx->a, ctx->stream_id);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  for (guint i = 0; i < max_pushes; i++) {
    // Create and push buffer from A 
    guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
    GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
        data, sizeof (data), 0, sizeof (data), NULL, NULL);
    gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
    gst_harness_push (a_send, buf);
  }

  return NULL;
}


GST_START_TEST (sctp_stress_many_streams)
{
  const guint a = 1, b = 2;
  SctpHarness *h = sctp_harness_new (TRUE);
  SctpSessionsStressCtx ctx[SCTP_STRESS_MANY_STREAMS_MAX];
  GThread *t[SCTP_STRESS_MANY_STREAMS_MAX];

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  for (guint i = 0; i < SCTP_STRESS_MANY_STREAMS_MAX; i++) {
    ctx[i].h = h;
    ctx[i].a = a;
    ctx[i].b = b;
    ctx[i].stream_id = i;
    t[i] = g_thread_new (NULL, sctp_stress_many_streams_func, &ctx[i]);
  }

  for (guint i = 0; i < SCTP_STRESS_MANY_STREAMS_MAX; i++)
    g_thread_join (t[i]);


  sctp_harness_teardown (h);
}

GST_END_TEST;


GST_START_TEST (sctp_abort_and_reconnect)
{
  const guint a = 1, b = 2;
  const guint sctp_ppid = 51;
  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  // Create send stream 
  a_send = sctp_harness_create_send_stream (h, a, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  sctp_harness_send_abort (h, b);

  sctp_harness_reconnect_session (h, a);

  // Create and push buffer
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (a_send, buf));

  // Destroy the send stream
  sctp_harness_destroy_send_stream (h, a, a);
  fail_unless (sctp_harness_wait_for_stream_destroyed (h, b, a));

  sctp_harness_teardown (h);
}

GST_END_TEST;


GST_START_TEST (sctp_push_before_connected)
{
  const guint a = 1, b = 2;
  const guint sctp_ppid = 51;
  SctpHarness *h = sctp_harness_new (FALSE);
  GstHarness *a_send;

  sctp_harness_session_new (h, a, FALSE);
  sctp_harness_session_new (h, b, FALSE);

  sctp_harness_connect_sessions (h, a, b);

  // Create send stream
  a_send = sctp_harness_create_send_stream (h, a, a);
  gst_harness_set_src_caps_str (a_send, "application/x-data");

  // Create and push buffer
  guint8 data[] = { 0x01, 0x02, 0x03, 0x04 };
  GstBuffer *buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
      data, sizeof (data), 0, sizeof (data), NULL, NULL);
  gst_sctp_buffer_add_send_meta (buf, sctp_ppid, TRUE, 0, 0);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (a_send, buf));

  fail_unless (sctp_harness_wait_for_association_established (h, a));
  fail_unless (sctp_harness_wait_for_association_established (h, b));

  // Expect recv stream to be created
  GstHarness *b_recv = sctp_harness_wait_for_stream_created (h, b, a);
  fail_unless (b_recv != NULL);

  // Retrieve buffer from recv stream
  GstBuffer *recvbuf = gst_harness_pull (b_recv);

  // Ensure it looks sane
  fail_unless_equals_int ((gint) gst_buffer_get_size (recvbuf), sizeof (data));
  fail_unless_equals_int ((gint) gst_buffer_memcmp (recvbuf, 0, data,
          sizeof (data)), 0);
  GstSctpReceiveMeta *recv_meta = gst_sctp_buffer_get_receive_meta (recvbuf);
  fail_unless_equals_int (recv_meta->ppid, sctp_ppid);

  gst_buffer_unref (recvbuf);

  // Destroy the send stream
  sctp_harness_destroy_send_stream (h, a, a);
  fail_unless (sctp_harness_wait_for_stream_destroyed (h, b, a));

  sctp_harness_teardown (h);
}

GST_END_TEST;


static Suite *
dcsctp_suite (void)
{
  Suite *s = suite_create ("sctp");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, sctp_init);
  tcase_add_test (tc_chain, sctp_disconnect);
  tcase_add_test (tc_chain, sctp_disconnect_unclean);
  tcase_add_test (tc_chain, sctp_shutdown);
  tcase_add_test (tc_chain, sctp_disconnect_unclean_stream_sock);
  tcase_add_test (tc_chain, sctp_stream_create_and_destroy);
  tcase_add_test (tc_chain, sctp_stream_create_and_reset);
  tcase_add_test (tc_chain, sctp_destroy_receive);
  tcase_add_test (tc_chain, sctp_abort_and_reconnect);
  tcase_add_test (tc_chain, sctp_push_before_connected);

  tcase_add_test (tc_chain, sctp_stress_encoder_decoder_instances);
  // This test is overkill for CI, so enable when testing racy conditions
  tcase_skip_broken_test (tc_chain, sctp_stress_sessions_send_and_disconnect);
  tcase_add_test (tc_chain, sctp_stress_many_streams);

  return s;
}

GST_CHECK_MAIN (dcsctp);
