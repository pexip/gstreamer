/* GStreamer
 *
 * unit test for rtphdrext-roi elements
 *
 * Copyright (C) 2020-2021 Pexip AS.
 *   @author: Camilo Celis Guzman <camilo@pexip.com>
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

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/rtp/gstrtphdrext.h>
#include <gst/video/gstvideometa.h>

#define URI GST_RTP_HDREXT_BASE "TBD:draft-ford-avtcore-roi-extension-00"
#define EXTMAP_ID 10

#define SRC_CAPS_STR "video/x-raw,format=I420"

GST_START_TEST (test_rtphdrext_roi_basic)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = gst_caps_from_string (SRC_CAPS_STR);
  gst_harness_set_src_caps (h, src_caps);

  gst_harness_add_src_parse (h, "videotestsrc ! "
      "capsfilter caps=\"" SRC_CAPS_STR "\"", FALSE);

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  /* verify that we can push and pull buffers */
  gst_harness_play (h);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push_from_src (h));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* verify there are NO RoI meta on the buffer pulled from the depayloader */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_harness_teardown (h);
}

GST_END_TEST;

static GstCaps *
_i420_caps (gint width, gint height)
{
  GstVideoInfo info;
  gst_video_info_init (&info);
  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_I420, width, height);
  GST_VIDEO_INFO_FPS_N (&info) = 1;
  GST_VIDEO_INFO_FPS_D (&info) = 1;
  GST_VIDEO_INFO_PAR_N (&info) = 1;
  GST_VIDEO_INFO_PAR_D (&info) = 1;
  return gst_video_info_to_caps (&info);
}

static GstBuffer *
_gst_harness_create_raw_buffer (GstHarness * h, GstCaps * caps)
{
  GstBuffer *buf;
  GstVideoInfo video_info;

  gst_video_info_init (&video_info);
  gst_video_info_from_caps (&video_info, caps);

  buf = gst_harness_create_buffer (h, GST_VIDEO_INFO_SIZE (&video_info));
  gst_buffer_memset (buf, 0, 0, GST_VIDEO_INFO_SIZE (&video_info));
  fail_unless (buf);

  gst_buffer_add_video_meta_full (buf,
      GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&video_info),
      GST_VIDEO_INFO_WIDTH (&video_info),
      GST_VIDEO_INFO_HEIGHT (&video_info),
      GST_VIDEO_INFO_N_PLANES (&video_info),
      video_info.offset, video_info.stride);

  GST_BUFFER_PTS (buf) = 0;
  GST_BUFFER_DURATION (buf) = GST_SECOND / 30;

  return buf;
}

GST_START_TEST (test_rtphdrext_roi_default_roi_type)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  GstMeta *meta;
  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  const GQuark default_roi_type = 0;
  GQuark pay_roi_type = ~default_roi_type;
  GQuark depay_roi_type = ~default_roi_type;

  gpointer state = NULL;
  gint num_roi_metas_found = 0;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = _i420_caps (640, 360);
  gst_harness_set_src_caps (h, gst_caps_ref (src_caps));

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  g_object_get (pay_ext, "roi-type", &pay_roi_type, NULL);
  g_object_get (depay_ext, "roi-type", &depay_roi_type, NULL);
  fail_unless_equals_int (pay_roi_type, default_roi_type);
  fail_unless_equals_int (depay_roi_type, default_roi_type);

  /* verify that we can push and pull buffers */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* verify there are NO RoI meta on the buffer pulled from the depayloader */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  /* add a RoI meta on an input buffer with the default RoI type
   * and expect this to be payloaded and depayloaded back as a RoI meta on
   * the output buffer */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  gst_buffer_add_video_region_of_interest_meta_id (buf,
      default_roi_type, 0, 0, 640, 360);

  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify that we get exactly one RoI meta on the output buffer with
   * the expected RoI coordinates and type */
  while ((meta = gst_buffer_iterate_meta (buf, &state))) {
    if (meta->info->api == meta_info->api) {
      GstVideoRegionOfInterestMeta *vmeta =
          (GstVideoRegionOfInterestMeta *) meta;
      fail_unless_equals_int ((gint) default_roi_type, (gint) vmeta->roi_type);
      fail_unless_equals_int (0, vmeta->x);
      fail_unless_equals_int (0, vmeta->y);
      fail_unless_equals_int (640, vmeta->w);
      fail_unless_equals_int (360, vmeta->h);
      num_roi_metas_found++;
    }
  }
  fail_unless_equals_int (1, num_roi_metas_found);
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_caps_unref (src_caps);
  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (test_rtphdrext_roi_unknown_roi_type)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  GQuark other_roi_type = g_quark_from_string ("SomeRoITypeStr");
  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = _i420_caps (640, 360);
  gst_harness_set_src_caps (h, gst_caps_ref (src_caps));

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  /* verify that we can push and pull buffers */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* verify there are NO RoI meta on the buffer pulled from the depayloader */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  /* add a RoI meta on an input buffer with an unknown RoI type and
   * don't expect this to be payloaded and depayloaded as a RoI meta on
   * the output buffer as we strictly only payload known RoI types */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  gst_buffer_add_video_region_of_interest_meta_id (buf,
      other_roi_type, 0, 0, 640, 360);

  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_caps_unref (src_caps);
  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (test_rtphdrext_roi_explicit_roi_type)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  GstMeta *meta;
  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  const GQuark other_roi_type = 0xFFFFFFFF;

  gpointer state = NULL;
  gint num_roi_metas_found = 0;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = _i420_caps (640, 360);
  gst_harness_set_src_caps (h, gst_caps_ref (src_caps));

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  /* verify that we can push and pull buffers */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  gst_buffer_unref (gst_harness_pull (h));

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* explicitly set a roi-type on both payloader and depayloader extensions
   * and expect a buffer with such roi-type added to their RoI meta to be
   * correctly payloaded and depayloaded */
  g_object_set (pay_ext, "roi-type", other_roi_type, NULL);
  g_object_set (depay_ext, "roi-type", other_roi_type, NULL);

  buf = _gst_harness_create_raw_buffer (h, src_caps);
  gst_buffer_add_video_region_of_interest_meta_id (buf,
      other_roi_type, 0, 0, 640, 360);

  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify that we get exactly one RoI meta on the output buffer with
   * the expected RoI coordinates and type */
  while ((meta = gst_buffer_iterate_meta (buf, &state))) {
    if (meta->info->api == meta_info->api) {
      GstVideoRegionOfInterestMeta *vmeta =
          (GstVideoRegionOfInterestMeta *) meta;
      fail_unless_equals_int ((gint) other_roi_type, (gint) vmeta->roi_type);
      fail_unless_equals_int (0, vmeta->x);
      fail_unless_equals_int (0, vmeta->y);
      fail_unless_equals_int (640, vmeta->w);
      fail_unless_equals_int (360, vmeta->h);
      num_roi_metas_found++;
    }
  }
  fail_unless_equals_int (1, num_roi_metas_found);
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_caps_unref (src_caps);
  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (test_rtphdrext_roi_explicit_roi_type_pay_only)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  GstMeta *meta;
  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  const GQuark default_roi_type = 0;
  const GQuark other_roi_type = ~default_roi_type;

  gpointer state = NULL;
  gint num_roi_metas_found = 0;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = _i420_caps (640, 360);
  gst_harness_set_src_caps (h, gst_caps_ref (src_caps));

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  /* verify that we can push and pull buffers */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* verify there are NO RoI meta on the buffer pulled from the depayloader */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  /* set a roi-type on the payloader only and expect this to be
   * partially correctly depayloaded as the depayloader will add a meta for
   * ANY HDREXT-ROI that has been payloaded by a GStreamer payloader with
   * the added rtphdrext-roi element. However, the roi-type on the RoI meta
   * would be the default one for the depayloader */
  g_object_set (pay_ext, "roi-type", other_roi_type, NULL);

  buf = _gst_harness_create_raw_buffer (h, src_caps);
  gst_buffer_add_video_region_of_interest_meta_id (buf,
      other_roi_type, 0, 0, 640, 360);

  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify that we get exactly one RoI meta on the output buffer with
   * the expected RoI coordinates and type */
  while ((meta = gst_buffer_iterate_meta (buf, &state))) {
    if (meta->info->api == meta_info->api) {
      GstVideoRegionOfInterestMeta *vmeta =
          (GstVideoRegionOfInterestMeta *) meta;
      fail_unless_equals_int ((gint) default_roi_type, (gint) vmeta->roi_type);
      fail_unless_equals_int (0, vmeta->x);
      fail_unless_equals_int (0, vmeta->y);
      fail_unless_equals_int (640, vmeta->w);
      fail_unless_equals_int (360, vmeta->h);
      num_roi_metas_found++;
    }
  }
  fail_unless_equals_int (1, num_roi_metas_found);
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_caps_unref (src_caps);
  gst_harness_teardown (h);
}

GST_END_TEST;

GST_START_TEST (test_rtphdrext_roi_explicit_roi_type_depay_only)
{
  GstHarness *h;
  GstCaps *src_caps, *pay_pad_caps, *expected_caps;

  GstElement *pay, *depay;
  GstRTPHeaderExtension *pay_ext, *depay_ext;
  GstPad *pay_pad;

  GstBuffer *buf;

  const GstMetaInfo *meta_info = GST_VIDEO_REGION_OF_INTEREST_META_INFO;

  const GQuark default_roi_type = 0;
  const GQuark other_roi_type = ~default_roi_type;

  h = gst_harness_new_parse ("rtpvrawpay ! rtpvrawdepay");

  src_caps = _i420_caps (640, 360);
  gst_harness_set_src_caps (h, gst_caps_ref (src_caps));

  pay = gst_harness_find_element (h, "rtpvrawpay");
  depay = gst_harness_find_element (h, "rtpvrawdepay");

  pay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));
  depay_ext =
      GST_RTP_HEADER_EXTENSION (gst_element_factory_make ("rtphdrextroi",
          NULL));

  gst_rtp_header_extension_set_id (pay_ext, EXTMAP_ID);
  gst_rtp_header_extension_set_id (depay_ext, EXTMAP_ID);

  g_signal_emit_by_name (pay, "add-extension", pay_ext);
  g_signal_emit_by_name (depay, "add-extension", depay_ext);

  /* verify that we can push and pull buffers */
  buf = _gst_harness_create_raw_buffer (h, src_caps);
  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify the presence of RoI URI on the depayloader caps */
  pay_pad = gst_element_get_static_pad (pay, "src");
  pay_pad_caps = gst_pad_get_current_caps (pay_pad);
  expected_caps =
      gst_caps_from_string ("application/x-rtp, extmap-" G_STRINGIFY (EXTMAP_ID)
      "=" URI);
  fail_unless (gst_caps_is_subset (pay_pad_caps, expected_caps));
  gst_object_unref (pay_pad);
  gst_caps_unref (pay_pad_caps);
  gst_caps_unref (expected_caps);

  /* verify there are NO RoI meta on the buffer pulled from the depayloader */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  /* set a roi-type on the depayloader only */
  g_object_set (depay_ext, "roi-type", other_roi_type, NULL);

  buf = _gst_harness_create_raw_buffer (h, src_caps);
  gst_buffer_add_video_region_of_interest_meta_id (buf,
      other_roi_type, 0, 0, 640, 360);

  fail_unless_equals_int (GST_FLOW_OK, gst_harness_push (h, buf));
  buf = gst_harness_pull (h);
  fail_unless (buf);

  /* verify that we get no metas as we would not be able to payload an
   * unknown type hence no RTP header extension is added and the depayloader
   * wont, ofcourse add any RoI metas on the output buffer */
  fail_if (gst_buffer_get_meta (buf, meta_info->api));
  gst_buffer_unref (buf);

  gst_object_unref (pay_ext);
  gst_object_unref (depay_ext);
  gst_object_unref (pay);
  gst_object_unref (depay);
  gst_caps_unref (src_caps);
  gst_harness_teardown (h);
}

GST_END_TEST;


static Suite *
rtphdrext_roi_suite (void)
{
  Suite *s = suite_create ("rtphdrext_roi");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, test_rtphdrext_roi_basic);
  tcase_add_test (tc_chain, test_rtphdrext_roi_default_roi_type);
  tcase_add_test (tc_chain, test_rtphdrext_roi_unknown_roi_type);
  tcase_add_test (tc_chain, test_rtphdrext_roi_explicit_roi_type);
  tcase_add_test (tc_chain, test_rtphdrext_roi_explicit_roi_type_pay_only);
  tcase_add_test (tc_chain, test_rtphdrext_roi_explicit_roi_type_depay_only);

  return s;
}

GST_CHECK_MAIN (rtphdrext_roi)
