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

#include "gstacamdeviceprovider.h"

#include <camera/NdkCameraManager.h>

struct _GstAcamDeviceProvider
{
  GstDeviceProvider parent;

  ACameraManager *manager;

  struct ACameraManager_AvailabilityListener listeners;
};

struct _GstAcamDevice
{
  GstDevice parent;

  gint camera_index;
};

G_DEFINE_TYPE (GstAcamDeviceProvider, gst_acam_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

G_DEFINE_TYPE (GstAcamDevice, gst_acam_device, GST_TYPE_DEVICE);

GST_DEVICE_PROVIDER_REGISTER_DEFINE (acamdeviceprovider, "acamdeviceprovider",
    GST_RANK_PRIMARY, GST_TYPE_ACAM_DEVICE_PROVIDER);

GST_DEBUG_CATEGORY_STATIC (gst_acam_device_provider_debug);
#define GST_CAT_DEFAULT (gst_acam_device_provider_debug)
#define device_provider_parent_class gst_acam_device_provider_parent_class


static GstAcamDevice *
gst_acam_device_provider_create_device (GstAcamDeviceProvider *
    provider, const char *camera_id, gint camera_index)
{
  GstAcamDevice *device;

  // FIXME: add a descriptive display name
  gchar *display_name = g_strdup_printf ("Camera (%s)", camera_id);
  GstCaps *caps = gst_caps_new_empty_simple ("video/x-raw");

  GST_TRACE_OBJECT (provider,
      "Creating device with camera_id: %s, camera_index: %d", camera_id,
      camera_index);

  device = g_object_new (GST_TYPE_ACAM_DEVICE,  //
      "device-class", "Video/Source",   //
      "display-name", display_name,     //
      "caps", caps,             //
      "camera-index", camera_index,     //
      NULL);

  return device;
}

static GList *
gst_acam_device_provider_probe (GstDeviceProvider * object)
{
  GList *devices;

  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (object);
  ACameraIdList *id_list;
  gint i;

  if (ACameraManager_getCameraIdList (provider->manager,
          &id_list) != ACAMERA_OK) {
    GST_ERROR_OBJECT (provider, "Failed to get camera id list");
    return NULL;
  }

  GST_DEBUG_OBJECT (provider, "Found %d cameras", id_list->numCameras);

  for (i = 0; i < id_list->numCameras; i++) {
    const char *camera_id = id_list->cameraIds[i];
    GstAcamDevice *device =
        gst_acam_device_provider_create_device (provider, camera_id, i);

    devices = g_list_append (devices, GST_DEVICE_CAST (device));
  }

  ACameraManager_deleteCameraIdList (id_list);
  return devices;
}

static gboolean
gst_acam_device_provider_start (GstDeviceProvider * object)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (object);

  GST_DEBUG_OBJECT (provider, "Starting...");

  if (!provider->manager) {
    GST_ERROR_OBJECT (provider,
        "Starting without an instance of ACameraManager");
    return FALSE;
  }

  if (ACameraManager_registerAvailabilityCallback (provider->manager,
          &provider->listeners) != ACAMERA_OK) {
    GST_ERROR_OBJECT (provider, "Couldn't register Availability Callbacks");
    return FALSE;
  }

  return TRUE;
}

static void
gst_acam_device_provider_stop (GstDeviceProvider * object)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (object);

  GST_DEBUG_OBJECT (provider, "Stopping...");

  if (!provider->manager) {
    GST_ERROR_OBJECT (provider,
        "Stopping without an instance of ACameraManager");
    return;
  }

  if (ACameraManager_unregisterAvailabilityCallback (provider->manager,
          &provider->listeners) != ACAMERA_OK) {
    GST_ERROR_OBJECT (provider, "Couldn't unregister Availability Callbacks");
    return;
  }
}

static gint
gst_acam_device_provider_find_camera_index (GstAcamDeviceProvider * provider,
    const char *camera_id)
{
  gint i, idx = -1;
  ACameraIdList *id_list;

  if (ACameraManager_getCameraIdList (provider->manager,
          &id_list) != ACAMERA_OK) {
    GST_ERROR_OBJECT (provider, "Failed to get camera id list");
    return -1;
  }

  for (i = 0; i < id_list->numCameras; i++) {
    const char *id = id_list->cameraIds[i];
    if (g_strcmp0 (camera_id, id) == 0) {
      idx = i;
      break;
    }
  }

  ACameraManager_deleteCameraIdList (id_list);
  return idx;
}

static void
gst_acam_device_provider_on_camera_available (void *context,
    const char *camera_id)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (context);
  gint camera_index;
  GstAcamDevice *device;

  camera_index =
      gst_acam_device_provider_find_camera_index (provider, camera_id);
  device =
      gst_acam_device_provider_create_device (provider, camera_id,
      camera_index);

  GST_INFO_OBJECT (provider, "Camera became available (%s)", camera_id);
  gst_device_provider_device_add (provider, GST_DEVICE_CAST (device));
}

static void
gst_acam_device_provider_on_camera_unavailable (void *context,
    const char *camera_id)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (context);
  GstDevice *device;
  ACameraMetadata *metadata = NULL;

  // if we can't retrieve the metadata for the camera, it has been disconnected
  if (ACameraManager_getCameraCharacteristics (provider->manager,
          camera_id, &metadata) != ACAMERA_OK) {

    device = gst_acam_device_provider_create_device (provider, camera_id, -1);
    GST_INFO_OBJECT (provider, "Camera became unavailable (%s)", camera_id);
    gst_device_provider_device_remove (provider, device);
    g_object_unref (device);
  }

  if (metadata)
    ACameraMetadata_free (metadata);
}

static void
gst_acam_device_provider_init (GstAcamDeviceProvider * provider)
{
  provider->manager = ACameraManager_create ();

  provider->listeners.onCameraAvailable =
      gst_acam_device_provider_on_camera_available;
  provider->listeners.onCameraUnavailable =
      gst_acam_device_provider_on_camera_unavailable;
  provider->listeners.context = provider;
}

static void
gst_acam_device_provider_finalize (GObject * object)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (object);

  if (provider->manager) {
    ACameraManager_delete (provider->manager);
    provider->manager = NULL;
  }

  G_OBJECT_CLASS (device_provider_parent_class)->finalize (object);
}

static void
gst_acam_device_provider_class_init (GstAcamDeviceProviderClass * klass)
{
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  dm_class->probe = gst_acam_device_provider_probe;
  dm_class->start = gst_acam_device_provider_start;
  dm_class->stop = gst_acam_device_provider_stop;

  gobject_class->finalize = gst_acam_device_provider_finalize;

  gst_device_provider_class_set_static_metadata (dm_class,
      "Android Camera Device Provider", "Source/Video",
      "List and monitor Android camera devices",
      "Tulio Beloqui <tulio@pexip.com>");

  GST_DEBUG_CATEGORY_INIT (gst_acam_device_provider_debug,
      "acamdeviceprovider", 0, "Android Camera Device Provider");
}

static GstElement *
gst_acam_device_create_element (GstDevice * object, const gchar * name)
{
  GstAcamDevice *device = GST_ACAM_DEVICE_CAST (object);
  GstElement *elem;

  elem = gst_element_factory_make ("ahc2src", name);
  g_object_set (elem, "camera-index", device->camera_index, NULL);

  return elem;
}

enum
{
  PROP_CAMERA_INDEX = 1,
};

static void
gst_acam_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAcamDevice *device = GST_ACAM_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_CAMERA_INDEX:
      g_value_set_int (value, device->camera_index);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_acam_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAcamDevice *device = GST_ACAM_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_CAMERA_INDEX:
      device->camera_index = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_acam_device_class_init (GstAcamDeviceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);

  dev_class->create_element = gst_acam_device_create_element;
  object_class->get_property = gst_acam_device_get_property;
  object_class->set_property = gst_acam_device_set_property;

  g_object_class_install_property (object_class, PROP_CAMERA_INDEX,
      g_param_spec_int ("camera-index", "Camera Index",
          "The camera device index", 0, G_MAXINT, 0,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_acam_device_init (GstAcamDevice * device)
{
}
