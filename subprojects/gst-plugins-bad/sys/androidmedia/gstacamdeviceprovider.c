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

  gchar *camera_id;
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

#define CAM_FRONT "Front Camera"
#define CAM_BACK "Back Camera"
#define CAM_EXT "External Camera"
#define CAM_UNKNOWN "Unknown Camera"


static gchar *
_build_camera_display_name (gint camera_index, uint8_t lens_facing,
    gfloat lens_aperture)
{
  if (camera_index == -1)
    return g_strdup (CAM_UNKNOWN);

  if (lens_facing == ACAMERA_LENS_FACING_FRONT)
    return g_strdup_printf ("%s (f/%.2f)", CAM_FRONT, lens_aperture);
  if (lens_facing == ACAMERA_LENS_FACING_BACK)
    return g_strdup_printf ("%s (f/%.2f)", CAM_BACK, lens_aperture);
  if (lens_facing == ACAMERA_LENS_FACING_EXTERNAL)
    return g_strdup_printf ("%s (%d, f/%.2f)", CAM_EXT, camera_index + 1,
        lens_aperture);

  return g_strdup (CAM_UNKNOWN);
}

static gchar *
gst_acam_device_provider_get_device_display_name (GstAcamDeviceProvider *
    provider, const char *camera_id, gint camera_index)
{
  ACameraMetadata *metadata = NULL;
  ACameraMetadata_const_entry entry;
  uint8_t lens_facing = 0;
  gfloat lens_aperture = 0.f;

  if (ACameraManager_getCameraCharacteristics (provider->manager,
          camera_id, &metadata) != ACAMERA_OK) {

    GST_ERROR ("Failed to retrieve camera (%s) metadata", camera_id);
    return _build_camera_display_name (-1, 0, lens_aperture);
  }

  if (ACameraMetadata_getConstEntry (metadata,
          ACAMERA_LENS_FACING, &entry) != ACAMERA_OK) {
    GST_ERROR ("Failed to retrieve camera (%s) LENS_FACING", camera_id);
    camera_index = -1;
  } else {
    lens_facing = entry.data.u8[0];
  }

  if (ACameraMetadata_getConstEntry (metadata,
          ACAMERA_LENS_INFO_AVAILABLE_APERTURES, &entry) != ACAMERA_OK) {
    GST_ERROR
        ("Failed to retrieve camera (%s) LENS_INFO_AVAILABLE_FOCAL_LENGTHS",
        camera_id);
  } else {
    lens_aperture = entry.data.f[0];
  }

  ACameraMetadata_free (metadata);
  return _build_camera_display_name (camera_index, lens_facing, lens_aperture);
}

static GstAcamDevice *
gst_acam_device_provider_create_device (GstAcamDeviceProvider *
    provider, const char *camera_id, gint camera_index)
{
  GstAcamDevice *device;
  gchar *display_name;
  GstCaps *caps;

  GST_TRACE_OBJECT (provider,
      "Creating device with camera_id: %s, camera_index: %d", camera_id,
      camera_index);

  display_name =
      gst_acam_device_provider_get_device_display_name (provider, camera_id,
      camera_index);
  caps = gst_caps_new_empty_simple ("video/x-raw");

  device = g_object_new (GST_TYPE_ACAM_DEVICE,  //
      "device-class", "Video/Source",   //
      "display-name", display_name,     //
      "caps", caps,             //
      "camera-id", g_strdup (camera_id),        //
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
gst_acam_device_provider_find_camera_index (GstAcamDeviceProvider *
    provider, const char *camera_id)
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
  gst_device_provider_device_add (GST_DEVICE_PROVIDER_CAST (provider),
      GST_DEVICE_CAST (device));
}

static void
gst_acam_device_provider_remove_device (GstAcamDeviceProvider * acamprovider,
    const char *camera_id_to_remove)
{
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER_CAST (acamprovider);
  GstDevice *device = NULL;
  GList *item;

  GST_OBJECT_LOCK (provider);
  for (item = provider->devices; item; item = item->next) {
    device = item->data;
    gchar *camera_id;
    gboolean found;

    g_object_get (device, "camera-id", &camera_id, NULL);
    found = (g_strcmp0 (camera_id_to_remove, camera_id) == 0);
    g_free (camera_id);

    if (found) {
      gst_object_ref (device);
      break;
    }

    device = NULL;
  }
  GST_OBJECT_UNLOCK (provider);

  if (device) {
    gst_device_provider_device_remove (provider, device);
    g_object_unref (device);
  }
}


static void
gst_acam_device_provider_on_camera_unavailable (void *context,
    const char *camera_id)
{
  GstAcamDeviceProvider *provider = GST_ACAM_DEVICE_PROVIDER_CAST (context);
  ACameraMetadata *metadata = NULL;

  // if we can't retrieve the metadata for the camera, it has been disconnected
  if (ACameraManager_getCameraCharacteristics (provider->manager,
          camera_id, &metadata) != ACAMERA_OK) {
    GST_INFO_OBJECT (provider, "Camera became unavailable (%s)", camera_id);
    gst_acam_device_provider_remove_device (provider, camera_id);
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
  PROP_CAMERA_ID = 2,
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
    case PROP_CAMERA_ID:
      g_value_set_string (value, (gchar *) device->camera_id);
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
    case PROP_CAMERA_ID:
      if (device->camera_id)
        g_free (device->camera_id);

      device->camera_id = g_strdup (g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_acam_device_class_init (GstAcamDeviceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);

  dev_class->create_element = gst_acam_device_create_element;
  gobject_class->get_property = gst_acam_device_get_property;
  gobject_class->set_property = gst_acam_device_set_property;

  g_object_class_install_property (gobject_class, PROP_CAMERA_INDEX,
      g_param_spec_int ("camera-index", "Camera Index",
          "The camera device index", 0, G_MAXINT, 0,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_CAMERA_ID,
      g_param_spec_string ("camera-id", "Camera ID",
          "The Unique ID for the camera",
          NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_acam_device_init (GstAcamDevice * device)
{
}
