/* GStreamer
 * Copyright (C) <2020> Havard Graff <havard@pexip.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_TX_FEEDBACK_H__
#define __GST_TX_FEEDBACK_H__

#include <gst/gst.h>
#include <gst/net/net-prelude.h>

G_BEGIN_DECLS

typedef struct _GstTxFeedback GstTxFeedback;
typedef struct _GstTxFeedbackInterface GstTxFeedbackInterface;
typedef struct _GstTxFeedbackMeta GstTxFeedbackMeta;

#define GST_TYPE_TX_FEEDBACK \
  (gst_tx_feedback_get_type ())
#define GST_TX_FEEDBACK(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_TX_FEEDBACK, GstTxFeedback))
#define GST_IS_TX_FEEDBACK(obj) \
      (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_TX_FEEDBACK))
#define GST_TX_FEEDBACK_GET_INTERFACE(obj) \
    (G_TYPE_INSTANCE_GET_INTERFACE ((obj), GST_TYPE_TX_FEEDBACK, GstTxFeedbackInterface))

/**
 * GstTxFeedbackInterface:
 * @iface: the parent interface
 * @tx_feedback: a transmit feedback
 *
 * TxFeedback interface.
 */
struct _GstTxFeedbackInterface {
  GTypeInterface iface;

  /* virtual functions */
  void (*tx_feedback) (GstTxFeedback * feedback,
      guint64 buffer_id, GstClockTime ts);
};

GST_NET_API
GType gst_tx_feedback_get_type (void);


/**
 * GstTxFeedbackMeta:
 * @meta: the parent type
 * @buffer_id: A #guint64 with an identifier for the current buffer
 *
 * Buffer metadata for transmission-time feedback.
 */
struct _GstTxFeedbackMeta {
  GstMeta meta;

  guint64 buffer_id;
  GstTxFeedback *feedback;
};

GST_NET_API
GstTxFeedbackMeta * gst_buffer_add_tx_feedback_meta (GstBuffer      *buffer,
    guint64 buffer_id, GstTxFeedback * feedback);
GST_NET_API
GstTxFeedbackMeta * gst_buffer_get_tx_feedback_meta (GstBuffer      *buffer);

GST_NET_API
void gst_tx_feedback_meta_set_tx_time (GstTxFeedbackMeta * meta,
    GstClockTime ts);

GST_NET_API
GType gst_tx_feedback_meta_api_get_type (void);
#define GST_TX_FEEDBACK_META_API_TYPE (gst_tx_feedback_meta_api_get_type())

GST_NET_API
const GstMetaInfo *gst_tx_feedback_meta_get_info (void);
#define GST_TX_FEEDBACK_META_INFO (gst_tx_feedback_meta_get_info())

G_END_DECLS

#endif /* __GST_TX_FEEDBACK_H__ */

