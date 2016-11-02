/* GStreamer
 * Copyright (C) 2016 Pexip AS
 *               Erlend Graff <erlend@pexip.com>
 *
 * gstpriqueue.h: binomial heap implementation of a priority queue
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

#ifndef __GST_PRIQUEUE_H__
#define __GST_PRIQUEUE_H__

#include <glib.h>
#include <stdio.h>

G_BEGIN_DECLS

typedef struct _GstPriQueue     GstPriQueue;
typedef struct _GstPriQueueElem GstPriQueueElem;
typedef struct _GstPriQueueIter GstPriQueueIter;

typedef gint (* GstPriQueueCompareFunc) (const GstPriQueueElem  * elem_a,
                                         const GstPriQueueElem  * elem_b,
                                         gpointer                 user_data);

struct _GstPriQueueNode
{
  /*< private >*/
  struct _GstPriQueueNode  *parent;
  struct _GstPriQueueNode  *children_head;
  struct _GstPriQueueNode  *next;
  gint                      order;
};

struct _GstPriQueueElem
{
  /*< private >*/
  struct _GstPriQueueNode   node;
};

struct _GstPriQueueIter
{
  /*< private >*/
  struct _GstPriQueueNode  *node;
};

GstPriQueue *
gst_pri_queue_create          (GstPriQueueCompareFunc     cmp_func,
                               gpointer                   user_data);

void
gst_pri_queue_destroy         (GstPriQueue              * pq,
                               GDestroyNotify             elem_destroy_func);

gsize
gst_pri_queue_size            (const GstPriQueue        * pq);

void
gst_pri_queue_insert          (GstPriQueue              * pq,
                               GstPriQueueElem          * elem);

void
gst_pri_queue_remove          (GstPriQueue              * pq,
                               GstPriQueueElem          * elem);

void
gst_pri_queue_update          (GstPriQueue              * pq,
                               GstPriQueueElem          * elem);

GstPriQueueElem *
gst_pri_queue_get_min         (const GstPriQueue        * pq);

GstPriQueueElem *
gst_pri_queue_pop_min         (GstPriQueue              * pq);

GstPriQueue *
gst_pri_queue_meld            (GstPriQueue              * pqa,
                               GstPriQueue              * pqb);

/*
 * Iterator API
 */

void
gst_pri_queue_iter_init       (GstPriQueueIter          * iter,
                               GstPriQueue              * pq);

gboolean
gst_pri_queue_iter_next       (GstPriQueueIter          * iter,
                               GstPriQueueElem         ** elem);

/*
 * Debug API
 */

typedef gint (* GstPriQueueWriteElem) (FILE                   * out,
                                       const GstPriQueueElem  * elem,
                                       gpointer                 user_data);

gboolean
gst_pri_queue_is_valid        (const GstPriQueue        * pq);

void
gst_pri_queue_write_dot_file  (const GstPriQueue        * pq,
                               FILE                     * out,
                               GstPriQueueWriteElem       write_elem_func,
                               gpointer                   user_data);

G_END_DECLS

#endif /* __GST_PRIQUEUE_H__ */
