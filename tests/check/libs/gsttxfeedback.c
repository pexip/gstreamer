/* GStreamer
 * Copyright (C) <2020> Havard Graff <havard@pexip.com>
 *
 * gsttxfeedback.c: Unit test for the TX Feedback
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

#include <gst/check/gstcheck.h>
#include <gst/net/gstnet.h>

typedef struct
{
  GObject object;
  guint64 buffer_id;
  GstClockTime ts;
} GDummyObject;

typedef GObjectClass GDummyObjectClass;

static void
g_dummy_object_tx_feedback (GstTxFeedback * parent, guint64 buffer_id,
    GstClockTime ts)
{
  GDummyObject *obj = (GDummyObject *) parent;
  obj->buffer_id = buffer_id;
  obj->ts = ts;
}

static void
g_dummy_object_tx_feedback_init (gpointer g_iface,
    G_GNUC_UNUSED gpointer iface_data)
{
  GstTxFeedbackInterface *iface = g_iface;
  iface->tx_feedback = g_dummy_object_tx_feedback;
}

GType g_dummy_object_get_type (void);
G_DEFINE_TYPE_WITH_CODE (GDummyObject, g_dummy_object,
    G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (GST_TYPE_TX_FEEDBACK,
        g_dummy_object_tx_feedback_init));

#define G_TYPE_DUMMY_OBJECT g_dummy_object_get_type()

static void
g_dummy_object_class_init (G_GNUC_UNUSED GDummyObjectClass * klass)
{
}

static void
g_dummy_object_init (G_GNUC_UNUSED GDummyObject * enc)
{
}

GST_START_TEST (test_basic)
{
  GstTxFeedbackMeta *meta;
  GDummyObject *obj;
  GstBuffer *buf;
  guint64 buffer_id = 42;
  GstClockTime ts = 123456789;

  obj = g_object_new (G_TYPE_DUMMY_OBJECT, NULL);
  fail_unless (obj);

  buf = gst_buffer_new ();

  /* add TxFeedback buffer meta, with a unique ID and the receving object */
  gst_buffer_add_tx_feedback_meta (buf, buffer_id, GST_TX_FEEDBACK (obj));

  /* verify the meta is on the buffer */
  meta = gst_buffer_get_tx_feedback_meta (buf);
  fail_unless (meta);

  /* now set the transmit time of this buffer */
  gst_tx_feedback_meta_set_tx_time (meta, ts);

  /* and verify that the object was notified of this buffer being sent */
  fail_unless_equals_int (obj->buffer_id, buffer_id);
  fail_unless_equals_int (obj->ts, ts);

  g_object_unref (obj);
}

GST_END_TEST;

static Suite *
gst_tx_feedback_suite (void)
{
  Suite *s = suite_create ("GstTxFeedback");
  TCase *tc_chain = tcase_create ("generic tests");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_basic);

  return s;
}

GST_CHECK_MAIN (gst_tx_feedback);
