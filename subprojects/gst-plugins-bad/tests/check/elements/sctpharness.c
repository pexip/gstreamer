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


#include "sctpharness.h"

#include <stdio.h>

#include <gst/check/gstcheck.h>

#define GET_HARNESS_LOCK(h) (&h->harness_lock)
#define GET_HARNESS_COND(h) (&h->harness_cond)
#define HARNESS_LOCK(h)   (g_mutex_lock   (GET_HARNESS_LOCK (h)))
#define HARNESS_UNLOCK(h) (g_mutex_unlock (GET_HARNESS_LOCK (h)))
#define HARNESS_WAIT_UNTIL(h, t) (g_cond_wait_until (GET_HARNESS_COND (h), GET_HARNESS_LOCK (h), t))
#define HARNESS_SIGNAL(h) (g_cond_signal  (GET_HARNESS_COND (h)))

#define GET_SESSION_LOCK(s) (&s->session_lock)
#define GET_SESSION_COND(s) (&s->session_cond)
#define SESSION_LOCK(s)   (g_mutex_lock   (GET_SESSION_LOCK (s)))
#define SESSION_UNLOCK(s) (g_mutex_unlock (GET_SESSION_LOCK (s)))
#define SESSION_WAIT_UNTIL(s, t) (g_cond_wait_until (GET_SESSION_COND (s), GET_SESSION_LOCK (s), t))
#define SESSION_SIGNAL(s) (g_cond_signal  (GET_SESSION_COND (s)))

typedef struct
{
  SctpHarness *h;

  GMutex session_lock;
  GCond session_cond;

  guint id;
  gboolean aggressive_heartbeat;

  GstElement *sctpenc;
  GstElement *valve;
  GstElement *sctpdec;

  // These two exist purely to stop teardown of the pad
  // harnesses from breaking the element state, too
  GstHarness *sctpenc_h;
  GstHarness *sctpdec_h;

  gboolean established;
  gulong established_handler_id;

  gboolean restarted;
  gulong restarted_handler_id;

  GHashTable *stream_id_to_send_h;

  GHashTable *stream_id_to_recv_h;
  gulong pad_added_handler_id;
  gulong pad_removed_handler_id;
} SctpSession;

// sctpenc:
//   sink_%u -> src
//
//   remote-sctp-port
//   sctp-association-id
//   use-sock-stream
//
//   sctp-association-established (t/f)
//
//   bytes-sent (n)
//
// sctpdec:
//   sink -> src_%u
//
//   local-sctp-port
//   sctp-association-id
//
//   pad-added (pad)
//   pad-removed (pad)
//   no-more-pads
//
//   sctp-association-restarted
//
//   reset-stream (id)

static void
sctp_harness_association_established (SctpSession * s,
    gboolean arg, GstElement * sctpenc)
{
  GST_INFO ("association established session=%u arg=%d", s->id, arg);
  (void) sctpenc;
  SESSION_LOCK (s);
  s->established = arg;
  SESSION_SIGNAL (s);
  SESSION_UNLOCK (s);
}

static void
sctp_harness_association_restarted (SctpSession * s, GstElement * sctpdec)
{
  GST_INFO ("association restarted session=%u", s->id);
  (void) sctpdec;
  SESSION_LOCK (s);
  s->restarted = TRUE;
  SESSION_SIGNAL (s);
  SESSION_UNLOCK (s);
}

static void
sctp_harness_pad_added (SctpSession * s, GstPad * pad, GstElement * sctpdec)
{
  SESSION_LOCK (s);
  if (s->pad_added_handler_id != 0) {
    guint stream_id;
    gchar *src_name = gst_pad_get_name (pad);
    g_assert (sscanf (src_name, "src_%u", &stream_id) == 1);
    GstHarness *recv_h = gst_harness_new_with_element (sctpdec, NULL, src_name);
    g_free (src_name);

    g_assert (!g_hash_table_lookup (s->stream_id_to_recv_h,
            GUINT_TO_POINTER (stream_id)));
    g_hash_table_insert (s->stream_id_to_recv_h,
        GUINT_TO_POINTER (stream_id), recv_h);
  }
  SESSION_SIGNAL (s);
  SESSION_UNLOCK (s);
}

static void
sctp_harness_pad_removed (SctpSession * s, GstPad * pad, GstElement * sctpdec)
{
  guint stream_id;
  gchar *src_name;
  (void) sctpdec;

  src_name = gst_pad_get_name (pad);
  g_assert (sscanf (src_name, "src_%u", &stream_id) == 1);
  g_free (src_name);

  SESSION_LOCK (s);
  if (s->pad_removed_handler_id != 0) {
    g_assert (g_hash_table_remove (s->stream_id_to_recv_h,
            GUINT_TO_POINTER (stream_id)));
  }
  SESSION_SIGNAL (s);
  SESSION_UNLOCK (s);
}

// call with SESSION_LOCK acquired
static void
session_disconnect_signals_unlocked (SctpSession * s)
{
  if (s->established_handler_id != 0)
    g_signal_handler_disconnect (s->sctpenc, s->established_handler_id);

  if (s->restarted_handler_id != 0)
    g_signal_handler_disconnect (s->sctpdec, s->restarted_handler_id);

  if (s->pad_added_handler_id != 0)
    g_signal_handler_disconnect (s->sctpdec, s->pad_added_handler_id);

  if (s->pad_removed_handler_id != 0)
    g_signal_handler_disconnect (s->sctpdec, s->pad_removed_handler_id);

  s->established_handler_id = 0;
  s->restarted_handler_id = 0;
  s->pad_added_handler_id = 0;
  s->pad_removed_handler_id = 0;
}

static void
session_teardown (SctpSession * s)
{
  g_hash_table_destroy (s->stream_id_to_recv_h);
  g_hash_table_destroy (s->stream_id_to_send_h);

  gst_harness_teardown (s->sctpenc_h);
  gst_harness_teardown (s->sctpdec_h);

  gst_element_set_state (s->sctpenc, GST_STATE_NULL);
  gst_element_set_state (s->valve, GST_STATE_NULL);
  gst_element_set_state (s->sctpdec, GST_STATE_NULL);
  gst_object_unref (s->sctpenc);
  gst_object_unref (s->valve);
  gst_object_unref (s->sctpdec);

  g_mutex_clear (GET_SESSION_LOCK (s));
  g_cond_clear (GET_SESSION_COND (s));

  g_free (s);
}

static void
sctp_harness_link_elements (GstElement * src, GstElement * sink)
{
  GstPad *srcpad = gst_element_get_static_pad (src, "src");
  GstPad *sinkpad = gst_element_get_static_pad (sink, "sink");

  GstPadLinkReturn ret = gst_pad_link (srcpad, sinkpad);
  fail_unless_equals_uint64 (ret, GST_PAD_LINK_OK);

  gst_object_unref (srcpad);
  gst_object_unref (sinkpad);
}

guint
sctp_harness_session_new (SctpHarness * h, guint id,
    gboolean aggressive_heartbeat)
{
  SctpSession *s = g_new0 (SctpSession, 1);

  s->h = h;

  g_mutex_init (GET_SESSION_LOCK (s));
  g_cond_init (GET_SESSION_COND (s));

  s->id = id;
  s->aggressive_heartbeat = aggressive_heartbeat;

  s->stream_id_to_send_h =
      g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gst_harness_teardown);
  s->stream_id_to_recv_h =
      g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gst_harness_teardown);

  s->sctpenc = gst_element_factory_make ("dcsctpenc", NULL);
  s->valve = gst_element_factory_make ("valve", NULL);
  s->sctpdec = gst_element_factory_make ("dcsctpdec", NULL);

  GstClock *systemclock = gst_system_clock_obtain ();
  gst_element_set_clock (s->sctpenc, systemclock);
  gst_element_set_clock (s->valve, systemclock);
  gst_element_set_clock (s->sctpdec, systemclock);
  gst_object_unref (systemclock);

  sctp_harness_link_elements (s->sctpenc, s->valve);

  g_object_set (s->valve, "drop", TRUE, NULL);
  g_object_set (s->sctpenc,
      "sctp-association-id", s->id,
      "remote-sctp-port", 5000,
      "use-sock-stream", h->use_sock_stream,
      "aggressive-heartbeat", s->aggressive_heartbeat, NULL);
  g_object_set (s->sctpdec,
      "sctp-association-id", s->id, "local-sctp-port", 5000, NULL);

  s->established_handler_id = g_signal_connect_swapped (s->sctpenc,
      "sctp-association-established",
      G_CALLBACK (sctp_harness_association_established), s);

  s->restarted_handler_id = g_signal_connect_swapped (s->sctpdec,
      "sctp-association-restarted",
      G_CALLBACK (sctp_harness_association_restarted), s);

  s->pad_added_handler_id = g_signal_connect_swapped (s->sctpdec,
      "pad-added", G_CALLBACK (sctp_harness_pad_added), s);
  s->pad_removed_handler_id = g_signal_connect_swapped (s->sctpdec,
      "pad-removed", G_CALLBACK (sctp_harness_pad_removed), s);

  gst_element_set_state (s->sctpenc, GST_STATE_PLAYING);
  gst_element_set_state (s->valve, GST_STATE_PLAYING);
  gst_element_set_state (s->sctpdec, GST_STATE_PLAYING);

  s->sctpenc_h = gst_harness_new_with_element (s->sctpenc, NULL, NULL);
  s->sctpdec_h = gst_harness_new_with_element (s->sctpdec, NULL, NULL);

  HARNESS_LOCK (h);
  g_hash_table_insert (h->sessions, GUINT_TO_POINTER (s->id), s);
  HARNESS_UNLOCK (h);

  return s->id;
}

static void
sctp_harness_session_destroy_unlocked (SctpHarness * h, guint id)
{
  // Shut the valve of the session to avoid unexpected packets flowing
  // during/after session teardown. We need to do this because the the
  // sctpenc ! valve from one session will often be connected to the sctpdec
  // from another session and thus we have no simple way of ensuring that
  // shutdown order is correct when we destroy sessions.
  SctpSession *s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);
  g_object_set (s->valve, "drop", TRUE, NULL);
  session_disconnect_signals_unlocked (s);
  SESSION_UNLOCK (s);
}

void
sctp_harness_session_destroy (SctpHarness * h, guint id)
{
  HARNESS_LOCK (h);
  sctp_harness_session_destroy_unlocked (h, id);
  g_assert (g_hash_table_remove (h->sessions, GUINT_TO_POINTER (id)));
  HARNESS_UNLOCK (h);
}

SctpHarness *
sctp_harness_new (gboolean use_sock_stream)
{
  SctpHarness *h = g_new0 (SctpHarness, 1);

  g_mutex_init (GET_HARNESS_LOCK (h));
  g_cond_init (GET_HARNESS_COND (h));

  h->sessions =
      g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) session_teardown);

  h->use_sock_stream = use_sock_stream;

  return h;
}

void
sctp_harness_teardown (SctpHarness * h)
{
  GHashTableIter iter;
  gpointer key, value;

  HARNESS_LOCK (h);

  g_hash_table_iter_init (&iter, h->sessions);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    sctp_harness_session_destroy_unlocked (h, GPOINTER_TO_UINT (key));
  }
  g_hash_table_destroy (h->sessions);
  HARNESS_UNLOCK (h);

  g_mutex_clear (GET_HARNESS_LOCK (h));
  g_cond_clear (GET_HARNESS_COND (h));

  g_free (h);
}

GstHarness *
sctp_harness_create_send_stream (SctpHarness * h,
    guint session_id, guint stream_id)
{
  SctpSession *s;
  GstHarness *send_h;
  gchar *sink_name;

  sink_name = g_strdup_printf ("sink_%u", stream_id);

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));
  SESSION_LOCK (s);

  g_assert (!g_hash_table_lookup (s->stream_id_to_send_h,
          GUINT_TO_POINTER (stream_id)));
  send_h = gst_harness_new_with_element (s->sctpenc, sink_name, NULL);
  g_free (sink_name);
  g_hash_table_insert (s->stream_id_to_send_h,
      GUINT_TO_POINTER (stream_id), send_h);

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return send_h;
}

void
sctp_harness_destroy_send_stream (SctpHarness * h,
    guint session_id, guint stream_id)
{
  SctpSession *s;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));
  SESSION_LOCK (s);

  g_assert (g_hash_table_remove (s->stream_id_to_send_h,
          GUINT_TO_POINTER (stream_id)));

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);
}

void
sctp_harness_reset_stream (SctpHarness * h, guint session_id, guint stream_id)
{
  SctpSession *s;
  GstHarness *recv_h;
  GstElement *sctpdec;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));
  SESSION_LOCK (s);
  recv_h = g_hash_table_lookup (s->stream_id_to_recv_h,
      GUINT_TO_POINTER (stream_id));
  sctpdec = g_object_ref (recv_h->element);
  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);
  g_signal_emit_by_name (sctpdec, "reset-stream", stream_id);
  g_object_unref (sctpdec);
}

void
sctp_harness_send_abort (SctpHarness * h, guint session_id)
{
  HARNESS_LOCK (h);
  SctpSession *s =
      g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));

  SESSION_LOCK (s);
  GstElement *sctpenc = g_object_ref (s->sctpenc);
  SESSION_UNLOCK (s);

  HARNESS_UNLOCK (h);

  g_signal_emit_by_name (sctpenc, "send-abort", "sctpharness test abort");
  g_object_unref (sctpenc);
}

void
sctp_harness_connect_sessions (SctpHarness * h, guint id1, guint id2)
{
  SctpSession *s1, *s2;

  HARNESS_LOCK (h);
  s1 = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id1));
  s2 = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id2));
  SESSION_LOCK (s1);
  SESSION_LOCK (s2);

  sctp_harness_link_elements (s1->valve, s2->sctpdec);
  sctp_harness_link_elements (s2->valve, s1->sctpdec);

  g_object_set (s1->valve, "drop", FALSE, NULL);
  g_object_set (s2->valve, "drop", FALSE, NULL);

  SESSION_UNLOCK (s2);
  SESSION_UNLOCK (s1);
  HARNESS_UNLOCK (h);
}

void
sctp_harness_break_network (SctpHarness * h, guint id)
{
  SctpSession *s;

  // The SCTP stack can transition an association into the OPEN state
  // while there are still handshake messages to be sent or received.
  // Specifically, it is often the case that an association is declared
  // connected while there is still a COOKIE-ACK to be sent on the wire.
  // If we issue a disconnection request during this window, then no
  // ABORT will be sent on the wire (and, instead, the usual association
  // time out logic will apply). Given the default timer settings,
  // it will take a very long time for the remote side to time out the
  // association and tests will give up long before that happens.
  // Avoid the race by pausing before splitting the network.
  g_usleep (50 * G_TIME_SPAN_MILLISECOND);

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  HARNESS_UNLOCK (h);

  GST_INFO ("breaking network");
  g_object_set (s->valve, "drop", TRUE, NULL);
}

void
sctp_harness_unbreak_network (SctpHarness * h, guint id)
{
  SctpSession *s;

  /* Allow some time for any outstanding ABORT messages to be lost */
  g_usleep (50 * G_TIME_SPAN_MILLISECOND);

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  HARNESS_UNLOCK (h);

  GST_INFO ("unbreaking network");
  g_object_set (s->valve, "drop", FALSE, NULL);
}

void
sctp_harness_disconnect_session (SctpHarness * h, guint id)
{
  SctpSession *s;
  GstElement *sctpenc;
  gboolean ret;

  // The SCTP stack can transition an association into the OPEN state
  // while there are still handshake messages to be sent or received.
  // Specifically, it is often the case that an association is declared
  // connected while there is still a COOKIE-ACK to be sent on the wire.
  // If we issue a disconnection request during this window, then no
  // ABORT will be sent on the wire (and, instead, the usual association
  // time out logic will apply). Given the default timer settings,
  // it will take a very long time for the remote side to time out the
  // association and tests will give up long before that happens.
  // Avoid the race by pausing before issuing the disconnection request.
  g_usleep (50 * G_TIME_SPAN_MILLISECOND);

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);
  sctpenc = g_object_ref (s->sctpenc);
  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);
  g_signal_emit_by_name (sctpenc, "disconnect", &ret);
  (void) ret;
  g_object_unref (sctpenc);
}

void
sctp_harness_reconnect_session (SctpHarness * h, guint id)
{
  SctpSession *s;
  GstElement *sctpenc;

  /* Allow some time for the underlying SCTP stack to clean up after disconnect */
  g_usleep (50 * G_TIME_SPAN_MILLISECOND);

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);
  sctpenc = g_object_ref (s->sctpenc);
  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);
  g_signal_emit_by_name (sctpenc, "reconnect");
  g_object_unref (sctpenc);
}

gboolean
sctp_harness_wait_for_association_established (SctpHarness * h, guint id)
{
  gint timeout_sec = 60;
  gint64 now = g_get_monotonic_time ();
  SctpSession *s;
  gboolean ret;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);

  while (!s->established) {
    if (!SESSION_WAIT_UNTIL (s, now + G_USEC_PER_SEC * timeout_sec))
      break;
  }
  ret = s->established;

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return ret;
}

gboolean
sctp_harness_wait_for_association_disestablished (SctpHarness * h, guint id)
{
  gint timeout_sec = 90;
  gint64 now = g_get_monotonic_time ();
  SctpSession *s;
  gboolean ret;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);

  while (s->established) {
    if (!SESSION_WAIT_UNTIL (s, now + G_USEC_PER_SEC * timeout_sec))
      break;
  }
  ret = !s->established;

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return ret;
}

gboolean
sctp_harness_wait_for_association_restarted (SctpHarness * h, guint id)
{
  gint timeout_sec = 60;
  gint64 now = g_get_monotonic_time ();
  SctpSession *s;
  gboolean ret;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (id));
  SESSION_LOCK (s);

  while (!s->restarted) {
    if (!SESSION_WAIT_UNTIL (s, now + G_USEC_PER_SEC * timeout_sec))
      break;
  }
  ret = s->restarted;
  s->restarted = FALSE;

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return ret;
}

GstHarness *
sctp_harness_wait_for_stream_created (SctpHarness * h,
    guint session_id, guint stream_id)
{
  gint timeout_sec = 60;
  gint64 now = g_get_monotonic_time ();
  SctpSession *s;
  GstHarness *ret;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));
  SESSION_LOCK (s);

  while (!g_hash_table_contains (s->stream_id_to_recv_h,
          GUINT_TO_POINTER (stream_id))) {
    if (!SESSION_WAIT_UNTIL (s, now + G_USEC_PER_SEC * timeout_sec))
      break;
  }
  ret = g_hash_table_lookup (s->stream_id_to_recv_h,
      GUINT_TO_POINTER (stream_id));

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return ret;
}

gboolean
sctp_harness_wait_for_stream_destroyed (SctpHarness * h,
    guint session_id, guint stream_id)
{
  gint timeout_sec = 90;
  gint64 now = g_get_monotonic_time ();
  SctpSession *s;
  gboolean ret;

  HARNESS_LOCK (h);
  s = g_hash_table_lookup (h->sessions, GUINT_TO_POINTER (session_id));
  SESSION_LOCK (s);

  while (g_hash_table_contains (s->stream_id_to_recv_h,
          GUINT_TO_POINTER (stream_id))) {
    if (!SESSION_WAIT_UNTIL (s, now + G_USEC_PER_SEC * timeout_sec))
      break;
  }
  ret = !g_hash_table_contains (s->stream_id_to_recv_h,
      GUINT_TO_POINTER (stream_id));

  SESSION_UNLOCK (s);
  HARNESS_UNLOCK (h);

  return ret;
}
