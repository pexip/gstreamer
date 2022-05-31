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

#include "gstamdeviceprovider.h"

#include "gstjniutils.h"

struct _GstAmDeviceProvider
{
  GstDeviceProvider parent;

  jobject audioDeviceCB;
};

struct _GstAmDevice
{
  GstDevice parent;

  gint device_id;
};

enum
{
  ADDED_DEVICES,
  REMOVED_DEVICES,
};

G_DEFINE_TYPE (GstAmDeviceProvider, gst_am_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

G_DEFINE_TYPE (GstAmDevice, gst_am_device, GST_TYPE_DEVICE);

GST_DEVICE_PROVIDER_REGISTER_DEFINE (amdeviceprovider, "amdeviceprovider",
    GST_RANK_PRIMARY, GST_TYPE_AM_DEVICE_PROVIDER);

GST_DEBUG_CATEGORY_STATIC (gst_am_device_provider_debug);
#define GST_CAT_DEFAULT (gst_am_device_provider_debug)
#define device_provider_parent_class gst_am_device_provider_parent_class

#define GET_DEVICES_ALL 0x00000003

/* from: https://developer.android.com/reference/android/media/AudioDeviceInfo */
typedef enum
{
  TYPE_AUX_LINE = 19,
  TYPE_BLE_BROADCAST = 30,
  TYPE_BLE_HEADSET = 26,
  TYPE_BLE_SPEAKER = 27,
  TYPE_BLUETOOTH_A2DP = 8,
  TYPE_BLUETOOTH_SCO = 7,
  TYPE_BUILTIN_EARPIECE = 1,
  TYPE_BUILTIN_MIC = 15,
  TYPE_BUILTIN_SPEAKER = 2,
  TYPE_BUILTIN_SPEAKER_SAFE = 24,
  TYPE_BUS = 21,
  TYPE_DOCK = 13,
  TYPE_FM = 14,
  TYPE_FM_TUNER = 16,
  TYPE_HDMI = 9,
  TYPE_HDMI_ARC = 10,
  TYPE_HDMI_EARC = 29,
  TYPE_HEARING_AID = 23,
  TYPE_IP = 20,
  TYPE_LINE_ANALOG = 5,
  TYPE_LINE_DIGITAL = 6,
  TYPE_REMOTE_SUBMIX = 25,
  TYPE_TELEPHONY = 18,
  TYPE_TV_TUNER = 17,
  TYPE_UNKNOWN = 0,
  TYPE_USB_ACCESSORY = 12,
  TYPE_USB_DEVICE = 11,
  TYPE_USB_HEADSET = 22,
  TYPE_WIRED_HEADPHONES = 4,
  TYPE_WIRED_HEADSET = 3,
} AudioInfo_DeviceType;

static jobject
jni_get_global_context (JNIEnv * env)
{
  jclass activityThread;
  jmethodID currentActivityThread;
  jobject activity;
  jmethodID getApplication;
  jobject ctx;

  activityThread = (*env)->FindClass (env, "android/app/ActivityThread");
  currentActivityThread =
      (*env)->GetStaticMethodID (env, activityThread, "currentActivityThread",
      "()Landroid/app/ActivityThread;");
  activity = (*env)->CallStaticObjectMethod (env, activityThread,
      currentActivityThread);
  getApplication =
      (*env)->GetMethodID (env, activityThread, "getApplication",
      "()Landroid/app/Application;");

  ctx = (*env)->CallObjectMethod (env, activity, getApplication);
  return ctx;
}

static jobject
jni_get_AudioManager_Instance (JNIEnv * env, jobject context)
{
  jclass contextClass;
  jfieldID audioServiceField;
  jstring audioService;
  jmethodID getSystemServiceID;
  jobject audioManager;

  contextClass = (*env)->FindClass (env, "android/content/Context");
  audioServiceField =
      (*env)->GetStaticFieldID (env, contextClass, "AUDIO_SERVICE",
      "Ljava/lang/String;");
  audioService =
      (jstring) (*env)->GetStaticObjectField (env, contextClass,
      audioServiceField);
  getSystemServiceID =
      (*env)->GetMethodID (env, contextClass, "getSystemService",
      "(Ljava/lang/String;)Ljava/lang/Object;");

  audioManager = (*env)->CallObjectMethod (env, context,
      getSystemServiceID, audioService);

  g_assert (audioManager != NULL);

  return audioManager;
}

static gint
jni_AudioInfo_getId (JNIEnv * env, jobject audioInfo)
{
  jclass audioInfoClass;
  jmethodID getId;

  audioInfoClass = (*env)->FindClass (env, "android/media/AudioDeviceInfo");
  getId = (*env)->GetMethodID (env, audioInfoClass, "getId", "()I");

  return (gint) (*env)->CallIntMethod (env, audioInfo, getId);
}

static gboolean
jni_AudioInfo_isSink (JNIEnv * env, jobject audioInfo)
{
  jclass audioInfoClass;
  jmethodID isSink;

  audioInfoClass = (*env)->FindClass (env, "android/media/AudioDeviceInfo");
  isSink = (*env)->GetMethodID (env, audioInfoClass, "isSink", "()Z");

  return (gint) (*env)->CallBooleanMethod (env, audioInfo, isSink);
}

static gboolean
jni_AudioInfo_isSource (JNIEnv * env, jobject audioInfo)
{
  jclass audioInfoClass;
  jmethodID isSource;

  audioInfoClass = (*env)->FindClass (env, "android/media/AudioDeviceInfo");
  isSource = (*env)->GetMethodID (env, audioInfoClass, "isSource", "()Z");

  return (gint) (*env)->CallBooleanMethod (env, audioInfo, isSource);
}

static gchar *
jni_String_GetStringUTFChars (JNIEnv * env, jstring str)
{
  gchar *value = NULL;
  const char *utf;

  utf = (*env)->GetStringUTFChars (env, str, NULL);
  if (utf) {
    value = g_strdup (utf);
    (*env)->ReleaseStringUTFChars (env, str, utf);
  }

  return value;
}

static jstring
jni_Object_toString (JNIEnv * env, jobject obj)
{
  jclass objClass;
  jmethodID toString;

  objClass = (*env)->FindClass (env, "java/lang/Object");
  toString =
      (*env)->GetMethodID (env, objClass, "toString", "()Ljava/lang/String;");

  return (*env)->CallObjectMethod (env, obj, toString);
}

static gchar *
jni_AudioInfo_getProductName (JNIEnv * env, jobject audioInfo)
{
  jclass audioInfoClass;

  jmethodID getProductName;

  jobject productNameCharSeq;
  jobject productNameString;

  audioInfoClass = (*env)->FindClass (env, "android/media/AudioDeviceInfo");

  getProductName =
      (*env)->GetMethodID (env, audioInfoClass, "getProductName",
      "()Ljava/lang/CharSequence;");

  productNameCharSeq =
      (*env)->CallObjectMethod (env, audioInfo, getProductName);
  productNameString = jni_Object_toString (env, productNameCharSeq);

  return jni_String_GetStringUTFChars (env, productNameString);
}

static void
gst_am_device_provider_remove_device (GstAmDeviceProvider * amprovider,
    JNIEnv * env, jobject audioInfo)
{
  GstDeviceProvider *provider = GST_DEVICE_PROVIDER_CAST (amprovider);
  GstDevice *device = NULL;
  GList *item;
  gint id_to_remove;

  id_to_remove = jni_AudioInfo_getId (env, audioInfo);

  GST_OBJECT_LOCK (provider);
  for (item = provider->devices; item; item = item->next) {
    device = item->data;
    gint device_id;

    g_object_get (device, "device-id", &device_id, NULL);
    if (id_to_remove == device_id) {
      gst_object_ref (device);
      break;
    }

    device = NULL;
  }
  GST_OBJECT_UNLOCK (provider);

  if (device) {
    GST_DEBUG_OBJECT (provider, "Removing %" GST_PTR_FORMAT, device);
    gst_device_provider_device_remove (provider, GST_DEVICE (device));
    g_object_unref (device);
  }
}

static gchar *
jni_get_AudioInfo_DeviceType_Name (AudioInfo_DeviceType deviceType)
{
  switch (deviceType) {
    case TYPE_BLE_BROADCAST:
    case TYPE_BLE_HEADSET:
    case TYPE_BLE_SPEAKER:
    case TYPE_BLUETOOTH_A2DP:
    case TYPE_BLUETOOTH_SCO:
      return g_strdup ("Bluetooth");

    case TYPE_BUILTIN_EARPIECE:
    case TYPE_BUILTIN_MIC:
    case TYPE_BUILTIN_SPEAKER:
    case TYPE_BUILTIN_SPEAKER_SAFE:
      return g_strdup ("Built-in");

    case TYPE_HDMI:
    case TYPE_HDMI_ARC:
    case TYPE_HDMI_EARC:
      return g_strdup ("HDMI");

    case TYPE_USB_ACCESSORY:
    case TYPE_USB_DEVICE:
    case TYPE_USB_HEADSET:
      return g_strdup ("USB");

    case TYPE_WIRED_HEADPHONES:
    case TYPE_WIRED_HEADSET:
      return g_strdup ("Wired");

    default:
      break;
  }

  return NULL;
}

static AudioInfo_DeviceType
jni_AudioInfo_getDeviceType (JNIEnv * env, jobject audioInfo)
{
  jclass audioInfoClass;
  jmethodID getType;

  audioInfoClass = (*env)->FindClass (env, "android/media/AudioDeviceInfo");
  getType = (*env)->GetMethodID (env, audioInfoClass, "getType", "()I");

  return (AudioInfo_DeviceType) (*env)->CallIntMethod (env, audioInfo, getType);
}


static GstDevice *
gst_am_device_from_audio_info (JNIEnv * env, jobject audioInfo)
{
  gint id;
  gchar *product_name;
  gchar *display_name;
  gchar *type_name;
  gboolean is_sink;
  gboolean is_source;
  AudioInfo_DeviceType deviceType;
  GstDevice *device;
  GstCaps *caps;
  const gchar *device_class = NULL;

  is_sink = jni_AudioInfo_isSink (env, audioInfo);
  is_source = jni_AudioInfo_isSource (env, audioInfo);

  if (!is_sink && !is_source)
    return NULL;

  deviceType = jni_AudioInfo_getDeviceType (env, audioInfo);
  type_name = jni_get_AudioInfo_DeviceType_Name (deviceType);

  /* filter the enumerated devices by supported type */
  if (!type_name)
    return NULL;

  id = jni_AudioInfo_getId (env, audioInfo);
  product_name = jni_AudioInfo_getProductName (env, audioInfo);
  display_name = g_strdup_printf ("%s (%s)", product_name, type_name);
  // FIXME: call getAudioProfiles() on the audioInfo and transform to caps
  caps = gst_caps_new_empty_simple ("audio/x-raw");

  g_free (type_name);
  g_free (product_name);

  if (is_source)
    device_class = "Audio/Source";
  if (is_sink)
    device_class = "Audio/Sink";

  device = g_object_new (GST_TYPE_AM_DEVICE,    //
      "device-id", id,          //
      "device-class", device_class,     //
      "display-name", display_name,     //
      "caps", caps,             //
      NULL);

  return GST_DEVICE_CAST (device);
}

static void
gst_am_device_provider_add_device (GstAmDeviceProvider * provider, JNIEnv * env,
    jobject audioInfo)
{
  GstDevice *device = gst_am_device_from_audio_info (env, audioInfo);

  if (device) {
    GST_DEBUG_OBJECT (provider, "Adding %" GST_PTR_FORMAT, device);
    gst_device_provider_device_add (GST_DEVICE_PROVIDER_CAST (provider),
        device);
  }
}

static void
gst_am_device_provider_on_audio_devices_changed (GstAmDeviceProvider * provider,
    JNIEnv * env, jobjectArray audioDevices, int action_type)
{
  jsize i;
  jsize numAudioDevices;

  numAudioDevices = (*env)->GetArrayLength (env, audioDevices);
  GST_DEBUG_OBJECT (provider, "Audio devices %s (%d)",
      action_type == ADDED_DEVICES ? "added" : "removed", numAudioDevices);

  for (i = 0; i < numAudioDevices; i++) {
    jobject audioInfo;
    audioInfo = (*env)->GetObjectArrayElement (env, audioDevices, i);

    if (action_type == ADDED_DEVICES)
      gst_am_device_provider_add_device (provider, env, audioInfo);
    else if (action_type == REMOVED_DEVICES)
      gst_am_device_provider_remove_device (provider, env, audioInfo);
  }
}

static void
gst_am_device_provider_on_audio_devices_added (JNIEnv * env, jobject thiz,
    long long context, jobjectArray addedDevices)
{
  GstAmDeviceProvider *provider =
      GST_AM_DEVICE_PROVIDER_CAST (JLONG_TO_GPOINTER (context));

  gst_am_device_provider_on_audio_devices_changed (provider, env,
      addedDevices, ADDED_DEVICES);
}

static void
gst_am_device_provider_on_audio_devices_removed (JNIEnv * env,
    jobject thiz, long long context, jobjectArray removedDevices)
{
  GstAmDeviceProvider *provider =
      GST_AM_DEVICE_PROVIDER_CAST (JLONG_TO_GPOINTER (context));

  gst_am_device_provider_on_audio_devices_changed (provider, env,
      removedDevices, REMOVED_DEVICES);
}

static jobject
gst_am_device_provider_create_callback (GstAmDeviceProvider * provider,
    JNIEnv * env)
{
  jclass class;
  jobject callback = NULL;

  jmethodID constructor_id;
  jmethodID setContext;

  class =
      gst_amc_jni_get_application_class (env,
      "org/freedesktop/gstreamer/androidmedia/GstAmAudioDeviceCallback", NULL);
  if (!class) {
    GST_ERROR ("Failed to load GstAmAudioDeviceCallback class");
    return NULL;
  }

  constructor_id =
      gst_amc_jni_get_method_id (env, NULL, class, "<init>", "()V");
  if (!constructor_id) {
    GST_ERROR ("Failed to get method id for constructor");
    goto done;
  }

  setContext =
      gst_amc_jni_get_method_id (env, NULL, class, "setContext", "(J)V");
  if (!setContext) {
    GST_ERROR ("Can't find setContext method");
    goto done;
  }

  callback = gst_amc_jni_new_object (env, NULL, TRUE, class, constructor_id);

  /* call setContext on GstAmAudioDeviceCallback */
  if (!gst_amc_jni_call_void_method (env, NULL, callback, setContext,
          GPOINTER_TO_JLONG (provider))) {
    GST_ERROR ("setContext call failed");
    goto done;
  }

done:
  if (class)
    gst_amc_jni_object_unref (env, class);

  return callback;
}

static void
jni_AudioManager_registerAudioDeviceCallback (JNIEnv * env,
    jobject audioManager, jobject audioDeviceCallback)
{
  jclass audioManagerClass;
  jmethodID registerAudioDeviceCallback;

  audioManagerClass = (*env)->FindClass (env, "android/media/AudioManager");
  registerAudioDeviceCallback =
      (*env)->GetMethodID (env, audioManagerClass,
      "registerAudioDeviceCallback",
      "(Landroid/media/AudioDeviceCallback;Landroid/os/Handler;)V");

  (*env)->CallVoidMethod (env, audioManager, registerAudioDeviceCallback,
      audioDeviceCallback, NULL);
}

static void
jni_AudioManager_unregisterAudioDeviceCallback (JNIEnv * env,
    jobject audioManager, jobject audioDeviceCallback)
{
  jclass audioManagerClass;
  jmethodID unregisterAudioDeviceCallback;

  audioManagerClass = (*env)->FindClass (env, "android/media/AudioManager");
  unregisterAudioDeviceCallback =
      (*env)->GetMethodID (env, audioManagerClass,
      "unregisterAudioDeviceCallback",
      "(Landroid/media/AudioDeviceCallback;)V");

  (*env)->CallVoidMethod (env, audioManager, unregisterAudioDeviceCallback,
      audioDeviceCallback);
}

static jobjectArray
jni_AudioManager_getDevices (JNIEnv * env, jobject audioManager,
    gint deviceType)
{
  jobjectArray audioDevices;    // AudioDeviceInfo[]
  jclass audioManagerClass;
  jmethodID getDevicesID;

  audioManagerClass = (*env)->FindClass (env, "android/media/AudioManager");
  getDevicesID = (*env)->GetMethodID (env, audioManagerClass, "getDevices",
      "(I)[Landroid/media/AudioDeviceInfo;");
  audioDevices =
      (*env)->CallObjectMethod (env, audioManager, getDevicesID, deviceType);

  return audioDevices;
}

static GList *
gst_am_device_provider_probe (GstDeviceProvider * object)
{
  GList *devices = NULL;
  JNIEnv *env;
  jobject ctx;
  jobject audioManager;
  jsize i, numAudioDevices;
  jobjectArray audioDevices;

  env = gst_amc_jni_get_env ();
  ctx = jni_get_global_context (env);
  if (!ctx) {
    GST_ERROR ("Can't get an instance of global context");
    return NULL;
  }

  audioManager = jni_get_AudioManager_Instance (env, ctx);
  if (!audioManager) {
    GST_ERROR ("Can't get an instance of AudioManager");
    return NULL;
  }

  audioDevices =
      jni_AudioManager_getDevices (env, audioManager, GET_DEVICES_ALL);
  if (!audioDevices) {
    GST_ERROR ("No audio devices found!");
    return NULL;
  }

  numAudioDevices = (*env)->GetArrayLength (env, audioDevices);
  GST_DEBUG ("numAudioDevices: %u", numAudioDevices);
  for (i = 0; i < numAudioDevices; i++) {
    jobject audioInfo;
    GstDevice *device;

    audioInfo = (*env)->GetObjectArrayElement (env, audioDevices, i);
    device = gst_am_device_from_audio_info (env, audioInfo);

    if (device) {
      gst_object_ref_sink (device);
      devices = g_list_append (devices, device);
    }
  }

  return devices;
}

static gboolean
gst_am_device_provider_start (GstDeviceProvider * object)
{
  GstAmDeviceProvider *provider;
  JNIEnv *env;
  jobject ctx;
  jobject audioDeviceCallback;
  jobject audioManager;

  provider = GST_AM_DEVICE_PROVIDER_CAST (object);
  if (provider->audioDeviceCB != NULL) {
    GST_ERROR ("Already started!");
    return FALSE;
  }

  env = gst_amc_jni_get_env ();
  audioDeviceCallback = gst_am_device_provider_create_callback (provider, env);
  if (!audioDeviceCallback) {
    return FALSE;
  }

  GST_DEBUG_OBJECT (provider, "Starting...");

  ctx = jni_get_global_context (env);
  if (!ctx) {
    GST_ERROR ("Can't get an instance of global context");
    return FALSE;
  }

  audioManager = jni_get_AudioManager_Instance (env, ctx);
  if (!audioManager) {
    GST_ERROR ("Can't get an instance of AudioManager");
    return FALSE;
  }

  provider->audioDeviceCB = audioDeviceCallback;
  jni_AudioManager_registerAudioDeviceCallback (env, audioManager,
      audioDeviceCallback);

  return TRUE;
}

static void
gst_am_device_provider_stop (GstDeviceProvider * object)
{
  GstAmDeviceProvider *provider;
  JNIEnv *env;
  jobject ctx;
  jobject audioManager;

  provider = GST_AM_DEVICE_PROVIDER_CAST (object);
  if (provider->audioDeviceCB == NULL) {
    GST_ERROR ("No callback set");
    return;
  }

  GST_DEBUG_OBJECT (provider, "Stopping...");

  env = gst_amc_jni_get_env ();
  ctx = jni_get_global_context (env);
  if (!ctx) {
    GST_ERROR ("Can't get an instance of global context");
    return;
  }

  audioManager = jni_get_AudioManager_Instance (env, ctx);
  if (!audioManager) {
    GST_ERROR ("Can't get an instance of AudioManager");
    return;
  }

  jni_AudioManager_unregisterAudioDeviceCallback (env, audioManager,
      provider->audioDeviceCB);
  gst_amc_jni_object_unref (env, provider->audioDeviceCB);
  provider->audioDeviceCB = NULL;
}


static void
gst_am_device_provider_populate_devices (GstDeviceProvider * provider)
{
  GList *devices, *it;

  devices = gst_am_device_provider_probe (provider);

  for (it = devices; it != NULL; it = g_list_next (it)) {
    GstDevice *device = GST_DEVICE_CAST (it->data);
    gst_device_provider_device_add (provider, device);
  }

  g_list_free_full (devices, gst_object_unref);
}

static void
gst_am_device_provider_init (GstAmDeviceProvider * provider)
{
  gst_am_device_provider_populate_devices (GST_DEVICE_PROVIDER_CAST (provider));
}

static void
gst_am_device_provider_finalize (GObject * object)
{
  GstAmDeviceProvider *provider = GST_AM_DEVICE_PROVIDER_CAST (object);
  (void) provider;

  G_OBJECT_CLASS (device_provider_parent_class)->finalize (object);
}

static void
gst_am_device_provider_register_jni_native_methods ()
{
  jclass class;
  JNIEnv *env;

  JNINativeMethod native_methods[] = {
    {"native_onAudioDevicesAdded", "(J[Landroid/media/AudioDeviceInfo;)V",
        (void *) gst_am_device_provider_on_audio_devices_added},
    {"native_onAudioDevicesRemoved", "(J[Landroid/media/AudioDeviceInfo;)V",
        (void *) gst_am_device_provider_on_audio_devices_removed},
  };

  env = gst_amc_jni_get_env ();
  class =
      gst_amc_jni_get_application_class (env,
      "org/freedesktop/gstreamer/androidmedia/GstAmAudioDeviceCallback", NULL);
  if (!class) {
    GST_ERROR ("Failed to load GstAmAudioDeviceCallback class");
    return;
  }

  (*env)->RegisterNatives (env, class,
      (const JNINativeMethod *) &native_methods, G_N_ELEMENTS (native_methods));
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to register native methods");
  }

  gst_amc_jni_object_unref (env, class);
}

static void
gst_am_device_provider_class_init (GstAmDeviceProviderClass * klass)
{
  GstDeviceProviderClass *dm_class = GST_DEVICE_PROVIDER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  dm_class->probe = gst_am_device_provider_probe;
  dm_class->start = gst_am_device_provider_start;
  dm_class->stop = gst_am_device_provider_stop;

  gobject_class->finalize = gst_am_device_provider_finalize;

  gst_device_provider_class_set_static_metadata (dm_class,
      "Android Audio Device Provider", "Sink/Source/Audio",
      "List and monitor Android audio devices",
      "Tulio Beloqui <tulio@pexip.com>");

  GST_DEBUG_CATEGORY_INIT (gst_am_device_provider_debug,
      "amdeviceprovider", 0, "Android Audio Device Provider");

  gst_am_device_provider_register_jni_native_methods ();
}

enum
{
  PROP_DEVICE_ID = 1,
};

static void
gst_am_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAmDevice *device = GST_AM_DEVICE_CAST (object);

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
gst_am_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmDevice *device = GST_AM_DEVICE_CAST (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      device->device_id = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


static void
gst_am_device_class_init (GstAmDeviceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = gst_am_device_get_property;
  object_class->set_property = gst_am_device_set_property;

  g_object_class_install_property (object_class, PROP_DEVICE_ID,
      g_param_spec_int ("device-id", "Audio Device ID",
          "The audio device id", 0, G_MAXINT, -1,
          G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
gst_am_device_init (GstAmDevice * device)
{
}
