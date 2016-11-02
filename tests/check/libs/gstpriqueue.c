/* GStreamer
 * Copyright (C) 2016 Pexip AS
 *               Erlend Graff <erlend@pexip.com>
 *
 * gstpriqueue.c: unit tests for GstPriQueue
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

#include <gst/gstmacros.h>
#include <gst/base/gstpriqueue.h>
#include <gst/check/gstcheck.h>

struct item_data
{
  gint key;
  gint inserted;
  GstPriQueueElem pq_elem;
};

#define ELEM_TO_ITEM(elem) (GST_CONTAINER_OF (elem, struct item_data, pq_elem))

static gint
compare_items (const GstPriQueueElem * elem_a, const GstPriQueueElem * elem_b,
    G_GNUC_UNUSED gpointer user_data)
{
  return ELEM_TO_ITEM (elem_a)->key - ELEM_TO_ITEM (elem_b)->key;
}

static gint
compare_items_with_check (const GstPriQueueElem * elem_a,
    const GstPriQueueElem * elem_b, gpointer user_data)
{
  fail_unless_equals_int (-12345, GPOINTER_TO_INT (user_data));
  return compare_items (elem_a, elem_b, NULL);
}

GST_START_TEST (test_random_modifications)
{
  const gint num_items = 1023;
  const gint num_iter = 500;
  const gint key_min = -1000;
  const gint key_max = 1000;
  struct item_data *items;
  GstPriQueueElem *lookup_elem;
  GstPriQueue *pq;
  GRand *rand;
  gint i, minidx, idx, num_inserted;

  rand = g_rand_new_with_seed (0);
  items = g_new0 (struct item_data, num_items);

  pq = gst_pri_queue_create (compare_items_with_check,
      GINT_TO_POINTER (-12345));

  num_inserted = 0;
  for (i = 0; i < num_iter; i++) {
    /* Ensure valid */
    fail_unless (gst_pri_queue_is_valid (pq));

    /* Test get_min and size */

    fail_unless_equals_int (num_inserted, gst_pri_queue_size (pq));

    minidx = -1;
    lookup_elem = gst_pri_queue_get_min (pq);
    fail_unless_equals_int (! !num_inserted, ! !lookup_elem);

    if (num_inserted) {
      minidx = ELEM_TO_ITEM (lookup_elem) - items;
      for (idx = 0; idx < num_items; idx++) {
        /* There may be multiple "smallest" elements (i.e. other elements may
         * have the same priority/key as the one we just retrieved), hence the
         * use of `>=`.
         */
        if (items[idx].inserted)
          fail_unless (items[idx].key >= items[minidx].key);
      }
    }

    /* Test insert, remove, and update */

    idx = g_rand_int_range (rand, 0, num_items);

    if (!items[idx].inserted) {
      /* Just to prevent insertions from dominating the types of operations
       * performed.
       */
      if (g_rand_int_range (rand, 0, 5) != 0) {
        i--;
        continue;
      }

      items[idx].key = g_rand_int_range (rand, key_min, key_max);
      gst_pri_queue_insert (pq, &items[idx].pq_elem);
      items[idx].inserted = TRUE;
      num_inserted++;
    } else {
      switch (g_rand_int_range (rand, 0, 4)) {
        case 0:
          gst_pri_queue_remove (pq, &items[idx].pq_elem);
          items[idx].inserted = FALSE;
          num_inserted--;
          break;
        case 1:
          lookup_elem = gst_pri_queue_pop_min (pq);
          fail_unless_equals_pointer (&items[minidx].pq_elem, lookup_elem);
          items[minidx].inserted = FALSE;
          num_inserted--;
          break;
        case 2:
          gst_pri_queue_remove (pq, &items[idx].pq_elem);
          items[idx].key = g_rand_int_range (rand, key_min, key_max);
          gst_pri_queue_insert (pq, &items[idx].pq_elem);
          break;
        case 3:
          items[idx].key = g_rand_int_range (rand, key_min, key_max);
          gst_pri_queue_update (pq, &items[idx].pq_elem);
          break;
        default:
          g_assert_not_reached ();
          break;
      }
    }
  }

  while (TRUE) {
    /* Ensure valid */
    fail_unless (gst_pri_queue_is_valid (pq));

    /* Test get_min and size */

    fail_unless_equals_int (num_inserted, gst_pri_queue_size (pq));

    lookup_elem = gst_pri_queue_get_min (pq);
    fail_unless_equals_int (! !num_inserted, ! !lookup_elem);

    if (num_inserted) {
      minidx = ELEM_TO_ITEM (lookup_elem) - items;
      for (idx = 0; idx < num_items; idx++) {
        /* There may be multiple "smallest" elements (i.e. other elements may
         * have the same priority/key as the one we just retrieved), hence the
         * use of `>=`.
         */
        if (items[idx].inserted)
          fail_unless (items[idx].key >= items[minidx].key);
      }
    }

    if (!num_inserted)
      break;

    /* Test remove and update */

    /* Find random inserted item */
    idx = g_rand_int_range (rand, 0, num_items);
    while (!items[idx].inserted)
      idx = (idx + 1) % num_items;

    switch (g_rand_int_range (rand, 0, 2)) {
      case 0:
        gst_pri_queue_remove (pq, &items[idx].pq_elem);
        items[idx].inserted = FALSE;
        num_inserted--;
        break;
      case 1:
        items[idx].key = g_rand_int_range (rand, key_min, key_max);
        gst_pri_queue_update (pq, &items[idx].pq_elem);
        break;
      default:
        g_assert_not_reached ();
        break;
    }
  }

  gst_pri_queue_destroy (pq, NULL);

  g_free (items);
  g_rand_free (rand);
}

GST_END_TEST;

#define NUM_ITEMS 127

static gint
compare_pointers (const GstPriQueueElem * elem_a,
    const GstPriQueueElem * elem_b, G_GNUC_UNUSED gpointer user_data)
{
  return elem_a - elem_b;
}

GST_START_TEST (test_sorted_insertion)
{
  GstPriQueueElem *elems;
  GstPriQueue *pq;
  gint i;

  elems = g_new (GstPriQueueElem, NUM_ITEMS);

  pq = gst_pri_queue_create (compare_pointers, NULL);
  for (i = 0; i < NUM_ITEMS; i++) {
    gst_pri_queue_insert (pq, &elems[i]);
    fail_unless_equals_int (i + 1, gst_pri_queue_size (pq));
    fail_unless_equals_pointer (&elems[0], gst_pri_queue_get_min (pq));
    fail_unless (gst_pri_queue_is_valid (pq));
  }
  gst_pri_queue_destroy (pq, NULL);

  pq = gst_pri_queue_create (compare_pointers, NULL);
  for (i = NUM_ITEMS - 1; i >= 0; i--) {
    gst_pri_queue_insert (pq, &elems[i]);
    fail_unless_equals_int (NUM_ITEMS - i, gst_pri_queue_size (pq));
    fail_unless_equals_pointer (&elems[i], gst_pri_queue_get_min (pq));
    fail_unless (gst_pri_queue_is_valid (pq));
  }
  gst_pri_queue_destroy (pq, NULL);

  g_free (elems);
}

GST_END_TEST;

GST_START_TEST (test_sorted_removal)
{
  GstPriQueueElem *elems;
  GstPriQueue *pq;
  gint i;

  elems = g_new (GstPriQueueElem, NUM_ITEMS);
  pq = gst_pri_queue_create (compare_pointers, NULL);

  for (i = 0; i < NUM_ITEMS; i++)
    gst_pri_queue_insert (pq, &elems[i]);

  for (i = 0; i < NUM_ITEMS; i++) {
    fail_unless_equals_pointer (&elems[i], gst_pri_queue_get_min (pq));
    gst_pri_queue_remove (pq, &elems[i]);
    fail_unless_equals_int (NUM_ITEMS - 1 - i, gst_pri_queue_size (pq));
    fail_unless (gst_pri_queue_is_valid (pq));
  }

  fail_unless_equals_pointer (NULL, gst_pri_queue_get_min (pq));

  for (i = 0; i < NUM_ITEMS; i++)
    gst_pri_queue_insert (pq, &elems[i]);

  for (i = NUM_ITEMS - 1; i >= 0; i--) {
    fail_unless_equals_pointer (&elems[0], gst_pri_queue_get_min (pq));
    gst_pri_queue_remove (pq, &elems[i]);
    fail_unless_equals_int (i, gst_pri_queue_size (pq));
    fail_unless (gst_pri_queue_is_valid (pq));
  }

  fail_unless_equals_pointer (NULL, gst_pri_queue_get_min (pq));

  gst_pri_queue_destroy (pq, NULL);
  g_free (elems);
}

GST_END_TEST;

GST_START_TEST (test_pop_min)
{
  GstPriQueueElem *elems;
  GstPriQueue *pq;
  gint i;

  elems = g_new (GstPriQueueElem, NUM_ITEMS);
  pq = gst_pri_queue_create (compare_pointers, NULL);

  for (i = 0; i < NUM_ITEMS; i++)
    gst_pri_queue_insert (pq, &elems[i]);

  for (i = 0; i < NUM_ITEMS; i++) {
    fail_unless_equals_pointer (&elems[i], gst_pri_queue_pop_min (pq));
    fail_unless_equals_int (NUM_ITEMS - 1 - i, gst_pri_queue_size (pq));
    fail_unless (gst_pri_queue_is_valid (pq));
  }

  fail_unless_equals_pointer (NULL, gst_pri_queue_pop_min (pq));

  gst_pri_queue_destroy (pq, NULL);
  g_free (elems);
}

GST_END_TEST;

static void
increase_inserted (gpointer elem)
{
  struct item_data *item = ELEM_TO_ITEM (elem);
  item->inserted += 1;
}

GST_START_TEST (test_destroy)
{
  struct item_data *items;
  GstPriQueue *pq;
  gint i;

  items = g_new (struct item_data, NUM_ITEMS);

  pq = gst_pri_queue_create (compare_items, NULL);
  for (i = 0; i < NUM_ITEMS; i++) {
    /* Note: we abuse the `inserted` field to keep track of how many times each
     * the destroy func is called for each element.
     */
    items[i].inserted = 0;
    items[i].key = i;
    gst_pri_queue_insert (pq, &items[i].pq_elem);
  }

  gst_pri_queue_destroy (pq, increase_inserted);

  for (i = 0; i < NUM_ITEMS; i++) {
    /* Each element should be visited by destroy func exactly once */
    fail_unless_equals_int (1, items[i].inserted);
  }

  g_free (items);
}

GST_END_TEST;

GST_START_TEST (test_meld)
{
  GstPriQueueElem *elems;
  GstPriQueue *pqa, *pqb, *pq_meld;
  gint i;

  elems = g_new (GstPriQueueElem, NUM_ITEMS);
  pqa = gst_pri_queue_create (compare_pointers, NULL);
  pqb = gst_pri_queue_create (compare_pointers, NULL);

  for (i = 0; i < NUM_ITEMS / 2; i++)
    gst_pri_queue_insert (pqa, &elems[i]);

  for (; i < NUM_ITEMS; i++)
    gst_pri_queue_insert (pqb, &elems[i]);

  pq_meld = gst_pri_queue_meld (pqa, pqb);
  fail_unless_equals_pointer (pqa, pq_meld);

  fail_unless_equals_int (NUM_ITEMS, gst_pri_queue_size (pqa));
  fail_unless (gst_pri_queue_is_valid (pqa));

  for (i = 0; i < NUM_ITEMS; i++) {
    fail_unless_equals_pointer (&elems[i], gst_pri_queue_pop_min (pqa));
    fail_unless_equals_int (NUM_ITEMS - 1 - i, gst_pri_queue_size (pqa));
    fail_unless (gst_pri_queue_is_valid (pqa));
  }

  gst_pri_queue_destroy (pqa, NULL);

  g_free (elems);
}

GST_END_TEST;

GST_START_TEST (test_iter)
{
  struct item_data *items;
  GstPriQueueElem *elem;
  GstPriQueueIter iter;
  GstPriQueue *pq;
  gint i;

  items = g_new0 (struct item_data, NUM_ITEMS);
  pq = gst_pri_queue_create (compare_items, NULL);

  for (i = 0; i < NUM_ITEMS; i++)
    gst_pri_queue_insert (pq, &items[i].pq_elem);

  gst_pri_queue_iter_init (&iter, pq);
  while (gst_pri_queue_iter_next (&iter, &elem))
    ELEM_TO_ITEM (elem)->inserted += 1;

  for (i = 0; i < NUM_ITEMS; i++)
    fail_unless_equals_int (1, items[i].inserted);

  gst_pri_queue_destroy (pq, NULL);
  g_free (items);
}

GST_END_TEST;

static Suite *
gst_pri_queue_suite (void)
{
  Suite *s = suite_create ("GstPriQueue");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, test_random_modifications);
  tcase_add_test (tc_chain, test_sorted_insertion);
  tcase_add_test (tc_chain, test_sorted_removal);
  tcase_add_test (tc_chain, test_pop_min);
  tcase_add_test (tc_chain, test_destroy);
  tcase_add_test (tc_chain, test_meld);
  tcase_add_test (tc_chain, test_iter);

  return s;
}

GST_CHECK_MAIN (gst_pri_queue);
