/* GStreamer unit tests for the funnel
 *
 * Copyright (C) 2008 Collabora, Nokia
 * @author: Olivier Crete <olivier.crete@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
*/


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gst/check/gstharness.h>
#include <gst/check/gstcheck.h>

struct TestData
{
  GstElement *funnel;
  GstPad *funnelsrc, *funnelsink11, *funnelsink22;
  GstPad *mysink, *mysrc1, *mysrc2;
  GstCaps *mycaps;
};

static void
setup_test_objects (struct TestData *td, GstPadChainFunction chain_func)
{
  td->mycaps = gst_caps_new_empty_simple ("test/test");

  td->funnel = gst_element_factory_make ("funnel", NULL);

  td->funnelsrc = gst_element_get_static_pad (td->funnel, "src");
  fail_unless (td->funnelsrc != NULL);

  td->funnelsink11 = gst_element_request_pad_simple (td->funnel, "sink_11");
  fail_unless (td->funnelsink11 != NULL);
  fail_unless (!strcmp (GST_OBJECT_NAME (td->funnelsink11), "sink_11"));

  td->funnelsink22 = gst_element_request_pad_simple (td->funnel, "sink_22");
  fail_unless (td->funnelsink22 != NULL);
  fail_unless (!strcmp (GST_OBJECT_NAME (td->funnelsink22), "sink_22"));

  fail_unless (gst_element_set_state (td->funnel, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_SUCCESS);

  td->mysink = gst_pad_new ("sink", GST_PAD_SINK);
  gst_pad_set_chain_function (td->mysink, chain_func);
  gst_pad_set_active (td->mysink, TRUE);

  td->mysrc1 = gst_pad_new ("src1", GST_PAD_SRC);
  gst_pad_set_active (td->mysrc1, TRUE);
  gst_check_setup_events_with_stream_id (td->mysrc1, td->funnel, td->mycaps,
      GST_FORMAT_BYTES, "test1");

  td->mysrc2 = gst_pad_new ("src2", GST_PAD_SRC);
  gst_pad_set_active (td->mysrc2, TRUE);
  gst_check_setup_events_with_stream_id (td->mysrc2, td->funnel, td->mycaps,
      GST_FORMAT_BYTES, "test2");

  fail_unless (GST_PAD_LINK_SUCCESSFUL (gst_pad_link (td->funnelsrc,
              td->mysink)));

  fail_unless (GST_PAD_LINK_SUCCESSFUL (gst_pad_link (td->mysrc1,
              td->funnelsink11)));

  fail_unless (GST_PAD_LINK_SUCCESSFUL (gst_pad_link (td->mysrc2,
              td->funnelsink22)));

}

static void
release_test_objects (struct TestData *td)
{
  gst_pad_set_active (td->mysink, FALSE);
  gst_pad_set_active (td->mysrc1, FALSE);
  gst_pad_set_active (td->mysrc1, FALSE);

  gst_object_unref (td->mysink);
  gst_object_unref (td->mysrc1);
  gst_object_unref (td->mysrc2);

  fail_unless (gst_element_set_state (td->funnel, GST_STATE_NULL) ==
      GST_STATE_CHANGE_SUCCESS);

  gst_object_unref (td->funnelsrc);
  gst_element_release_request_pad (td->funnel, td->funnelsink11);
  gst_object_unref (td->funnelsink11);
  gst_element_release_request_pad (td->funnel, td->funnelsink22);
  gst_object_unref (td->funnelsink22);

  gst_caps_unref (td->mycaps);
  gst_object_unref (td->funnel);
}

static gint bufcount = 0;
static gint alloccount = 0;

static GstFlowReturn
chain_ok (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstObject * parent,
    GstBuffer * buffer)
{
  bufcount++;

  gst_buffer_unref (buffer);

  return GST_FLOW_OK;
}

GST_START_TEST (test_funnel_simple)
{
  struct TestData td;

  setup_test_objects (&td, chain_ok);

  bufcount = 0;
  alloccount = 0;

  fail_unless (gst_pad_push (td.mysrc1, gst_buffer_new ()) == GST_FLOW_OK);
  fail_unless (gst_pad_push (td.mysrc2, gst_buffer_new ()) == GST_FLOW_OK);

  fail_unless (bufcount == 2);

  release_test_objects (&td);
}

GST_END_TEST;

guint num_eos = 0;

static gboolean
eos_event_func (GstPad * pad, GstObject * parent, GstEvent * event)
{
  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS)
    ++num_eos;

  return gst_pad_event_default (pad, parent, event);
}

GST_START_TEST (test_funnel_eos)
{
  struct TestData td;
  GstSegment segment;

  setup_test_objects (&td, chain_ok);

  num_eos = 0;
  bufcount = 0;

  gst_pad_set_event_function (td.mysink, eos_event_func);

  fail_unless (gst_pad_push (td.mysrc1, gst_buffer_new ()) == GST_FLOW_OK);
  fail_unless (gst_pad_push (td.mysrc2, gst_buffer_new ()) == GST_FLOW_OK);

  fail_unless (bufcount == 2);

  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_eos ()));
  fail_unless (num_eos == 0);

  fail_unless (gst_pad_push (td.mysrc1, gst_buffer_new ()) == GST_FLOW_EOS);
  fail_unless (gst_pad_push (td.mysrc2, gst_buffer_new ()) == GST_FLOW_OK);

  fail_unless (bufcount == 3);

  fail_unless (gst_pad_push_event (td.mysrc2, gst_event_new_eos ()));
  fail_unless (num_eos == 1);

  fail_unless (gst_pad_push (td.mysrc1, gst_buffer_new ()) == GST_FLOW_EOS);
  fail_unless (gst_pad_push (td.mysrc2, gst_buffer_new ()) == GST_FLOW_EOS);

  fail_unless (bufcount == 3);

  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_flush_start ()));
  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_flush_stop (TRUE)));

  gst_segment_init (&segment, GST_FORMAT_BYTES);
  gst_pad_push_event (td.mysrc1, gst_event_new_segment (&segment));
  gst_pad_push_event (td.mysrc2, gst_event_new_segment (&segment));

  fail_unless (gst_pad_push (td.mysrc1, gst_buffer_new ()) == GST_FLOW_OK);
  fail_unless (gst_pad_push (td.mysrc2, gst_buffer_new ()) == GST_FLOW_EOS);

  fail_unless (bufcount == 4);

  fail_unless (gst_pad_unlink (td.mysrc1, td.funnelsink11));
  gst_element_release_request_pad (td.funnel, td.funnelsink11);
  gst_object_unref (td.funnelsink11);
  fail_unless (num_eos == 2);

  td.funnelsink11 = gst_element_request_pad_simple (td.funnel, "sink_11");
  fail_unless (td.funnelsink11 != NULL);
  fail_unless (!strcmp (GST_OBJECT_NAME (td.funnelsink11), "sink_11"));

  fail_unless (GST_PAD_LINK_SUCCESSFUL (gst_pad_link (td.mysrc1,
              td.funnelsink11)));

  /* This will fail because everything is EOS already */
  fail_if (gst_pad_push_event (td.mysrc1, gst_event_new_eos ()));
  fail_unless (num_eos == 2);

  fail_unless (gst_pad_unlink (td.mysrc1, td.funnelsink11));
  gst_element_release_request_pad (td.funnel, td.funnelsink11);
  gst_object_unref (td.funnelsink11);
  fail_unless (num_eos == 2);

  /* send only eos to check, it handles empty streams */
  td.funnelsink11 = gst_element_request_pad_simple (td.funnel, "sink_11");
  fail_unless (td.funnelsink11 != NULL);
  fail_unless (!strcmp (GST_OBJECT_NAME (td.funnelsink11), "sink_11"));

  fail_unless (GST_PAD_LINK_SUCCESSFUL (gst_pad_link (td.mysrc1,
              td.funnelsink11)));

  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_flush_start ()));
  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_flush_stop (TRUE)));
  fail_unless (gst_pad_push_event (td.mysrc2, gst_event_new_flush_start ()));
  fail_unless (gst_pad_push_event (td.mysrc2, gst_event_new_flush_stop (TRUE)));

  fail_unless (gst_pad_push_event (td.mysrc1, gst_event_new_eos ()));
  fail_unless (gst_pad_push_event (td.mysrc2, gst_event_new_eos ()));
  fail_unless (num_eos == 3);

  fail_unless (gst_pad_unlink (td.mysrc1, td.funnelsink11));
  gst_element_release_request_pad (td.funnel, td.funnelsink11);
  gst_object_unref (td.funnelsink11);
  fail_unless (num_eos == 3);

  td.funnelsink11 = gst_element_request_pad_simple (td.funnel, "sink_11");
  fail_unless (td.funnelsink11 != NULL);
  fail_unless (!strcmp (GST_OBJECT_NAME (td.funnelsink11), "sink_11"));

  release_test_objects (&td);
}

GST_END_TEST;

GST_START_TEST (test_funnel_stress)
{
  GstHarness *h0 = gst_harness_new_with_padnames ("funnel", "sink_0", "src");
  GstHarness *h1 = gst_harness_new_with_element (h0->element, "sink_1", NULL);
  GstHarnessThread *req, *push0, *push1;
  GstPadTemplate *templ =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (h0->element),
      "sink_%u");
  GstCaps *caps = gst_caps_from_string ("testcaps");
  GstBuffer *buf = gst_buffer_new ();
  GstSegment segment;

  gst_segment_init (&segment, GST_FORMAT_TIME);

  req = gst_harness_stress_requestpad_start (h0, templ, NULL, NULL, TRUE);
  push0 = gst_harness_stress_push_buffer_start (h0, caps, &segment, buf);
  push1 = gst_harness_stress_push_buffer_start (h1, caps, &segment, buf);

  gst_caps_unref (caps);
  gst_buffer_unref (buf);

  /* test-length */
  g_usleep (G_USEC_PER_SEC * 1);

  gst_harness_stress_thread_stop (push1);
  gst_harness_stress_thread_stop (push0);
  gst_harness_stress_thread_stop (req);

  gst_harness_teardown (h1);
  gst_harness_teardown (h0);
}

GST_END_TEST;

GST_START_TEST (test_funnel_event_handling)
{
  GstHarness *h, *h0, *h1;
  GstEvent *event;
  GstCaps *mycaps0, *mycaps1, *caps;

  h = gst_harness_new_with_padnames ("funnel", NULL, "src");

  /* request a sinkpad, with some caps */
  h0 = gst_harness_new_with_element (h->element, "sink_0", NULL);
  mycaps0 = gst_caps_new_empty_simple ("mycaps0");
  gst_harness_set_src_caps (h0, gst_caps_ref (mycaps0));

  /* request a second sinkpad, also with caps */
  h1 = gst_harness_new_with_element (h->element, "sink_1", NULL);
  mycaps1 = gst_caps_new_empty_simple ("mycaps1");
  gst_harness_set_src_caps (h1, gst_caps_ref (mycaps1));

  /* push a buffer on the first pad */
  fail_unless_equals_int (GST_FLOW_OK,
      gst_harness_push (h0, gst_buffer_new ()));
  gst_buffer_unref (gst_harness_pull (h));

  /* verify stream-start */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_STREAM_START);
  gst_event_unref (event);

  /* verify caps "mycaps0" */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_CAPS);
  gst_event_parse_caps (event, &caps);
  gst_check_caps_equal (mycaps0, caps);
  gst_caps_unref (caps);
  gst_event_unref (event);

  /* verify segment */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_SEGMENT);
  gst_event_unref (event);

  /* verify a custom event pushed on second pad does not make it through,
     since active pad is the first pad */
  fail_unless (gst_harness_push_event (h1,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
              gst_structure_new_empty ("test"))));
  fail_if (gst_harness_try_pull_event (h));

  /* but same event on the first pad will make it through */
  fail_unless (gst_harness_push_event (h0,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
              gst_structure_new_empty ("test"))));
  gst_event_unref (gst_harness_pull_event (h));

  /* push a buffer on the second pad */
  fail_unless_equals_int (GST_FLOW_OK,
      gst_harness_push (h1, gst_buffer_new ()));
  gst_buffer_unref (gst_harness_pull (h));

  /* verify stream-start */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_STREAM_START);
  gst_event_unref (event);

  /* verify caps "mycaps1" */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_CAPS);
  gst_event_parse_caps (event, &caps);
  gst_check_caps_equal (mycaps1, caps);
  gst_caps_unref (caps);
  gst_event_unref (event);

  /* verify segment */
  event = gst_harness_pull_event (h);
  fail_unless_equals_int (GST_EVENT_TYPE (event), GST_EVENT_SEGMENT);
  gst_event_unref (event);

  /* verify a custom event pushed on first pad does not make it through,
     since active pad is the second pad */
  fail_unless (gst_harness_push_event (h0,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
              gst_structure_new_empty ("test"))));
  fail_if (gst_harness_try_pull_event (h));

  /* but same event on the second pad will make it through */
  fail_unless (gst_harness_push_event (h1,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
              gst_structure_new_empty ("test"))));
  gst_event_unref (gst_harness_pull_event (h));

  gst_harness_teardown (h);
  gst_harness_teardown (h0);
  gst_harness_teardown (h1);
}

GST_END_TEST;

GST_START_TEST (test_funnel_custom_sticky)
{
  GstHarness *h, *h0, *h1;
  GstEvent *event;
  const GstStructure *s;
  const gchar *value = NULL;

  h = gst_harness_new_with_padnames ("funnel", NULL, "src");

  /* request a sinkpad, with some caps */
  h0 = gst_harness_new_with_element (h->element, "sink_0", NULL);
  gst_harness_set_src_caps_str (h0, "mycaps0");

  /* request a second sinkpad, also with caps */
  h1 = gst_harness_new_with_element (h->element, "sink_1", NULL);
  gst_harness_set_src_caps_str (h1, "mycaps1");

  while ((event = gst_harness_try_pull_event (h)))
    gst_event_unref (event);

  fail_unless (gst_harness_push_event (h0,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM_STICKY,
              gst_structure_new ("test", "key", G_TYPE_STRING, "value0",
                  NULL))));

  fail_unless (gst_harness_push_event (h1,
          gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM_STICKY,
              gst_structure_new ("test", "key", G_TYPE_STRING, "value1",
                  NULL))));

  /* Send a buffer through first pad, expect the event to be the first one */
  fail_unless_equals_int (GST_FLOW_OK,
      gst_harness_push (h0, gst_buffer_new ()));
  for (;;) {
    event = gst_harness_pull_event (h);
    if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_DOWNSTREAM_STICKY)
      break;
    gst_event_unref (event);
  }
  s = gst_event_get_structure (event);
  fail_unless (s);
  fail_unless (gst_structure_has_name (s, "test"));
  value = gst_structure_get_string (s, "key");
  fail_unless_equals_string (value, "value0");
  gst_event_unref (event);
  gst_buffer_unref (gst_harness_pull (h));

  /* Send a buffer through second pad, expect the event to be the second one
   */
  fail_unless_equals_int (GST_FLOW_OK,
      gst_harness_push (h1, gst_buffer_new ()));
  for (;;) {
    event = gst_harness_pull_event (h);
    if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_DOWNSTREAM_STICKY)
      break;
    gst_event_unref (event);
  }
  s = gst_event_get_structure (event);
  fail_unless (s);
  fail_unless (gst_structure_has_name (s, "test"));
  value = gst_structure_get_string (s, "key");
  fail_unless_equals_string (value, "value1");
  gst_event_unref (event);
  gst_buffer_unref (gst_harness_pull (h));

  /* Send a buffer through first pad, expect the event to again be the first
   * one
   */
  fail_unless_equals_int (GST_FLOW_OK,
      gst_harness_push (h0, gst_buffer_new ()));
  for (;;) {
    event = gst_harness_pull_event (h);
    if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_DOWNSTREAM_STICKY)
      break;
    gst_event_unref (event);
  }
  s = gst_event_get_structure (event);
  fail_unless (s);
  fail_unless (gst_structure_has_name (s, "test"));
  value = gst_structure_get_string (s, "key");
  fail_unless_equals_string (value, "value0");
  gst_event_unref (event);
  gst_buffer_unref (gst_harness_pull (h));

  gst_harness_teardown (h);
  gst_harness_teardown (h0);
  gst_harness_teardown (h1);
}

GST_END_TEST;


static Suite *
funnel_suite (void)
{
  Suite *s = suite_create ("funnel");
  TCase *tc_chain;

  tc_chain = tcase_create ("funnel simple");
  tcase_add_test (tc_chain, test_funnel_simple);
  tcase_add_test (tc_chain, test_funnel_eos);
  tcase_add_test (tc_chain, test_funnel_stress);
  tcase_add_test (tc_chain, test_funnel_event_handling);
  tcase_add_test (tc_chain, test_funnel_custom_sticky);
  suite_add_tcase (s, tc_chain);

  return s;
}

GST_CHECK_MAIN (funnel);
