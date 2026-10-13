/* GStreamer AVF device identity tests
 *
 * Copyright (C) 2026 Dominique Leroux
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
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstdevice.h>
#include <gst/gstdevicemonitor.h>

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#include <gst/gstmacos.h>
#endif

typedef struct
{
  gchar *unique_id;
  gchar *display_name;
  guint probe_index;
} AvfDeviceInfo;

static void
free_avf_device_info (gpointer data)
{
  AvfDeviceInfo *info = data;

  if (info == NULL)
    return;

  g_free (info->unique_id);
  g_free (info->display_name);
  g_free (info);
}

static gboolean
running_in_ci (void)
{
  return g_getenv ("CI") != NULL;
}

static GPtrArray *
collect_avf_devices (GstDeviceMonitor * monitor,
    const gchar * device_class_prefix)
{
  GList *devices, *iter;
  GPtrArray *result;

  if (monitor == NULL)
    return NULL;

  result = g_ptr_array_new_with_free_func (free_avf_device_info);
  devices = gst_device_monitor_get_devices (monitor);
  for (iter = devices; iter != NULL; iter = g_list_next (iter)) {
    GstDevice *device = GST_DEVICE (iter->data);
    GstStructure *props;
    const gchar *unique_id;
    AvfDeviceInfo *info;
    const gchar *device_class;

    device_class = gst_device_get_device_class (device);
    if (device_class == NULL
        || !g_str_has_prefix (device_class, device_class_prefix))
      continue;

    props = gst_device_get_properties (device);
    fail_unless (props != NULL, "AVF device is missing properties");

    unique_id = gst_structure_get_string (props, "avf.unique_id");
    if (unique_id == NULL || *unique_id == '\0') {
      g_clear_pointer (&props, gst_structure_free);
      continue;
    }

    info = g_new0 (AvfDeviceInfo, 1);
    info->unique_id = g_strdup (unique_id);
    info->display_name = gst_device_get_display_name (device);
    info->probe_index = result->len;
    g_ptr_array_add (result, info);

    g_clear_pointer (&props, gst_structure_free);
  }

  g_list_free_full (devices, gst_object_unref);

  return result;
}

static GstDeviceMonitor *
create_started_avf_monitor (const gchar * device_class)
{
  GstDeviceMonitor *monitor = gst_device_monitor_new ();

  fail_unless (monitor != NULL, "Could not create GstDeviceMonitor");
  gst_device_monitor_add_filter (monitor, device_class, NULL);
  fail_unless (gst_device_monitor_start (monitor),
      "Could not start GstDeviceMonitor");

  return monitor;
}

static void
fail_if_avf_device_mapping_changed (GPtrArray * baseline, GPtrArray * current,
    guint attempt_index, const AvfDeviceInfo * opened)
{
  fail_unless (baseline != NULL);
  fail_unless (current != NULL);

  if (baseline->len != current->len) {
    ck_abort_msg ("AVF device enumeration changed size on attempt %u after "
        "opening %s [%s]: got %u device(s) instead of %u", attempt_index,
        GST_STR_NULL (opened->display_name), GST_STR_NULL (opened->unique_id),
        current->len, baseline->len);
  }

  for (guint i = 0; i < MIN ((guint) 2, baseline->len); i++) {
    const AvfDeviceInfo *expected = g_ptr_array_index (baseline, i);
    const AvfDeviceInfo *observed = g_ptr_array_index (current, i);

    if (g_strcmp0 (expected->unique_id, observed->unique_id) != 0) {
      ck_abort_msg ("AVF device mapping changed on attempt %u after opening "
          "%s [%s]: position %u was %s [%s] and became %s [%s]",
          attempt_index, GST_STR_NULL (opened->display_name),
          GST_STR_NULL (opened->unique_id), i,
          GST_STR_NULL (expected->display_name),
          GST_STR_NULL (expected->unique_id),
          GST_STR_NULL (observed->display_name),
          GST_STR_NULL (observed->unique_id));
    }
  }
}

static void
open_device_by_unique_id_and_check_unique_id (GstElement * pipeline,
    GstElement * src, GstElement * sink, const AvfDeviceInfo * info,
    gboolean set_device_index, gint device_index, guint attempt_index)
{
  const guint max_pulled_samples = 1;
  GstBus *bus = gst_element_get_bus (pipeline);
  GstStateChangeReturn sret;
  gchar *opened_unique_id = NULL;
  gchar *opened_device_name = NULL;
  gchar *failure = NULL;

  fail_unless (bus != NULL);
  if (set_device_index) {
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
        "*\"device-index\" property of avfvideosrc is deprecated and non-functional*");
    g_object_set (src,
        "device-index", device_index, "unique-id", info->unique_id, NULL);
    g_test_assert_expected_messages ();
  } else {
    g_object_set (src, "unique-id", info->unique_id, NULL);
  }

  sret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (sret != GST_STATE_CHANGE_FAILURE,
      "Could not set AVF pipeline to PLAYING for %s",
      GST_STR_NULL (info->display_name));

  sret = gst_element_get_state (pipeline, NULL, NULL, 10 * GST_SECOND);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, 0, GST_MESSAGE_ERROR);
    if (msg == NULL) {
      failure =
          g_strdup_printf
          ("AVF pipeline failed for %s without posting an error",
          GST_STR_NULL (info->display_name));
    } else {
      GError *error = NULL;
      gchar *debug = NULL;
      gst_message_parse_error (msg, &error, &debug);
      failure =
          g_strdup_printf ("AVF pipeline failed on attempt %u for %s: %s (%s)",
          attempt_index, GST_STR_NULL (info->display_name),
          error ? error->message : "unknown error", GST_STR_NULL (debug));
      g_clear_error (&error);
      g_clear_pointer (&debug, g_free);
      g_clear_pointer (&msg, gst_message_unref);
    }
    goto done;
  }

  g_object_get (src,
      "unique-id", &opened_unique_id, "device-name", &opened_device_name, NULL);
  if (g_strcmp0 (opened_unique_id, info->unique_id) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened wrong device on attempt %u for %s: requested unique-id %s resolved to unique-id %s (%s)",
        attempt_index, GST_STR_NULL (info->display_name),
        GST_STR_NULL (info->unique_id), GST_STR_NULL (opened_unique_id),
        GST_STR_NULL (opened_device_name));
    goto done;
  }

  if (g_strcmp0 (opened_device_name, info->display_name) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened unstable device-name on attempt %u for unique-id %s: got %s instead of %s",
        attempt_index, GST_STR_NULL (info->unique_id),
        GST_STR_NULL (opened_device_name), GST_STR_NULL (info->display_name));
    goto done;
  }

  for (guint i = 0; i < max_pulled_samples; i++) {
    GstSample *sample =
        gst_app_sink_try_pull_sample (GST_APP_SINK (sink), GST_SECOND);
    if (sample == NULL)
      break;

    gst_sample_unref (sample);
  }

done:
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);
  g_clear_pointer (&opened_unique_id, g_free);
  g_clear_pointer (&opened_device_name, g_free);

  if (failure != NULL) {
    gchar *message = failure;
    failure = NULL;
    ck_abort_msg ("%s", message);
    g_free (message);
  }
}

GST_START_TEST (test_avf_device_index_compatibility)
{
  const guint switch_iterations = 3;
  GstDeviceMonitor *monitor = create_started_avf_monitor ("Video/Source");
  GPtrArray *initial_devices = collect_avf_devices (monitor, "Video/Source");
  AvfDeviceInfo *device_a;
  AvfDeviceInfo *device_b;
  GstElement *pipeline, *src, *sink, *capsfilter;
  GstCaps *caps;

  if (initial_devices->len < 2) {
    GST_WARNING
        ("Skipping AVF device-index compatibility test: need at least two AVF devices, found %u",
        initial_devices->len);
    g_ptr_array_unref (initial_devices);
    gst_device_monitor_stop (monitor);
    gst_object_unref (monitor);
    return;
  }

  gst_device_monitor_stop (monitor);
  g_clear_pointer (&monitor, gst_object_unref);

  device_a = g_ptr_array_index (initial_devices, 0);
  device_b = g_ptr_array_index (initial_devices, 1);

  GST_INFO
      ("Opening AVF devices with %u alternating switch iteration(s) while setting mismatched deprecated device-index values: %s [%s], %s [%s]",
      switch_iterations, GST_STR_NULL (device_a->display_name),
      device_a->unique_id, GST_STR_NULL (device_b->display_name),
      device_b->unique_id);

  src = gst_element_factory_make ("avfvideosrc", NULL);
  fail_unless (src != NULL, "Could not create avfvideosrc");
  fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (src),
          "unique-id") != NULL, "AVF source does not expose unique-id");

  sink = gst_element_factory_make ("appsink", NULL);
  fail_unless (sink != NULL, "Could not create appsink");
  g_object_set (sink,
      "emit-signals", FALSE,
      "async", FALSE, "sync", FALSE, "max-buffers", 8u, "drop", TRUE, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  fail_unless (capsfilter != NULL, "Could not create capsfilter");
  caps = gst_caps_from_string ("video/x-raw,format=BGRA");
  fail_unless (caps != NULL, "Could not build BGRA caps");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  pipeline = gst_pipeline_new ("avfdeviceidentity-device-index");
  fail_unless (pipeline != NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  fail_unless (gst_element_link_many (src, capsfilter, sink, NULL));

  for (guint iteration = 0; iteration < switch_iterations; iteration++) {
    const AvfDeviceInfo *expected =
        ((iteration % 2) == 0) ? device_a : device_b;
    gint mismatched_index =
        (expected == device_a) ? (gint) device_b->probe_index :
        (gint) device_a->probe_index;

    open_device_by_unique_id_and_check_unique_id (pipeline, src, sink,
        expected, TRUE, mismatched_index, iteration + 1);
  }

  gst_object_unref (pipeline);
  g_ptr_array_unref (initial_devices);
}

GST_END_TEST;

GST_START_TEST (test_avf_device_order_instability)
{
  const guint switch_iterations = 3;
  GstDeviceMonitor *monitor = create_started_avf_monitor ("Video/Source");
  GPtrArray *initial_devices = collect_avf_devices (monitor, "Video/Source");
  const AvfDeviceInfo *device_a;
  const AvfDeviceInfo *device_b;
  GstElement *pipeline, *src, *sink, *capsfilter;
  GstCaps *caps;
  guint attempt_index = 0;

  if (initial_devices->len < 2) {
    GST_WARNING ("Skipping AVF device-order instability test: need at least "
        "two AVF devices, found %u", initial_devices->len);
    g_ptr_array_unref (initial_devices);
    gst_device_monitor_stop (monitor);
    gst_object_unref (monitor);
    return;
  }

  gst_device_monitor_stop (monitor);
  g_clear_pointer (&monitor, gst_object_unref);

  GST_INFO ("Opening the first two AVF devices by unique-id across %u full "
      "iteration(s) while checking whether positions 0 and 1 keep mapping to "
      "the same devices after sample capture and a fresh device probe",
      switch_iterations);

  device_a = g_ptr_array_index (initial_devices, 0);
  device_b = g_ptr_array_index (initial_devices, 1);

  src = gst_element_factory_make ("avfvideosrc", NULL);
  fail_unless (src != NULL, "Could not create avfvideosrc");
  fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (src),
          "unique-id") != NULL, "AVF source does not expose unique-id");

  sink = gst_element_factory_make ("appsink", NULL);
  fail_unless (sink != NULL, "Could not create appsink");
  g_object_set (sink,
      "emit-signals", FALSE,
      "async", FALSE, "sync", FALSE, "max-buffers", 8u, "drop", TRUE, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  fail_unless (capsfilter != NULL, "Could not create capsfilter");
  caps = gst_caps_from_string ("video/x-raw,format=BGRA");
  fail_unless (caps != NULL, "Could not build BGRA caps");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  pipeline = gst_pipeline_new ("avfdeviceidentity-device-order");
  fail_unless (pipeline != NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  fail_unless (gst_element_link_many (src, capsfilter, sink, NULL));

  for (guint iteration = 0; iteration < switch_iterations; iteration++) {
    GPtrArray *current_devices;

    open_device_by_unique_id_and_check_unique_id (pipeline, src, sink,
        device_a, FALSE, -1, ++attempt_index);
    monitor = create_started_avf_monitor ("Video/Source");
    current_devices = collect_avf_devices (monitor, "Video/Source");
    gst_device_monitor_stop (monitor);
    g_clear_pointer (&monitor, gst_object_unref);
    fail_if_avf_device_mapping_changed (initial_devices, current_devices,
        attempt_index, device_a);
    g_ptr_array_unref (current_devices);

    open_device_by_unique_id_and_check_unique_id (pipeline, src, sink,
        device_b, FALSE, -1, ++attempt_index);
    monitor = create_started_avf_monitor ("Video/Source");
    current_devices = collect_avf_devices (monitor, "Video/Source");
    gst_device_monitor_stop (monitor);
    g_clear_pointer (&monitor, gst_object_unref);
    fail_if_avf_device_mapping_changed (initial_devices, current_devices,
        attempt_index, device_b);
    g_ptr_array_unref (current_devices);
  }

  gst_object_unref (pipeline);
  g_ptr_array_unref (initial_devices);
}

GST_END_TEST;

GST_START_TEST (test_avf_unique_id_stability)
{
  const guint switch_iterations = 3;
  GstDeviceMonitor *monitor = create_started_avf_monitor ("Video/Source");
  GPtrArray *initial_devices = collect_avf_devices (monitor, "Video/Source");
  AvfDeviceInfo *device_a;
  AvfDeviceInfo *device_b;
  GstElement *pipeline, *src, *sink, *capsfilter;
  GstCaps *caps;

  if (initial_devices->len < 1) {
    GST_WARNING
        ("Skipping AVF unique-id stability test: need at least one AVF device, found %u",
        initial_devices->len);
    g_ptr_array_unref (initial_devices);
    gst_device_monitor_stop (monitor);
    gst_object_unref (monitor);
    return;
  }

  gst_device_monitor_stop (monitor);
  g_clear_pointer (&monitor, gst_object_unref);

  device_a = g_ptr_array_index (initial_devices, 0);
  device_b = (initial_devices->len >= 2) ?
      g_ptr_array_index (initial_devices, 1) : device_a;

  if (device_a == device_b) {
    GST_INFO
        ("Opening single AVF device by unique-id with %u successive open iteration(s): %s [%s]",
        switch_iterations, GST_STR_NULL (device_a->display_name),
        device_a->unique_id);
  } else {
    GST_INFO
        ("Opening AVF devices by unique-id with %u alternating switch iteration(s): %s [%s], %s [%s]",
        switch_iterations, GST_STR_NULL (device_a->display_name),
        device_a->unique_id, GST_STR_NULL (device_b->display_name),
        device_b->unique_id);
  }

  src = gst_element_factory_make ("avfvideosrc", NULL);
  fail_unless (src != NULL, "Could not create avfvideosrc");
  fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (src),
          "unique-id") != NULL, "AVF source does not expose unique-id");

  sink = gst_element_factory_make ("appsink", NULL);
  fail_unless (sink != NULL, "Could not create appsink");
  g_object_set (sink,
      "emit-signals", FALSE,
      "async", FALSE, "sync", FALSE, "max-buffers", 8u, "drop", TRUE, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  fail_unless (capsfilter != NULL, "Could not create capsfilter");
  caps = gst_caps_from_string ("video/x-raw,format=BGRA");
  fail_unless (caps != NULL, "Could not build BGRA caps");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  pipeline = gst_pipeline_new ("avfdeviceidentity-unique-id");
  fail_unless (pipeline != NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  fail_unless (gst_element_link_many (src, capsfilter, sink, NULL));

  for (guint iteration = 0; iteration < switch_iterations; iteration++) {
    const AvfDeviceInfo *expected =
        ((iteration % 2) == 0) ? device_a : device_b;
    open_device_by_unique_id_and_check_unique_id (pipeline, src, sink,
        expected, FALSE, -1, iteration + 1);
  }

  gst_object_unref (pipeline);
  g_ptr_array_unref (initial_devices);
}

GST_END_TEST;

GST_START_TEST (test_avf_screen_provider_configuration)
{
  GstDeviceMonitor *monitor;
  GList *devices, *iter;
  gboolean found_screen = FALSE;

  monitor = create_started_avf_monitor ("Source/Monitor");
  devices = gst_device_monitor_get_devices (monitor);

  for (iter = devices; iter != NULL; iter = g_list_next (iter)) {
    GstDevice *device = GST_DEVICE (iter->data);
    GstStructure *props = gst_device_get_properties (device);
    const gchar *unique_id;
    gboolean capture_screen = FALSE;
    guint64 display_id = 0;
    gchar *device_unique_id = NULL;
    gchar *configured_unique_id = NULL;
    GstElement *element;

    fail_unless (props != NULL, "Screen device is missing properties");

    unique_id = gst_structure_get_string (props, "avf.unique_id");
    fail_unless (unique_id != NULL && *unique_id != '\0',
        "Screen device is missing avf.unique_id");
    fail_unless (gst_structure_get_boolean (props, "avf.capture_screen",
            &capture_screen) && capture_screen,
        "Screen device is missing avf.capture_screen");
    fail_unless (gst_structure_get_uint64 (props, "avf.display_id", &display_id)
        && display_id != 0, "Screen device is missing avf.display_id");

    g_object_get (device, "unique-id", &device_unique_id, NULL);
    fail_unless_equals_string (device_unique_id, unique_id);

    element = gst_device_create_element (device, NULL);
    fail_unless (element != NULL,
        "Could not create element from screen device %s",
        GST_STR_NULL (gst_device_get_display_name (device)));

    g_object_get (element,
        "capture-screen", &capture_screen, "unique-id", &configured_unique_id,
        NULL);
    fail_unless (capture_screen,
        "Screen device element was not configured for screen capture");
    fail_unless_equals_string (configured_unique_id, unique_id);

    gst_object_unref (element);
    g_clear_pointer (&configured_unique_id, g_free);
    g_clear_pointer (&device_unique_id, g_free);
    g_clear_pointer (&props, gst_structure_free);
    found_screen = TRUE;
  }

  if (!found_screen) {
    GST_WARNING
        ("Skipping AVF screen provider configuration test: no screens found");
  }

  g_list_free_full (devices, gst_object_unref);
  gst_device_monitor_stop (monitor);
  gst_object_unref (monitor);
}

GST_END_TEST;

static Suite *
avfdeviceidentity_suite (void)
{
  Suite *s = suite_create ("avfdeviceidentity");
  TCase *tc_basic = tcase_create ("device_index");
  const gchar *enable_device_index_stress =
      g_getenv ("GST_AVF_ENABLE_DEVICE_INDEX_STRESS");

  suite_add_tcase (s, tc_basic);
  if (!running_in_ci ()) {
    tcase_add_test (tc_basic, test_avf_unique_id_stability);
  } else {
    GST_INFO ("Not registering AVF unique-id stability test on CI");
  }
  tcase_add_test (tc_basic, test_avf_screen_provider_configuration);
  if (g_strcmp0 (enable_device_index_stress, "1") == 0) {
    tcase_add_test (tc_basic, test_avf_device_order_instability);
    tcase_add_test (tc_basic, test_avf_device_index_compatibility);
  } else {
    GST_INFO
        ("Not registering AVF device-order/deprecated-compatibility tests; "
        "set GST_AVF_ENABLE_DEVICE_INDEX_STRESS=1 to enable them");
  }

  return s;
}

static int
run_tests ()
{
  Suite *s = avfdeviceidentity_suite ();
  return gst_check_run_suite_nofork (s, "avfdeviceidentity", __FILE__);
}

int
main (int argc, char **argv)
{
  gst_check_init (&argc, &argv);
#if TARGET_OS_OSX
  return gst_macos_main_simple ((GstMainFuncSimple) run_tests, NULL);
#else
  return run_tests (argc, argv, NULL);
#endif
}
