/* GStreamer
 * Copyright (C) 2016 Hyunjun Ko <zzoon@igalia.com>
 *
 * gstosxaudiodeviceprovider.c: OSX audio device probing and monitoring
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

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include "gstosxaudiosrc.h"
#include "gstosxaudiosink.h"
#include "gstosxaudiodeviceprovider.h"

GST_DEBUG_CATEGORY_STATIC (gst_osxaudio_device_debug);
#define GST_CAT_DEFAULT gst_osxaudio_device_debug

#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_OSX_AUDIO_SRC_CAPS)
    );

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_OSX_AUDIO_SINK_CAPS)
    );

static GstDevice *
gst_osx_audio_device_copy (GstOsxAudioDevice * device, gboolean is_default);

static GstOsxAudioDevice *gst_osx_audio_device_new (AudioDeviceID device_id,
    const gchar * device_name, GstOsxAudioDeviceType type, gboolean is_default,
    GstCoreAudio * core_audio);

G_DEFINE_TYPE (GstOsxAudioDeviceProvider, gst_osx_audio_device_provider,
    GST_TYPE_DEVICE_PROVIDER);
GST_DEVICE_PROVIDER_REGISTER_DEFINE (osxaudiodeviceprovider,
    "osxaudiodeviceprovider", GST_RANK_PRIMARY,
    GST_TYPE_OSX_AUDIO_DEVICE_PROVIDER);

static gboolean gst_osx_audio_device_provider_start (GstDeviceProvider *
    provider);
static void gst_osx_audio_device_provider_stop (GstDeviceProvider * provider);
static GList *gst_osx_audio_device_provider_probe (GstDeviceProvider *
    provider);
static void
gst_osx_audio_device_provider_update_devices (GstOsxAudioDeviceProvider * self);
static void
gst_osx_audio_device_provider_update_default_device (GstOsxAudioDeviceProvider *
    self, GstOsxAudioDeviceType type, AudioDeviceID prev_default_id, AudioDeviceID curr_default_id);

static OSStatus 
_audio_device_get_transport_type (AudioDeviceID device_id, UInt32 * out_transportType)
{
  OSStatus status = noErr;
  UInt32 propertySize = sizeof (UInt32);
  UInt32 transportType = 0;

  AudioObjectPropertyAddress addr = {
    kAudioDevicePropertyTransportType,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };

  status = AudioObjectGetPropertyData (device_id,
      &addr, 0, NULL, &propertySize, &transportType);
  if (status != noErr) {
    GST_WARNING ("failed getting device transport-type property for id: %u status: %d", device_id, (int) status);
    return status;
  }

  *out_transportType = transportType;

  return status;
}

static gboolean
_audio_device_has_scope (AudioDeviceID device_id,
    AudioObjectPropertyScope scope)
{
  OSStatus status = noErr;
  UInt32 propertySize = 0;

  AudioObjectPropertyAddress prop = {
    kAudioDevicePropertyStreams,
    scope,
    kAudioObjectPropertyElementMain
  };

  status = AudioObjectGetPropertyDataSize (device_id,
      &prop, 0, NULL, &propertySize);
  if (status != noErr) {
    GST_WARNING ("failed getting device property: %d", (int) status);
    return FALSE;
  }

  return propertySize > 0;
}

static gboolean
_valid_transport_type (UInt32 transport_type)
{
  switch (transport_type) {
    case kAudioDeviceTransportTypeBuiltIn:
    case kAudioDeviceTransportTypePCI:
    case kAudioDeviceTransportTypeUSB:
    case kAudioDeviceTransportTypeFireWire:
    case kAudioDeviceTransportTypeBluetooth:
    case kAudioDeviceTransportTypeBluetoothLE:
    case kAudioDeviceTransportTypeHDMI:
    case kAudioDeviceTransportTypeDisplayPort:
    case kAudioDeviceTransportTypeAirPlay:
    case kAudioDeviceTransportTypeAVB:
    case kAudioDeviceTransportTypeThunderbolt:
    case kAudioDeviceTransportTypeVirtual:
      return TRUE;
      break;
    case kAudioDeviceTransportTypeUnknown:
    case kAudioDeviceTransportTypeAggregate:
    default:
      return FALSE;
      break;
  }
}

static AudioDeviceID *
_audio_system_get_devices (gint *ndevices)
{
  OSStatus status = noErr;
  UInt32 propertySize = 0;
  AudioDeviceID *devices = NULL;

  AudioObjectPropertyAddress prop = {
    kAudioHardwarePropertyDevices,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };

  status = AudioObjectGetPropertyDataSize (kAudioObjectSystemObject,
      &prop, 0, NULL, &propertySize);
  if (status != noErr) {
    GST_WARNING ("failed getting number of devices: %d", (int) status);
    return NULL;
  }

  devices = (AudioDeviceID *) g_malloc (propertySize);
  if (!devices)
    return NULL;

  *ndevices = propertySize / sizeof (AudioDeviceID);
  status = AudioObjectGetPropertyData (kAudioObjectSystemObject,
      &prop, 0, NULL, &propertySize, devices);
  if (status != noErr) {
    GST_WARNING ("failed getting the list of devices: %d", (int) status);
    g_free (devices);
    *ndevices = 0;
    return NULL;
  }

  return devices;
}

static AudioDeviceID
_audio_system_get_default_device (AudioObjectPropertySelector selector)
{
  OSStatus status = noErr;
  AudioDeviceID deviceID = 0;
  AudioObjectPropertyAddress propertyAddress;
  UInt32 propertySize;

  //sets which property to check
  propertyAddress.mSelector = selector;
  propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
  propertyAddress.mElement = kAudioObjectPropertyElementMain;
  propertySize = sizeof (AudioDeviceID);

  //gets property (system output device)
  status = AudioObjectGetPropertyData (kAudioObjectSystemObject,
      &propertyAddress, 0, NULL, &propertySize, &deviceID);
  if (status != noErr) {
    GST_WARNING ("failed getting default device: %d", (int) status);
    return FALSE;
  }

  return deviceID;
}

/* MARKED (child) */
static AudioDeviceID
_audio_system_get_default_input ()
{
  return _audio_system_get_default_device (kAudioHardwarePropertyDefaultInputDevice);
}

/* MARKED (child) */
static AudioDeviceID
_audio_system_get_default_output ()
{
  return _audio_system_get_default_device (kAudioHardwarePropertyDefaultOutputDevice);
}

static inline gboolean
_audio_system_set_runloop (CFRunLoopRef runLoop)
{
  OSStatus status = noErr;

  gboolean res = FALSE;

  AudioObjectPropertyAddress runloopAddress = {
    kAudioHardwarePropertyRunLoop,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };

  status = AudioObjectSetPropertyData (kAudioObjectSystemObject,
      &runloopAddress, 0, NULL, sizeof (CFRunLoopRef), &runLoop);
  if (status == noErr) {
    res = TRUE;
  } else {
    GST_ERROR ("failed to set runloop to %p: %d", runLoop, (int) status);
  }

  return res;
}

static const gchar*
_device_type_to_string (GstOsxAudioDeviceType type)
{
  switch (type)
  {
    case GST_OSX_AUDIO_DEVICE_TYPE_SOURCE:
      return "input";
    case GST_OSX_AUDIO_DEVICE_TYPE_SINK:
      return "output";
    default:
      return "unknown";
      break;
  }
}

static void
gst_osx_audio_device_provider_finalize (GObject *object)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (object);

  if (self->device_id_map)
    g_hash_table_destroy (self->device_id_map);
  self->device_id_map = NULL;

  g_mutex_clear (&self->mutex);
  g_cond_clear (&self->cond);

  G_OBJECT_CLASS (gst_osx_audio_device_provider_parent_class)->finalize
      (object);
}

static void
gst_osx_audio_device_provider_class_init (GstOsxAudioDeviceProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);

  dm_class->probe = gst_osx_audio_device_provider_probe;
  dm_class->start = gst_osx_audio_device_provider_start;
  dm_class->stop = gst_osx_audio_device_provider_stop;

  object_class->finalize = gst_osx_audio_device_provider_finalize;

  gst_device_provider_class_set_static_metadata (dm_class,
      "OSX Audio Device Provider", "Source/Sink/Audio",
      "List and monitor OSX audio source and sink devices",
      "Hyunjun Ko <zzoon@igalia.com>, Tulio Beloqui <tulio@pexip.com>");

  GST_DEBUG_CATEGORY_INIT (gst_osxaudio_device_debug,
      "osxaudiodeviceprovider", 0, "OSX Audio Device Provider");

  _audio_system_set_runloop (NULL);
}

static void
gst_osx_audio_device_provider_init (GstOsxAudioDeviceProvider *self)
{
  g_mutex_init (&self->mutex);
  g_cond_init (&self->cond);
  self->current_default_input = kAudioDeviceUnknown;
  self->current_default_output = kAudioDeviceUnknown;
}

static void
gst_osx_audio_device_provider_populate_devices (GstDeviceProvider *provider)
{
  GList *devices, *it;

  devices = gst_osx_audio_device_provider_probe (provider);

  for (it = devices; it != NULL; it = g_list_next (it)) {
    GstDevice *device = GST_DEVICE_CAST (it->data);
    if (device)
      gst_device_provider_device_add (provider, device);
  }

  g_list_free_full (devices, gst_object_unref);
}

static void
_gst_osx_audio_device_provider_update (GstOsxAudioDeviceProvider * self)
{
  gboolean update_devices = self->update_devices;
  gboolean input_changed = FALSE;
  gboolean output_changed = FALSE;

  AudioDeviceID prev_default_input = self->current_default_input;
  AudioDeviceID prev_default_output = self->current_default_output;
  AudioDeviceID curr_default_input = _audio_system_get_default_input ();
  AudioDeviceID curr_default_output = _audio_system_get_default_output ();

  if (curr_default_input != self->current_default_input) {
    self->current_default_input = curr_default_input;
    input_changed = TRUE;
  }

  if (curr_default_output != self->current_default_output) {
    self->current_default_output = curr_default_output;
    output_changed = TRUE;
  }

  self->update_devices = FALSE;

  if (update_devices)
    gst_osx_audio_device_provider_update_devices (self);

  if (input_changed)
    gst_osx_audio_device_provider_update_default_device (self,
      GST_OSX_AUDIO_DEVICE_TYPE_SOURCE, prev_default_input, curr_default_input);

  if (output_changed)
    gst_osx_audio_device_provider_update_default_device (self,
      GST_OSX_AUDIO_DEVICE_TYPE_SINK, prev_default_output, curr_default_output);
}

static void
gst_osx_audio_device_provider_update (GstOsxAudioDeviceProvider * self)
{
  dispatch_queue_t coreAudioQueue = gst_macos_get_core_audio_dispatch_queue();
  dispatch_sync (coreAudioQueue, ^{
    _gst_osx_audio_device_provider_update (self);
  });
}

/* MARKED (parent) */
static gpointer
gst_osx_audio_device_provider_listener (gpointer data)
{
  GstOsxAudioDeviceProvider * self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (data);

  g_mutex_lock (&self->mutex);

  while (self->running) {
    gboolean wait = self->running && !self->update_devices;
    if (wait) {
      g_cond_wait (&self->cond, &self->mutex);
    }
    if (!self->running) {
      break;
    }

    g_mutex_unlock (&self->mutex);

    gst_osx_audio_device_provider_update (self);

    g_mutex_lock (&self->mutex);

  }

  g_mutex_unlock (&self->mutex);

  return NULL;
}

static OSStatus
gst_osx_audio_device_change_block (GstOsxAudioDeviceProvider * self,
    guint32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses)
{
  g_mutex_lock (&self->mutex);
  for (guint32 i = 0; i < inNumberAddresses; i++) {
    switch (inAddresses[i].mSelector) {
      case kAudioHardwarePropertyDevices:
      case kAudioHardwarePropertyDefaultOutputDevice:
      case kAudioHardwarePropertyDefaultInputDevice:
      {
        self->update_devices = TRUE;
        g_cond_signal (&self->cond);
        break;
      }
      default:
        break;
    }
  }
  g_mutex_unlock (&self->mutex);

  return noErr;
}

static gboolean
gst_osx_audio_device_provider_add_remove_listeners (GstDeviceProvider * provider, gboolean add)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);
  dispatch_queue_t inDispatchQueue = gst_macos_get_core_audio_dispatch_queue ();
  AudioObjectPropertyAddress deviceListAddr = {
    .mSelector = kAudioObjectPropertySelectorWildcard,
    .mScope = kAudioObjectPropertyScopeGlobal,
    .mElement = kAudioObjectPropertyElementMain
  };

  OSStatus status;

  if (add) {
    status = AudioObjectAddPropertyListenerBlock (kAudioObjectSystemObject,
      &deviceListAddr,
      inDispatchQueue,
      self->listenerBlock);
  } else {
    status = AudioObjectRemovePropertyListenerBlock (kAudioObjectSystemObject,
      &deviceListAddr,
      inDispatchQueue,
      self->listenerBlock);
  }

  if (status != noErr) {
    GST_ERROR ("Failed to register AudioObjectAddPropertyListener %d", status);
    return FALSE;
  }

  return TRUE;
}

static gboolean 
gst_osx_audio_device_provider_register_listeners (GstDeviceProvider * provider)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);
  self->listenerBlock = Block_copy(^(UInt32 inNumberAddresses, const AudioObjectPropertyAddress inAddresses[]) {
      gst_osx_audio_device_change_block (self, inNumberAddresses, inAddresses);  
  });
  return gst_osx_audio_device_provider_add_remove_listeners (provider, TRUE);
}

static void
gst_osx_audio_device_provider_unregister_listeners (GstDeviceProvider * provider)
{
  gst_osx_audio_device_provider_add_remove_listeners (provider, FALSE);
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);
  Block_release (self->listenerBlock);
  self->listenerBlock = NULL;
}

static gboolean
gst_osx_audio_device_provider_start (GstDeviceProvider *provider)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);

  GST_INFO_OBJECT (provider, "Starting...");

  if (!gst_osx_audio_device_provider_register_listeners (provider)) {
    /* cleanup if there was any event registered... */
    gst_osx_audio_device_provider_unregister_listeners (provider);
    return FALSE;
  }

  g_mutex_lock (&self->mutex);
  gst_osx_audio_device_provider_populate_devices (provider);
  
  self->running = TRUE;
  self->update_devices = FALSE;
  self->thread = g_thread_new ("osx-audio-device-provider", gst_osx_audio_device_provider_listener, self);
  g_mutex_unlock (&self->mutex);

  return TRUE;
}

static void
gst_osx_audio_device_provider_stop (GstDeviceProvider *provider)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);

  GST_INFO_OBJECT (provider, "Stopping...");

  g_mutex_lock (&self->mutex);
  gst_osx_audio_device_provider_unregister_listeners (provider);

  self->running = FALSE;
  self->update_devices = FALSE;

  g_cond_signal (&self->cond);
  g_mutex_unlock (&self->mutex);

  g_thread_join (self->thread);
  self->thread = NULL;
}

static AudioObjectPropertyScope
_audio_scope_from_type (GstOsxAudioDeviceType type)
{
  if (type == GST_OSX_AUDIO_DEVICE_TYPE_SOURCE)
    return kAudioDevicePropertyScopeInput;
  return kAudioDevicePropertyScopeOutput;
}

/* MARKED (child) */
static GstOsxAudioDevice *
gst_osx_audio_device_provider_probe_device (GstOsxAudioDeviceProvider *self, AudioDeviceID device_id, const gchar * device_name,
    GstOsxAudioDeviceType type)
{
  GstOsxAudioDevice *device = NULL;
  GstCoreAudio *core_audio = NULL;
  AudioObjectPropertyScope scope = _audio_scope_from_type (type);
  gboolean is_default;

  if (!_audio_device_has_scope (device_id, scope)) {
    GST_LOG ("Device id: %u is missing the %s scope!", device_id, _device_type_to_string (type));
    goto done;
  }

  /* don't open audiounits as part of probing */
  if (0) {
    core_audio = g_object_new (GST_TYPE_CORE_AUDIO,
      "is-src", (scope == kAudioDevicePropertyScopeInput), "device", device_id, NULL);

    if (!gst_core_audio_select_device (core_audio)) {
      GST_ERROR ("CoreAudio device coult not be selected");
      goto done;
    }

    if (!gst_core_audio_open (core_audio)) {
      GST_ERROR ("CoreAudio device could not be opened");
      goto done;
    }
  }

  if (scope == kAudioDevicePropertyScopeInput)
    is_default = (device_id == self->current_default_input);
  else
    is_default = (device_id == self->current_default_output);

  device = gst_osx_audio_device_new (device_id, device_name, type, is_default, core_audio);
  gst_object_ref_sink (device);
  
  if (G_UNLIKELY (GST_LEVEL_INFO <= _gst_debug_min) &&
        GST_LEVEL_INFO <= gst_debug_category_get_threshold (GST_CAT_DEFAULT)) {
      GST_INFO ("Created %s Device ID: %u Name: %s", _device_type_to_string (type), device_id, device_name);
  }

  if (core_audio)
    gst_core_audio_close (core_audio);

done:
  if (core_audio)
    g_object_unref (core_audio);

  return device;
}

/* MARKED (child) */
static GHashTable *
gst_osx_audio_device_provider_create_device_map ()
{
  GHashTable *map;
  gint ndevices;
  AudioDeviceID *device_ids;
  gint i;

  map = g_hash_table_new (NULL, NULL);

  ndevices = 0;
  device_ids = _audio_system_get_devices (&ndevices);
  GST_INFO ("found %d audio device(s)", ndevices);

  if (!device_ids || ndevices == 0)
    return map;

  for (i = 0; i < ndevices; i++) {
    AudioDeviceID id = device_ids[i];
    UInt32 transport_type;

    if (_audio_device_get_transport_type (id, &transport_type) != noErr)
      continue;

    if (!_valid_transport_type (transport_type)) {
      GST_DEBUG ("Skipping device id: %u invalid transport type from list", id);
      continue;
    }

    if (!g_hash_table_insert (map, GUINT_TO_POINTER (id),
            GUINT_TO_POINTER (id))) {
      GST_ERROR ("Device UUID already present in the map, invalid state! %u",
          id);
    }
  }

  g_free (device_ids);

  return map;
}

/* MARKED (child) */
static void
gst_osx_audio_device_provider_probe_internal (GstOsxAudioDeviceProvider * self, GHashTable *device_id_map, GstOsxAudioDeviceType type, GList ** devices)
{
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, device_id_map);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    AudioDeviceID device_id = GPOINTER_TO_INT (key);
    char* device_name = gst_core_audio_device_get_prop (device_id, kAudioObjectPropertyName);
    GstOsxAudioDevice *device = NULL;

    if (!device_name) {
      GST_DEBUG ("could not get device name for device id: %u", device_id);
      continue;
    }

    device = gst_osx_audio_device_provider_probe_device (self, device_id, device_name, type);
    g_free (device_name);

    if (!device) {
      GST_DEBUG ("Device id %u cannot be open", device_id);
      continue;
    }

    *devices = g_list_prepend (*devices, device);
  }
}

/* MARKED (parent) */
static GList *
_gst_osx_audio_device_provider_probe (GstDeviceProvider *provider)
{
  GstOsxAudioDeviceProvider *self = GST_OSX_AUDIO_DEVICE_PROVIDER_CAST (provider);
  GList *devices = NULL;

  if (self->device_id_map)
    g_hash_table_destroy (self->device_id_map);
  self->device_id_map = gst_osx_audio_device_provider_create_device_map ();

  self->current_default_input = _audio_system_get_default_input ();
  self->current_default_output = _audio_system_get_default_output ();

  gst_osx_audio_device_provider_probe_internal (self, self->device_id_map,
      GST_OSX_AUDIO_DEVICE_TYPE_SOURCE, &devices);
  gst_osx_audio_device_provider_probe_internal (self, self->device_id_map,
      GST_OSX_AUDIO_DEVICE_TYPE_SINK, &devices);

  return devices;
}

static GList *
gst_osx_audio_device_provider_probe (GstDeviceProvider *provider)
{
  __block GList *ret;
  dispatch_queue_t coreAudioQueue = gst_macos_get_core_audio_dispatch_queue();
  dispatch_sync (coreAudioQueue, ^{
    ret = _gst_osx_audio_device_provider_probe (provider);
  });
  return ret;
}

static GstDevice *
gst_osx_audio_device_provider_find_device_by_id (GstDeviceProvider *provider,
    AudioDeviceID device_id, const gchar *filter_class)
{
  GstDevice *device = NULL;
  gboolean found = FALSE;
  GList *it;

  GST_OBJECT_LOCK (provider);
  for (it = provider->devices; it != NULL; it = it->next) {
    device = GST_DEVICE_CAST (it->data);
    gint id = -1;

    if (filter_class != NULL && !gst_device_has_classes (device, filter_class)) {
      continue;
    }

    g_object_get (device, "device-id", &id, NULL);
    if (device_id != -1 && device_id == (AudioDeviceID) id) {
      found = TRUE;
      break;
    }
  }
  GST_OBJECT_UNLOCK (provider);

  if (found)
    return device;
  return NULL;
}

static void
gst_osx_audio_device_provider_remove_device_by_id (GstDeviceProvider *provider,
    AudioDeviceID device_id)
{
  GstDevice *device;

  /* AudioDeviceID can be the same for multiple Gst devices, so make sure we remove all of them */
  while ((device =
          gst_osx_audio_device_provider_find_device_by_id (provider,
              device_id, NULL)) != NULL) {

    if (G_UNLIKELY (GST_LEVEL_INFO <= _gst_debug_min) &&
        GST_LEVEL_INFO <= gst_debug_category_get_threshold (GST_CAT_DEFAULT)) {
      gchar *device_name = gst_device_get_display_name (device);
      GST_INFO ("Removing Device ID: %u Name: %s", device_id, device_name);
      g_free (device_name);
    }

    gst_device_provider_device_remove (provider, device);
  }
}

static void
gst_osx_audio_device_provider_add (GstDeviceProvider * provider, AudioDeviceID device_id, GstDevice * device, GstOsxAudioDeviceType type)
{
  if (G_UNLIKELY (GST_LEVEL_INFO <= _gst_debug_min) &&
      GST_LEVEL_INFO <= gst_debug_category_get_threshold (GST_CAT_DEFAULT)) {

    gchar *device_name = gst_device_get_display_name (device);
    GST_INFO ("Adding %s device ID: %u Name: %s", _device_type_to_string (type), device_id, device_name);
    g_free (device_name);
  }

  gst_device_provider_device_add (provider, device);
}

/* MARKED (child) */
/* CALL with GstOsxAudioDeviceProvider mutex acquired */
static void
gst_osx_audio_device_provider_update_devices (GstOsxAudioDeviceProvider *self)
{
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER_CAST (self);
  GHashTable *current_devices_map;
  GHashTable *new_devices_map;

  GHashTableIter iter;
  gpointer key;

  new_devices_map = gst_osx_audio_device_provider_create_device_map ();
  current_devices_map = self->device_id_map;
  g_assert (current_devices_map);

  if (g_hash_table_size (new_devices_map) == 0) {
    /* core audio could be failing if we dont get *any* valid device... */
    g_hash_table_destroy (new_devices_map);
    return;
  }

  /* iterate over our current devices map */
  g_hash_table_iter_init (&iter, current_devices_map);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    AudioDeviceID device_id;
    gboolean to_remove = !g_hash_table_contains (new_devices_map, key);

    if (!to_remove) {
      /* skip if we *still* have the device in the new map */
      continue;
    }

    device_id = GPOINTER_TO_UINT (key);
    gst_osx_audio_device_provider_remove_device_by_id (provider, device_id);
  }

  /* iterate over the *new* devices map, we only look for devices to ADD */
  g_hash_table_iter_init (&iter, new_devices_map);
  while (g_hash_table_iter_next (&iter, &key, NULL)) {
    AudioDeviceID device_id;
    UInt32 transport_type;
    GstOsxAudioDevice *device;
    gboolean to_add = !g_hash_table_contains (current_devices_map, key);
    char* device_name = NULL;

    if (!to_add) {
      /* skip if we already have the device in our map */
      continue;
    }

    device_id = GPOINTER_TO_UINT (key);
    if (_audio_device_get_transport_type (device_id, &transport_type) != noErr)
      continue;
    if (!_valid_transport_type (transport_type)) {
      GST_DEBUG ("Skipping device id: %u invalid transport type from list",
          device_id);
      continue;
    }

    device_name = gst_core_audio_device_get_prop (device_id, kAudioObjectPropertyName);

    device =
        gst_osx_audio_device_provider_probe_device (self, device_id, device_name, GST_OSX_AUDIO_DEVICE_TYPE_SOURCE);
    if (device) {
      gst_osx_audio_device_provider_add (provider, device_id, GST_DEVICE_CAST (device), GST_OSX_AUDIO_DEVICE_TYPE_SOURCE);
    }

    device =
        gst_osx_audio_device_provider_probe_device (self, device_id, device_name, GST_OSX_AUDIO_DEVICE_TYPE_SINK);
    if (device) {
      gst_osx_audio_device_provider_add (provider, device_id, GST_DEVICE_CAST (device), GST_OSX_AUDIO_DEVICE_TYPE_SINK);
    }
  }

  /* switch to the new map */
  g_hash_table_destroy (self->device_id_map);
  self->device_id_map = new_devices_map;
}

static void
gst_osx_audio_device_provider_update_device (GstDeviceProvider * provider, GstOsxAudioDeviceType type, AudioDeviceID device_id, gboolean is_default)
{
  const gchar *device_class = type == GST_OSX_AUDIO_DEVICE_TYPE_SOURCE ? "Audio/Source" : "Audio/Sink";
  GstDevice *changed_device; 
  GstDevice *device;

  /*
  The AudioDeviceID is allowed to be different each time you re-plug the same
  device, so we might not have it in the internal list by the time it was
  removed and reported as changed as system-default.
  */
  changed_device = gst_osx_audio_device_provider_find_device_by_id (provider, device_id, device_class);
  if (!changed_device) {
    GST_DEBUG ("device id: %u marked to change but we don't have it in our list!", device_id);
    return;
  }

  /* create a copy of this device but with the new is_default value */
  device = gst_osx_audio_device_copy (GST_OSX_AUDIO_DEVICE_CAST (changed_device), is_default);
  gst_device_provider_device_changed (provider, device, changed_device);
}

/* MARKED (child) */
static void
gst_osx_audio_device_provider_update_default_device (GstOsxAudioDeviceProvider
    *self, GstOsxAudioDeviceType type, AudioDeviceID prev_default_id, AudioDeviceID curr_default_id)
{
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER_CAST (self);

  if (G_UNLIKELY (GST_LEVEL_INFO <= _gst_debug_min) &&
      GST_LEVEL_INFO <= gst_debug_category_get_threshold (GST_CAT_DEFAULT)) {
    gchar *name = gst_core_audio_device_get_prop (curr_default_id, kAudioObjectPropertyName);
    gchar *prev_name = gst_core_audio_device_get_prop (prev_default_id, kAudioObjectPropertyName);
    GST_INFO ("default %s changed from id: %d name: %s to id: %d name: %s",
      _device_type_to_string (type), prev_default_id, prev_name, curr_default_id, name);
    g_free (name);
    g_free (prev_name);
  }

  gst_osx_audio_device_provider_update_device (provider, type, prev_default_id, FALSE);
  gst_osx_audio_device_provider_update_device (provider, type, curr_default_id, TRUE);
}

enum
{
  PROP_DEVICE_ID = 1,
};

G_DEFINE_TYPE (GstOsxAudioDevice, gst_osx_audio_device, GST_TYPE_DEVICE);

static void gst_osx_audio_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_osx_audio_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static GstElement *gst_osx_audio_device_create_element (GstDevice * device,
    const gchar * name);

static void
gst_osx_audio_device_class_init (GstOsxAudioDeviceClass *klass)
{
  GstDeviceClass *dev_class = GST_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  dev_class->create_element = gst_osx_audio_device_create_element;

  object_class->get_property = gst_osx_audio_device_get_property;
  object_class->set_property = gst_osx_audio_device_set_property;

  g_object_class_install_property (object_class, PROP_DEVICE_ID,
      g_param_spec_int ("device-id", "Device ID", "Device ID of input device",
          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_osx_audio_device_init (GstOsxAudioDevice *device)
{
}

static GstElement *
gst_osx_audio_device_create_element (GstDevice *device, const gchar *name)
{
  GstOsxAudioDevice *osxdev = GST_OSX_AUDIO_DEVICE (device);
  GstElement *elem;

  elem = gst_element_factory_make (osxdev->element, name);
  g_object_set (elem, "device", osxdev->device_id, NULL);

  return elem;
}

static GstDevice *
gst_osx_audio_device_copy (GstOsxAudioDevice * device, gboolean is_default)
{
  gchar * display_name = gst_device_get_display_name (GST_DEVICE_CAST (device));
  gchar * device_class = gst_device_get_device_class (GST_DEVICE_CAST (device));
  GstCaps * caps = gst_device_get_caps (GST_DEVICE_CAST (device));
  GstStructure * props = gst_device_get_properties (GST_DEVICE_CAST (device));

  gst_structure_set (props,
    "is-default", G_TYPE_BOOLEAN, is_default, NULL);

  GstOsxAudioDevice *copy = g_object_new (GST_TYPE_OSX_AUDIO_DEVICE, "device-id",
      device->device_id, "display-name", display_name, "caps", caps,
      "device-class", device_class, "properties", props, NULL);

  copy->element = device->element;

  g_free (display_name);
  g_free (device_class);
  gst_caps_unref (caps);
  gst_structure_free (props);

  return GST_DEVICE_CAST (copy);
}

/* MARKED (child) */
static GstOsxAudioDevice *
gst_osx_audio_device_new (AudioDeviceID device_id, const gchar * device_name,
    GstOsxAudioDeviceType type, gboolean is_default, GstCoreAudio * core_audio)
{
  GstOsxAudioDevice *gstdev;
  const gchar *element_name = NULL;
  const gchar *klass = NULL;
  GstCaps *template_caps, *caps;
  gchar * uuid = NULL;
  GstStructure *props = gst_structure_new_empty ("properties");

  g_return_val_if_fail (device_id > 0, NULL);
  g_return_val_if_fail (device_name, NULL);

  switch (type) {
    case GST_OSX_AUDIO_DEVICE_TYPE_SOURCE:
      element_name = "osxaudiosrc";
      klass = "Audio/Source";
      template_caps = gst_static_pad_template_get_caps (&src_factory);
      break;
    case GST_OSX_AUDIO_DEVICE_TYPE_SINK:
      element_name = "osxaudiosink";
      klass = "Audio/Sink";
      template_caps = gst_static_pad_template_get_caps (&sink_factory);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (core_audio) {
    caps = gst_core_audio_probe_caps (core_audio, template_caps);
    is_default = core_audio->is_default;
    if (core_audio->unique_id != NULL)
      uuid = g_strdup (core_audio->unique_id);
  } else {
    caps = gst_caps_ref (template_caps);
  }
  gst_caps_unref (template_caps);

  if (!uuid)
    uuid = gst_core_audio_device_get_prop (device_id, kAudioDevicePropertyDeviceUID);

  gst_structure_set (props,
    "is-default", G_TYPE_BOOLEAN, is_default,
    "unique-id", G_TYPE_STRING, uuid, NULL);

  gstdev = g_object_new (GST_TYPE_OSX_AUDIO_DEVICE, "device-id",
      device_id, "display-name", device_name, "caps", caps,
      "properties", props, "device-class", klass, NULL);

  gstdev->element = element_name;

  gst_structure_free (props);
  g_free (uuid);

  return gstdev;
}


static void
gst_osx_audio_device_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  GstOsxAudioDevice *device;

  device = GST_OSX_AUDIO_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      g_value_set_int (value, device->device_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_osx_audio_device_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  GstOsxAudioDevice *device;

  device = GST_OSX_AUDIO_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      device->device_id = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}