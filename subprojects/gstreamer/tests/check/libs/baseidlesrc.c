/* GStreamer
 *
 * some unit tests for GstBaseIdleSrc
 *
 * Copyright (C) 2023 Havard Graff <hgr@pexip.com>
 *               2023 Camilo Celis Guzman <camilo@pexip.com>
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
#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/check/gstconsistencychecker.h>
#include <gst/base/gstbaseidlesrc.h>

typedef GstBaseIdleSrc TestIdleSrc;
typedef GstBaseIdleSrcClass TestIdleSrcClass;

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GType test_idle_src_get_type (void);

G_DEFINE_TYPE (TestIdleSrc, test_idle_src, GST_TYPE_BASE_IDLE_SRC);

static void
test_idle_src_init (TestIdleSrc * src)
{
}

static void
test_idle_src_class_init (TestIdleSrcClass * klass)
{
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &src_template);
}

GST_START_TEST (baseidlesrc_up_and_down)
{
  GstElement *src;
  GstHarness *h;

  src = g_object_new (test_idle_src_get_type (), NULL);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");

  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (baseidlesrc_submit_buffer)
{
  GstElement *src;
  GstHarness *h;
  GstBaseIdleSrc *base_src;
  GstBuffer *buf;
  guint i;

  src = g_object_new (test_idle_src_get_type (), NULL);
  base_src = GST_BASE_IDLE_SRC (src);

  h = gst_harness_new_with_element (GST_ELEMENT (src), NULL, "src");

  gst_harness_set_sink_caps_str (h, "foo/bar");
  gst_harness_play (h);

  for (i = 0; i < 5; i++) {
    fail_unless_equals_int (GST_FLOW_OK,
        gst_base_idle_src_alloc_buffer (base_src, 100, &buf));
    gst_base_idle_src_submit_buffer (base_src, buf);
    gst_buffer_unref (gst_harness_pull (h));
  }

  gst_harness_teardown (h);
}

GST_END_TEST;

static Suite *
baseidlesrc_suite (void)
{
  Suite *s = suite_create ("GstBaseIdleSrc");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (s, tc);
  tcase_add_test (tc, baseidlesrc_up_and_down);
  tcase_add_test (tc, baseidlesrc_submit_buffer);

  return s;
}

GST_CHECK_MAIN (baseidlesrc);
