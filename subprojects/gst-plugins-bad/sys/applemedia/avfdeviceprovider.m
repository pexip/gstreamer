/* GStreamer
 * Copyright (C) 2019 Josh Matthews <josh@joshmatthews.net>
 * Copyright (c) 2024, Pexip AS
 *  @author: Tulio Beloqui <tulio@pexip.com
 *
 * avfdeviceprovider.c: AVF device probing and monitoring
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#import <AVFoundation/AVFoundation.h>
#include "avfvideosrc.h"
#include "avfdeviceprovider.h"

#include <string.h>

#include <gst/gst.h>

static GstDevice *gst_avf_device_new (AVCaptureDevice * device);

G_DEFINE_TYPE (GstAVFDeviceProvider, gst_avf_device_provider,
               GST_TYPE_DEVICE_PROVIDER);

#define GST_AVF_DEVICE_PROVIDER_IMPL(obj) \
  ((__bridge GstAVFDeviceProviderImpl *) GST_AVF_DEVICE_PROVIDER_CAST (obj)->impl)

static void gst_avf_device_provider_finalize (GObject * obj);

static GList *gst_avf_device_provider_probe (GstDeviceProvider * provider);
static gboolean gst_avf_device_provider_start (GstDeviceProvider * provider);
static void gst_avf_device_provider_stop (GstDeviceProvider * provider);

static void gst_avf_device_provider_on_device_added (GstDeviceProvider * provider, AVCaptureDevice * device);
static void gst_avf_device_provider_on_device_removed (GstDeviceProvider * provider, const char * device_unique_id);

@interface GstAVFDeviceProviderImpl : NSObject {
  gpointer parent;
}

- (id)init;
- (id)initWithParent:(gpointer)parent;

- (void)deviceWasConnected:(NSNotification *)notification;
- (void)deviceWasDisconnected:(NSNotification *)notification;

@end

@implementation GstAVFDeviceProviderImpl

- (id)init
{
  return [self initWithParent:NULL];
}

- (id)initWithParent:(gpointer)p
{
  if ((self = [super init])) {
    parent = p;
  }

  return self;
}

- (void)deviceWasConnected:(NSNotification *)notification
{
  AVCaptureDevice * device = notification.object;
  if ([device hasMediaType:  AVMediaTypeVideo]) 
    gst_avf_device_provider_on_device_added (GST_DEVICE_PROVIDER_CAST (parent), device);
}


- (void)deviceWasDisconnected:(NSNotification *)notification
{
  AVCaptureDevice * device = notification.object;
  if ([device hasMediaType:  AVMediaTypeVideo]) {
    const char * device_unique_id = [[device uniqueID] UTF8String];
    gst_avf_device_provider_on_device_removed (GST_DEVICE_PROVIDER_CAST (parent), device_unique_id);
  }
}

@end

static void
gst_avf_device_provider_class_init (GstAVFDeviceProviderClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);

  object_class->finalize = gst_avf_device_provider_finalize;

  dm_class->probe = gst_avf_device_provider_probe;
  dm_class->start = gst_avf_device_provider_start;
  dm_class->stop = gst_avf_device_provider_stop;

  gst_avf_video_src_debug_init ();

  gst_device_provider_class_set_static_metadata (dm_class,
                                                 "AVF Device Provider", "Source/Video",
                                                 "List and provide AVF source devices",
                                                 "Josh Matthews <josh@joshmatthews.net>");
}

static void
gst_avf_device_provider_init (GstAVFDeviceProvider * self)
{
  self->impl = (__bridge_retained gpointer)[[GstAVFDeviceProviderImpl alloc] initWithParent:self];
}

static void
gst_avf_device_provider_finalize (GObject * obj)
{
  CFBridgingRelease(GST_AVF_DEVICE_PROVIDER_CAST(obj)->impl);
  G_OBJECT_CLASS (gst_avf_device_provider_parent_class)->finalize (obj);
}

static gint
gst_avf_device_get_int_prop (GstDevice * device, const gchar * prop_name)
{
  GstStructure *props;
  g_object_get (device, "properties", &props, NULL);

  gint ret = 0;
  g_assert (gst_structure_get_int (props, prop_name, &ret));
  gst_structure_free (props);
  return ret; 
}

static int
gst_avf_device_get_string_prop (GstDevice * device, const gchar * prop_name)
{
  GstStructure *props;
  g_object_get (device, "properties", &props, NULL);

  gchar * ret = g_strdup (gst_structure_get_string (props, prop_name));
  gst_structure_free (props);
  return ret;
}

static GstStructure *
gst_av_capture_device_get_props (AVCaptureDevice *device)
{
  char *unique_id, *model_id;
  GstStructure *props = gst_structure_new_empty ("avf-proplist");

  unique_id = g_strdup ([[device uniqueID] UTF8String]);
  model_id = g_strdup ([[device modelID] UTF8String]);

  gst_structure_set (props,
    "device.api", G_TYPE_STRING, "avf",
    "avf.unique_id", G_TYPE_STRING, unique_id,
    "avf.model_id", G_TYPE_STRING, model_id,
    "avf.has_flash", G_TYPE_BOOLEAN, [device hasFlash],
    "avf.has_torch", G_TYPE_BOOLEAN, [device hasTorch],
    "avf.position", G_TYPE_INT, [device position],
  NULL);

  g_free (unique_id);
  g_free (model_id);

#if !HAVE_IOS
  char *manufacturer = g_strdup ([[device manufacturer] UTF8String]);
  gst_structure_set (props,
    "avf.manufacturer", G_TYPE_STRING, manufacturer,
  NULL);

  g_free (manufacturer);
#endif

  return props;
}

/*  
 * Compare the devices by position:
 * AVCaptureDevicePositionFront=2
 * AVCaptureDevicePositionBack=1
 * AVCaptureDevicePositionUnspecified=0
 *
 * We want the high positions first so for ios we will put the "front" camera first.
 */
static gint
gst_avf_device_compare_func (gconstpointer a, gconstpointer b)
{
  gint position_a = gst_avf_device_get_int_prop (GST_DEVICE_CAST(a), "avf.position");
  gint position_b = gst_avf_device_get_int_prop (GST_DEVICE_CAST(b), "avf.position");

  if (position_a > position_b)
    return -1;

  if (position_a == position_b)
    return 0;

  return 1;
}

static GList *
gst_avf_device_provider_probe (GstDeviceProvider * provider)
{
  GList *result = NULL;

  NSMutableArray<AVCaptureDeviceType> *deviceTypes = [NSMutableArray arrayWithArray:@[
#if defined(HOST_IOS)
                                                AVCaptureDeviceTypeBuiltInUltraWideCamera,
                                                AVCaptureDeviceTypeBuiltInDualWideCamera,
                                                AVCaptureDeviceTypeBuiltInTelephotoCamera,
                                                AVCaptureDeviceTypeBuiltInDualCamera,
                                                AVCaptureDeviceTypeBuiltInTripleCamera,
#endif
                                                AVCaptureDeviceTypeBuiltInWideAngleCamera
                                                ]];

  if (@available(iOS 17, macOS 14, *)) {
    [deviceTypes addObject:AVCaptureDeviceTypeContinuityCamera];
    [deviceTypes addObject:AVCaptureDeviceTypeExternal];
  } else {
#if defined(HOST_DARWIN)
    [deviceTypes addObject:AVCaptureDeviceTypeExternalUnknown];
#endif
  }

  AVCaptureDeviceDiscoverySession *discovery_sess;
  discovery_sess = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
      mediaType:AVMediaTypeVideo
      position:AVCaptureDevicePositionUnspecified];
  NSArray<AVCaptureDevice *> *devices = discovery_sess.devices;
  
  for (int i = 0; i < [devices count]; i++) {
    AVCaptureDevice *device = [devices objectAtIndex:i];
    GstDevice *gst_device = gst_avf_device_new (device);

    result = g_list_prepend (result, gst_object_ref_sink (gst_device));
  }

  result = g_list_sort (result, gst_avf_device_compare_func);

  return result;
}


static void
gst_avf_device_provider_populate_devices (GstDeviceProvider * provider)
{
  GList *devices, *it;

  devices = gst_avf_device_provider_probe (provider);

  for (it = devices; it != NULL; it = g_list_next (it)) {
    GstDevice *device = GST_DEVICE_CAST (it->data);
    if (device)
      gst_device_provider_device_add (provider, device);
  }

  g_list_free_full (devices, gst_object_unref);
}

static gboolean
gst_avf_device_provider_start (GstDeviceProvider * provider)
{
  GST_INFO_OBJECT (provider, "Starting...");

  gst_avf_device_provider_populate_devices (provider);

  [NSNotificationCenter.defaultCenter addObserver:GST_AVF_DEVICE_PROVIDER_IMPL(provider)
                                      selector:@selector(deviceWasConnected:)
                                      name:AVCaptureDeviceWasConnectedNotification
                                      object:nil];

  [NSNotificationCenter.defaultCenter addObserver:GST_AVF_DEVICE_PROVIDER_IMPL(provider)
                                      selector:@selector(deviceWasDisconnected:)
                                      name:AVCaptureDeviceWasDisconnectedNotification
                                      object:nil];

  return TRUE;
}

static void
gst_avf_device_provider_stop (GstDeviceProvider * provider)
{
  GST_INFO_OBJECT (provider, "Stoping...");

  [NSNotificationCenter.defaultCenter removeObserver:GST_AVF_DEVICE_PROVIDER_IMPL(provider)
                                      name:AVCaptureDeviceWasConnectedNotification
                                      object:nil];

  [NSNotificationCenter.defaultCenter removeObserver:GST_AVF_DEVICE_PROVIDER_IMPL(provider)
                                      name:AVCaptureDeviceWasDisconnectedNotification
                                      object:nil];

  [NSNotificationCenter.defaultCenter removeObserver:GST_AVF_DEVICE_PROVIDER_IMPL(provider)];
}

static void
gst_avf_device_provider_on_device_added (GstDeviceProvider * provider, AVCaptureDevice * device)
{
  gst_device_provider_device_add (provider, gst_avf_device_new (device));
}

static void
gst_avf_device_provider_on_device_removed (GstDeviceProvider * provider, const char * id_to_remove)
{
  GstDevice *device = NULL;
  GList *item;

  GST_OBJECT_LOCK (provider);
  for (item = provider->devices; item; item = item->next) {
    device = item->data;
    gchar *dev_unique_id = gst_avf_device_get_string_prop (device, "avf.unique_id");
    gboolean found;

    found = (g_strcmp0 ((const gchar *)id_to_remove, dev_unique_id) == 0);
    g_free (dev_unique_id);

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

enum
{
  PROP_0,
};

G_DEFINE_TYPE (GstAvfDevice, gst_avf_device, GST_TYPE_DEVICE);

static GstElement *gst_avf_device_create_element (GstDevice * device,
                                                 const gchar * name);
static gboolean gst_avf_device_reconfigure_element (GstDevice * device,
                                                   GstElement * element);

static void gst_avf_device_get_property (GObject * object, guint prop_id,
                                         GValue * value, GParamSpec * pspec);
static void gst_avf_device_set_property (GObject * object, guint prop_id,
                                         const GValue * value, GParamSpec * pspec);


static void
gst_avf_device_class_init (GstAvfDeviceClass * klass)
{
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  dev_class->create_element = gst_avf_device_create_element;
  dev_class->reconfigure_element = gst_avf_device_reconfigure_element;

  object_class->get_property = gst_avf_device_get_property;
  object_class->set_property = gst_avf_device_set_property;
}

static void
gst_avf_device_init (GstAvfDevice * device)
{
}

static GstElement *
gst_avf_device_create_element (GstDevice * device, const gchar * name)
{
  GstAvfDevice *avf_dev = GST_AVF_DEVICE (device);
  GstElement *elem;

  elem = gst_element_factory_make (avf_dev->element, name);
  g_object_set (elem, "device-index", avf_dev->device_index, NULL);

  return elem;
}

static gboolean
gst_avf_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  GstAvfDevice *avf_dev = GST_AVF_DEVICE (device);

  if (!strcmp (avf_dev->element, "avfvideosrc") && GST_IS_AVF_VIDEO_SRC (element)) {
    g_object_set (element, "device-index", avf_dev->device_index, NULL);
    return TRUE;
  }

  return FALSE;
}

static void
gst_avf_device_get_property (GObject * object, guint prop_id,
                             GValue * value, GParamSpec * pspec)
{
  GstAvfDevice *device;

  device = GST_AVF_DEVICE_CAST (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_avf_device_set_property (GObject * object, guint prop_id,
                             const GValue * value, GParamSpec * pspec)
{
  GstAvfDevice *device;

  device = GST_AVF_DEVICE_CAST (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstDevice *
gst_avf_device_new (AVCaptureDevice * device)
{
  AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
  GstCaps *caps = gst_av_capture_device_get_caps (device, output, GST_AVF_VIDEO_SOURCE_ORIENTATION_DEFAULT);
  GstStructure *props = gst_av_capture_device_get_props (device);
  const gchar *device_name = [[device localizedName] UTF8String];

  GstAvfDevice *gstdev = g_object_new (GST_TYPE_AVF_DEVICE,
                         "display-name", device_name, "caps", caps, "device-class", "Video/Source",
                         "properties", props, NULL);

  gstdev->type = "Video/Source";
  gstdev->element = "avfvideosrc";

  gst_structure_free (props);

  return GST_DEVICE (gstdev);
}
