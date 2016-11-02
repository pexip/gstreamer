/* GStreamer
 * Copyright (C) 2016 Pexip AS
 *               Erlend Graff <erlend@pexip.com>
 *
 * gstpriqueue.c: binomial heap implementation of a priority queue
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

#include "gstpriqueue.h"

#include <gst/gstmacros.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * SECTION:gstpriqueue
 * @title: Priority Queues
 * @short_description: collections optimized for dequeuing elements in priority
 * order
 *
 * #GstPriQueue provides the API of a priority queue, where the only supported
 * lookup operation is to find/dequeue the element with the highest priority,
 * which is defined as the <emphasis>smallest</emphasis> value according to the
 * supplied comparison function.
 *
 * The #GstPriQueue is implemented internally as a binomial heap. This makes it
 * possible to find or remove the smallest element with an upper time bound of
 * `O(log n)`. Insertions have a worst-case time of `O(log n)`, but an amortized
 * asymptotic bound of `O(1)` over `n` consecutive insertions. Deletions are
 * supported in `O(log n)`, but deleting an arbitrary element (other than the
 * smallest element) is much more expensive than an insertion (up to a constant
 * factor of approximately 5 in the worst case). An update operation
 * (reinsertion of an element after its value has changed) is also implemented
 * in `O(log n)`, and is faster than a corresponding delete + (re-)insert.
 * Finally, the binomial heap structure also supports merging two priority
 * queues, in `O(log n)` time.
 *
 * The #GstPriQueue does not handle memory allocation of its elements. Instead,
 * the responsibility for allocating and freeing a #GstPriQueueElem, whenever
 * it is inserted into or removed from the #GstPriQueue, is given to the caller.
 * Typically, the caller will embed the #GstPriQueueElem structure inside a
 * containing "parent" structure that will represent the value of the element.
 * For example:
 * |[<!-- language="C" -->
 * typedef struct
 * {
 *   ...
 *   GstPriQueueElem pq_elem;
 *   ...
 * } SomeValue;
 *
 * ...
 *
 * SomeValue *val = create_new_some_value ();
 *
 * gst_pri_queue_insert (pq, &val->pq_elem);
 * ]|
 * The GST_CONTAINER_OF() macro defined in <filename>gst/gstmacros.h</filename>
 * can then be used to retrieve the containing `SomeValue` of a
 * #GstPriQueueElem:
 * |[<!-- language="C" -->
 * GstPriQueueElem *elem;
 * SomeValue *val;
 *
 * elem = gst_pri_queue_pop_min (pq);
 * if (elem) {
 *   val = GST_CONTAINER_OF (elem, SomeValue, pq_elem);
 *   ...
 * }
 * ]|
 * The comparison function will also typically make use of GST_CONTAINER_OF():
 * |[<!-- language="C" -->
 * static gint
 * compare_some_value_pq_elems (const GstPriQueueElem * a,
 *     const GstPriQueueElem * b, gpointer user_data)
 * {
 *   SomeValue *val_a = GST_CONTAINER_OF (a, SomeValue, pq_elem);
 *   SomeValue *val_b = GST_CONTAINER_OF (b, SomeValue, pq_elem);
 *
 *   return compare_some_value (val_a, val_b, user_data);
 * }
 *
 * ...
 *
 * GstPriQueue *pq;
 *
 * pq = gst_pri_queue_create (compare_some_value_pq_elems, some_user_data);
 * ]|
 */

#define NODE2ELEM(nodeptr) (GST_CONTAINER_OF ((nodeptr), GstPriQueueElem, node))
#define ELEM2NODE(elemptr) (&(elemptr)->node)

typedef struct _GstPriQueueNode GstPriQueueNode;

/**
 * GstPriQueue: (skip)
 *
 * The #GstPriQueue struct is an opaque data type representing a priority
 * queue.
 */
struct _GstPriQueue
{
  GstPriQueueNode *head;
  GstPriQueueCompareFunc cmp_func;
  gpointer user_data;
  gsize size;
};

/**
 * GstPriQueueElem: (skip)
 *
 * The #GstPriQueueElem struct represents an element in a #GstPriQueue.
 * This struct must be treated as an opaque data type, but has a public
 * definition to allow it to be embedded in another "parent" structure
 * representing the value of an element.
 */

/**
 * GstPriQueueIter: (skip)
 *
 * The #GstPriQueueIter struct represents an iterator that can be used to
 * iterate over all the elements in a #GstPriQueue. This struct must be treated
 * as an opaque data type, but has a public definition to allow it to be
 * allocated on the stack.
 *
 * A #GstPriQueueIter is initialized using gst_pri_queue_iter_init(), and
 * gst_pri_queue_iter_next() is used to do the actual iteration. Note that
 * iteration over elements in a #GstPriQueue is performed in an arbitrary order,
 * and <emphasis>not</emphasis> in priority order. The first element of an
 * iteration sequence is not even guaranteed to be the smallest element returned
 * by gst_pri_queue_get_min() or gst_pri_queue_pop_min(), or any of the smallest
 * elements in the #GstPriQueue if there are multiple such.
 */

typedef struct
{
  GstPriQueueNode *parent;
  GstPriQueueNode **list_pos;
  GstPriQueueNode *children_head;
  gint order;
} BinomTreePos;

static inline gint
compare_nodes (const GstPriQueue * pq, GstPriQueueNode * a, GstPriQueueNode * b)
{
  return pq->cmp_func (NODE2ELEM (a), NODE2ELEM (b), pq->user_data);
}

/* Remove @delnode from the list pointed to by @head. This may change what @head
 * points to.
 *
 * Note: @head MUST be the actual head of the list!
 */
static inline GstPriQueueNode **
list_remove_node (GstPriQueueNode ** head, GstPriQueueNode * delnode)
{
  GstPriQueueNode **pnext;
  GstPriQueueNode *node;

  for (pnext = head; (node = *pnext) != delnode;)
    pnext = &node->next;

  *pnext = node->next;
  return pnext;
}

/* Insert node at given position in list */
static inline void
list_insert_node (GstPriQueueNode ** pnext, GstPriQueueNode * insnode)
{
  insnode->next = *pnext;
  *pnext = insnode;
}

static inline void
init_node (GstPriQueueNode * node)
{
  node->parent = NULL;
  node->children_head = NULL;
  node->next = NULL;
  node->order = 0;
}

/* Merge two binomial trees of the same order `n` into a binomial tree of order
 * `(n + 1)`.
 */
static inline GstPriQueueNode *
merge_tree (const GstPriQueue * pq, GstPriQueueNode * a, GstPriQueueNode * b)
{
  GstPriQueueNode *new_root, *new_subtree;

  if (compare_nodes (pq, a, b) <= 0) {
    new_root = a;
    new_subtree = b;
  } else {
    new_root = b;
    new_subtree = a;
  }

  list_insert_node (&new_root->children_head, new_subtree);
  new_subtree->parent = new_root;
  new_root->order++;

  return new_root;
}

/* Reverses list, and sets parent to %NULL. */
static inline GstPriQueueNode *
subtree_list_to_heap_list (GstPriQueueNode * head)
{
  GstPriQueueNode *node, *new_head, *next;

  new_head = NULL;
  node = head;
  while (node) {
    next = node->next;
    node->next = new_head;
    new_head = node;
    node->parent = NULL;
    node = next;
  }

  return new_head;
}

static inline GstPriQueueNode **
get_containing_list (GstPriQueue * pq, GstPriQueueNode * node)
{
  return node->parent ? &node->parent->children_head : &pq->head;
}

static inline GstPriQueueNode **
remove_node_from_containing_list (GstPriQueue * pq, GstPriQueueNode * delnode)
{
  return list_remove_node (get_containing_list (pq, delnode), delnode);
}

/* Add a single node to the given binomial heap list (list of root binomial
 * trees). This will likely change the head of the binomial heap list.
 */
static inline void
binom_heap_list_add_node (GstPriQueue * pq, GstPriQueueNode ** head,
    GstPriQueueNode * insnode)
{
  GstPriQueueNode *next;

  while ((next = *head) && insnode->order == next->order) {
    (void) list_remove_node (head, next);
    insnode = merge_tree (pq, next, insnode);
  }

  list_insert_node (head, insnode);
}

/* Merge the binomial heap list (list of root binomial trees) pointed to by
 * @head_b (B-list) into @pq's binomial heap list (A-list). This will modify
 * the A-list, and destroy the B-list.
 */
static void
binom_heap_union (GstPriQueue * pq, GstPriQueueNode * head_b)
{
  GstPriQueueNode **pnext_a;
  GstPriQueueNode *next_a, *node;

  pnext_a = &pq->head;
  while ((next_a = *pnext_a) && head_b) {
    if (head_b->order > next_a->order) {
      pnext_a = &next_a->next;
    } else if (head_b->order < next_a->order) {
      /* Remove head of B-list, and insert into A-list */
      node = head_b;
      (void) list_remove_node (&head_b, node);
      list_insert_node (pnext_a, node);
    } else {
      /* Remove node from A-list, and add to B-list */
      node = next_a;
      (void) list_remove_node (pnext_a, node);
      binom_heap_list_add_node (pq, &head_b, node);
    }
  }

  if (head_b) {
    /* A-list exhausted, append rest of B-list to end */
    *pnext_a = head_b;
  }
}

static inline void
remove_tree_node (GstPriQueue * pq, GstPriQueueNode * delnode,
    BinomTreePos * pos)
{
  pos->parent = delnode->parent;
  pos->list_pos = remove_node_from_containing_list (pq, delnode);
  pos->children_head = delnode->children_head;
  pos->order = delnode->order;
}

/* Note: this function does not set insnode->parent. */
static inline void
insert_tree_node (BinomTreePos pos, GstPriQueueNode * insnode)
{
  GstPriQueueNode *child;

  for (child = pos.children_head; child; child = child->next)
    child->parent = insnode;

  insnode->order = pos.order;
  list_insert_node (pos.list_pos, insnode);
  insnode->children_head = pos.children_head;
}

static inline gboolean
should_decrease (GstPriQueue * pq, GstPriQueueNode * parent,
    GstPriQueueNode * node, gboolean node_is_minus_inf)
{
  return parent && (node_is_minus_inf || compare_nodes (pq, node, parent) < 0);
}

/* Try to move the given node upwards in its binomial tree until the min-heap
 * invariant is satisfied. If @is_minus_if is %TRUE, the node will end up at
 * the root of its binomial tree, otherwise its new position is determined by
 * its value according to the comparison function.
 *
 * Returns %TRUE if @node was moved upwards in its binomial tree, else
 * %FALSE.
 */
static inline gboolean
decrease_key (GstPriQueue * pq, GstPriQueueNode * node, gboolean is_minus_inf)
{
  BinomTreePos current_pos, parent_pos;

  if (!should_decrease (pq, node->parent, node, is_minus_inf))
    return FALSE;

  remove_tree_node (pq, node, &current_pos);

  do {
    remove_tree_node (pq, current_pos.parent, &parent_pos);
    insert_tree_node (current_pos, current_pos.parent);

    /* Check if current_pos.parent was inserted at head of its new containing
     * list. If so, we must manually update parent_pos.children_head.
     */
    if (parent_pos.children_head == current_pos.parent->next)
      parent_pos.children_head = current_pos.parent;

    current_pos = parent_pos;
  } while (should_decrease (pq, current_pos.parent, node, is_minus_inf));

  insert_tree_node (current_pos, node);
  node->parent = current_pos.parent;

  return TRUE;
}

/* Update node's position in its own binomial subtree after its value might
 * have changed.
 */
static inline void
increase_key (GstPriQueue * pq, GstPriQueueNode * node)
{
  GstPriQueueNode **list_pos;
  GstPriQueueNode *parent, *head;

  parent = node->parent;
  list_pos = remove_node_from_containing_list (pq, node);

  head = subtree_list_to_heap_list (node->children_head);
  node->children_head = NULL;
  node->order = 0;
  binom_heap_list_add_node (pq, &head, node);

  list_insert_node (list_pos, head);
  head->parent = parent;
}

static inline void
remove_heap_root (GstPriQueue * pq, GstPriQueueNode * delnode)
{
  (void) list_remove_node (&pq->head, delnode);
  binom_heap_union (pq, subtree_list_to_heap_list (delnode->children_head));
}

static inline GstPriQueueNode *
get_min_root (const GstPriQueue * pq)
{
  GstPriQueueNode *node, *min_node;

  node = pq->head;
  if (!node)
    return NULL;

  min_node = node;
  for (node = node->next; node; node = node->next) {
    if (compare_nodes (pq, node, min_node) < 0)
      min_node = node;
  }

  return min_node;
}

/*
 * Debug helper functions
 */

static void
write_dot_node (const GstPriQueueNode * node, FILE * out,
    GstPriQueueWriteElem write_elem_func, gpointer user_data)
{
  fprintf (out, "  %" G_GUINTPTR_FORMAT " [label=\"", (guintptr) node);
  write_elem_func (out, NODE2ELEM (node), user_data);
  fprintf (out, "\"];\n");
}

static void
_write_dot_children (const GstPriQueue * pq, GstPriQueueNode * root, FILE * out,
    GstPriQueueWriteElem write_elem_func, gpointer user_data)
{
  GstPriQueueNode *node;

  for (node = root->children_head; node; node = node->next) {
    write_dot_node (node, out, write_elem_func, user_data);

    fprintf (out, "  %" G_GUINTPTR_FORMAT, (guintptr) root);
    fprintf (out, " -> %" G_GUINTPTR_FORMAT, (guintptr) node);
    fprintf (out, " [color=red];\n");

    if (node->parent) {
      fprintf (out, "  %" G_GUINTPTR_FORMAT, (guintptr) node);
      fprintf (out, " -> %" G_GUINTPTR_FORMAT, (guintptr) node->parent);
      fprintf (out, " [color=blue];\n");
    }

    if (node->children_head)
      _write_dot_children (pq, node, out, write_elem_func, user_data);
  }
}

static void
_write_dot_tree (const GstPriQueue * pq, GstPriQueueNode * tree, FILE * out,
    GstPriQueueWriteElem write_elem_func, gpointer user_data)
{
  /* Binomial heap list (list of root binomial trees) is in increasing order,
   * but we want the trees to be in decreasing order (from left to right) in the
   * DOT file, so recurse to get a kind of post-order traversal.
   */
  if (tree->next)
    _write_dot_tree (pq, tree->next, out, write_elem_func, user_data);

  write_dot_node (tree, out, write_elem_func, user_data);
  _write_dot_children (pq, tree, out, write_elem_func, user_data);
}

static gboolean
_binom_tree_is_invariant (const GstPriQueue * pq, GstPriQueueNode * root,
    gsize * size)
{
  GstPriQueueNode *child;
  gsize subtree_size;
  gint res, num_children, expected_order;

  *size = 1;
  num_children = 0;
  expected_order = root->order;

  for (child = root->children_head; child; child = child->next) {
    num_children++;
    expected_order--;

    if (G_UNLIKELY (child->order != expected_order))
      return FALSE;

    res = compare_nodes (pq, root, child);
    if (G_UNLIKELY (res > 0))
      return FALSE;

    if (G_UNLIKELY (child->parent != root))
      return FALSE;

    if (G_UNLIKELY (!_binom_tree_is_invariant (pq, child, &subtree_size)))
      return FALSE;

    *size += subtree_size;
  }

  if (G_UNLIKELY (expected_order != 0))
    return FALSE;

  if (G_UNLIKELY (num_children != root->order))
    return FALSE;

  return TRUE;
}

static inline gboolean
is_heap_list_order_increasing (const GstPriQueueNode * head)
{
  const GstPriQueueNode *node;

  for (node = head; node && node->next; node = node->next) {
    if (node->order >= node->next->order)
      return FALSE;
  }

  return TRUE;
}

/*
 * Public API
 */

/**
 * gst_pri_queue_create: (skip)
 * @cmp_func: the #GstPriQueueCompareFunc used to compare elements in the
 * #GstPriQueue. It should return a number < 0 if the first element is smaller
 * (i.e. has a higher priority) than the second.
 * @user_data: user data passed to @cmp_func
 *
 * Creates a new #GstPriQueue. The @cmp_func is used to determine which of any
 * two elements in the #GstPriQueue is smaller (i.e. has the highest priority).
 *
 * Returns: a new #GstPriQueue
 */
 /**
  * GstPriQueueCompareFunc: (skip)
  * @elem_a: a #GstPriQueueElem
  * @elem_b: a #GstPriQueueElem to compare with
  * @user_data: user data
  *
  * Specifies the type of a comparison function used to compare two elements in
  * the #GstPriQueue. The function should return a negative integer if the first
  * element is smaller (i.e. has a higher priority) than the second, 0 if they
  * are equal, or a positive integer if the first element is larger (i.e. has a
  * lower priority).
  *
  * Returns: negative value if @elem_a < @elem_b ; zero if @elem_a = @elem_b ;
  * positive value if @elem_a > @elem_b
  */
GstPriQueue *
gst_pri_queue_create (GstPriQueueCompareFunc cmp_func, gpointer user_data)
{
  GstPriQueue *pq;

  pq = g_new (GstPriQueue, 1);
  pq->cmp_func = cmp_func;
  pq->user_data = user_data;
  pq->head = NULL;
  pq->size = 0;

  return pq;
}

/**
 * gst_pri_queue_destroy: (skip)
 * @pq: the #GstPriQueue to destroy
 * @elem_destroy_func: (nullable): a function that is called for each
 * element in the #GstPriQueue before it is destroyed (e.g. to free the memory
 * allocated for the element).
 *
 * Destroys the #GstPriQueue.
 */
void
gst_pri_queue_destroy (GstPriQueue * pq, GDestroyNotify elem_destroy_func)
{
  GstPriQueueElem *elem;
  GstPriQueueIter iter;

  gst_pri_queue_iter_init (&iter, pq);
  while (gst_pri_queue_iter_next (&iter, &elem)) {
    if (elem_destroy_func)
      elem_destroy_func (elem);
    else
      init_node (ELEM2NODE (elem));
  }

  g_free (pq);
}

/**
 * gst_pri_queue_size: (skip)
 * @pq: a #GstPriQueue
 *
 * Returns the number of elements in the #GstPriQueue. This function is
 * implemented in `O(1)` time.
 *
 * Returns: the number of elements in the #GstPriQueue
 */
gsize
gst_pri_queue_size (const GstPriQueue * pq)
{
  return pq->size;
}

/**
 * gst_pri_queue_insert: (skip)
 * @pq: a #GstPriQueue
 * @elem: the #GstPriQueueElem to be inserted
 *
 * Inserts an element into the #GstPriQueue. This function should only be called
 * for elements that are not already inserted into the #GstPriQueue.
 */
void
gst_pri_queue_insert (GstPriQueue * pq, GstPriQueueElem * elem)
{
  GstPriQueueNode *node;

  node = ELEM2NODE (elem);
  init_node (node);
  binom_heap_list_add_node (pq, &pq->head, node);
  pq->size++;
}

/**
 * gst_pri_queue_remove: (skip)
 * @pq: a #GstPriQueue
 * @elem: the #GstPriQueueElem to remove
 *
 * Removes a #GstPriQueueElem from the #GstPriQueue. This function should only
 * be called for elements that are inserted into the #GstPriQueue.
 */
void
gst_pri_queue_remove (GstPriQueue * pq, GstPriQueueElem * elem)
{
  GstPriQueueNode *delnode;

  delnode = ELEM2NODE (elem);
  (void) decrease_key (pq, delnode, TRUE);
  remove_heap_root (pq, delnode);
  pq->size--;

  init_node (delnode);
}

/**
 * gst_pri_queue_update: (skip)
 * @pq: a #GstPriQueue
 * @elem: a #GstPriQueueElem that has changed its associated value (priority)
 *
 * Updates an element's position in the #GstPriQueue after its value (priority)
 * has changed such that the priority queue's comparison function may return a
 * different value when comparing it to other elements. This function should
 * only be called for elements that are inserted into the #GstPriQueue.
 *
 * Changing the value associated with a #GstPriQueueElem without calling
 * gst_pri_queue_update() afterwards will most likely lead to the #GstPriQueue
 * becoming invalid.
 */
void
gst_pri_queue_update (GstPriQueue * pq, GstPriQueueElem * elem)
{
  GstPriQueueNode *node;

  node = ELEM2NODE (elem);
  if (!decrease_key (pq, node, FALSE))
    increase_key (pq, node);
}

/**
 * gst_pri_queue_get_min: (skip)
 * @pq: a #GstPriQueue
 *
 * Returns the #GstPriQueueElem with the smallest value (i.e highest priority)
 * without dequeuing it from the #GstPriQueue. If the #GstPriQueue contains
 * multiple elements with the same (smallest) value, the returned
 * #GstPriQueueElem will be one of these. Two consecutive calls to
 * gst_pri_queue_get_min() are guaranteed to return the same #GstPriQueueElem
 * if the #GstPriQueue has not been modified in between the calls.
 *
 * Returns: the smallest #GstPriQueueElem in the #GstPriQueue, or %NULL if the
 * #GstPriQueue is empty.
 */
GstPriQueueElem *
gst_pri_queue_get_min (const GstPriQueue * pq)
{
  GstPriQueueNode *node;

  node = get_min_root (pq);
  return NODE2ELEM (node);
}

/**
 * gst_pri_queue_pop_min: (skip)
 * @pq: a #GstPriQueue
 *
 * Dequeues the #GstPriQueueElem with the smallest value (i.e highest priority)
 * from the #GstPriQueue. If the #GstPriQueue contains multiple elements with
 * the same (smallest) value, the dequeued #GstPriQueueElem will be one of
 * these.
 *
 * Returns: the smallest #GstPriQueueElem in the #GstPriQueue, or %NULL if the
 * #GstPriQueue is empty.
 */
GstPriQueueElem *
gst_pri_queue_pop_min (GstPriQueue * pq)
{
  GstPriQueueNode *delnode;

  delnode = get_min_root (pq);
  if (!delnode)
    return NULL;

  remove_heap_root (pq, delnode);
  pq->size--;

  init_node (delnode);
  return NODE2ELEM (delnode);
}

/**
 * gst_pri_queue_meld: (skip)
 * @pqa: a #GstPriQueue
 * @pqb: a #GstPriQueue to merge with
 *
 * Merge two priority queues. The second #GstPriQueue @pqb will be destroyed,
 * and all its elements will be inserted into the first #GstPriQueue @pqa. This
 * function is implemented in `O(log n)` time.
 *
 * Returns: @pqa after the elements from @pqb have been merged into it.
 */
GstPriQueue *
gst_pri_queue_meld (GstPriQueue * pqa, GstPriQueue * pqb)
{
  binom_heap_union (pqa, pqb->head);
  pqa->size += pqb->size;

  pqb->head = NULL;
  gst_pri_queue_destroy (pqb, NULL);

  return pqa;
}

/*
 * Iterator API
 */

/**
 * gst_pri_queue_iter_init: (skip)
 * @iter: a #GstPriQueueIter
 * @pq: a #GstPriQueue
 *
 * Initialize an iterator over the elements in @pq.
 *
 * Note that the iterator becomes invalid if the #GstPriQueue @pq is modified.
 */
void
gst_pri_queue_iter_init (GstPriQueueIter * iter, GstPriQueue * pq)
{
  iter->node = pq->head;
}

/**
 * gst_pri_queue_iter_next: (skip)
 * @iter: a #GstPriQueueIter
 * @elem: (out) (nullable) (optional): a location to store the #GstPriQueueElem,
 * or %NULL
 *
 * Retrieves the #GstPriQueueElem at the iterator's current position, and
 * advances the iterator.
 *
 * Example:
 * |[<!-- language="C" -->
 * GstPriQueueIter iter;
 * GstPriQueueElem *elem;
 *
 * gst_pri_queue_iter_init (&iter, pq);
 * while (gst_pri_queue_iter_next (&iter, &elem)) {
 *   SomeValue *value = GST_CONTAINER_OF (elem, SomeValue, pq_elem);
 *   ...
 * }
 * ]|
 *
 * Note that iteration over elements in a #GstPriQueue is performed in an
 * arbitrary order, and <emphasis>not</emphasis> in priority order. The first
 * element of an iteration sequence is not even guaranteed to be the smallest
 * element returned by gst_pri_queue_get_min() or gst_pri_queue_pop_min(), or
 * any of the smallest elements in the #GstPriQueue if there are multiple such.
 *
 * Returns: %TRUE if a #GstPriQueueElem could be retrieved, or %FALSE if the end
 * of the #GstPriQueue has been reached.
 */
gboolean
gst_pri_queue_iter_next (GstPriQueueIter * iter, GstPriQueueElem ** elem)
{
  GstPriQueueNode *node, *next;

  node = iter->node;
  if (!node)
    return FALSE;

  if (node->children_head) {
    next = node->children_head;
  } else {
    for (next = node; next && !next->next;)
      next = next->parent;

    if (next)
      next = next->next;
  }

  iter->node = next;
  if (elem)
    *elem = NODE2ELEM (node);

  return TRUE;
}

/*
 * Debug API
 */

/**
 * gst_pri_queue_is_valid: (skip)
 * @pq: a #GstPriQueue
 *
 * Check that the invariants of the internal data structures have not been
 * violated.
 * <note><para>
 * This function is intended for testing and/or debugging purposes only.
 * </para></note>
 *
 * Returns: %TRUE if no invariants have been violated, else %FALSE.
 */
gboolean
gst_pri_queue_is_valid (const GstPriQueue * pq)
{
  GstPriQueueNode *heap;
  gsize size, heap_size;

  if (G_UNLIKELY (!is_heap_list_order_increasing (pq->head)))
    return FALSE;

  size = 0;
  for (heap = pq->head; heap; heap = heap->next) {
    if (G_UNLIKELY (!_binom_tree_is_invariant (pq, heap, &heap_size)))
      return FALSE;

    size += heap_size;

    if (G_UNLIKELY (heap->parent != NULL))
      return FALSE;
  }

  if (G_UNLIKELY (size != pq->size))
    return FALSE;

  return TRUE;
}

/**
 * gst_pri_queue_write_dot_file: (skip)
 * @pq: a #GstPriQueue
 * @out: the output stream to which the DOT file is written
 * @write_elem_func: a #GstPriQueueWriteElem used as callback to write elements
 * to the DOT file
 * @user_data: user data passed to @write_elem_func
 *
 * Write a DOT representation of the internal data structures to the given
 * output stream.
 * <note><para>
 * This function is intended for testing and/or debugging purposes only.
 * </para></note>
 */
 /**
 * GstPriQueueWriteElem: (skip)
 * @out: the #FILE to where the element is written
 * @elem: the element to write
 * @user_data: user data
 *
 * A #GstPriQueueWriteElem is a callback supplied to
 * gst_pri_queue_write_dot_file() for writing elements to a DOT file.
 *
 * Returns: the number of bytes written.
 */
void
gst_pri_queue_write_dot_file (const GstPriQueue * pq, FILE * out,
    GstPriQueueWriteElem write_elem_func, gpointer user_data)
{
  fprintf (out, "digraph graphname {\n");

  if (pq->head)
    _write_dot_tree (pq, pq->head, out, write_elem_func, user_data);

  fprintf (out, "}\n");
}
