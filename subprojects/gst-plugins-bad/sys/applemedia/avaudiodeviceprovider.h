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

#ifndef __GST_AV_AUDIO_DEVICE_PROVIDER_H__
#define __GST_AV_AUDIO_DEVICE_PROVIDER_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_AV_AUDIO_DEVICE_PROVIDER (gst_av_audio_device_provider_get_type ())
#define GST_AV_AUDIO_DEVICE_PROVIDER_CAST(obj) ((GstAVAudioDeviceProvider *)(obj))

#define GST_TYPE_AV_AUDIO_DEVICE (gst_av_audio_device_get_type ())
#define GST_AV_AUDIO_DEVICE_CAST(obj) ((GstAVAudioDevice *)(obj))

G_DECLARE_FINAL_TYPE (GstAVAudioDevice, gst_av_audio_device, GST, AV_AUDIO_DEVICE,
    GstDevice)

G_DECLARE_FINAL_TYPE (GstAVAudioDeviceProvider, gst_av_audio_device_provider, GST,
    AV_AUDIO_DEVICE_PROVIDER, GstDeviceProvider)

GST_DEVICE_PROVIDER_REGISTER_DECLARE (avaudiodeviceprovider);

G_END_DECLS


#endif /* __GST_AV_AUDIO_DEVICE_PROVIDER_H__ */
