/* GStreamer
 * Copyright (C) 2014 Collabora
 *   Author: Olivier Crete <olivier.crete@collabora.com>
 *
 * gstdevice.c: Unit test for GstDevice
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

#include <gst/check/gstcheck.h>
#include <gst/gst.h>

typedef struct _GstTestDevice
{
  GstDevice parent;
} GstTestDevice;

typedef struct _GstTestDeviceClass
{
  GstDeviceClass parent_class;
} GstTestDeviceClass;

GType gst_test_device_get_type (void);

G_DEFINE_TYPE (GstTestDevice, gst_test_device, GST_TYPE_DEVICE);

static GstElement *
gst_test_device_create_element (GstDevice * device, const gchar * name)
{
  return gst_bin_new (name);
}

static gboolean
gst_test_device_reconfigure_element (GstDevice * device, GstElement * element)
{
  if (!strcmp (GST_ELEMENT_NAME (element), "reconfigurable"))
    return TRUE;
  else
    return FALSE;
}

static void
gst_test_device_class_init (GstTestDeviceClass * klass)
{
  GstDeviceClass *dclass = GST_DEVICE_CLASS (klass);

  dclass->create_element = gst_test_device_create_element;
  dclass->reconfigure_element = gst_test_device_reconfigure_element;
}

static void
gst_test_device_init (GstTestDevice * self)
{
}

#define DEVICE_CLASS "Test0/Test1/Test2/Test3/Test4/TestDev"
#define DISPLAY_NAME "Test device"

static GstDevice *
test_device_new (void)
{
  GstCaps *caps = gst_caps_new_empty_simple ("video/test");
  GstDevice *device = g_object_new (gst_test_device_get_type (), "caps", caps,
      "display-name", DISPLAY_NAME, "device-class", DEVICE_CLASS, NULL);

  gst_caps_unref (caps);

  return device;
}

GST_START_TEST (test_device)
{
  GstDevice *device = test_device_new ();
  GstCaps *caps;
  gchar *display_name;
  gchar *device_class;
  GstCaps *compare_caps = gst_caps_new_empty_simple ("video/test");
  GstElement *element;

  caps = gst_device_get_caps (device);
  display_name = gst_device_get_display_name (device);
  device_class = gst_device_get_device_class (device);

  ck_assert_str_eq (DISPLAY_NAME, display_name);
  ck_assert_str_eq (DEVICE_CLASS, device_class);
  gst_check_caps_equal (caps, compare_caps);

  g_free (display_name);
  g_free (device_class);
  gst_caps_unref (caps);

  ck_assert (gst_device_has_classes (device, "Test1"));
  ck_assert (gst_device_has_classes (device, "Test2/Test1"));

  element = gst_device_create_element (device, "reconfigurable");
  ck_assert (GST_IS_BIN (element));

  ck_assert (gst_device_reconfigure_element (device, element));

  gst_element_set_name (element, "no-no");

  ck_assert (!gst_device_reconfigure_element (device, element));

  gst_object_unref (element);

  gst_caps_unref (compare_caps);
  gst_object_unref (device);
}

GST_END_TEST;


typedef struct _GstTestDeviceProvider
{
  GstDeviceProvider parent;

} GstTestDeviceProvider;

typedef struct _GstTestDeviceProviderClass
{
  GstDeviceProviderClass parent_class;
} GstTestDeviceProviderClass;

GType gst_test_device_provider_get_type (void);

G_DEFINE_TYPE (GstTestDeviceProvider, gst_test_device_provider,
    GST_TYPE_DEVICE_PROVIDER);

int num_devices = 1;

static GList *
gst_test_device_provider_probe (GstDeviceProvider * provider)
{
  int i;
  GList *devs = NULL;

  for (i = 0; i < num_devices; i++)
    devs = g_list_prepend (devs, test_device_new ());

  return devs;
}

static void
gst_test_device_provider_class_init (GstTestDeviceProviderClass * klass)
{
  GstDeviceProviderClass *dpclass = GST_DEVICE_PROVIDER_CLASS (klass);

  dpclass->probe = gst_test_device_provider_probe;

  gst_device_provider_class_set_static_metadata (dpclass,
      "Test Device Provider", "Test0/Test1/Test2/Test3/TestProvider",
      "List but does NOT monitor test devices",
      "Olivier Crete <olivier.crete@collabora.com>");
}

static void
gst_test_device_provider_init (GstTestDeviceProvider * self)
{
}

GST_DEVICE_PROVIDER_REGISTER_DECLARE (testdeviceprovider);

GST_DEVICE_PROVIDER_REGISTER_DEFINE (testdeviceprovider, "testdeviceprovider",
    1, gst_test_device_provider_get_type ())
     static void register_test_device_provider (void)
{
  gst_device_provider_register (NULL, "testdeviceprovider", 1,
      gst_test_device_provider_get_type ());
}

GST_START_TEST (test_device_provider_factory)
{
  GstDeviceProvider *dp, *dp2;
  GList *factories;
  GstDeviceProviderFactory *f;

  GST_DEVICE_PROVIDER_REGISTER (testdeviceprovider, NULL);

  factories = gst_device_provider_factory_list_get_device_providers (1);

  ck_assert_ptr_ne (factories, NULL);

  f = gst_device_provider_factory_find ("testdeviceprovider");
  ck_assert_ptr_ne (f, NULL);

  gst_plugin_feature_list_free (factories);

  ck_assert (gst_device_provider_factory_has_classes (f, "Test2"));
  ck_assert (gst_device_provider_factory_has_classes (f, "Test2/Test0"));
  ck_assert (!gst_device_provider_factory_has_classes (f, "Test2/TestN/Test0"));
  ck_assert (!gst_device_provider_factory_has_classes (f, "TestN"));
  ck_assert (!gst_device_provider_factory_has_classes (f, "Test"));

  dp = gst_device_provider_factory_get (f);

  gst_object_unref (f);

  dp2 = gst_device_provider_factory_get_by_name ("testdeviceprovider");

  ck_assert_ptr_eq (dp, dp2);

  gst_object_unref (dp);
  gst_object_unref (dp2);

  dp2 = gst_device_provider_factory_get_by_name ("testdeviceprovider");
  ck_assert_ptr_eq (dp, dp2);
  gst_object_unref (dp2);
}

GST_END_TEST;

GST_START_TEST (test_device_provider)
{
  GstDeviceProvider *dp;
  GList *devs;
  GstBus *bus;

  register_test_device_provider ();

  dp = gst_device_provider_factory_get_by_name ("testdeviceprovider");
  num_devices = 0;
  ck_assert_ptr_ne (dp, NULL);
  ck_assert_ptr_eq (gst_device_provider_get_devices (dp), NULL);

  num_devices = 1;

  devs = gst_device_provider_get_devices (dp);
  ck_assert (g_list_length (devs) == 1);
  ck_assert (GST_IS_DEVICE (devs->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  fail_if (gst_device_provider_can_monitor (dp));
  fail_if (gst_device_provider_is_started (dp));
  ck_assert (gst_device_provider_start (dp));

  bus = gst_device_provider_get_bus (dp);
  ck_assert (GST_IS_BUS (bus));
  gst_object_unref (bus);

  ck_assert (gst_device_provider_is_started (dp));
  gst_device_provider_stop (dp);

  gst_object_unref (dp);
}

GST_END_TEST;

typedef struct _GstTestDeviceProviderMonitor
{
  GstDeviceProvider parent;

} GstTestDeviceProviderMonitor;

typedef struct _GstTestDeviceProviderMonitorClass
{
  GstDeviceProviderClass parent_class;
} GstTestDeviceProviderMonitorClass;

GType gst_test_device_provider_monitor_get_type (void);

G_DEFINE_TYPE (GstTestDeviceProviderMonitor, gst_test_device_provider_monitor,
    GST_TYPE_DEVICE_PROVIDER);

static gboolean
gst_test_device_provider_monitor_start (GstDeviceProvider * monitor)
{
  GList *devices = gst_test_device_provider_probe (monitor);

  for (GList * iter = devices; iter; iter = iter->next) {
    gst_device_provider_device_add (monitor, GST_DEVICE (iter->data));
  }

  /* This device is only added after starting the monitor. If the monitor is not
   * started yet, only devices from gst_test_device_provider_probe() are
   * available. */
  gst_device_provider_device_add (monitor, test_device_new ());

  /* Device references were floating, so were transferred in
   * gst_device_provider_device_add() */
  g_list_free (devices);
  return TRUE;
}

static void
gst_test_device_provider_monitor_class_init (GstTestDeviceProviderMonitorClass *
    klass)
{
  GstDeviceProviderClass *dpclass = GST_DEVICE_PROVIDER_CLASS (klass);

  dpclass->probe = gst_test_device_provider_probe;
  dpclass->start = gst_test_device_provider_monitor_start;

  gst_device_provider_class_set_static_metadata (dpclass,
      "Test Device Provider Monitor",
      "Test0/Test1/Test2/Test4/TestProviderMonitor",
      "List and monitors Test devices",
      "Olivier Crete <olivier.crete@collabora.com>");
}

static void
gst_test_device_provider_monitor_init (GstTestDeviceProviderMonitor * self)
{
}

static void
register_test_device_provider_monitor (void)
{
  gst_device_provider_register (NULL, "testdeviceprovidermonitor", 2,
      gst_test_device_provider_monitor_get_type ());
}


GST_START_TEST (test_device_provider_monitor)
{
  GstDeviceProvider *dp;
  GList *devs;
  GstBus *bus;
  GstDevice *mydev;
  GstDevice *dev;
  GstMessage *msg;

  register_test_device_provider_monitor ();

  dp = gst_device_provider_factory_get_by_name ("testdeviceprovidermonitor");

  bus = gst_device_provider_get_bus (dp);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_eq (msg, NULL);

  ck_assert (gst_device_provider_can_monitor (dp));
  ck_assert (gst_device_provider_start (dp));

  devs = gst_device_provider_get_devices (dp);
  ck_assert_int_eq (g_list_length (devs), 2);
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  mydev = test_device_new ();
  ck_assert (g_object_is_floating (mydev));
  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 1);

  gst_device_provider_device_add (dp, mydev);
  ck_assert (!g_object_is_floating (mydev));
  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 2);

  devs = gst_device_provider_get_devices (dp);
  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 3);

  fail_unless_equals_int (g_list_length (devs), 3);
  ck_assert_ptr_eq (g_list_nth_data (devs, 2), mydev);
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 2);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_DEVICE_ADDED);
  gst_message_unref (msg);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_DEVICE_ADDED);
  gst_message_unref (msg);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_DEVICE_ADDED);

  gst_message_parse_device_added (msg, &dev);
  ck_assert_ptr_eq (dev, mydev);
  gst_object_unref (dev);
  gst_message_unref (msg);

  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 1);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_eq (msg, NULL);

  gst_device_provider_device_remove (dp, mydev);
  devs = gst_device_provider_get_devices (dp);
  ck_assert_int_eq (g_list_length (devs), 2);
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);

  ck_assert (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_DEVICE_REMOVED);

  gst_message_parse_device_removed (msg, &dev);
  ck_assert_ptr_eq (dev, mydev);
  ASSERT_OBJECT_REFCOUNT (mydev, "dev", 2);
  gst_object_unref (dev);
  gst_message_unref (msg);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_eq (msg, NULL);

  gst_device_provider_stop (dp);
  gst_object_unref (bus);
  ASSERT_OBJECT_REFCOUNT (dp, "monitor", 2);
  gst_object_unref (dp);

  /* Is singleton, so system keeps a ref */
  ASSERT_OBJECT_REFCOUNT (dp, "monitor", 1);
}

GST_END_TEST;


GST_START_TEST (test_device_monitor)
{
  GstDeviceProvider *dp, *dp2;
  GstDeviceMonitor *mon;
  GList *devs;
  guint id, id2;
  GstDevice *mydev;
  GstMessage *msg;
  GstBus *bus;
  GstDevice *dev;

  register_test_device_provider ();
  register_test_device_provider_monitor ();

  dp = gst_device_provider_factory_get_by_name ("testdeviceprovider");
  dp2 = gst_device_provider_factory_get_by_name ("testdeviceprovidermonitor");

  mon = gst_device_monitor_new ();

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_ptr_eq (devs, NULL);

  id = gst_device_monitor_add_filter (mon, "TestProvider", NULL);
  ck_assert_int_gt (id, 0);

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_ptr_eq (devs, NULL);

  ck_assert (gst_device_monitor_add_filter (mon, "TestDevice", NULL) == 0);
  ASSERT_CRITICAL (gst_device_monitor_remove_filter (mon, 0));

  ck_assert (gst_device_monitor_remove_filter (mon, id));

  id = gst_device_monitor_add_filter (mon, "Test3", NULL);
  ck_assert_int_gt (id, 0);
  devs = gst_device_monitor_get_devices (mon);
  ck_assert_int_eq (g_list_length (devs), 1);
  ck_assert (GST_IS_DEVICE (devs->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  id2 = gst_device_monitor_add_filter (mon, "Test1", NULL);
  ck_assert (id2 > 0);
  devs = gst_device_monitor_get_devices (mon);
  ck_assert_int_eq (g_list_length (devs), 2);
  ck_assert (GST_IS_DEVICE (devs->data));
  ck_assert (GST_IS_DEVICE (devs->next->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  ck_assert (gst_device_monitor_remove_filter (mon, id));

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_int_eq (g_list_length (devs), 2);
  ck_assert (GST_IS_DEVICE (devs->data));
  ck_assert (GST_IS_DEVICE (devs->next->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  ck_assert (gst_device_monitor_start (mon));

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_int_eq (g_list_length (devs), 3);
  ck_assert (GST_IS_DEVICE (devs->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  gst_device_monitor_stop (mon);

  ck_assert (gst_device_monitor_remove_filter (mon, id2));

  id = gst_device_monitor_add_filter (mon, "Test4", NULL);
  ck_assert (id > 0);

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_int_eq (g_list_length (devs), 1);
  ck_assert (GST_IS_DEVICE (devs->data));
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  bus = gst_device_monitor_get_bus (mon);

  ck_assert (gst_device_monitor_start (mon));

  msg = gst_bus_timed_pop (bus, GST_CLOCK_TIME_NONE);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert_int_eq (GST_MESSAGE_TYPE (msg), GST_MESSAGE_DEVICE_ADDED);
  gst_message_unref (msg);

  msg = gst_bus_timed_pop (bus, GST_CLOCK_TIME_NONE);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert_int_eq (GST_MESSAGE_TYPE (msg), GST_MESSAGE_DEVICE_ADDED);
  gst_message_unref (msg);

  msg = gst_bus_timed_pop (bus, GST_CLOCK_TIME_NONE);
  ck_assert_ptr_ne (msg, NULL);
  ck_assert_int_eq (GST_MESSAGE_TYPE (msg), GST_MESSAGE_DEVICE_MONITOR_STARTED);
  gst_message_unref (msg);

  mydev = test_device_new ();
  gst_device_provider_device_add (dp2, mydev);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);

  ck_assert_int_eq (GST_MESSAGE_TYPE (msg), GST_MESSAGE_DEVICE_ADDED);

  gst_message_parse_device_added (msg, &dev);
  ck_assert_ptr_eq (dev, mydev);
  gst_object_unref (dev);
  gst_message_unref (msg);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_eq (msg, NULL);

  gst_device_provider_device_remove (dp2, mydev);
  devs = gst_device_monitor_get_devices (mon);
  ck_assert_ptr_eq (g_list_find (devs, mydev), NULL);
  g_list_free_full (devs, (GDestroyNotify) gst_object_unref);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_ne (msg, NULL);

  ck_assert (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_DEVICE_REMOVED);

  gst_message_parse_device_removed (msg, &dev);
  ck_assert_ptr_eq (dev, mydev);
  gst_object_unref (dev);
  gst_message_unref (msg);

  msg = gst_bus_pop (bus);
  ck_assert_ptr_eq (msg, NULL);


  gst_device_monitor_stop (mon);

  gst_object_unref (bus);
  gst_object_unref (mon);

  gst_object_unref (dp);
  gst_object_unref (dp2);

  /* should work fine without any filters */
  mon = gst_device_monitor_new ();
  ck_assert (gst_device_monitor_start (mon));
  gst_device_monitor_stop (mon);
  gst_object_unref (mon);
}

GST_END_TEST;

/* gst_device_monitor_stop() while the start thread is still starting
 * providers. There are three providers, started in rank order:
 *
 *  - "ok" starts successfully. Once it is started, the start thread has
 *    handed it over to the list of started providers that stop() takes.
 *  - "fast" fails to start straight away. Its probe() is called by
 *    gst_device_monitor_get_devices() before the slow provider is probed, and
 *    lets the slow provider return from start().
 *  - "slow" blocks in start() until it is released, then returns
 *    stop_race.slow_start_result.
 *
 * The slow provider holds its start lock while it is blocked, so
 * gst_device_monitor_get_devices() can't probe it before it is released. It
 * does wait for the start thread to be detached, which stop() does in the same
 * critical section where it takes the started providers. That is how the test
 * knows that the stop request is in before the slow provider returns. */
static struct
{
  GMutex lock;
  GCond cond;
  /* The slow provider only blocks while armed */
  gboolean armed;
  gboolean slow_entered;
  gboolean slow_released;
  gboolean slow_start_result;
  guint ok_started;
  guint ok_stopped;
  guint slow_stopped;
} stop_race;

typedef struct _GstTestOkProvider
{
  GstDeviceProvider parent;
} GstTestOkProvider;

typedef struct _GstTestOkProviderClass
{
  GstDeviceProviderClass parent_class;
} GstTestOkProviderClass;

GType gst_test_ok_provider_get_type (void);

G_DEFINE_TYPE (GstTestOkProvider, gst_test_ok_provider,
    GST_TYPE_DEVICE_PROVIDER);

static gboolean
gst_test_ok_provider_start (GstDeviceProvider * provider)
{
  g_mutex_lock (&stop_race.lock);
  stop_race.ok_started++;
  g_mutex_unlock (&stop_race.lock);

  return TRUE;
}

static void
gst_test_ok_provider_stop (GstDeviceProvider * provider)
{
  g_mutex_lock (&stop_race.lock);
  stop_race.ok_stopped++;
  g_mutex_unlock (&stop_race.lock);
}

static void
gst_test_ok_provider_class_init (GstTestOkProviderClass * klass)
{
  GstDeviceProviderClass *dpclass = GST_DEVICE_PROVIDER_CLASS (klass);

  dpclass->start = gst_test_ok_provider_start;
  dpclass->stop = gst_test_ok_provider_stop;

  gst_device_provider_class_set_static_metadata (dpclass, "Test Ok Provider",
      "TestStopRace/Ok", "Starts successfully", "Unit test");
}

static void
gst_test_ok_provider_init (GstTestOkProvider * self)
{
}

typedef struct _GstTestFastProvider
{
  GstDeviceProvider parent;
} GstTestFastProvider;

typedef struct _GstTestFastProviderClass
{
  GstDeviceProviderClass parent_class;
} GstTestFastProviderClass;

GType gst_test_fast_provider_get_type (void);

G_DEFINE_TYPE (GstTestFastProvider, gst_test_fast_provider,
    GST_TYPE_DEVICE_PROVIDER);

static gboolean
gst_test_fast_provider_start (GstDeviceProvider * provider)
{
  return FALSE;
}

static GList *
gst_test_fast_provider_probe (GstDeviceProvider * provider)
{
  g_mutex_lock (&stop_race.lock);
  stop_race.slow_released = TRUE;
  g_cond_broadcast (&stop_race.cond);
  g_mutex_unlock (&stop_race.lock);

  return NULL;
}

static void
gst_test_fast_provider_class_init (GstTestFastProviderClass * klass)
{
  GstDeviceProviderClass *dpclass = GST_DEVICE_PROVIDER_CLASS (klass);

  dpclass->start = gst_test_fast_provider_start;
  dpclass->probe = gst_test_fast_provider_probe;

  gst_device_provider_class_set_static_metadata (dpclass, "Test Fast Provider",
      "TestStopRace/Fail", "Fails to start immediately", "Unit test");
}

static void
gst_test_fast_provider_init (GstTestFastProvider * self)
{
}

typedef struct _GstTestSlowProvider
{
  GstDeviceProvider parent;
} GstTestSlowProvider;

typedef struct _GstTestSlowProviderClass
{
  GstDeviceProviderClass parent_class;
} GstTestSlowProviderClass;

GType gst_test_slow_provider_get_type (void);

G_DEFINE_TYPE (GstTestSlowProvider, gst_test_slow_provider,
    GST_TYPE_DEVICE_PROVIDER);

static gboolean
gst_test_slow_provider_start (GstDeviceProvider * provider)
{
  gboolean ret;

  g_mutex_lock (&stop_race.lock);

  if (!stop_race.armed) {
    g_mutex_unlock (&stop_race.lock);
    return FALSE;
  }

  stop_race.slow_entered = TRUE;
  g_cond_broadcast (&stop_race.cond);

  while (!stop_race.slow_released)
    g_cond_wait (&stop_race.cond, &stop_race.lock);

  ret = stop_race.slow_start_result;
  g_mutex_unlock (&stop_race.lock);

  return ret;
}

static void
gst_test_slow_provider_stop (GstDeviceProvider * provider)
{
  g_mutex_lock (&stop_race.lock);
  stop_race.slow_stopped++;
  g_mutex_unlock (&stop_race.lock);
}

static void
gst_test_slow_provider_class_init (GstTestSlowProviderClass * klass)
{
  GstDeviceProviderClass *dpclass = GST_DEVICE_PROVIDER_CLASS (klass);

  dpclass->start = gst_test_slow_provider_start;
  dpclass->stop = gst_test_slow_provider_stop;

  gst_device_provider_class_set_static_metadata (dpclass, "Test Slow Provider",
      "TestStopRace/Fail/Slow", "Blocks in start() until released",
      "Unit test");
}

static void
gst_test_slow_provider_init (GstTestSlowProvider * self)
{
}

static gpointer
stop_monitor_thread (gpointer data)
{
  gst_device_monitor_stop (GST_DEVICE_MONITOR (data));

  return NULL;
}

static gboolean
wait_for_slow_provider (void)
{
  gint64 end_time = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  gboolean entered;

  g_mutex_lock (&stop_race.lock);
  while (!stop_race.slow_entered) {
    if (!g_cond_wait_until (&stop_race.cond, &stop_race.lock, end_time))
      break;
  }
  entered = stop_race.slow_entered;
  g_mutex_unlock (&stop_race.lock);

  return entered;
}

static void
check_stop_while_starting (gboolean with_ok_provider,
    gboolean slow_start_result)
{
  GstDeviceProvider *ok, *slow;
  GstDeviceMonitor *mon;
  GThread *stopper;
  GList *devs;
  gint ok_refs, slow_refs;
  guint ok_started, ok_stopped, slow_stopped;

  gst_device_provider_register (NULL, "teststopokprovider", 4,
      gst_test_ok_provider_get_type ());
  gst_device_provider_register (NULL, "teststopfastprovider", 3,
      gst_test_fast_provider_get_type ());
  gst_device_provider_register (NULL, "teststopslowprovider", 2,
      gst_test_slow_provider_get_type ());

  ok = gst_device_provider_factory_get_by_name ("teststopokprovider");
  slow = gst_device_provider_factory_get_by_name ("teststopslowprovider");
  ok_refs = GST_OBJECT_REFCOUNT_VALUE (ok);
  slow_refs = GST_OBJECT_REFCOUNT_VALUE (slow);

  g_mutex_lock (&stop_race.lock);
  stop_race.armed = TRUE;
  stop_race.slow_entered = FALSE;
  stop_race.slow_released = FALSE;
  stop_race.slow_start_result = slow_start_result;
  stop_race.ok_started = 0;
  stop_race.ok_stopped = 0;
  stop_race.slow_stopped = 0;
  g_mutex_unlock (&stop_race.lock);

  mon = gst_device_monitor_new ();
  /* Filtering on "Fail" leaves out the ok provider */
  ck_assert_int_gt (gst_device_monitor_add_filter (mon,
          with_ok_provider ? "TestStopRace" : "TestStopRace/Fail", NULL), 0);
  ck_assert (gst_device_monitor_start (mon));

  fail_unless (wait_for_slow_provider (), "The slow provider was not started");

  /* stop() joins the start thread, which is blocked in the slow provider
   * until get_devices() below releases it */
  stopper = g_thread_new ("stop-monitor", stop_monitor_thread, mon);

  devs = gst_device_monitor_get_devices (mon);
  ck_assert_ptr_eq (devs, NULL);

  g_thread_join (stopper);

  gst_object_unref (mon);

  g_mutex_lock (&stop_race.lock);
  stop_race.armed = FALSE;
  ok_started = stop_race.ok_started;
  ok_stopped = stop_race.ok_stopped;
  slow_stopped = stop_race.slow_stopped;
  g_mutex_unlock (&stop_race.lock);

  /* Every provider that was started is stopped exactly once and the monitor
   * has dropped all of its references */
  fail_unless_equals_int (ok_started, with_ok_provider ? 1 : 0);
  fail_unless_equals_int (ok_stopped, ok_started);
  fail_unless_equals_int (slow_stopped, slow_start_result ? 1 : 0);
  ASSERT_OBJECT_REFCOUNT (ok, "ok provider", ok_refs);
  ASSERT_OBJECT_REFCOUNT (slow, "slow provider", slow_refs);

  gst_object_unref (ok);
  gst_object_unref (slow);
}

/* No provider is started when the start thread is stopped */
GST_START_TEST (test_device_monitor_stop_while_starting_failed)
{
  check_stop_while_starting (FALSE, FALSE);
}

GST_END_TEST;

/* The start thread has started a provider before it is stopped */
GST_START_TEST (test_device_monitor_stop_while_starting_failed_after_started)
{
  check_stop_while_starting (TRUE, FALSE);
}

GST_END_TEST;

/* The provider the start thread is stopped in starts successfully */
GST_START_TEST (test_device_monitor_stop_while_starting_succeeded)
{
  check_stop_while_starting (TRUE, TRUE);
}

GST_END_TEST;


static Suite *
gst_device_suite (void)
{
  Suite *s = suite_create ("GstDevice");
  TCase *tc_chain = tcase_create ("device tests");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_device);
  tcase_add_test (tc_chain, test_device_provider_factory);
  tcase_add_test (tc_chain, test_device_provider);
  tcase_add_test (tc_chain, test_device_provider_monitor);
  tcase_add_test (tc_chain, test_device_monitor);
  tcase_add_test (tc_chain, test_device_monitor_stop_while_starting_failed);
  tcase_add_test (tc_chain,
      test_device_monitor_stop_while_starting_failed_after_started);
  tcase_add_test (tc_chain, test_device_monitor_stop_while_starting_succeeded);

  return s;
}

GST_CHECK_MAIN (gst_device);
