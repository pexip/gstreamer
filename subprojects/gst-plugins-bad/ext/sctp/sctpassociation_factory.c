/*
 * Copyright (c) 2023, Pexip AS
 *  @author: Tulio Beloqui <tulio@pexip.com>
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sctpassociation_factory.h"

#include <gst/gst.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

GST_DEBUG_CATEGORY_STATIC (gst_sctp_association_factory_debug_category);
#define GST_CAT_DEFAULT gst_sctp_association_factory_debug_category

G_LOCK_DEFINE_STATIC (associations_lock);

static GHashTable *associations_by_id = NULL;
static GHashTable *associations_refs = NULL;

static GMainContext *main_context = NULL;
static GMainLoop *main_loop = NULL;
static GThread *main_loop_thread = NULL;

static GMutex unref_mutex;
static GCond unref_cond;
static gboolean unref_called;


static gpointer
gst_sctp_association_factory_main_loop_run (gpointer data)
{
  g_main_loop_run (main_loop);
  return NULL;
}

static gboolean
gst_sctp_association_factory_main_loop_quit (gpointer data)
{
  g_main_loop_quit (main_loop);
  return FALSE;
}

static void
gst_sctp_association_factory_init (void)
{
  g_assert (associations_refs == NULL);
  g_assert (main_loop == NULL);

  static gsize initialized = 0;
  if (g_once_init_enter (&initialized)) {
    gsize _initialized = 1;
    GST_DEBUG_CATEGORY_INIT (gst_sctp_association_factory_debug_category,
        "sctpassociationfactory", 0,
        "debug category for sctpassociation factory");
    g_once_init_leave (&initialized, _initialized);
  }

  g_mutex_init (&unref_mutex);
  g_cond_init (&unref_cond);

  associations_by_id =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
  associations_refs =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) g_free);

  main_context = g_main_context_new ();
  main_loop = g_main_loop_new (main_context, FALSE);
  main_loop_thread =
      g_thread_new ("sctp-timer", gst_sctp_association_factory_main_loop_run,
      NULL);
}

static void
gst_sctp_association_factory_deinit (void)
{
  /* demand all association have ben released */
  g_assert (g_hash_table_size (associations_refs) == 0);

  g_hash_table_destroy (associations_by_id);
  g_hash_table_destroy (associations_refs);
  associations_by_id = NULL;
  associations_refs = NULL;


  /* kill main loop and ctx */
  GSource *source = g_idle_source_new ();
  g_source_set_callback (source, gst_sctp_association_factory_main_loop_quit,
      NULL, NULL);
  g_source_attach (source, main_context);
  g_source_unref (source);
  g_thread_join (main_loop_thread);
  g_main_loop_unref (main_loop);
  g_main_context_unref (main_context);
  main_loop_thread = NULL;
  main_context = NULL;
  main_loop = NULL;

  g_mutex_clear (&unref_mutex);
  g_cond_clear (&unref_cond);
}

static gboolean
gst_sctp_association_factory_unref_in_main_loop (GstSctpAssociation * assoc)
{
  g_object_unref (assoc);

  g_mutex_lock (&unref_mutex);
  unref_called = TRUE;
  g_cond_signal (&unref_cond);
  g_mutex_unlock (&unref_mutex);

  return FALSE;
}

static void
gst_sctp_association_factory_association_ref (GstSctpAssociation * assoc)
{
  guint32 association_id = assoc->association_id;
  gint *ref_count = g_hash_table_lookup (associations_refs,
      GUINT_TO_POINTER (association_id));
  if (!ref_count) {
    ref_count = g_new0 (gint, 1);
    g_hash_table_insert (associations_refs, GUINT_TO_POINTER (association_id),
        (gpointer) ref_count);
  }

  g_atomic_int_inc (ref_count);
}

static void
gst_sctp_association_factory_association_unref (GstSctpAssociation * assoc)
{
  guint32 association_id = assoc->association_id;
  gint *ref_count = g_hash_table_lookup (associations_refs,
      GUINT_TO_POINTER (association_id));
  g_assert (ref_count);

  if (g_atomic_int_dec_and_test (ref_count)) {
    g_hash_table_remove (associations_by_id, GUINT_TO_POINTER (association_id));
    g_hash_table_remove (associations_refs, GUINT_TO_POINTER (association_id));
  }
}


/* PUBLIC */

GstSctpAssociation *
gst_sctp_association_factory_get (guint32 association_id)
{
  GstSctpAssociation *assoc;

  G_LOCK (associations_lock);

  if (!associations_by_id)
    gst_sctp_association_factory_init ();

  assoc =
      g_hash_table_lookup (associations_by_id,
      GUINT_TO_POINTER (association_id));
  if (!assoc) {
    assoc =
        g_object_new (GST_SCTP_TYPE_ASSOCIATION, "association-id",
        association_id, NULL);
    assoc->main_context = main_context;
    g_hash_table_insert (associations_by_id, GUINT_TO_POINTER (association_id),
        assoc);
  } else {
    g_object_ref (assoc);
  }

  gst_sctp_association_factory_association_ref (assoc);

  G_UNLOCK (associations_lock);
  return assoc;
}


void
gst_sctp_association_factory_release (GstSctpAssociation * assoc)
{
  G_LOCK (associations_lock);

  GSource *source = g_idle_source_new ();

  gst_sctp_association_factory_association_unref (assoc);

  g_mutex_lock (&unref_mutex);
  unref_called = FALSE;

  g_source_set_callback (source,
      (GSourceFunc) gst_sctp_association_factory_unref_in_main_loop, assoc,
      NULL);

  g_source_attach (source, main_context);
  g_source_unref (source);

  while (!unref_called)
    g_cond_wait (&unref_cond, &unref_mutex);
  g_mutex_unlock (&unref_mutex);

  if (g_hash_table_size (associations_by_id) == 0)
    gst_sctp_association_factory_deinit ();

  G_UNLOCK (associations_lock);
}
