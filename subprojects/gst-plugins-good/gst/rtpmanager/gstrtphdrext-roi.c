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

GST_DEBUG_CATEGORY_STATIC (rtp_hdrext_roi_debug);
#define GST_CAT_DEFAULT rtp_hdrext_roi_debug

/*
 * Internal roi-ext-hdr ID used as a safety mechanism, to make sure
 * that we not only only care about a specific roi_type (as in
 * GstVideoRegionOfInterestMeta) but also that it has been payloaded
 * and payloaded using the set of callbacks implemented below.
 */
#define ROI_HDR_EXT_URI GST_RTP_HDREXT_BASE"TBD:draft-ford-avtcore-roi-extension-00"

#define ROI_EXTHDR_SIZE 11
#define ROI_MAX_ROI_TYPES 15 /* 0 is unused and we allocate 1 byte for the ID */

#define DEFAULT_ROI_TYPE 126
#define DEFAULT_ROI_ID   8

enum
{
  PROP_0,
  PROP_ROI_TYPES,
};

struct _GstRTPHeaderExtensionRoi
{
  GstRTPHeaderExtension parent;

  GHashTable *roi_types;
  GHashTable *roi_ids;
};

G_DEFINE_TYPE_WITH_CODE (GstRTPHeaderExtensionRoi,
    gst_rtp_header_extension_roi, GST_TYPE_RTP_HEADER_EXTENSION,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "rtphdrextroi", 0,
        "RTP ROI Header Extensions"));
GST_ELEMENT_REGISTER_DEFINE (rtphdrextroi, "rtphdrextroi",
    GST_RANK_MARGINAL, GST_TYPE_RTP_HEADER_EXTENSION_ROI);

static void
gst_rtp_header_extension_roi_serialize_roi_types (GstRTPHeaderExtensionRoi *
    self, GValue * value)
{
  GHashTableIter iter;
  GValue val = G_VALUE_INIT;
  gpointer key;

  GST_DEBUG_OBJECT (self, "Serialize roi-types %p", self->roi_types);

  g_value_init (&val, G_TYPE_UINT);
  g_hash_table_iter_init (&iter, self->roi_types);

  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    guint roi_type = GPOINTER_TO_UINT (key);
    g_value_set_uint (&val, roi_type);
    gst_value_array_append_value (value, &val);
  }

  g_value_unset (&val);
}

static void
gst_rtp_header_extension_roi_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (object);
  switch (prop_id) {
    case PROP_ROI_TYPES:
      gst_rtp_header_extension_roi_serialize_roi_types (self, value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_warn_invalid_roi_type (const GValue * val)
{
  GValue str = G_VALUE_INIT;

  g_value_init (&str, G_TYPE_STRING);
  g_value_transform (val, &str);

  g_warning ("Invalid roi-type must be an unsigned int got '%s'",
      g_value_get_string (&str));

  g_value_unset (&str);
}

static void
gst_rtp_header_extension_roi_reset_roi_types (GstRTPHeaderExtensionRoi * self)
{
  GST_DEBUG_OBJECT (self, "Re-set roi-types");

  if (self->roi_types)
    g_hash_table_destroy (self->roi_types);
  if (self->roi_ids)
    g_hash_table_destroy (self->roi_ids);
  self->roi_types = g_hash_table_new (NULL, NULL);
  self->roi_ids = g_hash_table_new (NULL, NULL);
}

static void
gst_rtp_header_extension_roi_set_roi_types (GstRTPHeaderExtensionRoi * self,
    const GValue * value)
{
  guint num_roi_types = gst_value_array_get_size (value);

  GST_DEBUG_OBJECT (self, "Set roi-types with %p", value);

  if (num_roi_types == 0) {
    GST_INFO_OBJECT (self, "roi-types length is 0. Only the default roi-type "
        "of %u will be allowed", DEFAULT_ROI_TYPE);
    gst_rtp_header_extension_roi_reset_roi_types (self);
    g_hash_table_insert (self->roi_types,
        GUINT_TO_POINTER (DEFAULT_ROI_TYPE), GUINT_TO_POINTER (DEFAULT_ROI_ID));
    g_hash_table_insert (self->roi_ids,
        GUINT_TO_POINTER (DEFAULT_ROI_ID), GUINT_TO_POINTER (DEFAULT_ROI_TYPE));
    return;
  }

  /* verify our maximum number of roi-types supported:
   * we can only payload 1 RoI meta per roi-type; and given the a fixed size
   * per header extension we allowed a finite set of roi-tyes */
  if (num_roi_types > ROI_MAX_ROI_TYPES) {
    g_warning ("Maximum allowed set of roi-types surpassed. "
        "Current length=%d max-length=%d", num_roi_types,
        ROI_MAX_ROI_TYPES);
    return;
  }

  /* convert given array into 2 hash talbes: a bi-directional mapping of
   * roi-type to roi-id and vice versa. */
  gst_rtp_header_extension_roi_reset_roi_types (self);
  for (guint i = 0; i < num_roi_types; i++) {
    const GValue *val = gst_value_array_get_value (value, i);
    guint roi_type = DEFAULT_ROI_TYPE;
    guint roi_id = i + 1;       /* avoid using 0 as roi-id */

    if (!G_VALUE_HOLDS_UINT (val)) {
      _warn_invalid_roi_type (val);
      g_hash_table_destroy (self->roi_types);
      g_hash_table_destroy (self->roi_ids);
      return;
    }

    roi_type = g_value_get_uint (val);

    /* we dont allow duplicates */
    if (g_hash_table_lookup (self->roi_types, GUINT_TO_POINTER (roi_type))) {
      g_warning ("Invalid roi-types. No duplicates are allowed");
      g_hash_table_unref (self->roi_types);
      g_hash_table_unref (self->roi_ids);
    }

    GST_TRACE_OBJECT (self, "Adding %u:%u as roi-type", roi_type, roi_id);
    g_hash_table_insert (self->roi_types,
        GUINT_TO_POINTER (roi_type), GUINT_TO_POINTER (roi_id));
    g_hash_table_insert (self->roi_ids,
        GUINT_TO_POINTER (roi_id), GUINT_TO_POINTER (roi_type));
  }
}

static void
gst_rtp_header_extension_roi_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (object);
  switch (prop_id) {
    case PROP_ROI_TYPES:
      gst_rtp_header_extension_roi_set_roi_types (self, value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstRTPHeaderExtensionFlags
gst_rtp_header_extension_roi_get_supported_flags (GstRTPHeaderExtension * ext)
{
  // GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);
  // return g_hash_table_size (self->roi_types) >= 1 ?
  //     GST_RTP_HEADER_EXTENSION_ONE_BYTE : GST_RTP_HEADER_EXTENSION_TWO_BYTE;
  return GST_RTP_HEADER_EXTENSION_TWO_BYTE;
}

static gsize
gst_rtp_header_extension_roi_get_max_size (GstRTPHeaderExtension * ext,
    const GstBuffer * input_meta)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);
  return g_hash_table_size (self->roi_types) * ROI_EXTHDR_SIZE;
}

static gssize
gst_rtp_header_extension_roi_write (GstRTPHeaderExtension * ext,
    const GstBuffer * input_meta, GstRTPHeaderExtensionFlags write_flags,
    G_GNUC_UNUSED GstBuffer * output, guint8 * data, gsize size)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);
  GstVideoRegionOfInterestMeta *meta;
  gpointer state = NULL;
  guint offset = 0;

  g_return_val_if_fail (size >=
      gst_rtp_header_extension_roi_get_max_size (ext, NULL), -1);
  g_return_val_if_fail (write_flags &
      gst_rtp_header_extension_roi_get_supported_flags (ext), -1);

  gboolean written_roi_ids[ROI_MAX_ROI_TYPES + 1] = { 0 };
  while ((meta = (GstVideoRegionOfInterestMeta *)
          gst_buffer_iterate_meta_filtered ((GstBuffer *) input_meta, &state,
              GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE))) {
    GstStructure *extra_param_s;
    guint num_faces = 0;

    /* filter by known roi-types */
    gpointer roi_id_ptr = g_hash_table_lookup (self->roi_types,
        GUINT_TO_POINTER (meta->roi_type));
    guint roi_id = GPOINTER_TO_UINT (roi_id_ptr);
    GST_TRACE_OBJECT (self, "Got roi-type %u roi-id %u", meta->roi_type,
        roi_id);

    if (!roi_id_ptr)
      continue;

    if (written_roi_ids[roi_id]) {
      GST_TRACE_OBJECT (self, "Already written RoI meta with type %d. "
          "Skipping.", meta->roi_type);
      continue;
    }

    /* RoI coordinates */
    GST_WRITE_UINT16_BE (&data[offset + 0], meta->x);
    GST_WRITE_UINT16_BE (&data[offset + 2], meta->y);
    GST_WRITE_UINT16_BE (&data[offset + 4], meta->w);
    GST_WRITE_UINT16_BE (&data[offset + 6], meta->h);

    /* RoI type */
    g_assert (roi_id <= G_MAXUINT8);
    GST_WRITE_UINT8 (&data[offset + 8], (guint8) roi_id);

    /* RoI extra: number of faces */
    extra_param_s =
        gst_video_region_of_interest_meta_get_param (meta, "extra-param");
    if (extra_param_s)
      gst_structure_get_uint (extra_param_s, "num_faces", &num_faces);
    GST_WRITE_UINT16_BE (&data[offset + 9], num_faces);

    GST_TRACE_OBJECT (self, "Wrote header extension from RoI meta "
        "(%u %u,%u %ux%u %u)", meta->roi_type, meta->x, meta->y, meta->w,
        meta->h, num_faces);

    written_roi_ids[roi_id] = TRUE;
    offset += ROI_EXTHDR_SIZE;
  }

  return offset;
}

static gboolean
gst_rtp_header_extension_roi_read (GstRTPHeaderExtension * ext,
    GstRTPHeaderExtensionFlags read_flags, const guint8 * data, gsize size,
    GstBuffer * buffer)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (ext);

  /* read as many extension headers as we have signaled roi-types
   * XXX: Is this a valid assumption? it makes this only work with
   * GStreamer payloader */
  guint offset = 0;
  for (guint i = 0; i < g_hash_table_size (self->roi_types); i++) {
    guint16 roi_id = GST_READ_UINT8 (&data[offset + 8]);
    /* filter by roi-id (roi-type stored in the header extension) */
    gpointer rtp_type_ptr =
        g_hash_table_lookup (self->roi_ids, GUINT_TO_POINTER (roi_id));
    guint roi_type = GPOINTER_TO_UINT (rtp_type_ptr);
    if (rtp_type_ptr) {
      guint16 x = GST_READ_UINT16_BE (&data[offset + 0]);
      guint16 y = GST_READ_UINT16_BE (&data[offset + 2]);
      guint16 w = GST_READ_UINT16_BE (&data[offset + 4]);
      guint16 h = GST_READ_UINT16_BE (&data[offset + 6]);
      guint16 num_faces = GST_READ_UINT16_BE (&data[offset + 9]);
      GstVideoRegionOfInterestMeta *meta =
          gst_buffer_add_video_region_of_interest_meta_id (buffer,
          GPOINTER_TO_UINT (rtp_type_ptr), x, y, w, h);
      GstStructure *extra_param_s = gst_structure_new ("extra-param",
          "num_faces", G_TYPE_UINT, num_faces, NULL);
      gst_video_region_of_interest_meta_add_param (meta, extra_param_s);

      GST_TRACE_OBJECT (self, "Read header extension and added RoI meta "
          "(%u %u,%u %ux%u %u) to buffer %p", roi_type,
          x, y, w, h, num_faces, buffer);
    }
    offset += ROI_EXTHDR_SIZE;
  }
  return TRUE;
}

static void
gst_rtp_header_extension_roi_dispose (GObject * object)
{
  GstRTPHeaderExtensionRoi *self = GST_RTP_HEADER_EXTENSION_ROI_CAST (object);
  g_assert (self->roi_types);
  g_assert (self->roi_ids);
  g_hash_table_destroy (self->roi_types);
  g_hash_table_destroy (self->roi_ids);
  G_OBJECT_CLASS (gst_rtp_header_extension_roi_parent_class)->dispose (object);
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
  gobject_class->dispose =
      GST_DEBUG_FUNCPTR (gst_rtp_header_extension_roi_dispose);
  gobject_class->get_property = gst_rtp_header_extension_roi_get_property;
  gobject_class->set_property = gst_rtp_header_extension_roi_set_property;
  /**
   * rtphdrextroi:roi-type:
   *
   * What roi-type (GQuark) to write the extension-header for.
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class, PROP_ROI_TYPES,
      gst_param_spec_array ("roi-types", "RoI types",
          "Set of what roi-type(s) (GQuark(s)) to write the extension-header for",
          g_param_spec_uint ("roi-type", "RoI type",
              "What roi-type (GQuark) to write the extension-header for",
              1, G_MAXUINT32, DEFAULT_ROI_TYPE,
              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
              G_PARAM_STATIC_STRINGS),
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
  rtp_hdr_class->get_supported_flags =
      gst_rtp_header_extension_roi_get_supported_flags;
  rtp_hdr_class->get_max_size = gst_rtp_header_extension_roi_get_max_size;
  rtp_hdr_class->write = gst_rtp_header_extension_roi_write;
  rtp_hdr_class->read = gst_rtp_header_extension_roi_read;
  gst_element_class_set_static_metadata (gstelement_class,
      "Region-of-Interest (ROI) RTP Header Extension",
      GST_RTP_HDREXT_ELEMENT_CLASS,
      "Region-of-Interest (ROI) RTP Header Extension",
      "Havard Graff <havard@pexip.com>");
  gst_rtp_header_extension_class_set_uri (rtp_hdr_class, ROI_HDR_EXT_URI);
  GST_DEBUG_CATEGORY_INIT (rtp_hdrext_roi_debug, "rtphdrextroi", 0,
      "RTPHeaderExtensionRoI");
}

static void
gst_rtp_header_extension_roi_init (GstRTPHeaderExtensionRoi * self)
{
  GST_DEBUG_OBJECT (self, "creating element");
}
