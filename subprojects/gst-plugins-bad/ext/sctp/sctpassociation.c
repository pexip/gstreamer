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

#include "sctpassociation.h"

#include <gst/gst.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY_STATIC (gst_sctp_association_debug_category);
#define GST_CAT_DEFAULT gst_sctp_association_debug_category

GST_DEBUG_CATEGORY_STATIC (sctplib_log_category);
#define SCTPLIB_CAT sctplib_log_category

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


#define GST_SCTP_ASSOC_GET_MUTEX(assoc) (&assoc->association_mutex)
#define GST_SCTP_ASSOC_MUTEX_LOCK(assoc) (g_rec_mutex_lock (GST_SCTP_ASSOC_GET_MUTEX (assoc)))
#define GST_SCTP_ASSOC_MUTEX_UNLOCK(assoc) (g_rec_mutex_unlock (GST_SCTP_ASSOC_GET_MUTEX (assoc)))

/* Interface implementations */
static void gst_sctp_association_finalize (GObject * object);
static void gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void maybe_set_state_to_ready_unlocked (GstSctpAssociation * assoc);
static void gst_sctp_association_change_state_unlocked (GstSctpAssociation *
    assoc, GstSctpAssociationState new_state);
static void gst_sctp_association_cancel_pending_async (GstSctpAssociation *
    assoc);
static gboolean force_close_async (GstSctpAssociation * assoc);
static void
gst_sctp_association_reset_stream_unlocked (GstSctpAssociation * assoc,
    uint16_t stream_id);
static gboolean
gst_sctp_association_open_stream (GstSctpAssociation * assoc,
    guint16 stream_id);

#if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)

static void
gst_sctp_association_sctp_socket_log (SctpSocket_LoggingSeverity severity,
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

  gst_debug_log (SCTPLIB_CAT, level, __FILE__, GST_FUNCTION,
      __LINE__, NULL, msg, NULL);
}

#endif


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

  GST_DEBUG_CATEGORY_INIT (gst_sctp_association_debug_category,
      "sctpassociation", 0, "debug category for sctpassociation");
  GST_DEBUG_CATEGORY_INIT (sctplib_log_category,
      "sctplib", 0, "debug category for messages from dcSCTP");

#if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
  sctp_socket_register_logging_function (gst_sctp_association_sctp_socket_log);
#else
  sctp_socket_register_logging_function (NULL);
#endif
}

static void
gst_sctp_association_init (GstSctpAssociation * assoc)
{
  assoc->local_port = DEFAULT_LOCAL_SCTP_PORT;
  assoc->remote_port = DEFAULT_REMOTE_SCTP_PORT;
  assoc->state = GST_SCTP_ASSOCIATION_STATE_NEW;
  assoc->use_sock_stream = TRUE;

  assoc->pending_source_ids =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  assoc->stream_id_to_state =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) g_free);

  g_rec_mutex_init (GST_SCTP_ASSOC_GET_MUTEX (assoc));
}

static void
gst_sctp_association_finalize (GObject * object)
{
  GstSctpAssociation *assoc = GST_SCTP_ASSOCIATION (object);

  /* we have to cleanup any attached sources we might have pending */
  gst_sctp_association_cancel_pending_async (assoc);

  if (assoc->socket) {
    sctp_socket_free (assoc->socket);
    assoc->socket = NULL;
  }

  g_rec_mutex_clear (GST_SCTP_ASSOC_GET_MUTEX (assoc));
  g_hash_table_destroy (assoc->stream_id_to_state);
  g_hash_table_destroy (assoc->pending_source_ids);

  G_OBJECT_CLASS (gst_sctp_association_parent_class)->finalize (object);
}

static void
gst_sctp_association_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *assoc = GST_SCTP_ASSOCIATION (object);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_NEW) {
    switch (prop_id) {
      case PROP_LOCAL_PORT:
      case PROP_REMOTE_PORT:
        GST_ERROR_OBJECT (assoc,
            "These properties cannot be set in this state");
        GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
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

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return;
}

static void
gst_sctp_association_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSctpAssociation *assoc = GST_SCTP_ASSOCIATION (object);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

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

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
maybe_set_state_to_ready_unlocked (GstSctpAssociation * assoc)
{
  if ((assoc->state == GST_SCTP_ASSOCIATION_STATE_NEW)
      && (assoc->local_port != 0 && assoc->remote_port != 0)
      && (assoc->encoder_ctx.packet_out_cb != NULL)
      && (assoc->decoder_ctx.packet_received_cb != NULL)
      && (assoc->encoder_ctx.state_change_cb != NULL)) {
    gst_sctp_association_change_state_unlocked (assoc,
        GST_SCTP_ASSOCIATION_STATE_READY);
  }
}

static void
gst_sctp_association_cancel_pending_async (GstSctpAssociation * assoc)
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

/* common return function for all attached GSourceFunc */
static gboolean
gst_sctp_association_async_return (GstSctpAssociation * assoc)
{
  (void) assoc;
  GSource *source = g_main_current_source ();
  g_assert (source);
  g_assert (assoc->main_context == g_source_get_context (source));

  guint source_id = g_source_get_id (source);

  GST_LOG_OBJECT (assoc, "source_id=%u", source_id);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  g_hash_table_remove (assoc->pending_source_ids, GUINT_TO_POINTER (source_id));
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return FALSE;
}

/* call holding a lock on association_mutex */
static guint
gst_sctp_association_call_async (GstSctpAssociation * assoc, guint timeout_ms,
    GSourceFunc func, gpointer data, GDestroyNotify notify)
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

/* Public functions */

static SctpSocket_SendPacketStatus
gst_sctp_association_send_packet (void *user_data, const uint8_t * data,
    size_t len)
{
  GstSctpAssociation *assoc = user_data;

  GST_LOG_OBJECT (assoc, "Sendpacket ! %p %p %" G_GSIZE_FORMAT, assoc, data,
      len);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->encoder_ctx.packet_out_cb) {
    assoc->encoder_ctx.packet_out_cb (data, len, assoc->encoder_ctx.element);
  }
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return SCTP_SOCKET_SEND_PACKET_STATUS_SUCCESS;
}

static void
gst_sctp_association_notify_packet_received_unlocked (GstSctpAssociation *
    assoc, uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len)
{
  GstSctpAssociationPacketReceivedCb callback =
      assoc->decoder_ctx.packet_received_cb;
  gpointer decoder = assoc->decoder_ctx.element;

  if (decoder)
    gst_object_ref (decoder);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (callback)
    callback (data, len, stream_id, ppid, decoder);
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (decoder)
    gst_object_unref (decoder);
}

static void
gst_sctp_association_on_message_received (void *user_data,
    uint16_t stream_id, uint32_t ppid, const uint8_t * data, size_t len)
{
  GstSctpAssociation *assoc = user_data;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (!gst_sctp_association_open_stream (assoc, stream_id)) {
    GST_INFO_OBJECT (assoc,
        "Skipping receiving data on invalid state with stream id=%u",
        stream_id);
    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  gst_sctp_association_notify_packet_received_unlocked (assoc, stream_id, ppid,
      data, len);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
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
gst_sctp_association_handle_error (GstSctpAssociation * assoc,
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
gst_sctp_association_on_error (void *user_data, SctpSocket_Error error,
    const char *message)
{
  GstSctpAssociation *assoc = user_data;
  gst_sctp_association_handle_error (assoc, error, message);
}

static void
gst_sctp_association_on_aborted (void *user_data, SctpSocket_Error error,
    const char *message)
{
  GstSctpAssociation *assoc = user_data;
  gst_sctp_association_handle_error (assoc, error, message);
}

static void
gst_sctp_association_on_connected (void *user_data)
{
  GstSctpAssociation *assoc = user_data;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_CONNECTED);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
gst_sctp_association_on_closed (void *user_data)
{
  GstSctpAssociation *assoc = user_data;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  g_assert (assoc->socket);
  sctp_socket_free (assoc->socket);
  assoc->socket = NULL;
}

static void
gst_sctp_association_notify_restart (GstSctpAssociation * assoc)
{
  GstSctpAssociationRestartCb restart_cb;
  gpointer user_data;

  restart_cb = assoc->decoder_ctx.restart_cb;
  user_data = assoc->decoder_ctx.element;
  if (user_data)
    gst_object_ref (user_data);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (restart_cb)
    restart_cb (user_data);
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (user_data)
    gst_object_unref (user_data);
}

static void
gst_sctp_association_on_connection_restarted (void *user_data)
{
  GstSctpAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "Connection restarted!");

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_notify_restart (assoc);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
gst_sctp_association_on_streams_reset_failed (void *user_data,
    const uint16_t * streams, size_t len, const char *message)
{
  GstSctpAssociation *assoc = user_data;

  for (size_t i = 0; i < len; i++) {
    uint16_t stream_id = streams[i];
    GST_WARNING_OBJECT (assoc, "Outgoing stream %u reset failed, reason:%s",
        stream_id, message);
  }
}

static void
gst_sctp_association_notify_stream_reset (GstSctpAssociation * assoc,
    guint16 stream_id)
{
  GstSctpAssociationStreamResetCb stream_reset_cb;
  gpointer user_data;

  stream_reset_cb = assoc->decoder_ctx.stream_reset_cb;
  user_data = assoc->decoder_ctx.element;
  if (user_data)
    gst_object_ref (user_data);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
  if (stream_reset_cb)
    stream_reset_cb (stream_id, user_data);
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (user_data)
    gst_object_unref (user_data);
}

static void
gst_sctp_association_handle_stream_reset (GstSctpAssociation * assoc,
    const uint16_t * streams, size_t len, gboolean incoming_reset)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
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
        gst_sctp_association_reset_stream_unlocked (assoc, stream_id);

        // do not notify until we get the next reset notification from the socket
        notify_reset = FALSE;
      }

    } else {
      state->outgoing_reset_done = TRUE;
      notify_reset = state->incoming_reset_done;

      /* demand we come from a sane state */
      g_assert (state->closure_initiated);
    }

    if (notify_reset) {
      g_assert (g_hash_table_remove (assoc->stream_id_to_state,
              GUINT_TO_POINTER (stream_id)));
      gst_sctp_association_notify_stream_reset (assoc, stream_id);
    }
  }
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

}

static void
gst_sctp_association_on_streams_reset_performed (void *user_data,
    const uint16_t * streams, size_t len)
{
  GstSctpAssociation *assoc = user_data;
  gst_sctp_association_handle_stream_reset (assoc, streams, len, FALSE);
}

static void
gst_sctp_association_on_incoming_streams_reset (void *user_data,
    const uint16_t * streams, size_t len)
{
  GstSctpAssociation *assoc = user_data;
  gst_sctp_association_handle_stream_reset (assoc, streams, len, TRUE);
}

static void
gst_sctp_association_on_buffered_amount_low (void *user_data,
    uint16_t stream_id)
{
  GstSctpAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "stream_id=%u", stream_id);
}

static void
gst_sctp_association_on_total_buffered_amount_low (void *user_data)
{
  GstSctpAssociation *assoc = user_data;
  GST_INFO_OBJECT (assoc, "!");
}

typedef struct
{
  GstSctpAssociation *assoc;
  uint64_t timeout_id;
  guint source_id;

} GstSctpTimeout;

static gboolean
gst_sctp_association_timeout_handle_async (GstSctpTimeout * timeout)
{
  GstSctpAssociation *assoc = timeout->assoc;
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

  return gst_sctp_association_async_return (assoc);
}

static void
gst_sctp_association_timeout_start (void *user_data, void *void_timeout,
    int32_t milliseconds, uint64_t timeout_id)
{
  GstSctpAssociation *assoc = user_data;
  GstSctpTimeout *timeout = void_timeout;

  timeout->assoc = assoc;
  timeout->timeout_id = timeout_id;

  g_assert (milliseconds > 0);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  guint id = gst_sctp_association_call_async (assoc, (guint) milliseconds,
      (GSourceFunc) gst_sctp_association_timeout_handle_async,
      timeout, NULL);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  timeout->source_id = id;

  GST_LOG_OBJECT (assoc,
      "timeout=%p %" GST_TIME_FORMAT " (%d) timeout_id=%" G_GUINT64_FORMAT
      " source_id=%" G_GUINT32_FORMAT, timeout,
      GST_TIME_ARGS (GST_MSECOND * milliseconds), milliseconds, timeout_id, id);
}

static void
gst_sctp_association_timeout_stop (void *user_data, void *void_timeout)
{
  GstSctpTimeout *timeout = void_timeout;
  GstSctpAssociation *assoc = user_data;

  GST_LOG_OBJECT (assoc,
      "timeout=%p timeout_id=%" G_GUINT64_FORMAT " source_id=%"
      G_GUINT32_FORMAT, timeout, timeout->timeout_id, timeout->source_id);

  GSource *source = g_main_context_find_source_by_id (assoc->main_context,
      timeout->source_id);
  if (!source)
    return;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  g_assert (g_hash_table_remove (assoc->pending_source_ids,
          GUINT_TO_POINTER (timeout->source_id)));
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  g_source_destroy (source);
}

static void *
gst_sctp_association_timeout_create (void *user_data)
{
  GstSctpTimeout *timeout = g_new0 (GstSctpTimeout, 1);
  GST_LOG ("timeout=%p", timeout);
  return timeout;
}

static void
gst_sctp_association_timeout_delete (void *user_data, void *void_timeout)
{
  GST_LOG ("timeout=%p", void_timeout);
  GstSctpTimeout *timeout = void_timeout;
  g_free (timeout);
}

static uint64_t
gst_sctp_association_time_millis (void *user_data)
{
  (void) user_data;
  return (uint64_t) g_get_monotonic_time () / G_TIME_SPAN_MILLISECOND;
}

static uint32_t
gst_sctp_association_get_random_int (void *user_data, uint32_t low,
    uint32_t high)
{
  return (uint32_t) g_random_int_range ((int32_t) low,
      MAX ((int32_t) high, G_MAXINT32));
}

static void
gst_sctp_association_on_sent_packet (int64_t now, const uint8_t * data,
    size_t len)
{
  (void) now;
  GST_CAT_MEMDUMP (SCTPLIB_CAT, "Sent pkt", data, len);
}

static void
gst_sctp_association_on_received_packet (int64_t now, const uint8_t * data,
    size_t len)
{
  (void) now;
  GST_CAT_MEMDUMP (SCTPLIB_CAT, "Received pkt", data, len);
}

typedef struct
{
  /* common */
  GstSctpAssociation *assoc;
  uint8_t *data;
  size_t len;

  /* send data */
  guint16 stream_id;
  guint32 ppid;
  gboolean ordered;
  GstSctpAssociationPartialReliability pr;
  guint32 reliability_param;

} GstSctpAssociationAsyncContext;

static void
gst_sctp_association_async_ctx_free (GstSctpAssociationAsyncContext * ctx)
{
  if (ctx->data)
    g_free (ctx->data);
  g_free (ctx);
}

static gboolean
gst_sctp_association_connect_async (GstSctpAssociation * assoc)
{
  g_assert (!assoc->socket);

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
    .timeout_create = gst_sctp_association_timeout_create,
    .timeout_delete = gst_sctp_association_timeout_delete,
    .timeout_start = gst_sctp_association_timeout_start,
    .timeout_stop = gst_sctp_association_timeout_stop,
    .time_millis = gst_sctp_association_time_millis,
    .get_random_int = gst_sctp_association_get_random_int,
    .on_sent_packet = NULL,
    .on_received_packet = NULL,
    .user_data = assoc
  };

  gboolean aggressive_heartbeat;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  aggressive_heartbeat = assoc->aggressive_heartbeat;
  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_CONNECTING);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);


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
      GST_LEVEL_MEMDUMP <= gst_debug_category_get_threshold (SCTPLIB_CAT)) {
    callbacks.on_sent_packet = gst_sctp_association_on_sent_packet;
    callbacks.on_received_packet = gst_sctp_association_on_received_packet;
  }

  assoc->socket = sctp_socket_new (&opts, &callbacks);
  sctp_socket_connect (assoc->socket);

  return gst_sctp_association_async_return (assoc);
}

static gboolean
gst_sctp_association_incoming_packet_async (GstSctpAssociationAsyncContext *
    ctx)
{
  GstSctpAssociation *assoc = ctx->assoc;

  if (assoc->socket) {
    sctp_socket_receive_packet (assoc->socket, (uint8_t *) ctx->data,
        (size_t) ctx->len);
  } else {
    GST_LOG_OBJECT (ctx->assoc,
        "Couldn't process buffer (%p with length %" G_GSIZE_FORMAT
        "), missing socket", ctx->data, ctx->len);
  }

  return gst_sctp_association_async_return (assoc);
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

static gboolean
gst_sctp_association_send_data_async (GstSctpAssociationAsyncContext * ctx)
{
  GstSctpAssociation *assoc = ctx->assoc;
  g_assert (assoc);
  g_assert (assoc->socket);

  int32_t *lifetime = NULL;
  size_t *max_retransmissions = NULL;

  if (!assoc->use_sock_stream) {
    if (ctx->pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_TTL) {
      lifetime = g_new0 (int32_t, 1);
      *lifetime = (int32_t) ctx->reliability_param;
    } else if (ctx->pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_RTX) {
      max_retransmissions = g_new0 (size_t, 1);
      *max_retransmissions = (size_t) ctx->reliability_param;
    } else if (ctx->pr != GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_NONE) {
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

  return gst_sctp_association_async_return (assoc);
}

static gboolean
force_close_async (GstSctpAssociation * assoc)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (assoc->socket) {
    sctp_socket_close (assoc->socket);
    sctp_socket_free (assoc->socket);
    assoc->socket = NULL;
  }

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return gst_sctp_association_async_return (assoc);
}

static gboolean
gst_sctp_association_disconnect_async (GstSctpAssociation * assoc)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_change_state_unlocked (assoc,
      GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  g_assert (assoc->socket);
  sctp_socket_shutdown (assoc->socket);

  return gst_sctp_association_async_return (assoc);
}

void
gst_sctp_association_set_encoder_ctx (GstSctpAssociation * assoc,
    GstSctpAssociationEncoderCtx * ctx)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->encoder_ctx.element)
    gst_object_unref (assoc->encoder_ctx.element);

  g_assert (ctx);
  assoc->encoder_ctx = *ctx;

  if (ctx->element)
    assoc->encoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (assoc);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
gst_sctp_association_set_decoder_ctx (GstSctpAssociation * assoc,
    GstSctpAssociationDecoderCtx * ctx)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->decoder_ctx.element)
    gst_object_unref (assoc->decoder_ctx.element);

  g_assert (ctx);
  assoc->decoder_ctx = *ctx;

  if (ctx->element)
    assoc->decoder_ctx.element = gst_object_ref (ctx->element);

  maybe_set_state_to_ready_unlocked (assoc);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
gst_sctp_association_incoming_packet (GstSctpAssociation * assoc,
    const guint8 * buf, guint32 length)
{
  GstSctpAssociationAsyncContext *ctx =
      g_new0 (GstSctpAssociationAsyncContext, 1);

  ctx->assoc = assoc;
  ctx->data = g_memdup2 (buf, length);
  ctx->len = length;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) gst_sctp_association_incoming_packet_async,
      ctx, (GDestroyNotify) gst_sctp_association_async_ctx_free);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static gboolean
gst_sctp_association_open_stream (GstSctpAssociation * assoc, guint16 stream_id)
{
  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id));

  if (state) {
    if (state->closure_initiated ||
        state->incoming_reset_done || state->outgoing_reset_done) {
      return FALSE;
    }
  }

  state = g_new0 (GstSctpStreamState, 1);
  g_hash_table_insert (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id), (gpointer) state);
  return TRUE;
}

GstFlowReturn
gst_sctp_association_send_data (GstSctpAssociation * assoc, const guint8 * buf,
    guint32 length, guint16 stream_id, guint32 ppid, gboolean ordered,
    GstSctpAssociationPartialReliability pr, guint32 reliability_param,
    guint32 * bytes_sent_)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    if (assoc->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTED ||
        assoc->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
      GST_INFO_OBJECT (assoc, "Disconnected");
      GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
      return GST_FLOW_EOS;
    } else {
      GST_ERROR_OBJECT (assoc, "Association not connected yet");
      GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
      return GST_FLOW_ERROR;
    }
  }

  if (!gst_sctp_association_open_stream (assoc, stream_id)) {
    GST_INFO_OBJECT (assoc,
        "Skipping send data on invalid state with stream id:%u", stream_id);
    return GST_FLOW_ERROR;
  }

  GstSctpAssociationAsyncContext *ctx =
      g_new0 (GstSctpAssociationAsyncContext, 1);
  ctx->assoc = assoc;
  ctx->data = g_memdup2 (buf, length);
  ctx->len = length;
  ctx->stream_id = stream_id;
  ctx->ppid = ppid;
  ctx->ordered = ordered;
  ctx->pr = pr;
  ctx->reliability_param = reliability_param;

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) gst_sctp_association_send_data_async,
      ctx, (GDestroyNotify) gst_sctp_association_async_ctx_free);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (bytes_sent_)
    *bytes_sent_ = length;

  return GST_FLOW_OK;
}

typedef struct
{
  GstSctpAssociation *assoc;
  guint16 stream_id;
} GstSctpAssociationResetStreamCtx;

 // call from the context thread holding a lock on association_mutex  
static void
gst_sctp_association_reset_stream_unlocked (GstSctpAssociation * assoc,
    uint16_t stream_id)
{
  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id));
  if (!state) {
    GST_WARNING_OBJECT (assoc, "Couldn't reset stream %u, not present",
        stream_id);
    return;
  }

  state->closure_initiated = TRUE;

  if (assoc->socket) {
    SctpSocket_ResetStreamStatus status =
        sctp_socket_reset_streams (assoc->socket, &stream_id, 1);

    GST_DEBUG_OBJECT (assoc, "Reset stream %u status: %s", stream_id,
        reset_stream_status_to_string (status));

  } else {
    GST_LOG_OBJECT (assoc, "Couldn't reset stream %u, missing socket",
        stream_id);
  }
}

static gboolean
gst_sctp_association_reset_stream_async (GstSctpAssociationResetStreamCtx * ctx)
{
  GstSctpAssociation *assoc = ctx->assoc;

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  gst_sctp_association_reset_stream_unlocked (assoc, ctx->stream_id);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  return gst_sctp_association_async_return (assoc);
}

void
gst_sctp_association_reset_stream (GstSctpAssociation * assoc,
    guint16 stream_id)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);
  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    if (assoc->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTED ||
        assoc->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
      GST_INFO_OBJECT (assoc, "Disconnected");
      GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
      return;
    } else {
      GST_ERROR_OBJECT (assoc, "Association not connected yet");
      GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
      return;
    }
  }

  GstSctpStreamState *state = g_hash_table_lookup (assoc->stream_id_to_state,
      GUINT_TO_POINTER (stream_id));
  if (!state) {
    GST_INFO_OBJECT (assoc, "Stream id %u is not open, cannot reset!",
        stream_id);
    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  if (state->closure_initiated || state->incoming_reset_done
      || state->outgoing_reset_done) {
    GST_INFO_OBJECT (assoc,
        "Stream id %u is already resetting, cannot reset again", stream_id);
    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  GstSctpAssociationResetStreamCtx *ctx =
      g_new0 (GstSctpAssociationResetStreamCtx, 1);
  ctx->assoc = assoc;
  ctx->stream_id = stream_id;

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) gst_sctp_association_reset_stream_async,
      ctx, (GDestroyNotify) g_free);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

void
gst_sctp_association_force_close (GstSctpAssociation * assoc)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) force_close_async, NULL, NULL);
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

gboolean
gst_sctp_association_connect (GstSctpAssociation * assoc)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_READY &&
      assoc->state != GST_SCTP_ASSOCIATION_STATE_DISCONNECTED) {
    GST_WARNING_OBJECT (assoc,
        "SCTP association is in wrong state and cannot be started");

    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return FALSE;
  }

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) gst_sctp_association_connect_async, NULL, NULL);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
  return TRUE;
}

void
gst_sctp_association_disconnect (GstSctpAssociation * assoc)
{
  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (assoc->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
    return;
  }

  gst_sctp_association_call_async (assoc, 0,
      (GSourceFunc) gst_sctp_association_disconnect_async, NULL, NULL);

  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);
}

static void
gst_sctp_association_change_state_unlocked (GstSctpAssociation * assoc,
    GstSctpAssociationState new_state)
{
  gboolean notify = FALSE;
  GstSctpAssociationStateChangeCb callback = assoc->encoder_ctx.state_change_cb;
  gpointer encoder = assoc->encoder_ctx.element;

  if (assoc->state != new_state
      && assoc->state != GST_SCTP_ASSOCIATION_STATE_ERROR) {
    assoc->state = new_state;
    notify = TRUE;
  }

  /* return immediately if we don't have to notify */
  if (!notify)
    return;

  /* hold a ref on the encoder, so we make sure they outlives the callback execution */
  if (encoder)
    gst_object_ref (encoder);

  /* release the association mutex, so other calls can be done to the
     association */
  GST_SCTP_ASSOC_MUTEX_UNLOCK (assoc);

  if (callback)
    callback (assoc, new_state, encoder);

  GST_SCTP_ASSOC_MUTEX_LOCK (assoc);

  if (encoder)
    gst_object_unref (encoder);
}
