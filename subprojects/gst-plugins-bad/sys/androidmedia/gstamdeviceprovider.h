/* Gstreamer
 * Copyright (C)  2022 Pexip (https://pexip.com/)
 *   @author: Tulio Beloqui <tulio@pexip.com>
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

#ifndef __GST_AM_DEVICE_PROVIDER_H__
#define __GST_AM_DEVICE_PROVIDER_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_AM_DEVICE_PROVIDER (gst_am_device_provider_get_type ())
#define GST_AM_DEVICE_PROVIDER_CAST(obj) ((GstAmDeviceProvider *)(obj))

#define GST_TYPE_AM_DEVICE (gst_am_device_get_type ())
#define GST_AM_DEVICE_CAST(obj) ((GstAmDevice *)(obj))

G_DECLARE_FINAL_TYPE (GstAmDevice, gst_am_device, GST, AM_DEVICE,
    GstDevice)

G_DECLARE_FINAL_TYPE (GstAmDeviceProvider, gst_am_device_provider, GST,
    AM_DEVICE_PROVIDER, GstDeviceProvider)

GST_DEVICE_PROVIDER_REGISTER_DECLARE (amdeviceprovider);

G_END_DECLS

#endif /* __GST_AM_DEVICE_PROVIDER_H__ */
