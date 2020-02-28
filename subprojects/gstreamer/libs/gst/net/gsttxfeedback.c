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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gsttxfeedback.h"

static void
gst_tx_feedback_class_init (GstTxFeedbackInterface * iface)
{
  /* default virtual functions */
  iface->tx_feedback = NULL;
}

GType
gst_tx_feedback_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstTxFeedbackInterface),
      (GBaseInitFunc) gst_tx_feedback_class_init,
      NULL,                     /* base_finalize */
      NULL,                     /* class_init */
      NULL,                     /* class_finalize */
      NULL,                     /* class_data */
      0,                        /* instance size */
      0,                        /* n_preallocs */
      NULL,                     /* instance_init */
      NULL                      /* value_table */
    };

    _type =
        g_type_register_static (G_TYPE_INTERFACE, "GstTxFeedback", &info, 0);
    g_type_interface_add_prerequisite (_type, G_TYPE_OBJECT);
    g_once_init_leave (&type, (gsize) _type);
  }

  return type;
}

static void
gst_tx_feedback_send_timestamp (GstTxFeedback * parent,
    guint64 buffer_id, GstClockTime ts)
{
  GST_TX_FEEDBACK_GET_INTERFACE (parent)->tx_feedback (parent, buffer_id, ts);
}

static gboolean
tx_feedback_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstTxFeedbackMeta *nmeta = (GstTxFeedbackMeta *) meta;

  nmeta->buffer_id = 0;
  nmeta->feedback = NULL;

  return TRUE;
}

static gboolean
tx_feedback_meta_transform (GstBuffer * transbuf, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstTxFeedbackMeta *smeta, *dmeta;
  smeta = (GstTxFeedbackMeta *) meta;

  /* we always copy no matter what transform */
  dmeta = gst_buffer_add_tx_feedback_meta (transbuf,
      smeta->buffer_id, smeta->feedback);
  if (!dmeta)
    return FALSE;

  return TRUE;
}

static void
tx_feedback_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstTxFeedbackMeta *nmeta = (GstTxFeedbackMeta *) meta;

  if (nmeta->feedback)
    g_object_unref (nmeta->feedback);
  nmeta->feedback = NULL;
}

GType
gst_tx_feedback_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] = { "origin", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstTxFeedbackMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

const GstMetaInfo *
gst_tx_feedback_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & meta_info)) {
    const GstMetaInfo *mi = gst_meta_register (GST_TX_FEEDBACK_META_API_TYPE,
        "GstTxFeedbackMeta",
        sizeof (GstTxFeedbackMeta),
        tx_feedback_meta_init,
        tx_feedback_meta_free, tx_feedback_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & meta_info, (GstMetaInfo *) mi);
  }
  return meta_info;
}

/**
 * gst_tx_feedback_meta_set_tx_time:
 * @meta: a #GstTxFeedbackMeta
 * @ts: a @GstClockTime with the transmit time
 *
 * Notifies the interface about the buffer-id being transmitted at time ts
 *
 * Returns: (transfer none): a #GstTxFeedbackMeta connected to @buffer
 */
void
gst_tx_feedback_meta_set_tx_time (GstTxFeedbackMeta * meta, GstClockTime ts)
{
  gst_tx_feedback_send_timestamp (meta->feedback, meta->buffer_id, ts);
}

/**
 * gst_buffer_add_tx_feedback_meta:
 * @buffer: a #GstBuffer
 * @buffer_id: a #guint64 with a unique identifier for this buffer
 * @feedback: a #GstTxFeedback object implementing the #GstTxFeedbackInterface
 *
 * Returns: (transfer none): a #GstTxFeedbackMeta connected to @buffer
 */
GstTxFeedbackMeta *
gst_buffer_add_tx_feedback_meta (GstBuffer * buffer,
    guint64 buffer_id, GstTxFeedback * feedback)
{
  GstTxFeedbackMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (GST_IS_TX_FEEDBACK (feedback), NULL);

  meta = (GstTxFeedbackMeta *) gst_buffer_add_meta (buffer,
      GST_TX_FEEDBACK_META_INFO, NULL);

  meta->buffer_id = buffer_id;
  meta->feedback = g_object_ref (feedback);

  return meta;
}

/**
 * gst_buffer_get_tx_feedback_meta:
 * @buffer: a #GstBuffer
 *
 * Find the #GstTxFeedbackMeta on @buffer.
 *
 * Returns: (transfer none): the #GstTxFeedbackMeta or %NULL when there
 * is no such metadata on @buffer.
 */
GstTxFeedbackMeta *
gst_buffer_get_tx_feedback_meta (GstBuffer * buffer)
{
  return (GstTxFeedbackMeta *)
      gst_buffer_get_meta (buffer, GST_TX_FEEDBACK_META_API_TYPE);
}
