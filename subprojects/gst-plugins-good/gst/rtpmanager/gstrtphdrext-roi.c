/* GStreamer
 * Copyright (C) <2021> Havard Graff        <havard@pexip.com>
 *                      Camilo Celis Guzman <camilo@pexip.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more
 */

/**
 * SECTION:element-rtphdrextroi
 * @title: rtphdrextroi
 * @short_description: Region of interest (ROI) RTP Header Extension
 *
 *
 * Since: 1.20
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrtphdrext-roi.h"
#include <gst/video/gstvideometa.h>

/*
 * Internal roi-ext-hdr ID used as a safety mechanism, to make sure
 * that we not only only care about a specific roi_type (as in
 * GstVideoRegionOfInterestMeta) but also that it has been payloaded
 * and payloaded using the set of callbacks implemented below.
 */
#define ROI_EXTHDR_TYPE 0xFD

#define ROI_HDR_EXT_URI GST_RTP_HDREXT_BASE"TBD:draft-ford-avtcore-roi-extension-00"

enum
{
  PROP_0,
  PROP_ROI_TYPE,
};

struct _GstRTPHeaderExtensionRoi
{
  GstRTPHeaderExtension parent;

  guint roi_type;
};

G_DEFINE_TYPE_WITH_CODE (GstRTPHeaderExtensionRoi,
    gst_rtp_header_extension_roi, GST_TYPE_RTP_HEADER_EXTENSION,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "rtphdrextroi", 0,
        "RTP ROI Header Extensions"));
GST_ELEMENT_REGISTER_DEFINE (rtphdrextroi, "rtphdrextroi",
    GST_RANK_MARGINAL, GST_TYPE_RTP_HEADER_EXTENSION_ROI);

static void
gst_rtp_header_extension_roi_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (object);
  switch (prop_id) {
    case PROP_ROI_TYPE:
      g_value_set_uint (value, self->roi_type);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_header_extension_roi_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (object);
  switch (prop_id) {
    case PROP_ROI_TYPE:
      self->roi_type = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstRTPHeaderExtensionFlags
gst_rtp_header_extension_roi_get_supported_flags (GstRTPHeaderExtension * ext)
{
  return GST_RTP_HEADER_EXTENSION_ONE_BYTE;
}

static gsize
gst_rtp_header_extension_roi_get_max_size (GstRTPHeaderExtension * ext,
    const GstBuffer * input_meta)
{
  return 11;
}


static gssize
gst_rtp_header_extension_roi_write (GstRTPHeaderExtension * ext,
    const GstBuffer * input_meta, GstRTPHeaderExtensionFlags write_flags,
    G_GNUC_UNUSED GstBuffer * output, guint8 * data, gsize size)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);
  GstVideoRegionOfInterestMeta *meta;
  gpointer state = NULL;

  g_return_val_if_fail (size >=
      gst_rtp_header_extension_roi_get_max_size (ext, NULL), -1);
  g_return_val_if_fail (write_flags &
      gst_rtp_header_extension_roi_get_supported_flags (ext), -1);

  while ((meta = (GstVideoRegionOfInterestMeta *)
          gst_buffer_iterate_meta_filtered ((GstBuffer *) input_meta, &state,
              GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE))) {
    GstStructure *extra_param_s;
    guint num_faces = 0;

    /* we only really care to write RoI metas coming from pexfdbin
     * so we filter also by GstVideoRegionOfInterestMeta->roi_type */
    if (self->roi_type != meta->roi_type)
      continue;

    // FIXME: Issue 23027
    // If these asserts got hit the problem either in the scaler of in crop element
    // g_assert (meta->w > 0);
    // g_assert (meta->h > 0);

    /* RoI coordinates */
    GST_WRITE_UINT16_BE (&data[0], meta->x);
    GST_WRITE_UINT16_BE (&data[2], meta->y);
    GST_WRITE_UINT16_BE (&data[4], meta->w);
    GST_WRITE_UINT16_BE (&data[6], meta->h);

    /* RoI type (as in the roi-ext-hdr type) */
    GST_WRITE_UINT8 (&data[8], ROI_EXTHDR_TYPE);

    /* RoI - number of faces */

    extra_param_s =
        gst_video_region_of_interest_meta_get_param (meta, "extra-param");
    if (extra_param_s)
      gst_structure_get_uint (extra_param_s, "num_faces", &num_faces);
    GST_WRITE_UINT16_BE (&data[9], num_faces);

    return 11;
  }

  return 0;
}

static gboolean
gst_rtp_header_extension_roi_read (GstRTPHeaderExtension * ext,
    GstRTPHeaderExtensionFlags read_flags, const guint8 * data, gsize size,
    GstBuffer * buffer)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);
  guint16 roi_type = GST_READ_UINT8 (&data[8]);

  /* safety mechanism to only consider those roi-ext-hdr types
   * which we care about */
  if (roi_type == ROI_EXTHDR_TYPE) {
    guint16 x = GST_READ_UINT16_BE (&data[0]);
    guint16 y = GST_READ_UINT16_BE (&data[2]);
    guint16 w = GST_READ_UINT16_BE (&data[4]);
    guint16 h = GST_READ_UINT16_BE (&data[6]);
    // guint16 roi_type = GST_READ_UINT8 (&bytes[8]);
    guint16 num_faces = GST_READ_UINT16_BE (&data[9]);

    GstVideoRegionOfInterestMeta *meta =
        gst_buffer_add_video_region_of_interest_meta_id (buffer,
        self->roi_type, x, y, w, h);

    GstStructure *extra_param_s = gst_structure_new ("extra-param",
        "num_faces", G_TYPE_UINT, num_faces, NULL);
    // FIXME: Issue 23027
    // If these asserts got hit the problem in header corruption
    // Need to look at re-transmit (?)
    // g_assert (w > 0);
    // g_assert (h > 0);
    gst_video_region_of_interest_meta_add_param (meta, extra_param_s);

  }

  return TRUE;
}

static void
gst_rtp_header_extension_roi_class_init (GstRTPHeaderExtensionRoiClass * klass)
{
  GstRTPHeaderExtensionClass *rtp_hdr_class;
  GstElementClass *gstelement_class;
  GObjectClass *gobject_class;

  rtp_hdr_class = GST_RTP_HEADER_EXTENSION_CLASS (klass);
  gobject_class = (GObjectClass *) klass;
  gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->get_property = gst_rtp_header_extension_roi_get_property;
  gobject_class->set_property = gst_rtp_header_extension_roi_set_property;

  /**
   * rtphdrextroi:roi-type:
   *
   * What roi-type (GQuark) to write the extension-header for.
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class, PROP_ROI_TYPE,
      g_param_spec_uint ("roi-type", "ROI TYPE",
          "What roi-type (GQuark) to write the extension-header for",
          0, G_MAXUINT32, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  rtp_hdr_class->get_supported_flags =
      gst_rtp_header_extension_roi_get_supported_flags;
  rtp_hdr_class->get_max_size = gst_rtp_header_extension_roi_get_max_size;
  rtp_hdr_class->write = gst_rtp_header_extension_roi_write;
  rtp_hdr_class->read = gst_rtp_header_extension_roi_read;
  rtp_hdr_class->set_attributes_from_caps =
      gst_rtp_header_extension_set_attributes_from_caps_simple_sdp;
  rtp_hdr_class->set_caps_from_attributes =
      gst_rtp_header_extension_set_caps_from_attributes_simple_sdp;

  gst_element_class_set_static_metadata (gstelement_class,
      "Region-of-Interest (ROI) RTP Header Extension",
      GST_RTP_HDREXT_ELEMENT_CLASS,
      "Region-of-Interest (ROI) RTP Header Extension",
      "Havard Graff <havard@pexip.com>");
  gst_rtp_header_extension_class_set_uri (rtp_hdr_class, ROI_HDR_EXT_URI);
}

static void
gst_rtp_header_extension_roi_init (GstRTPHeaderExtensionRoi * self)
{
  GST_DEBUG_OBJECT (self, "creating element");
}
