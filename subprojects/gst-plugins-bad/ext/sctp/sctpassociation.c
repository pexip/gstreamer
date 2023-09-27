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

static void force_close_unlocked (GstSctpAssociation * self,
    gboolean change_state);
static struct socket *create_sctp_socket (GstSctpAssociation *
    gst_sctp_association);
static struct sockaddr_conn get_sctp_socket_address (GstSctpAssociation *
    gst_sctp_association, guint16 port);
static gboolean client_role_connect (GstSctpAssociation * self);
static int sctp_packet_out (void *addr, void *buffer, size_t length, guint8 tos,
    guint8 set_df);
static int receive_cb (struct socket *sock, union sctp_sockstore addr,
    void *data, size_t datalen, struct sctp_rcvinfo rcv_info, gint flags,
    void *ulp_info);
static void handle_notification (GstSctpAssociation * self,
    const union sctp_notification *notification, size_t length);
static void handle_association_changed (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac);
static void handle_stream_reset_event (GstSctpAssociation * self,
    const struct sctp_stream_reset_event *ssr);
static void handle_message (GstSctpAssociation * self, guint8 * data,
    guint32 datalen, guint16 stream_id, guint32 ppid);

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

#if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
#define USRSCTP_GST_DEBUG_LEVEL GST_LEVEL_DEBUG
static void
gst_usrsctp_debug (const gchar * format, ...)
{
  va_list varargs;

  va_start (varargs, format);
  gst_debug_log_valist (gst_sctp_debug_category, USRSCTP_GST_DEBUG_LEVEL,
      __FILE__, GST_FUNCTION, __LINE__, NULL, format, varargs);
  va_end (varargs);
}
#endif

static void
gst_sctp_association_init (GstSctpAssociation * self)
{
  self->local_port = DEFAULT_LOCAL_SCTP_PORT;
  self->remote_port = DEFAULT_REMOTE_SCTP_PORT;
  self->sctp_ass_sock = NULL;
  self->state = GST_SCTP_ASSOCIATION_STATE_NEW;
  self->use_sock_stream = TRUE;

  g_mutex_init (&self->association_mutex);
}

static void
gst_sctp_association_finalize (GObject * object)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (object);

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
        goto error;
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

error:
  g_mutex_unlock (&self->association_mutex);
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
}

static void
gst_sctp_association_usrsctp_init (void)
{
#if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
  usrsctp_init (0, sctp_packet_out, gst_usrsctp_debug);
#else
  usrsctp_init (0, sctp_packet_out, NULL);
#endif

  /* Explicit Congestion Notification */
  usrsctp_sysctl_set_sctp_ecn_enable (0);

  /* Do not send ABORTs in response to INITs (1).
   * Do not send ABORTs for received Out of the Blue packets (2).
   */
  usrsctp_sysctl_set_sctp_blackhole (2);

  /* Enable interleaving messages for different streams (incoming)
   * See: https://tools.ietf.org/html/rfc6458#section-8.1.20
   */
  usrsctp_sysctl_set_sctp_default_frag_interleave (2);

  usrsctp_sysctl_set_sctp_nr_outgoing_streams_default
      (DEFAULT_NUMBER_OF_SCTP_STREAMS);

#if defined(SCTP_DEBUG) && !defined(GST_DISABLE_GST_DEBUG)
  if (USRSCTP_GST_DEBUG_LEVEL <= GST_LEVEL_MAX
      && USRSCTP_GST_DEBUG_LEVEL <= _gst_debug_min
      && USRSCTP_GST_DEBUG_LEVEL <=
      gst_debug_category_get_threshold (gst_sctp_debug_category)) {
    usrsctp_sysctl_set_sctp_debug_on (SCTP_DEBUG_ALL);
  }
#endif
}

static void
gst_sctp_association_usrsctp_deinit (void)
{
  /* usrsctp_finish could fail, so retry for 5 seconds */
  gint ret;
  for (gint i = 0; i < 50; ++i) {
    ret = usrsctp_finish ();
    if (ret == 0) {
      GST_DEBUG ("usrsctp_finish() succeed");
      break;
    }

    GST_DEBUG ("usrsctp_finish() failed and returned %d", ret);
    g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  }

  if (ret != 0) {
    GST_WARNING ("usrsctp_finish() failed and returned %d", ret);
  }
}

/*
* Helper register/deregister functions to workaround bug sctplab/usrsctp#405
*
* The sctp socket can outlive the association, so we need to protect ourselves
* against being called with an invalid reference of GstSctpAssociation.
* To do so, only register/deregister when we create/close the socket in a
* thread-safe way.
*
*/
static void
gst_sctp_association_register (GstSctpAssociation * self)
{
  G_LOCK (associations_lock);

  /* demand we are not registering twice */
  g_assert (!g_hash_table_contains (ids_by_association, self));

  g_hash_table_insert (ids_by_association, self,
      GUINT_TO_POINTER (self->association_id));

  G_UNLOCK (associations_lock);

  usrsctp_register_address ((void *) self);
}

static void
gst_sctp_association_deregister (GstSctpAssociation * self)
{
  usrsctp_deregister_address ((void *) self);

  G_LOCK (associations_lock);

  /* demand we are not deregistering twice */
  g_assert (g_hash_table_contains (ids_by_association, self));

  g_hash_table_remove (ids_by_association, self);

  G_UNLOCK (associations_lock);
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
      "sctplib", 0, "debug category for messages from usrsctp");

  if (!associations_by_id) {
    g_assert (ids_by_association == NULL);

    associations_by_id =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
    ids_by_association =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);

    gst_sctp_association_usrsctp_init ();
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

    gst_sctp_association_usrsctp_deinit ();
  }
  G_UNLOCK (associations_lock);
}

static gboolean
gst_sctp_association_start_unlocked (GstSctpAssociation * self)
{
  if (self->state != GST_SCTP_ASSOCIATION_STATE_READY &&
      self->state != GST_SCTP_ASSOCIATION_STATE_DISCONNECTED) {
    GST_WARNING_OBJECT (self,
        "SCTP association is in wrong state and cannot be started");
    goto configure_required;
  }

  if ((self->sctp_ass_sock = create_sctp_socket (self)) == NULL)
    goto error;

  /* TODO: Support both server and client role */
  if (!client_role_connect (self)) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_ERROR);
    goto error;
  }

  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_CONNECTING);

  return TRUE;
error:
  gst_sctp_association_change_state_unlocked (self,
      GST_SCTP_ASSOCIATION_STATE_ERROR);
  return FALSE;
configure_required:
  return FALSE;
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
  if (self->sctp_ass_sock)
    usrsctp_conninput ((void *) self, (const void *) buf, (size_t) length, 0);
  g_mutex_unlock (&self->association_mutex);
}

GstFlowReturn
gst_sctp_association_send_data (GstSctpAssociation * self, const guint8 * buf,
    guint32 length, guint16 stream_id, guint32 ppid, gboolean ordered,
    GstSctpAssociationPartialReliability pr, guint32 reliability_param,
    guint32 * bytes_sent_)
{
  GstFlowReturn flow_ret;
  struct sctp_sendv_spa spa;
  gint32 bytes_sent = 0;
  struct sockaddr_conn remote_addr;

  g_mutex_lock (&self->association_mutex);
  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    if (self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTED ||
        self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
      GST_INFO_OBJECT (self, "Disconnected");
      flow_ret = GST_FLOW_EOS;
      g_mutex_unlock (&self->association_mutex);
      goto end;
    } else {
      GST_ERROR_OBJECT (self, "Association not connected yet");
      flow_ret = GST_FLOW_ERROR;
      g_mutex_unlock (&self->association_mutex);
      goto end;
    }
  }
  remote_addr = get_sctp_socket_address (self, self->remote_port);

  /* TODO: We probably want to split too large chunks into multiple packets
   * and only set the SCTP_EOR flag on the last one. Firefox is using 0x4000
   * as the maximum packet size
   */
  memset (&spa, 0, sizeof (spa));

  spa.sendv_sndinfo.snd_ppid = g_htonl (ppid);
  spa.sendv_sndinfo.snd_sid = stream_id;
  spa.sendv_sndinfo.snd_flags = SCTP_EOR | (ordered ? 0 : SCTP_UNORDERED);
  spa.sendv_sndinfo.snd_context = 0;
  spa.sendv_sndinfo.snd_assoc_id = 0;
  spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
  if (pr != GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_NONE) {
    spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
    spa.sendv_prinfo.pr_value = g_htonl (reliability_param);
    if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_TTL)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
    else if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_RTX)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
    else if (pr == GST_SCTP_ASSOCIATION_PARTIAL_RELIABILITY_BUF)
      spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_BUF;
  }

  bytes_sent =
      usrsctp_sendv (self->sctp_ass_sock, buf, length,
      (struct sockaddr *) &remote_addr, 1, (void *) &spa,
      (socklen_t) sizeof (struct sctp_sendv_spa), SCTP_SENDV_SPA, 0);

  g_mutex_unlock (&self->association_mutex);

  if (bytes_sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      bytes_sent = 0;
      /* Resending this buffer is taken care of by the gstsctpenc */
      flow_ret = GST_FLOW_OK;
      goto end;
    } else {
      GST_ERROR_OBJECT (self, "Error sending data on stream %u: (%u) %s",
          stream_id, errno, g_strerror (errno));
      flow_ret = GST_FLOW_ERROR;
      goto end;
    }
  }
  flow_ret = GST_FLOW_OK;

end:
  if (bytes_sent_)
    *bytes_sent_ = bytes_sent;

  return flow_ret;
}

void
gst_sctp_association_reset_stream (GstSctpAssociation * self, guint16 stream_id)
{
  struct sctp_reset_streams *srs;
  socklen_t length;

  g_mutex_lock (&self->association_mutex);

  if (self->state != GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    /* only allow resets on connected streams */
    g_mutex_unlock (&self->association_mutex);
    return;
  }

  length = (socklen_t) (sizeof (struct sctp_reset_streams) + sizeof (guint16));
  srs = (struct sctp_reset_streams *) g_malloc0 (length);
  srs->srs_flags = SCTP_STREAM_RESET_OUTGOING;
  srs->srs_number_streams = 1;
  srs->srs_stream_list[0] = stream_id;
  srs->srs_assoc_id = self->sctp_assoc_id;

  if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
          SCTP_RESET_STREAMS, srs, length) < 0) {
    GST_WARNING_OBJECT (self, "Resetting stream id=%u failed", stream_id);
  }

  g_mutex_unlock (&self->association_mutex);

  g_free (srs);
}

void
gst_sctp_association_force_close (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  force_close_unlocked (self, TRUE);
  g_mutex_unlock (&self->association_mutex);
}

static void
force_close_unlocked (GstSctpAssociation * self, gboolean change_state)
{
  if (self->sctp_ass_sock) {
    usrsctp_close (self->sctp_ass_sock);
    gst_sctp_association_deregister (self);
    self->sctp_ass_sock = NULL;
  }

  self->sctp_assoc_id = 0;

  if (change_state) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
  }
}

static void
gst_sctp_association_disconnect_unlocked (GstSctpAssociation * self,
    gboolean try_shutdown)
{
  if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);

    if (try_shutdown && self->use_sock_stream && self->sctp_ass_sock) {
      GST_INFO_OBJECT (self, "SCTP association shutting down");
      self->shutdown = FALSE;
      if (usrsctp_shutdown (self->sctp_ass_sock, SHUT_RDWR) == 0) {
        /* wait for shutdown to complete */
        guint cs_to_wait = 100; /* 1s */
        while (!self->shutdown && cs_to_wait > 0) {
          g_usleep (G_USEC_PER_SEC / 100);
          cs_to_wait--;
        }
        self->shutdown = FALSE;
      }
    }
  }

  /* Fall through to ensure the transition to disconnected occurs */
  if (self->state == GST_SCTP_ASSOCIATION_STATE_DISCONNECTING) {
    force_close_unlocked (self, FALSE);
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_DISCONNECTED);
    GST_INFO_OBJECT (self, "SCTP association disconnected!");
  }
}

void
gst_sctp_association_disconnect (GstSctpAssociation * self)
{
  g_mutex_lock (&self->association_mutex);
  gst_sctp_association_disconnect_unlocked (self, TRUE);
  g_mutex_unlock (&self->association_mutex);
}

static struct socket *
create_sctp_socket (GstSctpAssociation * self)
{
  struct socket *sock;
  struct linger l;
  struct sctp_event event;
  struct sctp_assoc_value stream_reset;
  int buf_size = 1024 * 1024;
  int value = 1;
  guint16 event_types[] = {
    SCTP_ASSOC_CHANGE,
    SCTP_PEER_ADDR_CHANGE,
    SCTP_REMOTE_ERROR,
    SCTP_SEND_FAILED,
    SCTP_SEND_FAILED_EVENT,
    SCTP_SHUTDOWN_EVENT,
    SCTP_ADAPTATION_INDICATION,
    SCTP_PARTIAL_DELIVERY_EVENT,
    /*SCTP_AUTHENTICATION_EVENT, */
    SCTP_STREAM_RESET_EVENT,
    /*SCTP_SENDER_DRY_EVENT, */
    /*SCTP_NOTIFICATIONS_STOPPED_EVENT, */
    /*SCTP_ASSOC_RESET_EVENT, */
    SCTP_STREAM_CHANGE_EVENT
  };
  guint32 i;
  guint sock_type = self->use_sock_stream ? SOCK_STREAM : SOCK_SEQPACKET;

  if ((sock =
          usrsctp_socket (AF_CONN, sock_type, IPPROTO_SCTP, receive_cb, NULL, 0,
              (void *) self)) == NULL) {
    GST_ERROR_OBJECT (self, "Could not open SCTP socket: (%u) %s", errno,
        g_strerror (errno));
    goto error;
  }

  if (usrsctp_setsockopt (sock, SOL_SOCKET, SO_RCVBUF,
          (const void *) &buf_size, sizeof (buf_size)) < 0) {
    GST_ERROR_OBJECT (self, "Could not change receive buffer size: (%u) %s",
        errno, g_strerror (errno));
    goto error;
  }
  if (usrsctp_setsockopt (sock, SOL_SOCKET, SO_SNDBUF,
          (const void *) &buf_size, sizeof (buf_size)) < 0) {
    GST_ERROR_OBJECT (self, "Could not change send buffer size: (%u) %s",
        errno, g_strerror (errno));
    goto error;
  }

  /* Properly return errors */
  if (usrsctp_set_non_blocking (sock, 1) < 0) {
    GST_ERROR_OBJECT (self,
        "Could not set non-blocking mode on SCTP socket: (%u) %s", errno,
        g_strerror (errno));
    goto error;
  }

  memset (&l, 0, sizeof (l));
  l.l_onoff = 1;
  l.l_linger = 0;
  if (usrsctp_setsockopt (sock, SOL_SOCKET, SO_LINGER, (const void *) &l,
          (socklen_t) sizeof (struct linger)) < 0) {
    GST_ERROR_OBJECT (self, "Could not set SO_LINGER on SCTP socket: (%u) %s",
        errno, g_strerror (errno));
    goto error;
  }

  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_REUSE_PORT, &value,
          sizeof (int))) {
    GST_DEBUG_OBJECT (self, "Could not set SCTP_REUSE_PORT: (%u) %s", errno,
        g_strerror (errno));
  }

  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_NODELAY, &value,
          sizeof (int))) {
    GST_DEBUG_OBJECT (self, "Could not set SCTP_NODELAY: (%u) %s", errno,
        g_strerror (errno));
    goto error;
  }

  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &value,
          sizeof (int))) {
    GST_ERROR_OBJECT (self, "Could not set SCTP_EXPLICIT_EOR: (%u) %s", errno,
        g_strerror (errno));
    goto error;
  }

  memset (&stream_reset, 0, sizeof (stream_reset));
  stream_reset.assoc_id = SCTP_ALL_ASSOC;
  stream_reset.assoc_value =
      SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
  if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET,
          &stream_reset, sizeof (stream_reset))) {
    GST_ERROR_OBJECT (self,
        "Could not set SCTP_ENABLE_STREAM_RESET | SCTP_ENABLE_CHANGE_ASSOC_REQ: (%u) %s",
        errno, g_strerror (errno));
    goto error;
  }

  memset (&event, 0, sizeof (event));
  event.se_assoc_id = SCTP_ALL_ASSOC;
  event.se_on = 1;
  for (i = 0; i < sizeof (event_types) / sizeof (event_types[0]); i++) {
    event.se_type = event_types[i];
    if (usrsctp_setsockopt (sock, IPPROTO_SCTP, SCTP_EVENT,
            &event, sizeof (event)) < 0) {
      GST_ERROR_OBJECT (self, "Failed to register event %u: (%u) %s",
          event_types[i], errno, g_strerror (errno));
    }
  }

  gst_sctp_association_register (self);

  return sock;
error:
  if (sock)
    usrsctp_close (sock);
  return NULL;
}

static struct sockaddr_conn
get_sctp_socket_address (GstSctpAssociation * gst_sctp_association,
    guint16 port)
{
  struct sockaddr_conn addr;

  memset ((void *) &addr, 0, sizeof (struct sockaddr_conn));
#ifdef __APPLE__
  addr.sconn_len = sizeof (struct sockaddr_conn);
#endif
  addr.sconn_family = AF_CONN;
  addr.sconn_port = g_htons (port);
  addr.sconn_addr = (void *) gst_sctp_association;

  return addr;
}

static gboolean
client_role_connect (GstSctpAssociation * self)
{
  struct sockaddr_conn local_addr, remote_addr;
  struct sctp_paddrparams paddrparams;
  socklen_t opt_len;
  gint ret;

  local_addr = get_sctp_socket_address (self, self->local_port);
  remote_addr = get_sctp_socket_address (self, self->remote_port);

  /* After an SCTP association is reported as disconnected, there is
   * a window of time before the underlying SCTP stack cleans up.
   * If a client-initiated reconnect request occurs during this window
   * then we will attempt to bind using the same address information
   * which will fail with EADDRINUSE. Handle this by retrying whenever
   * a bind fails in this way.
   */
  size_t retry_count = 1;
  do {
    ret =
        usrsctp_bind (self->sctp_ass_sock, (struct sockaddr *) &local_addr,
        sizeof (struct sockaddr_conn));
    if (ret < 0) {
      if (errno != EADDRINUSE || retry_count == 10) {
        GST_ERROR_OBJECT (self, "usrsctp_bind() error: (%u) %s", errno,
            g_strerror (errno));
        goto error;
      } else {
        GST_WARNING_OBJECT (self, "usrsctp_bind() error: (%u) %s", errno,
            g_strerror (errno));
      }
      g_usleep (G_USEC_PER_SEC / 100);
      retry_count++;
    }
  } while (ret < 0);

  ret =
      usrsctp_connect (self->sctp_ass_sock, (struct sockaddr *) &remote_addr,
      sizeof (struct sockaddr_conn));
  if (ret < 0 && errno != EINPROGRESS) {
    GST_ERROR_OBJECT (self, "usrsctp_connect() error: (%u) %s", errno,
        g_strerror (errno));
    goto error;
  }

  memset (&paddrparams, 0, sizeof (struct sctp_paddrparams));
  memcpy (&paddrparams.spp_address, &remote_addr,
      sizeof (struct sockaddr_conn));
  opt_len = (socklen_t) sizeof (struct sctp_paddrparams);
  ret =
      usrsctp_getsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
      SCTP_PEER_ADDR_PARAMS, &paddrparams, &opt_len);
  if (ret < 0) {
    GST_WARNING_OBJECT (self,
        "usrsctp_getsockopt(SCTP_PEER_ADDR_PARAMS) error: (%u) %s", errno,
        g_strerror (errno));
  } else {
    /* draft-ietf-rtcweb-data-channel-13 section 5: max initial MTU IPV4 1200, IPV6 1280 */
    paddrparams.spp_pathmtu = 1200;
    paddrparams.spp_flags &= ~SPP_PMTUD_ENABLE;
    paddrparams.spp_flags |= SPP_PMTUD_DISABLE;
    opt_len = (socklen_t) sizeof (struct sctp_paddrparams);
    ret = usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
        SCTP_PEER_ADDR_PARAMS, &paddrparams, opt_len);
    if (ret < 0) {
      GST_WARNING_OBJECT (self,
          "usrsctp_setsockopt(SCTP_PEER_ADDR_PARAMS) error: (%u) %s", errno,
          g_strerror (errno));
    } else {
      GST_DEBUG_OBJECT (self, "PMTUD disabled, MTU set to %u",
          paddrparams.spp_pathmtu);
    }
  }

  return TRUE;
error:
  return FALSE;
}

static gboolean
association_is_valid (GstSctpAssociation * self)
{
  gboolean valid = FALSE;

  G_LOCK (associations_lock);

  if (ids_by_association != NULL)
    valid = g_hash_table_contains (ids_by_association, self);

  G_UNLOCK (associations_lock);

  return valid;
}

static int
sctp_packet_out (void *addr, void *buffer, size_t length, guint8 tos,
    guint8 set_df)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (addr);

  if (association_is_valid (self) && self->encoder_ctx.packet_out_cb) {
    self->encoder_ctx.packet_out_cb (self, buffer, length,
        self->encoder_ctx.element);
  }

  return 0;
}

static int
receive_cb (struct socket *sock, union sctp_sockstore addr, void *data,
    size_t datalen, struct sctp_rcvinfo rcv_info, gint flags, void *ulp_info)
{
  GstSctpAssociation *self = GST_SCTP_ASSOCIATION (ulp_info);

  if (!association_is_valid (self)) {
    return 1;
  }


  /* If we acquire the lock here it means this is the SCTP timer thread, if
     the thread has already acquired the lock, its coming from
     gst_sctp_association_incoming_packet()  */
  gboolean acquired_lock = g_mutex_trylock (&self->association_mutex);

  if (!data) {
    /* This is a notification that socket shutdown is complete */
    GST_INFO_OBJECT (self, "Received shutdown complete notification");
    self->shutdown = TRUE;
  } else {
    if (flags & MSG_NOTIFICATION) {
      handle_notification (self, (const union sctp_notification *) data,
          datalen);

      /* We use this instead of a bare `free()` so that we use the `free` from
       * the C runtime that usrsctp was built with. This makes a difference on
       * Windows where libusrstcp and GStreamer can be linked to two different
       * CRTs. */
      usrsctp_freedumpbuffer (data);
    } else {
      handle_message (self, data, datalen, rcv_info.rcv_sid,
          ntohl (rcv_info.rcv_ppid));
    }
  }

  if (acquired_lock)
    g_mutex_unlock (&self->association_mutex);

  return 1;
}

static void
handle_notification (GstSctpAssociation * self,
    const union sctp_notification *notification, size_t length)
{
  g_assert (notification->sn_header.sn_length == length);

  switch (notification->sn_header.sn_type) {
    case SCTP_ASSOC_CHANGE:
      GST_DEBUG_OBJECT (self, "Event: SCTP_ASSOC_CHANGE");
      handle_association_changed (self, &notification->sn_assoc_change);
      break;
    case SCTP_PEER_ADDR_CHANGE:
      GST_DEBUG_OBJECT (self, "Event: SCTP_PEER_ADDR_CHANGE");
      break;
    case SCTP_REMOTE_ERROR:
      GST_ERROR_OBJECT (self, "Event: SCTP_REMOTE_ERROR (%u)",
          notification->sn_remote_error.sre_error);
      break;
    case SCTP_SEND_FAILED:
      GST_ERROR_OBJECT (self, "Event: SCTP_SEND_FAILED");
      break;
    case SCTP_SHUTDOWN_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_SHUTDOWN_EVENT");
      gst_sctp_association_change_state_unlocked (self,
          GST_SCTP_ASSOCIATION_STATE_DISCONNECTING);
      break;
    case SCTP_ADAPTATION_INDICATION:
      GST_DEBUG_OBJECT (self, "Event: SCTP_ADAPTATION_INDICATION");
      break;
    case SCTP_PARTIAL_DELIVERY_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_PARTIAL_DELIVERY_EVENT");
      break;
    case SCTP_AUTHENTICATION_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_AUTHENTICATION_EVENT");
      break;
    case SCTP_STREAM_RESET_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_STREAM_RESET_EVENT");
      handle_stream_reset_event (self, &notification->sn_strreset_event);
      break;
    case SCTP_SENDER_DRY_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_SENDER_DRY_EVENT");
      break;
    case SCTP_NOTIFICATIONS_STOPPED_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_NOTIFICATIONS_STOPPED_EVENT");
      break;
    case SCTP_ASSOC_RESET_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_ASSOC_RESET_EVENT");
      break;
    case SCTP_STREAM_CHANGE_EVENT:
      GST_DEBUG_OBJECT (self, "Event: SCTP_STREAM_CHANGE_EVENT");
      break;
    case SCTP_SEND_FAILED_EVENT:
      GST_ERROR_OBJECT (self, "Event: SCTP_SEND_FAILED_EVENT (%u)",
          notification->sn_send_failed_event.ssfe_error);
      break;
    default:
      break;
  }
}

static void
_apply_aggressive_heartbeat_unlocked (GstSctpAssociation * self)
{
  struct sctp_assocparams assoc_params;
  struct sctp_paddrparams peer_addr_params;
  struct sockaddr_conn addr;

  if (!self->aggressive_heartbeat)
    return;

  memset (&assoc_params, 0, sizeof (assoc_params));
  assoc_params.sasoc_assoc_id = self->sctp_assoc_id;
  assoc_params.sasoc_asocmaxrxt = 1;
  if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
          SCTP_ASSOCINFO, &assoc_params, sizeof (assoc_params))) {
    GST_WARNING_OBJECT (self, "Could not set SCTP_ASSOCINFO");
  }

  addr = get_sctp_socket_address (self, self->remote_port);
  memset (&peer_addr_params, 0, sizeof (peer_addr_params));
  memcpy (&peer_addr_params.spp_address, &addr, sizeof (addr));
  peer_addr_params.spp_flags = SPP_HB_ENABLE;
  peer_addr_params.spp_hbinterval = 10;
  if (usrsctp_setsockopt (self->sctp_ass_sock, IPPROTO_SCTP,
          SCTP_PEER_ADDR_PARAMS, &peer_addr_params,
          sizeof (peer_addr_params))) {
    GST_WARNING_OBJECT (self, "Could not set SCTP_PEER_ADDR_PARAMS");
  }
}

static void
handle_sctp_comm_up (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac)
{
  GST_INFO_OBJECT (self, "SCTP_COMM_UP");
  if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTING) {
    self->sctp_assoc_id = sac->sac_assoc_id;
    _apply_aggressive_heartbeat_unlocked (self);
    gst_sctp_association_change_state_unlocked (self,
        GST_SCTP_ASSOCIATION_STATE_CONNECTED);
    GST_INFO_OBJECT (self, "SCTP association connected!");
  } else if (self->state == GST_SCTP_ASSOCIATION_STATE_CONNECTED) {
    GST_INFO_OBJECT (self, "SCTP association already open");
  } else {
    GST_WARNING_OBJECT (self, "SCTP association in unexpected state: %d",
        self->state);
  }
}

static void
handle_sctp_comm_lost_or_shutdown (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac)
{
  GST_INFO_OBJECT (self, "SCTP event %s received",
      sac->sac_state == SCTP_COMM_LOST ?
      "SCTP_COMM_LOST" : "SCTP_SHUTDOWN_COMP");

  gst_sctp_association_disconnect_unlocked (self, FALSE);
}

static void
gst_sctp_association_notify_restart (GstSctpAssociation * self)
{
  GstSctpAssociationRestartCb restart_cb;
  gpointer user_data;

  restart_cb = self->decoder_ctx.restart_cb;
  user_data = self->decoder_ctx.element;
  if (user_data)
    gst_object_ref (user_data);

  if (restart_cb)
    restart_cb (self, user_data);

  if (user_data)
    gst_object_unref (user_data);
}

static void
gst_sctp_association_notify_stream_reset (GstSctpAssociation * self,
    guint16 stream_id)
{
  GstSctpAssociationStreamResetCb stream_reset_cb;
  gpointer user_data;

  stream_reset_cb = self->decoder_ctx.stream_reset_cb;
  user_data = self->decoder_ctx.element;
  if (user_data)
    gst_object_ref (user_data);

  if (stream_reset_cb)
    stream_reset_cb (self, stream_id, user_data);

  if (user_data)
    gst_object_unref (user_data);
}

static void
handle_association_changed (GstSctpAssociation * self,
    const struct sctp_assoc_change *sac)
{
  switch (sac->sac_state) {
    case SCTP_COMM_UP:
      handle_sctp_comm_up (self, sac);
      break;
    case SCTP_COMM_LOST:
      handle_sctp_comm_lost_or_shutdown (self, sac);
      break;
    case SCTP_RESTART:
      GST_INFO_OBJECT (self, "SCTP event SCTP_RESTART received");
      gst_sctp_association_notify_restart (self);
      break;
    case SCTP_SHUTDOWN_COMP:
      /* Occurs if in TCP mode when the far end sends SHUTDOWN */
      handle_sctp_comm_lost_or_shutdown (self, sac);
      break;
    case SCTP_CANT_STR_ASSOC:
      GST_WARNING_OBJECT (self, "SCTP event SCTP_CANT_STR_ASSOC received");
      gst_sctp_association_change_state_unlocked (self,
          GST_SCTP_ASSOCIATION_STATE_ERROR);
      break;
  }
}

static void
handle_stream_reset_event (GstSctpAssociation * self,
    const struct sctp_stream_reset_event *sr)
{
  guint32 i, n;
  if (!(sr->strreset_flags & SCTP_STREAM_RESET_DENIED) &&
      !(sr->strreset_flags & SCTP_STREAM_RESET_DENIED)) {
    n = (sr->strreset_length -
        sizeof (struct sctp_stream_reset_event)) / sizeof (uint16_t);
    for (i = 0; i < n; i++) {
      if (sr->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
        gst_sctp_association_notify_stream_reset (self,
            sr->strreset_stream_list[i]);
      }
    }
  }
}

static void
handle_message (GstSctpAssociation * self, guint8 * data, guint32 datalen,
    guint16 stream_id, guint32 ppid)
{
  if (self->decoder_ctx.packet_received_cb) {
    /* It's the callbacks job to free the data correctly */
    self->decoder_ctx.packet_received_cb (self, data, datalen, stream_id, ppid,
        self->decoder_ctx.element);
  } else {
    /* We use this instead of a bare `free()` so that we use the `free` from
     * the C runtime that usrsctp was built with. This makes a difference on
     * Windows where libusrstcp and GStreamer can be linked to two different
     * CRTs. */
    usrsctp_freedumpbuffer ((gchar *) data);
  }
}

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
