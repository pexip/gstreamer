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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dcsctpassociation.h"

#include <gst/gst.h>

GST_DEBUG_CATEGORY_STATIC (dcsctp_association_debug_category);
#define GST_CAT_DEFAULT dcsctp_association_debug_category

GST_DEBUG_CATEGORY_STATIC (dcsctplib_log_category);
#define DCSCTPLIB_CAT dcsctplib_log_category

#define DCSCTP_ASSOCIATION_STATE_TYPE (dcsctp_association_state_get_type())

static GType
dcsctp_association_state_get_type (void)
{
  static const GEnumValue values[] = {
    {DCSCTP_ASSOCIATION_STATE_NEW, "state-new", "state-new"},
    {DCSCTP_ASSOCIATION_STATE_READY, "state-ready", "state-ready"},
    {DCSCTP_ASSOCIATION_STATE_CONNECTING, "state-connecting",
        "state-connecting"},
    {DCSCTP_ASSOCIATION_STATE_CONNECTED, "state-connected",
        "state-connected"},
    {DCSCTP_ASSOCIATION_STATE_DISCONNECTING, "state-disconnecting",
        "state-disconnecting"},
    {DCSCTP_ASSOCIATION_STATE_DISCONNECTED, "state-disconnected",
        "state-disconnected"},
    {DCSCTP_ASSOCIATION_STATE_ERROR, "state-error", "state-error"},
    {0, NULL, NULL}
  };
  static GType id = 0;

  if (g_once_init_enter ((gsize *) & id)) {
    GType _id;
    _id = g_enum_register_static ("DCSCTPAssociationState", values);
    g_once_init_leave ((gsize *) & id, _id);
  }

  return id;
}

G_DEFINE_TYPE (DCSCTPAssociation, dcsctp_association, G_TYPE_OBJECT);

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

#define DEFAULT_NUMBER_OF_dcsctp_STREAMS 65535
#define DEFAULT_LOCAL_SCTP_PORT 0
#define DEFAULT_REMOTE_SCTP_PORT 0


#define DCSCTP_ASSOC_GET_MUTEX(assoc) (&assoc->association_mutex)
#define DCSCTP_ASSOC_MUTEX_LOCK(assoc) (g_rec_mutex_lock (DCSCTP_ASSOC_GET_MUTEX (assoc)))
#define DCSCTP_ASSOC_MUTEX_UNLOCK(assoc) (g_rec_mutex_unlock (DCSCTP_ASSOC_GET_MUTEX (assoc)))

/* Interface implementations */
static void dcsctp_association_finalize (GObject * object);
static void dcsctp_association_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void dcsctp_association_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static void maybe_set_state_to_ready_unlocked (DCSCTPAssociation * assoc);
static void dcsctp_association_change_state_unlocked (DCSCTPAssociation *
    assoc, DCSCTPAssociationState new_state);
static void dcsctp_association_cancel_pending_async (DCSCTPAssociation * assoc);
static gboolean force_close_async (DCSCTPAssociation * assoc);
static gboolean
dcsctp_association_open_stream (DCSCTPAssociation * assoc,
    guint16 stream_id);

#if defined(DCDCSCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)

static void
dcsctp_association_sctp_socket_log (SctpSocket_LoggingSeverity severity,
    const char *msg)
{
  GstDebugLevel level;

  switch (severity) {
    case SCTP_SOCKET_VERBOSE:
      level = GST_LEVEL_DEBUG;
      break;
    case SCTP_SOCKET_INFO:
      level = GST_LEVEL_INFO;
      break;
    case SCTP_SOCKET_WARNING:
      level = GST_LEVEL_WARNING;
      break;
    case SCTP_SOCKET_ERROR:
      level = GST_LEVEL_ERROR;
      break;
    case SCTP_SOCKET_NONE:
    default:
      level = GST_LEVEL_NONE;
      break;
  }

  gst_debug_log (DCSCTPLIB_CAT, level, __FILE__, GST_FUNCTION,
      __LINE__, NULL, msg, NULL);
}

#endif


static void
dcsctp_association_class_init (DCSCTPAssociationClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = dcsctp_association_finalize;
  gobject_class->set_property = dcsctp_association_set_property;
  gobject_class->get_property = dcsctp_association_get_property;

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
      "The state of the SCTP association", DCSCTP_ASSOCIATION_STATE_TYPE,
      DCSCTP_ASSOCIATION_STATE_NEW, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

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

  GST_DEBUG_CATEGORY_INIT (dcsctp_association_debug_category,
      "dcsctpassociation", 0, "debug category for dcsctpassociation");
  GST_DEBUG_CATEGORY_INIT (dcsctplib_log_category,
      "dcsctplib", 0, "debug category for messages from dcSCTP");

#if defined(DCDCSCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
  sctp_socket_register_logging_function (dcsctp_association_sctp_socket_log);
#else
  sctp_socket_register_logging_function (NULL);
#endif
}

static void
dcsctp_association_init (DCSCTPAssociation * assoc)
{
  assoc->local_port = DEFAULT_LOCAL_SCTP_PORT;
  assoc->remote_port = DEFAULT_REMOTE_SCTP_PORT;
  assoc->state = DCSCTP_ASSOCIATION_STATE_NEW;
  assoc->use_sock_stream = TRUE;

  assoc->pending_source_ids =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  assoc->stream_id_to_state =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) g_free);

  g_rec_mutex_init (DCSCTP_ASSOC_GET_MUTEX (assoc));
}

// Must be called from GSource async handler
static void
dcsctp_association_free_socket (DCSCTPAssociation * assoc)
{
  if (assoc->socket) {
    sctp_socket_free (assoc->socket);
    assoc->socket = NULL;
  }
}

static void
dcsctp_association_finalize (GObject * object)
{
  DCSCTPAssociation *assoc = DCSCTP_ASSOCIATION_CAST (object);

  // we have to cleanup any attached sources we might have pending
  dcsctp_association_cancel_pending_async (assoc);

  dcsctp_association_free_socket (assoc);

  g_rec_mutex_clear (DCSCTP_ASSOC_GET_MUTEX (assoc));
  g_hash_table_destroy (assoc->stream_id_to_state);
  g_hash_table_destroy (assoc->pending_source_ids);

  G_OBJECT_CLASS (dcsctp_association_parent_class)->finalize (object);
}

static void
dcsctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  DCSCTPAssociation *assoc = DCSCTP_ASSOCIATION_CAST (object);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->state != DCSCTP_ASSOCIATION_STATE_NEW) {
    switch (prop_id) {
      case PROP_LOCAL_PORT:
      case PROP_REMOTE_PORT:
        GST_ERROR_OBJECT (assoc,
            "These properties cannot be set in this state");
        DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
        return;
    }
  }

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      assoc->association_id = g_value_get_uint (value);
      break;
    case PROP_LOCAL_PORT:
      assoc->local_port = g_value_get_uint (value);
      break;
    case PROP_REMOTE_PORT:
      assoc->remote_port = g_value_get_uint (value);
      break;
    case PROP_STATE:
      assoc->state = g_value_get_enum (value);
      break;
    case PROP_USE_SOCK_STREAM:
      assoc->use_sock_stream = g_value_get_boolean (value);
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      assoc->aggressive_heartbeat = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (assoc, prop_id, pspec);
      break;
  }

  if (prop_id == PROP_LOCAL_PORT || prop_id == PROP_REMOTE_PORT)
    maybe_set_state_to_ready_unlocked (assoc);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return;
}

static void
dcsctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  DCSCTPAssociation *assoc = DCSCTP_ASSOCIATION_CAST (object);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  switch (prop_id) {
    case PROP_ASSOCIATION_ID:
      g_value_set_uint (value, assoc->association_id);
      break;
    case PROP_LOCAL_PORT:
      g_value_set_uint (value, assoc->local_port);
      break;
    case PROP_REMOTE_PORT:
      g_value_set_uint (value, assoc->remote_port);
      break;
    case PROP_STATE:
      g_value_set_enum (value, assoc->state);
      break;
    case PROP_USE_SOCK_STREAM:
      g_value_set_boolean (value, assoc->use_sock_stream);
      break;
    case PROP_AGGRESSIVE_HEARTBEAT:
      g_value_set_boolean (value, assoc->aggressive_heartbeat);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (assoc, prop_id, pspec);
      break;
  }

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
maybe_set_state_to_ready_unlocked (DCSCTPAssociation * assoc)
{
  if ((assoc->state == DCSCTP_ASSOCIATION_STATE_NEW)
      && (assoc->local_port != 0 && assoc->remote_port != 0)
      && (assoc->encoder_ctx.packet_out_cb != NULL)
      && (assoc->decoder_ctx.packet_received_cb != NULL)
      && (assoc->encoder_ctx.state_change_cb != NULL)) {
    dcsctp_association_change_state_unlocked (assoc,
        DCSCTP_ASSOCIATION_STATE_READY);
  }
}

static void
dcsctp_association_cancel_pending_async (DCSCTPAssociation * assoc)
{
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, assoc->pending_source_ids);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    guint source_id = GPOINTER_TO_UINT (key);
    GST_LOG_OBJECT (assoc, "source_id=%" G_GUINT32_FORMAT, source_id);
    GSource *source = g_main_context_find_source_by_id (assoc->main_context,
        source_id);
    if (source)
      g_source_destroy (source);
  }

  g_hash_table_remove_all (assoc->pending_source_ids);
}

// common return function for all attached GSourceFunc
static gboolean
dcsctp_association_async_return (DCSCTPAssociation * assoc)
{
  GSource *source = g_main_current_source ();
  g_assert (source);
  g_assert (assoc->main_context == g_source_get_context (source));

  guint source_id = g_source_get_id (source);

  GST_LOG_OBJECT (assoc, "source_id=%u", source_id);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  g_hash_table_remove (assoc->pending_source_ids, GUINT_TO_POINTER (source_id));
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return FALSE;
}

// call holding a lock on association_mutex
static guint
dcsctp_association_call_async (DCSCTPAssociation * assoc,
    guint timeout_ms, GSourceFunc func, gpointer data, GDestroyNotify notify)
{
  GSource *source = g_timeout_source_new (timeout_ms);
  g_source_set_callback (source, func, data != NULL ? data : assoc, notify);

  /* attach the source */
  guint source_id = g_source_attach (source, assoc->main_context);

  GST_LOG_OBJECT (assoc, "source_id=%u", source_id);

  /* register it, so we can cancel it later */
  g_assert (g_hash_table_insert (assoc->pending_source_ids,
          GUINT_TO_POINTER (source_id), NULL));

  g_source_unref (source);
  return source_id;
}

// Public functions

static SctpSocket_SendPacketStatus
dcsctp_association_send_packet (void *user_data, const uint8_t * data,
    size_t len)
{
  DCSCTPAssociation *assoc = user_data;

  GST_LOG_OBJECT (assoc, "Sendpacket ! %p %p %" G_GSIZE_FORMAT, assoc, data,
      len);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->encoder_ctx.packet_out_cb) {
    assoc->encoder_ctx.packet_out_cb (data, len, assoc->encoder_ctx.element);
  }
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return SCTP_SOCKET_SEND_PACKET_STATUS_SUCCESS;
}

static void
dcsctp_association_notify_packet_received_unlocked (DCSCTPAssociation *
    assoc, uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len)
{
  DCSCTPAssociationPacketReceivedCb callback =
      assoc->decoder_ctx.packet_received_cb;
  gpointer decoder = assoc->decoder_ctx.element;

  if (decoder)
    gst_object_ref (decoder);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (callback)
    callback (data, len, stream_id, ppid, decoder);
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (decoder)
    gst_object_unref (decoder);
}

static void
dcsctp_association_on_message_received (void *user_data,
    uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len)
{
  DCSCTPAssociation *assoc = user_data;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (!dcsctp_association_open_stream (assoc, stream_id)) {
    GST_INFO_OBJECT (assoc,
        "Skipping receiving data on invalid state with stream id=%u",
        stream_id);
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  dcsctp_association_notify_packet_received_unlocked (assoc, stream_id,
      ppid, data, len);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static const gchar *
sctp_socket_error_to_string (SctpSocket_Error error)
{
  switch (error) {
    case SCTP_SOCKET_SUCCESS:
      return "Success";
    case SCTP_SOCKET_ERROR_TOO_MANY_RETRIES:
      return "Too many retries";
    case SCTP_SOCKET_ERROR_NOT_CONNECTED:
      return "Not connected";
    case SCTP_SOCKET_ERROR_PARSE_FAILED:
      return "Parse failed";
    case SCTP_SOCKET_ERROR_WRONG_SEQUENCE:
      return "Wrong sequence";
    case SCTP_SOCKET_ERROR_PEER_REPORTED:
      return "Peer reported";
    case SCTP_SOCKET_ERROR_PROTOCOL_VIOLATION:
      return "Protocol violation";
    case SCTP_SOCKET_ERROR_RESOURCE_EXHAUSTION:
      return "Resource exhaustion";
    case SCTP_SOCKET_ERROR_UNSUPPORTED_OPERATION:
      return "Unsuported operation";
    default:
      return "Unknown SCTP socket error";
  }
}

static void
dcsctp_association_handle_error (DCSCTPAssociation * assoc,
    SctpSocket_Error error, const char *message)
{
  GST_ERROR_OBJECT (assoc, "error: %s - %s",
      sctp_socket_error_to_string (error), message);
  g_assert (error != SCTP_SOCKET_SUCCESS);

  if (error == SCTP_SOCKET_ERROR_TOO_MANY_RETRIES
      || error == SCTP_SOCKET_ERROR_PEER_REPORTED) {
    GST_DEBUG_OBJECT (assoc, "Too many retries! disconnecting...");
    force_close_async (assoc);
    return;
  }
}

static void
dcsctp_association_on_error (void *user_data, SctpSocket_Error error,
    const char *message)
{
  DCSCTPAssociation *assoc = user_data;
  dcsctp_association_handle_error (assoc, error, message);
}

static void
dcsctp_association_on_aborted (void *user_data, SctpSocket_Error error,
    const char *message)
{
  DCSCTPAssociation *assoc = user_data;
  dcsctp_association_handle_error (assoc, error, message);
}

static void
dcsctp_association_on_connected (void *user_data)
{
  DCSCTPAssociation *assoc = user_data;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_CONNECTED);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}


static void
dcsctp_association_notify_restart (DCSCTPAssociation * assoc)
{
  DCSCTPAssociationRestartCb restart_cb;
  GstElement *elem;

  restart_cb = assoc->decoder_ctx.restart_cb;
  elem = assoc->decoder_ctx.element;
  if (elem)
    gst_object_ref (elem);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (restart_cb)
    restart_cb (elem);
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (elem)
    gst_object_unref (elem);
}

static void
dcsctp_association_on_connection_restarted (void *user_data)
{
  DCSCTPAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "Connection restarted!");

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  dcsctp_association_notify_restart (assoc);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
dcsctp_association_on_streams_reset_failed (void *user_data,
    const uint16_t * streams, size_t len, const char *message)
{
  DCSCTPAssociation *assoc = user_data;

  for (size_t i = 0; i < len; i++) {
    uint16_t stream_id = streams[i];
    GST_WARNING_OBJECT (assoc, "Outgoing stream %u reset failed, reason:%s",
        stream_id, message);
  }
}

static void
dcsctp_association_notify_stream_reset (DCSCTPAssociation * assoc,
    guint16 stream_id)
{
  DCSCTPAssociationStreamResetCb stream_reset_cb;
  GstElement *elem;

  stream_reset_cb = assoc->decoder_ctx.stream_reset_cb;
  elem = assoc->decoder_ctx.element;
  if (elem)
    gst_object_ref (elem);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (stream_reset_cb)
    stream_reset_cb (stream_id, elem);
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (elem)
    gst_object_unref (elem);
}

static const gchar *
reset_stream_status_to_string (SctpSocket_ResetStreamStatus reset_status)
{
  switch (reset_status) {
    case SCTP_SOCKET_RESET_STREAM_STATUS_NOT_CONNECTED:
      return "Not connected";
    case SCTP_SOCKET_RESET_STREAM_STATUS_PERFORMED:
      return "Performed";
    case SCTP_SOCKET_RESET_STREAM_STATUS_NOT_SUPPORTED:
      return "Not supported";
    default:
      return "Unknown";
  }
}

typedef struct
{
  DCSCTPAssociation *assoc;
  guint16 stream_id;
} DCSCTPAssociationResetStreamCtx;


static gboolean
dcsctp_association_reset_stream_async (DCSCTPAssociationResetStreamCtx * ctx)
{
  DCSCTPAssociation *assoc = ctx->assoc;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (ctx->stream_id));
  if (!state) {
    GST_WARNING_OBJECT (assoc, "Couldn't reset stream %u, not present",
        ctx->stream_id);

    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return dcsctp_association_async_return (assoc);
  }

  state->closure_initiated = TRUE;

  if (assoc->socket) {
    SctpSocket_ResetStreamStatus status =
        sctp_socket_reset_streams (assoc->socket, &ctx->stream_id, 1);

    GST_DEBUG_OBJECT (assoc, "Reset stream %u status: %s", ctx->stream_id,
        reset_stream_status_to_string (status));

  } else {
    GST_LOG_OBJECT (assoc, "Couldn't reset stream %u, missing socket",
        ctx->stream_id);
  }

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return dcsctp_association_async_return (assoc);
}

static void
dcsctp_association_handle_stream_reset (DCSCTPAssociation * assoc,
    const uint16_t * streams, size_t len, gboolean incoming_reset)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  for (size_t i = 0; i < len; i++) {
    uint16_t stream_id = streams[i];
    gboolean notify_reset = FALSE;

    GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
        GUINT_TO_POINTER (stream_id));

    if (!state) {
      const gchar *reset_type = incoming_reset ? "incoming" : "outgoing";
      GST_DEBUG_OBJECT (assoc, "Ignoring %s reset on stream %u", reset_type,
          stream_id);
      continue;
    }

    if (incoming_reset) {
      state->incoming_reset_done = TRUE;
      notify_reset = state->outgoing_reset_done;

      if (!state->closure_initiated) {
        // When receiving an incoming stream reset event for a non local close
        // procedure, the association needs to reset the stream in the other
        // direction too.
        DCSCTPAssociationResetStreamCtx *ctx =
            g_new0 (DCSCTPAssociationResetStreamCtx, 1);
        ctx->assoc = assoc;
        ctx->stream_id = stream_id;

        dcsctp_association_call_async (assoc, 0,
            (GSourceFunc) dcsctp_association_reset_stream_async,
            ctx, (GDestroyNotify) g_free);

        // do not notify until we get the next reset notification from the socket
        notify_reset = FALSE;
      }

    } else {
      state->outgoing_reset_done = TRUE;
      notify_reset = state->incoming_reset_done;

      // demand we come from a sane state
      g_assert (state->closure_initiated);
    }

    if (notify_reset) {
      g_assert (g_hash_table_remove (assoc->stream_id_to_state,
              GUINT_TO_POINTER (stream_id)));
      dcsctp_association_notify_stream_reset (assoc, stream_id);
    }
  }
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

}

static void
dcsctp_association_on_streams_reset_performed (void *user_data,
    const uint16_t * streams, size_t len)
{
  DCSCTPAssociation *assoc = user_data;
  dcsctp_association_handle_stream_reset (assoc, streams, len, FALSE);
}

static void
dcsctp_association_on_incoming_streams_reset (void *user_data,
    const uint16_t * streams, size_t len)
{
  DCSCTPAssociation *assoc = user_data;
  dcsctp_association_handle_stream_reset (assoc, streams, len, TRUE);
}

static void
dcsctp_association_on_buffered_amount_low (void *user_data,
    uint16_t stream_id)
{
  DCSCTPAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "stream_id=%u", stream_id);
}

static void
dcsctp_association_on_total_buffered_amount_low (void *user_data)
{
  DCSCTPAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "!");
}

typedef struct
{
  DCSCTPAssociation *assoc;
  uint64_t timeout_id;
  guint source_id;

} GstSctpTimeout;

static gboolean
dcsctp_association_timeout_handle_async (GstSctpTimeout * timeout)
{
  DCSCTPAssociation *assoc = timeout->assoc;
  g_assert (assoc);

  if (assoc->socket) {
    GST_LOG_OBJECT (assoc,
        "timeout=%p  timeout_id=%" G_GUINT64_FORMAT, timeout,
        timeout->timeout_id);

    sctp_socket_handle_timeout (assoc->socket, timeout->timeout_id);
  } else {
    GST_INFO_OBJECT (assoc, "Couldn't handle timeout=%" G_GUINT64_FORMAT,
        timeout->timeout_id);
  }

  return dcsctp_association_async_return (assoc);
}

static void
dcsctp_association_timeout_start (void *user_data, void *void_timeout,
    int32_t milliseconds, uint64_t timeout_id)
{
  DCSCTPAssociation *assoc = user_data;
  GstSctpTimeout *timeout = void_timeout;

  timeout->assoc = assoc;
  timeout->timeout_id = timeout_id;

  g_assert (milliseconds > 0);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  guint id = dcsctp_association_call_async (assoc, (guint) milliseconds,
      (GSourceFunc) dcsctp_association_timeout_handle_async,
      timeout, NULL);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  timeout->source_id = id;

  GST_LOG_OBJECT (assoc,
      "timeout=%p %" GST_TIME_FORMAT " (%d) timeout_id=%" G_GUINT64_FORMAT
      " source_id=%" G_GUINT32_FORMAT, timeout,
      GST_TIME_ARGS (GST_MSECOND * milliseconds), milliseconds, timeout_id, id);
}

static void
dcsctp_association_timeout_stop (void *user_data, void *void_timeout)
{
  GstSctpTimeout *timeout = void_timeout;
  DCSCTPAssociation *assoc = user_data;

  GST_LOG_OBJECT (assoc,
      "timeout=%p timeout_id=%" G_GUINT64_FORMAT " source_id=%"
      G_GUINT32_FORMAT, timeout, timeout->timeout_id, timeout->source_id);

  GSource *source = g_main_context_find_source_by_id (assoc->main_context,
      timeout->source_id);
  if (!source)
    return;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  g_assert (g_hash_table_remove (assoc->pending_source_ids,
          GUINT_TO_POINTER (timeout->source_id)));
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  g_source_destroy (source);
}

static void *
dcsctp_association_timeout_create (void *user_data)
{
  GstSctpTimeout *timeout = g_new0 (GstSctpTimeout, 1);
  GST_LOG ("timeout=%p", timeout);
  return timeout;
}

static void
dcsctp_association_timeout_delete (void *user_data, void *void_timeout)
{
  GST_LOG ("timeout=%p", void_timeout);
  GstSctpTimeout *timeout = void_timeout;
  g_free (timeout);
}

static uint64_t
dcsctp_association_time_millis (void *user_data)
{
  (void) user_data;
  return (uint64_t) g_get_monotonic_time () / G_TIME_SPAN_MILLISECOND;
}

static uint32_t
dcsctp_association_get_random_int (void *user_data, uint32_t low, uint32_t high)
{
  return (uint32_t) g_random_int_range ((int32_t) low,
      MAX ((int32_t) high, G_MAXINT32));
}

static void
dcsctp_association_on_sent_packet (G_GNUC_UNUSED int64_t now,
    const uint8_t * data, size_t len)
{
  GST_CAT_MEMDUMP (DCSCTPLIB_CAT, "Sent pkt", data, (guint) len);
}

static void
dcsctp_association_on_received_packet (G_GNUC_UNUSED int64_t now,
    const uint8_t * data, size_t len)
{
  GST_CAT_MEMDUMP (DCSCTPLIB_CAT, "Received pkt", data, (guint) len);
}

typedef struct
{
  // common
  DCSCTPAssociation *assoc;
  uint8_t *data;
  size_t len;

  // send data
  guint16 stream_id;
  guint32 ppid;
  gboolean ordered;
  GstSctpSendMetaPartiallyReliability pr;
  guint32 reliability_param;

} DCSCTPAssociationAsyncContext;

static void
dcsctp_association_async_ctx_free (DCSCTPAssociationAsyncContext * ctx)
{
  if (ctx->data)
    g_free (ctx->data);
  g_free (ctx);
}


static gboolean
dcsctp_association_on_closed_async (DCSCTPAssociationAsyncContext * ctx)
{
  DCSCTPAssociation *assoc = ctx->assoc;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_DISCONNECTED);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  dcsctp_association_free_socket (assoc);

  return dcsctp_association_async_return (assoc);
}

static void
dcsctp_association_on_closed (void *user_data)
{
  DCSCTPAssociation *assoc = user_data;

  DCSCTPAssociationAsyncContext *ctx =
      g_new0 (DCSCTPAssociationAsyncContext, 1);
  ctx->assoc = assoc;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_on_closed_async,
      ctx, (GDestroyNotify) dcsctp_association_async_ctx_free);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static gboolean
dcsctp_association_connect_async (DCSCTPAssociation * assoc)
{
  g_assert (!assoc->socket);

  SctpSocket_Callbacks callbacks = {
    .send_packet = dcsctp_association_send_packet,
    .on_message_received = dcsctp_association_on_message_received,
    .on_error = dcsctp_association_on_error,
    .on_aborted = dcsctp_association_on_aborted,
    .on_connected = dcsctp_association_on_connected,
    .on_closed = dcsctp_association_on_closed,
    .on_connection_restarted = dcsctp_association_on_connection_restarted,
    .on_streams_reset_failed = dcsctp_association_on_streams_reset_failed,
    .on_streams_reset_performed = dcsctp_association_on_streams_reset_performed,
    .on_incoming_streams_reset = dcsctp_association_on_incoming_streams_reset,
    .on_buffered_amount_low = dcsctp_association_on_buffered_amount_low,
    .on_total_buffered_amount_low =
        dcsctp_association_on_total_buffered_amount_low,
    .timeout_create = dcsctp_association_timeout_create,
    .timeout_delete = dcsctp_association_timeout_delete,
    .timeout_start = dcsctp_association_timeout_start,
    .timeout_stop = dcsctp_association_timeout_stop,
    .time_millis = dcsctp_association_time_millis,
    .get_random_int = dcsctp_association_get_random_int,
    .on_sent_packet = NULL,
    .on_received_packet = NULL,
    .user_data = assoc
  };

  gboolean aggressive_heartbeat;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  aggressive_heartbeat = assoc->aggressive_heartbeat;
  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_CONNECTING);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);


  SctpSocket_Options opts;
  memset (&opts, 0, sizeof (SctpSocket_Options));

  opts.local_port = assoc->local_port;
  opts.remote_port = assoc->remote_port;
  opts.max_message_size = 256 * 1024;

  // When there is packet loss for a long time, the SCTP retry timers will use
  // exponential backoff, which can grow to very long durations and when the
  // connection recovers, it may take a long time to reach the new backoff
  // duration. By limiting it to a reasonable limit, the time to recover reduces.
  opts.max_timer_backoff_duration_ms = 3 * 1000;
  opts.heartbeat_interval_ms = aggressive_heartbeat ? 3 * 1000 : 30 * 1000;

  // keep the max rtx low so we can detect if the connection is broken
  opts.max_retransmissions = 3;
  opts.max_init_retransmits = -1;

  if (G_UNLIKELY (GST_LEVEL_MEMDUMP <= _gst_debug_min) &&
      GST_LEVEL_MEMDUMP <= gst_debug_category_get_threshold (DCSCTPLIB_CAT)) {
    callbacks.on_sent_packet = dcsctp_association_on_sent_packet;
    callbacks.on_received_packet = dcsctp_association_on_received_packet;
  }

  assoc->socket = sctp_socket_new (&opts, &callbacks);
  sctp_socket_connect (assoc->socket);

  return dcsctp_association_async_return (assoc);
}

static gboolean
dcsctp_association_incoming_packet_async (DCSCTPAssociationAsyncContext * ctx)
{
  DCSCTPAssociation *assoc = ctx->assoc;

  // We could receive a packet from DTLS-RTP via sctpdec before sctpenc has set
  // up our socket.
  if (assoc->socket) {
    sctp_socket_receive_packet (assoc->socket, (uint8_t *) ctx->data,
        (size_t) ctx->len);
  } else {
    GST_LOG_OBJECT (ctx->assoc,
        "Couldn't process buffer (%p with length %" G_GSIZE_FORMAT
        "), missing socket", ctx->data, ctx->len);
  }

  return dcsctp_association_async_return (assoc);
}

static const gchar *
send_status_to_string (SctpSocket_SendStatus send_status)
{
  switch (send_status) {
    case SCTP_SOCKET_STATUS_SUCCESS:
      return "Success";
    case SCTP_SOCKET_STATUS_MESSAGE_EMPTY:
      return "Message is empty";
    case SCTP_SOCKET_STATUS_MESSAGE_TOO_LARGE:
      return "Message is too large";
    case SCTP_SOCKET_STATUS_ERROR_RESOURCE_EXHAUSTION:
      return "Resource exhaustion";
    case SCTP_SOCKET_STATUS_ERROR_SHUTTING_DOWN:
      return "Shutting down";
    default:
      return "Unknown send status";
  }
}


static gboolean
dcsctp_association_send_abort_async (DCSCTPAssociationAsyncContext * ctx)
{
  DCSCTPAssociation *assoc = ctx->assoc;
  g_assert (assoc);
  if (assoc->socket) {
    sctp_socket_send_abort (assoc->socket, (const char *) ctx->data);
  } else {
    GST_WARNING_OBJECT (ctx->assoc, "Couldn't send abort, missing socket");
  }

  return dcsctp_association_async_return (assoc);
}

static gboolean
dcsctp_association_send_data_async (DCSCTPAssociationAsyncContext * ctx)
{
  DCSCTPAssociation *assoc = ctx->assoc;
  g_assert (assoc);
  if (!assoc->socket) {
    GST_WARNING_OBJECT (ctx->assoc,
        "Couldn't send data (%p with length %" G_GSIZE_FORMAT
        "), missing socket", ctx->data, ctx->len);
    return dcsctp_association_async_return (assoc);
  }

  int32_t *lifetime = NULL;
  size_t *max_retransmissions = NULL;

  if (!assoc->use_sock_stream) {
    if (ctx->pr == GST_SCTP_SEND_META_PARTIAL_RELIABILITY_TTL) {
      lifetime = g_new0 (int32_t, 1);
      *lifetime = (int32_t) ctx->reliability_param;
    } else if (ctx->pr == GST_SCTP_SEND_META_PARTIAL_RELIABILITY_RTX) {
      max_retransmissions = g_new0 (size_t, 1);
      *max_retransmissions = (size_t) ctx->reliability_param;
    } else if (ctx->pr != GST_SCTP_SEND_META_PARTIAL_RELIABILITY_NONE) {
      GST_DEBUG_OBJECT (assoc, "Ignoring reliability parameter %d", ctx->pr);
    }
  }

  SctpSocket_SendStatus send_status =
      sctp_socket_send (assoc->socket, ctx->data, ctx->len, ctx->stream_id,
      ctx->ppid, !ctx->ordered,
      lifetime, max_retransmissions);
  GST_LOG_OBJECT (assoc, "send_status=%d", send_status);

  if (send_status != SCTP_SOCKET_STATUS_SUCCESS) {
    GST_ERROR_OBJECT (assoc,
        "Error sending buffer:%p of %" G_GSIZE_FORMAT " bytes, status: %s",
        ctx->data, ctx->len, send_status_to_string (send_status));
  }

  g_free (lifetime);
  g_free (max_retransmissions);

  return dcsctp_association_async_return (assoc);
}

static gboolean
force_close_async (DCSCTPAssociation * assoc)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_DISCONNECTING);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (assoc->socket) {
    sctp_socket_close (assoc->socket);
  }
  dcsctp_association_free_socket (assoc);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_DISCONNECTED);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return dcsctp_association_async_return (assoc);
}

static gboolean
dcsctp_association_disconnect_async (DCSCTPAssociation * assoc)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  dcsctp_association_change_state_unlocked (assoc,
      DCSCTP_ASSOCIATION_STATE_DISCONNECTING);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (assoc->socket) {
    sctp_socket_shutdown (assoc->socket);
  } else {
    GST_WARNING_OBJECT (assoc, "Couldn't disconnect association; no socket");
  }

  return dcsctp_association_async_return (assoc);
}

void
dcsctp_association_set_encoder_ctx (DCSCTPAssociation * assoc,
    DCSCTPAssociationEncoderCtx * ctx)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->encoder_ctx.element)
    gst_object_unref (assoc->encoder_ctx.element);

  g_assert (ctx);
  assoc->encoder_ctx = *ctx;

  if (ctx->element)
    assoc->encoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (assoc);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
dcsctp_association_set_decoder_ctx (DCSCTPAssociation * assoc,
    DCSCTPAssociationDecoderCtx * ctx)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->decoder_ctx.element)
    gst_object_unref (assoc->decoder_ctx.element);

  g_assert (ctx);
  assoc->decoder_ctx = *ctx;

  if (ctx->element)
    assoc->decoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (assoc);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
dcsctp_association_incoming_packet (DCSCTPAssociation * assoc,
    const guint8 * buf, guint32 length)
{
  DCSCTPAssociationAsyncContext *ctx =
      g_new0 (DCSCTPAssociationAsyncContext, 1);

  ctx->assoc = assoc;
  ctx->data = g_memdup2 (buf, length);
  ctx->len = length;

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_incoming_packet_async,
      ctx, (GDestroyNotify) dcsctp_association_async_ctx_free);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static gboolean
dcsctp_association_open_stream (DCSCTPAssociation * assoc,
    guint16 stream_id)
{
  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id));

  if (state) {
    if (state->closure_initiated ||
        state->incoming_reset_done || state->outgoing_reset_done) {
      return FALSE;
    }

    /* if we have a state for this stream already, its open */
    return TRUE;
  }

  state = g_new0 (GstSctpStreamState, 1);
  g_hash_table_insert (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id), (gpointer) state);
  return TRUE;
}

void
dcsctp_association_send_abort (DCSCTPAssociation * assoc, const gchar * message)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  DCSCTPAssociationAsyncContext *ctx =
      g_new0 (DCSCTPAssociationAsyncContext, 1);
  ctx->assoc = assoc;
  ctx->data = (uint8_t *) g_strdup (message);

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_send_abort_async,
      ctx, (GDestroyNotify) dcsctp_association_async_ctx_free);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

GstFlowReturn
dcsctp_association_send_data (DCSCTPAssociation * assoc,
    const guint8 * buf, gsize length, guint16 stream_id, guint32 ppid,
    gboolean ordered, GstSctpSendMetaPartiallyReliability pr,
    guint32 reliability_param)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (!dcsctp_association_open_stream (assoc, stream_id)) {
    GST_INFO_OBJECT (assoc,
        "Skipping send data on invalid state with stream id:%u", stream_id);
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return GST_FLOW_ERROR;
  }

  DCSCTPAssociationAsyncContext *ctx =
      g_new0 (DCSCTPAssociationAsyncContext, 1);
  ctx->assoc = assoc;
  ctx->data = g_memdup2 (buf, length);
  ctx->len = length;
  ctx->stream_id = stream_id;
  ctx->ppid = ppid;
  ctx->ordered = ordered;
  ctx->pr = pr;
  ctx->reliability_param = reliability_param;

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_send_data_async,
      ctx, (GDestroyNotify) dcsctp_association_async_ctx_free);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return GST_FLOW_OK;
}

void
dcsctp_association_reset_stream (DCSCTPAssociation * assoc,
    guint16 stream_id)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->state != DCSCTP_ASSOCIATION_STATE_CONNECTED) {
    if (assoc->state == DCSCTP_ASSOCIATION_STATE_DISCONNECTED ||
        assoc->state == DCSCTP_ASSOCIATION_STATE_DISCONNECTING) {
      GST_INFO_OBJECT (assoc, "Disconnected");
    } else {
      GST_ERROR_OBJECT (assoc, "Association not connected yet");
    }
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id));
  if (!state) {
    GST_INFO_OBJECT (assoc, "Stream id %u is not open, cannot reset!",
        stream_id);
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  if (state->closure_initiated || state->incoming_reset_done
      || state->outgoing_reset_done) {
    GST_INFO_OBJECT (assoc,
        "Stream id %u is already resetting, cannot reset again", stream_id);
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  DCSCTPAssociationResetStreamCtx *ctx =
      g_new0 (DCSCTPAssociationResetStreamCtx, 1);
  ctx->assoc = assoc;
  ctx->stream_id = stream_id;

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_reset_stream_async,
      ctx, (GDestroyNotify) g_free);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
dcsctp_association_force_close (DCSCTPAssociation * assoc)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != DCSCTP_ASSOCIATION_STATE_CONNECTED) {
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) force_close_async, NULL, NULL);
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

gboolean
dcsctp_association_connect (DCSCTPAssociation * assoc)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != DCSCTP_ASSOCIATION_STATE_READY &&
      assoc->state != DCSCTP_ASSOCIATION_STATE_DISCONNECTED) {
    GST_WARNING_OBJECT (assoc,
        "SCTP association is in wrong state and cannot be started");

    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return FALSE;
  }

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_connect_async, NULL, NULL);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
  return TRUE;
}

void
dcsctp_association_disconnect (DCSCTPAssociation * assoc)
{
  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != DCSCTP_ASSOCIATION_STATE_CONNECTED) {
    DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  dcsctp_association_call_async (assoc, 0,
      (GSourceFunc) dcsctp_association_disconnect_async, NULL, NULL);

  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
dcsctp_association_change_state_unlocked (DCSCTPAssociation * assoc,
    DCSCTPAssociationState new_state)
{
  gboolean notify = FALSE;
  DCSCTPAssociationStateChangeCb callback = assoc->encoder_ctx.state_change_cb;
  gpointer encoder = assoc->encoder_ctx.element;

  if (assoc->state != new_state
      && assoc->state != DCSCTP_ASSOCIATION_STATE_ERROR) {
    assoc->state = new_state;
    notify = TRUE;
  }
  // return immediately if we don't have to notify 
  if (!notify)
    return;

  // hold a ref on the encoder, so we make sure they outlives the callback execution
  if (encoder)
    gst_object_ref (encoder);

  // release the association mutex, so other calls can be done to the association 
  DCSCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (callback)
    callback (assoc, new_state, encoder);

  DCSCTP_ASSOC_MUTEX_LOCK (assoc);

  if (encoder)
    gst_object_unref (encoder);
}
