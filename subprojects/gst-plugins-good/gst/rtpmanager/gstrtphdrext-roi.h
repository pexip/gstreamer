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

#ifndef __GST_RTPHDREXT_ROI_H__
#define __GST_RTPHDREXT_ROI_H__

#include <gst/gst.h>
#include <gst/rtp/gstrtphdrext.h>

G_BEGIN_DECLS

#define GST_TYPE_RTP_HEADER_EXTENSION_ROI (gst_rtp_header_extension_roi_get_type())

G_DECLARE_FINAL_TYPE (GstRTPHeaderExtensionRoi, gst_rtp_header_extension_roi, GST, RTP_HEADER_EXTENSION_ROI, GstRTPHeaderExtension)

GST_ELEMENT_REGISTER_DECLARE (rtphdrextroi);

#define GST_RTP_HEADER_EXTENSION_ROI_CAST(obj) ((GstRTPHeaderExtensionRoi *)obj)

G_END_DECLS

#endif /* __GST_RTPHDREXT_ROI_H__ */
