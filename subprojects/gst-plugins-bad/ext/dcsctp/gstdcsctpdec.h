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

#ifndef __GST_DCSCTP_DEC_H__
#define __GST_DCSCTP_DEC_H__

#include <gst/gst.h>
#include <gst/base/base.h>

#include "dcsctpassociation.h"

G_BEGIN_DECLS

#define GST_TYPE_DCSCTP_DEC (gst_dcsctp_dec_get_type())
#define GST_DCSCTP_DEC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DCSCTP_DEC, GstDCSCTPDec))
#define GST_DCSCTP_DEC_CAST(obj) (GstDCSCTPDec*)(obj)
#define GST_DCSCTP_DEC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DCSCTP_DEC, GstDCSCTPDecClass))
#define GST_IS_DCSCTP_DEC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DCSCTP_DEC))
#define GST_IS_DCSCTP_DEC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DCSCTP_DEC))
typedef struct _GstDCSCTPDec GstDCSCTPDec;
typedef struct _GstDCSCTPDecClass GstDCSCTPDecClass;

struct _GstDCSCTPDec
{
  GstElement element;

  GMutex association_mutex;

  GstFlowCombiner *flow_combiner;

  GstPad *sink_pad;
  guint association_id;
  guint local_sctp_port;

  DCSCTPAssociation *sctp_association;
};

struct _GstDCSCTPDecClass
{
  GstElementClass parent_class;

  void (*on_reset_stream) (GstDCSCTPDec * DCSCTP_dec, guint16 stream_id);
  void (*on_association_restart) (GstDCSCTPDec * DCSCTP_dec);
};

GType gst_dcsctp_dec_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (dcsctpdec);

G_END_DECLS

#endif /* __GST_DCSCTP_DEC_H__ */