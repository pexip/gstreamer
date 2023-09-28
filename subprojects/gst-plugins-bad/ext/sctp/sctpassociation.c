/*
 * Copyright (c) 2015, Collabora Ltd.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sctpassociation.h"

#include <gst/gst.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY_STATIC (gst_sctp_association_debug_category);
#define GST_CAT_DEFAULT gst_sctp_association_debug_category
GST_DEBUG_CATEGORY_STATIC (gst_sctp_debug_category);

#define GST_SCTP_ASSOCIATION_STATE_TYPE (gst_sctp_association_state_get_type())
static GType
gst_sctp_association_state_get_type (void)
{
  static const GEnumValue values[] = {
    {GST_SCTP_ASSOCIATION_STATE_NEW, "state-new", "state-new"},
    {GST_SCTP_ASSOCIATION_STATE_READY, "state-ready", "state-ready"},
    {GST_SCTP_ASSOCIATION_STATE_CONNECTING, "state-connecting",
        "state-connecting"},
    {GST_SCTP_ASSOCIATION_STATE_CONNECTED, "state-connected",
        "state-connected"},
    {GST_SCTP_ASSOCIATION_STATE_DISCONNECTING, "state-disconnecting",
        "state-disconnecting"},
    {GST_SCTP_ASSOCIATION_STATE_DISCONNECTED, "state-disconnected",
        "state-disconnected"},
    {GST_SCTP_ASSOCIATION_STATE_ERROR, "state-error", "state-error"},
    {0, NULL, NULL}
  };
  static GType id = 0;

  if (g_once_init_enter ((gsize *) & id)) {
    GType _id;
    _id = g_enum_register_static ("GstSctpAssociationState", values);
    g_once_init_leave ((gsize *) & id, _id);
  }

  return id;
}

G_DEFINE_TYPE (GstSctpAssociation, gst_sctp_association, G_TYPE_OBJECT);

enum
{
  PROP_0,

  PROP_ASSOCIATION_ID,
  PROP_LOCAL_PORT,
  PROP_REMOTE_PORT,
  PROP_STATE,
  PROP_USE_SOCK_STREAM,
  PROP_AGGRESSIVE_HEARTBEAT,

  NUM_PROPERTIES
};

static GParamSpec *properties[NUM_PROPERTIES];

#define DEFAULT_NUMBER_OF_SCTP_STREAMS 65535
#define DEFAULT_LOCAL_SCTP_PORT 0
#define DEFAULT_REMOTE_SCTP_PORT 0

G_LOCK_DEFINE_STATIC (associations_lock);
static GHashTable *associations_by_id = NULL;
static GHashTable *ids_by_association = NULL;

/* Interface implementations */
static void gst_sctp_association_finalize (GObject * object);
static void gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void maybe_set_state_to_ready_unlocked (GstSctpAssociation * self);
static void gst_sctp_association_change_state_unlocked (GstSctpAssociation *
    self, GstSctpAssociationState new_state);

static void
gst_sctp_association_class_init (GstSctpAssociationClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = gst_sctp_association_finalize;
  gobject_class->set_property = gst_sctp_association_set_property;
  gobject_class->get_property = gst_sctp_association_get_property;

  properties[PROP_ASSOCIATION_ID] = g_param_spec_uint ("association-id",
      "The SCTP association-id", "The SCTP association-id.", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LOCAL_PORT] = g_param_spec_uint ("local-port", "Local SCTP",
      "The local SCTP port for this association", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_REMOTE_PORT] =
      g_param_spec_uint ("remote-port", "Remote SCTP",
      "The remote SCTP port for this association", 0, G_MAXUSHORT,
      DEFAULT_LOCAL_SCTP_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_STATE] = g_param_spec_enum ("state", "SCTP Association state",
      "The state of the SCTP association", GST_SCTP_ASSOCIATION_STATE_TYPE,
      GST_SCTP_ASSOCIATION_STATE_NEW,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_USE_SOCK_STREAM] =
      g_param_spec_boolean ("use-sock-stream", "Use sock-stream",
      "When set to TRUE, a sequenced, reliable, connection-based connection is used."
      "When TRUE the partial reliability parameters of the channel is ignored.",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_AGGRESSIVE_HEARTBEAT] =
      g_param_spec_boolean ("aggressive-heartbeat", "Aggressive heartbeat",
      "When set to TRUE, set the heartbeat interval to 10ms and the assoc "
      "rtx max to 1.", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, NUM_PROPERTIES, properties);
}

// #if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
// #define SCTP_GST_DEBUG_LEVEL GST_LEVEL_DEBUG
// static void
// gst_usrsctp_debug (const gchar * format, ...)
// {
//   va_list varargs;

//   va_start (varargs, format);
//   gst_debug_log_valist (gst_sctp_debug_category, SCTP_GST_DEBUG_LEVEL,
//       __FILE__, GST_FUNCTION, __LINE__, NULL, format, varargs);
//   va_end (varargs);
// }
// #endif

static void
gst_sctp_association_init (GstSctpAssociation * self)
{
  self->local_port = DEFAULT_LOCAL_SCTP_PORT;
  self->remote_port = DEFAULT_REMOTE_SCTP_PORT;
  // self->sctp_ass_sock = NULL;
  self->state = GST_SCTP_ASSOCIATION_STATE_NEW;
  self->use_sock_stream = TRUE;

  g_mutex_init (&self->association_mutex);
}

static void
gst_sctp_association_finalize (GObject * object)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  if (self->socket)
    sctp_socket_free (self->socket);
  self->socket = NULL;

  /* no need to hold the association_lock, it is held under
     gst_sctp_association_unref */
  g_hash_table_remove (associations_by_id,
      GUINT_TO_POINTER (self->association_id));

  /* demand we are no longer registered */
  g_assert (!g_hash_table_contains (ids_by_association, self));

  g_mutex_clear (&self->association_mutex);

  G_OBJECT_CLASS (gst_sctp_association_parent_class)->finalize (object);
}

static void
gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_NEW) {
    switch (prop_id) {
      case PROP_LOCAL_PORT:
      case PROP_REMOTE_PORT:
        GST_ERROR_OBJECT (self, "These properties cannot be set in this state");
        g_mutex_unlock (&self->association_mutex);
        return;
    }
  }

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      self->association_id = g_value_get_uint (value);
      break;
    case PROP_LOCAL_PORT:
      self->local_port = g_value_get_uint (value);
      break;
    case PROP_REMOTE_PORT:
      self->remote_port = g_value_get_uint (value);
      break;
    case PROP_STATE:
      self->state = g_value_get_enum (value);
      break;
    case PROP_USE_SOCK_STREAM:
      self->use_sock_stream = g_value_get_boolean (value);
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      self->aggressive_heartbeat = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }

  if (prop_id == PROP_LOCAL_PORT || prop_id == PROP_REMOTE_PORT)
    maybe_set_state_to_ready_unlocked (self);

  g_mutex_unlock (&self->association_mutex);

  return;
}

static void
maybe_set_state_to_ready_unlocked (GstSctpAssociation * self)
{
  if ((self->state == GST_SCTP_ASSOCIATION_STATE_NEW)
      && (self->local_port != 0 && self->remote_port != 0)
      && (self->encoder_ctx.packet_out_cb != NULL)
      && (self->decoder_ctx.packet_received_cb != NULL)
      && (self->encoder_ctx.state_change_cb != NULL)) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_READY);
  }
}

static void
gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

  g_mutex_lock (&self->association_mutex);

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      g_value_set_uint (value, self->association_id);
      break;
    case PROP_LOCAL_PORT:
      g_value_set_uint (value, self->local_port);
      break;
    case PROP_REMOTE_PORT:
      g_value_set_uint (value, self->remote_port);
      break;
    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;
    case PROP_USE_SOCK_STREAM:
      g_value_set_boolean (value, self->use_sock_stream);
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      g_value_set_boolean (value, self->aggressive_heartbeat);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }

  g_mutex_unlock (&self->association_mutex);
}

/* Public functions */

GstSctpAssociation *
gst_sctp_association_get (guint32 association_id)
{
  GstSctpAssociation *association;

  G_LOCK (associations_lock);
  GST_DEBUG_CATEGORY_INIT (gst_sctp_association_debug_category,
      "sctpassociation", 0, "debug category for sctpassociation");
  GST_DEBUG_CATEGORY_INIT (gst_sctp_debug_category,
      "dcsctp", 0, "debug category for messages from dcSCTP");

  if (!associations_by_id) {
    g_assert (ids_by_association == NULL);

    associations_by_id =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
    ids_by_association =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  }

  association =
      g_hash_table_lookup (associations_by_id,
      GUINT_TO_POINTER (association_id));
  if (!association) {
    association =
        g_object_new (GST_SCTP_TYPE_ASSOCIATION, "association-id",
        association_id, NULL);
    g_hash_table_insert (associations_by_id, GUINT_TO_POINTER (association_id),
        association);
  } else {
    g_object_ref (association);
  }
  G_UNLOCK (associations_lock);
  return association;
}

GstSctpAssociation *
gst_sctp_association_ref (GstSctpAssociation * self)
{
  GstSctpAssociation *ref = NULL;
  G_LOCK (associations_lock);
  if (self)
    ref = g_object_ref (self);
  G_UNLOCK (associations_lock);
  return ref;
}

void
gst_sctp_association_unref (GstSctpAssociation * self)
{
  G_LOCK (associations_lock);
  g_object_unref (self);

  if (g_hash_table_size (associations_by_id) == 0) {
    /* demand all association have ben deregistered */
    g_assert (g_hash_table_size (ids_by_association) == 0);

    g_hash_table_destroy (associations_by_id);
    g_hash_table_destroy (ids_by_association);
    associations_by_id = NULL;
    ids_by_association = NULL;
  }

  G_UNLOCK (associations_lock);
}

static SctpSocket_SendPacketStatus
gst_sctp_association_send_packet (void *user_data, const uint8_t * data,
    size_t len)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  g_assert (self->encoder_ctx.packet_out_cb);
  g_assert (self->encoder_ctx.element);
  self->encoder_ctx.packet_out_cb (self, data, len, self->encoder_ctx.element);

  g_mutex_unlock (&self->association_mutex);

  return SCTP_SOCKET_SEND_PACKET_STATUS_SUCCESS;
}

static void
gst_sctp_association_on_message_received (void *user_data,
    uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  g_assert (self->decoder_ctx.packet_received_cb);
  g_assert (self->decoder_ctx.element);
  self->decoder_ctx.packet_received_cb (self, data, len, stream_id, ppid,
      self->decoder_ctx.element);

  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_on_error (void *user_data, SctpSocket_Error error)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  GST_ERROR ("error: %u", error);

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_ERROR);

  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_on_aborted (void *user_data, SctpSocket_Error error)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  GST_ERROR ("error: %u", error);

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_ERROR);

  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_on_connected (void *user_data)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_CONNECTED);

  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_on_closed (void *user_data)
{
  GstSctpAssociation *self = user_data;

  g_mutex_lock (&self->association_mutex);

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);

  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_on_connection_restarted (void *user_data)
{
  (void) user_data;
  GST_ERROR ("!");
}

static void
gst_sctp_association_on_streams_reset_failed (void *user_data,
    const uint16_t * streams, size_t len)
{
  GST_ERROR ("!");
  (void) user_data;
  (void) streams;
  (void) len;
}

static void
gst_sctp_association_on_streams_reset_performed (void *user_data,
    const uint16_t * streams, size_t len)
{
  GST_ERROR ("!");
  (void) user_data;
  (void) streams;
  (void) len;
}

static void
gst_sctp_association_on_incoming_streams_reset (void *user_data,
    const uint16_t * streams, size_t len)
{
  GST_ERROR ("!");
  (void) user_data;
  (void) streams;
  (void) len;
}

static void
gst_sctp_association_on_buffered_amount_low (void *user_data,
    uint16_t stream_id)
{
  GST_ERROR ("!");
  (void) user_data;
  (void) stream_id;

}

static void
gst_sctp_association_on_total_buffered_amount_low (void *user_data)
{
  GST_ERROR ("!");
  (void) user_data;
}

static gboolean
gst_sctp_association_start_unlocked (GstSctpAssociation * self)
{
  if (self->state != GST_SCTP_ASSOCIATION_STATE_READY &&
      self->state != GST_SCTP_ASSOCIATION_STATE_DISCONNECTED) {
    GST_WARNING_OBJECT (self,
        "SCTP association is in wrong state and cannot be started");
    return FALSE;
  }

  g_assert (!self->socket);

  SctpSocket_Callbacks callbacks = {
    .send_packet = gst_sctp_association_send_packet,
    .on_message_received = gst_sctp_association_on_message_received,
    .on_error = gst_sctp_association_on_error,
    .on_aborted = gst_sctp_association_on_aborted,
    .on_connected = gst_sctp_association_on_connected,
    .on_closed = gst_sctp_association_on_closed,
    .on_connection_restarted = gst_sctp_association_on_connection_restarted,
    .on_streams_reset_failed = gst_sctp_association_on_streams_reset_failed,
    .on_streams_reset_performed =
        gst_sctp_association_on_streams_reset_performed,
    .on_incoming_streams_reset = gst_sctp_association_on_incoming_streams_reset,
    .on_buffered_amount_low = gst_sctp_association_on_buffered_amount_low,
    .on_total_buffered_amount_low =
        gst_sctp_association_on_total_buffered_amount_low,
    .user_data = self
  };

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_CONNECTING);

  self->socket =
      sctp_socket_new (self->local_port, self->remote_port, 256 * 1024,
      &callbacks);
  sctp_socket_connect (self->socket);

  return TRUE;
}

gboolean
gst_sctp_association_start (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  gboolean ret = gst_sctp_association_start_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
  return ret;
}

void
gst_sctp_association_set_encoder_ctx (GstSctpAssociation * self,
    GstSctpAssociationEncoderCtx * ctx)
{
  g_return_if_fail (GST_SCTP_IS_ASSOCIATION (self));

  g_mutex_lock (&self->association_mutex);

  if (self->encoder_ctx.element)
    gst_object_unref (self->encoder_ctx.element);

  g_assert (ctx);
  self->encoder_ctx = *ctx;

  if (ctx->element)
    self->encoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
}

void
gst_sctp_association_set_decoder_ctx (GstSctpAssociation * self,
    GstSctpAssociationDecoderCtx * ctx)
{
  g_return_if_fail (GST_SCTP_IS_ASSOCIATION (self));

  g_mutex_lock (&self->association_mutex);

  if (self->decoder_ctx.element)
    gst_object_unref (self->decoder_ctx.element);

  g_assert (ctx);
  self->decoder_ctx = *ctx;

  if (ctx->element)
    self->decoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
}

void
gst_sctp_association_incoming_packet (GstSctpAssociation * self,
    const guint8 * buf, guint32 length)
{
  g_mutex_lock (&self->association_mutex);

  if (self->socket) {
    sctp_socket_receive_packet (self->socket, (const uint8_t *) buf,
        (size_t) length);
  } else {
    GST_WARNING ("Couldn't process buffer (%p with length %" G_GUINT32_FORMAT
        "), missing socket", buf, length);
  }

  g_mutex_unlock (&self->association_mutex);
}

GstFlowReturn
gst_sctp_association_send_data (GstSctpAssociation * self, const guint8 * buf,
    guint32 length, guint16 stream_id, guint32 ppid, gboolean ordered,
    GstSctpAssociationPartialReliability pr, guint32 reliability_param,
    guint32 * bytes_sent_)
{
  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    if (self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTED ||
        self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
      GST_INFO_OBJECT (self, "Disconnected");
      g_mutex_unlock (&self->association_mutex);
      return GST_FLOW_EOS;
    } else {
      GST_ERROR_OBJECT (self, "Association not connected yet");
      g_mutex_unlock (&self->association_mutex);
      return GST_FLOW_ERROR;
    }
  }

  /* TODO: check for stream id state */
  g_assert (self->socket);

  int32_t *lifetime = NULL;
  size_t *max_retransmissions = NULL;

  if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_TTL) {
    *lifetime = reliability_param;
  } else if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_RTX) {
    *max_retransmissions = reliability_param;
  } else {
    GST_DEBUG ("Ignoring reliability parameter %d", pr);
  }

  SctpSocket_SendStatus send_status =
      sctp_socket_send (self->socket, buf, length, stream_id, ppid, !ordered,
      lifetime, max_retransmissions);
  GST_ERROR ("send_status: %d", send_status);

  g_mutex_unlock (&self->association_mutex);

  if (send_status != SCTP_SOCKET_STATUS_SUCCESS)
    return GST_FLOW_ERROR;


  return GST_FLOW_OK;
}

void
gst_sctp_association_reset_stream (GstSctpAssociation * self, guint16 stream_id)
{
  (void) self;
  (void) stream_id;
}

static void
force_close_unlocked (GstSctpAssociation * self)
{
  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED)
    return;

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);

  g_assert (self->socket);
  sctp_socket_close (self->socket);
  sctp_socket_free (self->socket);
  self->socket = NULL;

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
}

void
gst_sctp_association_force_close (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  force_close_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
}

static void
gst_sctp_association_disconnect_unlocked (GstSctpAssociation * self)
{
  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED)
    return;

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);

  g_assert (self->socket);
  sctp_socket_shutdown (self->socket);
}

void
gst_sctp_association_disconnect (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  gst_sctp_association_disconnect_unlocked (self);
  g_mutex_unlock (&self->association_mutex);
}

// static void
// gst_sctp_association_notify_restart (GstSctpAssociation * self)
// {
//   GstSctpAssociationRestartCb restart_cb;
//   gpointer user_data;

//   restart_cb = self->decoder_ctx.restart_cb;
//   user_data = self->decoder_ctx.element;
//   if (user_data)
//     gst_object_ref (user_data);

//   if (restart_cb)
//     restart_cb (self, user_data);

//   if (user_data)
//     gst_object_unref (user_data);
// }

// static void
// gst_sctp_association_notify_stream_reset (GstSctpAssociation * self,
//     guint16 stream_id)
// {
//   GstSctpAssociationStreamResetCb stream_reset_cb;
//   gpointer user_data;

//   stream_reset_cb = self->decoder_ctx.stream_reset_cb;
//   user_data = self->decoder_ctx.element;
//   if (user_data)
//     gst_object_ref (user_data);

//   if (stream_reset_cb)
//     stream_reset_cb (self, stream_id, user_data);

//   if (user_data)
//     gst_object_unref (user_data);
// }

// static void
// handle_stream_reset_event (GstSctpAssociation * self,
//     const struct sctp_stream_reset_event *sr)
// {
//   guint32 i, n;
//   if (!(sr->strreset_flags & SCTP_STREAM_RESET_DENIED) &&
//       !(sr->strreset_flags & SCTP_STREAM_RESET_DENIED)) {
//     n = (sr->strreset_length -
//         sizeof (struct sctp_stream_reset_event)) / sizeof (uint16_t);
//     for (i = 0; i < n; i++) {
//       if (sr->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
//         gst_sctp_association_notify_stream_reset (self,
//             sr->strreset_stream_list[i]);
//       }
//     }
//   }
// }

static void
gst_sctp_association_change_state_unlocked (GstSctpAssociation * self,
    GstSctpAssociationState new_state)
{
  gboolean notify = FALSE;
  GstSctpAssociationStateChangeCb callback = self->encoder_ctx.state_change_cb;
  gpointer encoder = self->encoder_ctx.element;

  if (self->state != new_state
      && self->state != GST_SCTP_ASSOCIATION_STATE_ERROR) {
    self->state = new_state;
    notify = TRUE;
  }

  /* return immediately if we don't have to notify */
  if (!notify)
    return;

  /* hold a ref on the association and the encoder, so we make sure they
     outlives the callback execution */
  gst_sctp_association_ref (self);
  if (encoder)
    gst_object_ref (encoder);

  /* release the association mutex, so other calls can be done to the
     association */
  g_mutex_unlock (&self->association_mutex);

  if (callback)
    callback (self, new_state, encoder);

  g_mutex_lock (&self->association_mutex);

  if (encoder)
    gst_object_unref (encoder);
  gst_sctp_association_unref (self);
}
