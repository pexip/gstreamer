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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#import <AVFoundation/AVFoundation.h>

#include "avaudiodeviceprovider.h"

struct _GstAVAudioDeviceProvider
{
  GstDeviceProvider parent;
};

struct _GstAVAudioDevice
{
  GstDevice parent;

  gchar *type;
  gchar *uid;
};

G_DEFINE_TYPE (GstAVAudioDeviceProvider, gst_av_audio_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

G_DEFINE_TYPE (GstAVAudioDevice, gst_av_audio_device, GST_TYPE_DEVICE);

GST_DEVICE_PROVIDER_REGISTER_DEFINE (avaudiodeviceprovider,
    "avaudiodeviceprovider", GST_RANK_PRIMARY,
    GST_TYPE_AV_AUDIO_DEVICE_PROVIDER);

GST_DEBUG_CATEGORY_STATIC (gst_av_audio_device_provider_debug);
#define GST_CAT_DEFAULT (gst_av_audio_device_provider_debug)

static GstDevice *
gst_av_audio_device_new (gchar * name, gchar * type, gchar * uid,
    const gchar * device_class)
{
  GstAVAudioDevice *device = g_object_new (GST_TYPE_AV_AUDIO_DEVICE,
      "display-name", name,
      "device-class", device_class,
      "type", type,
      "uid", uid,
      "caps", gst_caps_new_empty_simple ("audio/x-raw"),
      NULL);

  return GST_DEVICE_CAST (device);
}

static GstDevice *
gst_device_from_port_description (AVAudioSessionPortDescription *
    portDesc, const gchar * device_class)
{
  gchar *name = g_strdup ([portDesc.portName UTF8String]);
  gchar *type = g_strdup ([portDesc.portType UTF8String]);
  gchar *uid = g_strdup ([portDesc.UID UTF8String]);

  return gst_av_audio_device_new (name, type, uid, device_class);
}

static GList *
gst_av_audio_device_provider_probe (GstDeviceProvider * object)
{
  GList *devices = NULL;
  AVAudioSession *audioSession =[AVAudioSession sharedInstance];

  NSString *sessionCategory = AVAudioSessionCategoryPlayAndRecord;
  AVAudioSessionCategoryOptions options =
      AVAudioSessionCategoryOptionAllowBluetooth |
      AVAudioSessionCategoryOptionAllowBluetoothA2DP |
      AVAudioSessionCategoryOptionDefaultToSpeaker;

[audioSession setCategory: sessionCategory withOptions: options error:nil];
[audioSession setMode: AVAudioSessionModeVoiceChat error:nil];
[audioSession setActive: YES error:nil];

  AVAudioSessionRouteDescription *route = audioSession.currentRoute;

  gboolean addedBuiltinSpk = FALSE;
  gboolean addedBuiltinRecv = FALSE;

  if (route != nil) {
    for (AVAudioSessionPortDescription * outPort in route.outputs) {
    if ([outPort.portType isEqualToString:AVAudioSessionPortBuiltInSpeaker])
        addedBuiltinSpk = TRUE;

    if ([outPort.portType isEqualToString:AVAudioSessionPortBuiltInReceiver])
        addedBuiltinRecv = TRUE;

      GstDevice *device =
          gst_device_from_port_description (outPort, "Audio/Sink");
      devices = g_list_append (devices, gst_object_ref_sink (device));
    }
  }

  for (AVAudioSessionPortDescription * inPort in[audioSession availableInputs]) {
    GstDevice *device =
        gst_device_from_port_description (inPort, "Audio/Source");
    devices = g_list_append (devices, gst_object_ref_sink (device));
  }

  if (!addedBuiltinSpk) {
    gchar *type = g_strdup ([AVAudioSessionPortBuiltInSpeaker UTF8String]);
    gchar *name = g_strdup_printf ("iPhone %s", type);
    GstDevice *dev = gst_av_audio_device_new (name, type, NULL, "Audio/Sink");

    devices = g_list_append (devices, gst_object_ref_sink (dev));
  }

  if (!addedBuiltinRecv) {
    gchar *type = g_strdup ([AVAudioSessionPortBuiltInReceiver UTF8String]);
    gchar *name = g_strdup_printf ("iPhone %s", type);
    GstDevice *dev = gst_av_audio_device_new (name, type, NULL, "Audio/Sink");

    devices = g_list_append (devices, gst_object_ref_sink (dev));
  }

  return devices;
}

static void
gst_av_audio_device_provider_init (GstAVAudioDeviceProvider * provider)
{
}

static void
gst_av_audio_device_provider_class_init (GstAVAudioDeviceProviderClass * klass)
{
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);
  dm_class->probe = gst_av_audio_device_provider_probe;

  gst_device_provider_class_set_static_metadata (dm_class,
      "AV Audio Device Provider", "Sink/Source/Audio",
      "List and monitor IOS audio devices", "Tulio Beloqui <tulio@pexip.com>");

  GST_DEBUG_CATEGORY_INIT (gst_av_audio_device_provider_debug,
      "avaudiodeviceprovider", 0, "AV Audio Device Provider for IOS");
}

enum
{
  PROP_DEVICE_TYPE = 1,
  PROP_DEVICE_UID,
};

static void
gst_av_audio_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAVAudioDevice *device = GST_AV_AUDIO_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE_TYPE:
      g_value_set_string (value, device->type);
      break;
    case PROP_DEVICE_UID:
      g_value_set_string (value, device->uid);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_av_audio_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAVAudioDevice *device = GST_AV_AUDIO_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE_TYPE:
      device->type = g_strdup (g_value_get_string (value));
      break;
    case PROP_DEVICE_UID:
      device->uid = g_strdup (g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_av_audio_device_finalize (GObject * object)
{
  GstAVAudioDevice *device = GST_AV_AUDIO_DEVICE_CAST (object);

  g_free (device->type);
  g_free (device->uid);

  G_OBJECT_CLASS (gst_av_audio_device_parent_class)->finalize (object);
}

static void
gst_av_audio_device_class_init (GstAVAudioDeviceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = gst_av_audio_device_get_property;
  object_class->set_property = gst_av_audio_device_set_property;
  object_class->finalize = gst_av_audio_device_finalize;

  g_object_class_install_property (object_class, PROP_DEVICE_TYPE,
      g_param_spec_string ("type",
          "Type",
          "The AVAudioSessionPort type for the device",
          NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_DEVICE_UID,
      g_param_spec_string ("uid",
          "UID",
          "A system-assigned unique identifier (UID) for the device",
          NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
gst_av_audio_device_init (GstAVAudioDevice * device)
{
}
