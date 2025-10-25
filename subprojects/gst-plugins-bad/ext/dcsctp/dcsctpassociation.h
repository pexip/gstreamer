/*
 * Copyright (c) 2015, Collabora Ltd.
 * Copyright (c) 2023, Pexip AS
 *  @author: Tulio Beloqui <tulio@pexip.com>
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

#ifndef __DCSCTP_ASSOCIATION_H__
#define __DCSCTP_ASSOCIATION_H__

#include <glib-object.h>
#include <gst/gst.h>

#include <gst/sctp/sctpsendmeta.h>

#include "lib/sctpsocket.h"

G_BEGIN_DECLS

#define DCSCTP_TYPE_ASSOCIATION                  (dcsctp_association_get_type ())
#define DCSCTP_ASSOCIATION(obj)                  (G_TYPE_CHECK_INSTANCE_CAST ((obj), DCSCTP_TYPE_ASSOCIATION, DCSCTPAssociation))
#define DCSCTP_ASSOCIATION_CAST(obj)             (DCSCTPAssociation *)(obj)
#define DCSCTP_IS_ASSOCIATION(obj)               (G_TYPE_CHECK_INSTANCE_TYPE ((obj), DCSCTP_TYPE_ASSOCIATION))
#define DCSCTP_ASSOCIATION_CLASS(klass)          (G_TYPE_CHECK_CLASS_CAST ((klass), DCSCTP_TYPE_ASSOCIATION, DCSCTPAssociationClass))
#define DCSCTP_IS_ASSOCIATION_CLASS(klass)       (G_TYPE_CHECK_CLASS_TYPE ((klass), DCSCTP_TYPE_ASSOCIATION))
#define DCSCTP_ASSOCIATION_GET_CLASS(obj)        (G_TYPE_INSTANCE_GET_CLASS ((obj), DCSCTP_TYPE_ASSOCIATION, DCSCTPAssociationClass))

typedef struct _DCSCTPAssociation DCSCTPAssociation;
typedef struct _DCSCTPAssociationClass DCSCTPAssociationClass;

typedef struct _DCSCTPAssociationEncoderCtx DCSCTPAssociationEncoderCtx;
typedef struct _DCSCTPAssociationDecoderCtx DCSCTPAssociationDecoderCtx;

typedef enum
{
  DCSCTP_ASSOCIATION_STATE_NEW,
  DCSCTP_ASSOCIATION_STATE_READY,
  DCSCTP_ASSOCIATION_STATE_CONNECTING,
  DCSCTP_ASSOCIATION_STATE_CONNECTED,
  DCSCTP_ASSOCIATION_STATE_DISCONNECTING,
  DCSCTP_ASSOCIATION_STATE_DISCONNECTED,
  DCSCTP_ASSOCIATION_STATE_ERROR
} DCSCTPAssociationState;

typedef void (*DCSCTPAssociationPacketOutCb) (const guint8 * data, gsize length, GstElement * element);
typedef void (*DCSCTPAssociationStateChangeCb) (DCSCTPAssociation *
    sctp_association, DCSCTPAssociationState state, GstElement * element);

struct _DCSCTPAssociationEncoderCtx
{
  GstElement * element;
  DCSCTPAssociationStateChangeCb state_change_cb;
  DCSCTPAssociationPacketOutCb packet_out_cb;
};

typedef void (*DCSCTPAssociationPacketReceivedCb) (const guint8 * data, gsize length,
    guint16 stream_id, guint ppid, GstElement * element);

typedef void (*DCSCTPAssociationStreamResetCb)(guint16 stream_id, GstElement * element);

typedef void (*DCSCTPAssociationRestartCb)(GstElement * element);

struct _DCSCTPAssociationDecoderCtx
{
  GstElement * element;
  DCSCTPAssociationPacketReceivedCb packet_received_cb;
  DCSCTPAssociationStreamResetCb stream_reset_cb;
  DCSCTPAssociationRestartCb restart_cb;
};

typedef struct
{
  // True when the local connection has initiated the reset.
  gboolean closure_initiated;
  // True when the local connection received OnIncomingStreamsReset
  gboolean incoming_reset_done;
  // True when the local connection received OnStreamsResetPerformed
  gboolean outgoing_reset_done;
} GstSctpStreamState;

struct _DCSCTPAssociation
{
  GObject parent_instance;

  guint32 association_id;
  guint16 local_port;
  guint16 remote_port;
  gboolean use_sock_stream;
  gboolean aggressive_heartbeat;

  // Must only be accessed from GSource async handlers
  SctpSocket * socket;

  GRecMutex association_mutex;

  DCSCTPAssociationState state;
  GHashTable * stream_id_to_state;

  GMainContext *main_context;

  GHashTable * pending_source_ids;

  DCSCTPAssociationEncoderCtx encoder_ctx;
  DCSCTPAssociationDecoderCtx decoder_ctx;
};

struct _DCSCTPAssociationClass
{
  GObjectClass parent_class;
};

GType dcsctp_association_get_type (void);

gboolean dcsctp_association_connect (DCSCTPAssociation * self);

void dcsctp_association_set_encoder_ctx (DCSCTPAssociation * self,
    DCSCTPAssociationEncoderCtx * ctx);
void dcsctp_association_set_decoder_ctx (DCSCTPAssociation * self,
    DCSCTPAssociationDecoderCtx * ctx);

void dcsctp_association_incoming_packet (DCSCTPAssociation * self,
    const guint8 * buf, guint32 length);
GstFlowReturn dcsctp_association_send_data (DCSCTPAssociation * self,
    const guint8 * buf, gsize length, guint16 stream_id, guint32 ppid,
    gboolean ordered, GstSctpSendMetaPartiallyReliability pr,
    guint32 reliability_param);
void dcsctp_association_send_abort (DCSCTPAssociation * assoc,
    const gchar * message);
void dcsctp_association_reset_stream (DCSCTPAssociation * self,
    guint16 stream_id);
void dcsctp_association_force_close (DCSCTPAssociation * self);
void dcsctp_association_disconnect (DCSCTPAssociation * self);

G_END_DECLS

#endif /* __DCSCTP_ASSOCIATION_H__ */
