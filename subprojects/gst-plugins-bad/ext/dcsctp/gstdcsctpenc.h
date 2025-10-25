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

#ifndef __GST_DCSCTP_ENC_H__
#define __GST_DCSCTP_ENC_H__

#include <gst/gst.h>
#include <gst/base/base.h>
#include "dcsctpassociation.h"

G_BEGIN_DECLS

#define GST_TYPE_DCSCTP_ENC (gst_dcsctp_enc_get_type())
#define GST_DCSCTP_ENC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DCSCTP_ENC, GstDCSCTPEnc))
#define GST_DCSCTP_ENC_CAST(obj) (GstDCSCTPEnc*)(obj)
#define GST_DCSCTP_ENC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DCSCTP_ENC, GstDCSCTPEncClass))
#define GST_IS_dcsctp_ENC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DCSCTP_ENC))
#define GST_IS_dcsctp_ENC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DCSCTP_ENC))
typedef struct _GstDCSCTPEnc GstDCSCTPEnc;
typedef struct _GstDCSCTPEncClass GstDCSCTPEncClass;
typedef struct _GstDCSCTPEncPrivate GstDCSCTPEncPrivate;

struct _GstDCSCTPEnc
{
  GstElement element;

  GMutex association_mutex;

  GstPad *src_pad;
  GstFlowReturn src_ret;
  gboolean need_stream_start_caps, need_segment;
  guint32 association_id;
  guint16 remote_dcsctp_port;
  gboolean use_sock_stream;
  gboolean aggressive_heartbeat;

  DCSCTPAssociation *sctp_association;
  GstDataQueue *outbound_dcsctp_packet_queue;

  GQueue pending_pads;
};

struct _GstDCSCTPEncClass
{
  GstElementClass parent_class;

  void (*on_dcsctp_association_is_established) (GstDCSCTPEnc * DCSCTP_enc,
      gboolean established);
    guint64 (*on_get_stream_bytes_sent) (GstDCSCTPEnc * DCSCTP_enc,
      guint16 stream_id);
  gboolean (*disconnect) (GstDCSCTPEnc * DCSCTP_enc);
  void (*reconnect) (GstDCSCTPEnc * DCSCTP_enc);
  void (*send_abort) (GstDCSCTPEnc * DCSCTP_enc, gchar *message);
};

GType gst_dcsctp_enc_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (dcsctpenc);

G_END_DECLS

#endif /* __GST_DCSCTP_ENC_H__ */
