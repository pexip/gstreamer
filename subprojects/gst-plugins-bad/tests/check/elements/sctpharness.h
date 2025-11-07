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

#ifndef __SCTP_HARNESS_H__
#define __SCTP_HARNESS_H__

#include <gst/check/gstharness.h>

G_BEGIN_DECLS

typedef struct
{
  GMutex harness_lock;
  GCond harness_cond;

  GHashTable * sessions;

  gboolean use_sock_stream;
} SctpHarness;

SctpHarness * sctp_harness_new (gboolean use_sock_stream);
void sctp_harness_teardown (SctpHarness * h);

guint sctp_harness_session_new (SctpHarness * h, guint id,
    gboolean aggressive_heartbeat);
void sctp_harness_session_destroy (SctpHarness * h, guint id);

GstHarness * sctp_harness_create_send_stream (SctpHarness * h,
    guint session_id, guint stream_id);
void sctp_harness_destroy_send_stream (SctpHarness * h,
    guint session_id, guint stream_id);

void sctp_harness_reset_stream (SctpHarness * h,
    guint session_id, guint stream_id);

void sctp_harness_connect_sessions (SctpHarness * h, guint id1, guint id2);
void sctp_harness_break_network (SctpHarness * h, guint id);
void sctp_harness_unbreak_network (SctpHarness * h, guint id);
void sctp_harness_disconnect_session (SctpHarness * h, guint id);
void sctp_harness_reconnect_session (SctpHarness * h, guint id);

gboolean sctp_harness_wait_for_association_established (SctpHarness * h,
    guint id);
gboolean sctp_harness_wait_for_association_disestablished (SctpHarness * h,
    guint id);
gboolean sctp_harness_wait_for_association_restarted (SctpHarness * h,
    guint id);

GstHarness * sctp_harness_wait_for_stream_created (SctpHarness * h,
    guint session_id, guint stream_id);
gboolean sctp_harness_wait_for_stream_destroyed (SctpHarness * h,
    guint session_id, guint stream_id);

void sctp_harness_send_abort (SctpHarness * h, guint session_id);

G_END_DECLS

#endif /* __SCTP_HARNESS_H__ */
