/*
 * GStreamer
 *
 *  Copyright 2006 Collabora Ltd,
 *  Copyright 2006 Nokia Corporation
 *   @author: Philippe Kalaf <philippe.kalaf@collabora.co.uk>.
 *  Copyright 2012-2016 Pexip
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnetsim.h"
#include <string.h>
#include <math.h>
#include <float.h>
#include <gst/rtp/gstrtpbuffer.h>

GST_DEBUG_CATEGORY (netsim_debug);
#define GST_CAT_DEFAULT (netsim_debug)

static GType
distribution_get_type (void)
{
  static gsize static_g_define_type_id = 0;
  if (g_once_init_enter (&static_g_define_type_id)) {
    static const GEnumValue values[] = {
      {DISTRIBUTION_UNIFORM, "uniform", "uniform"},
      {DISTRIBUTION_NORMAL, "normal", "normal"},
      {DISTRIBUTION_GAMMA, "gamma", "gamma"},
      {0, NULL, NULL}
    };
    GType g_define_type_id =
        g_enum_register_static ("GstNetSimDistribution", values);
    g_once_init_leave (&static_g_define_type_id, g_define_type_id);
  }
  return static_g_define_type_id;
}

#define GST_NET_SIM_LOCK(obj)   g_mutex_lock (&obj->mutex)
#define GST_NET_SIM_UNLOCK(obj) g_mutex_unlock (&obj->mutex)
#define GST_NET_SIM_SIGNAL(obj) g_cond_signal (&obj->cond)
#define GST_NET_SIM_WAIT(obj)   g_cond_wait (&obj->cond, &obj->mutex)

enum
{
  PROP_0,
  PROP_MIN_DELAY,
  PROP_MAX_DELAY,
  PROP_DELAY_DISTRIBUTION,
  PROP_DELAY_PROBABILITY,
  PROP_DROP_PROBABILITY,
  PROP_DUPLICATE_PROBABILITY,
  PROP_DROP_PACKETS,
  PROP_MAX_KBPS,
  PROP_MAX_BUCKET_SIZE,
  PROP_QUEUE_SIZE,
  PROP_MAX_QUEUE_DELAY,
  PROP_ALLOW_REORDERING,
  PROP_REPLACE_DROPPED_WITH_EMPTY
};

/* these numbers are nothing but wild guesses and don't reflect any reality */
#define DEFAULT_MIN_DELAY 200
#define DEFAULT_MAX_DELAY 400
#define DEFAULT_DELAY_DISTRIBUTION DISTRIBUTION_UNIFORM
#define DEFAULT_DELAY_PROBABILITY 0.0
#define DEFAULT_DROP_PROBABILITY 0.0
#define DEFAULT_DUPLICATE_PROBABILITY 0.0
#define DEFAULT_DROP_PACKETS 0
#define DEFAULT_MAX_KBPS -1
#define DEFAULT_MAX_BUCKET_SIZE -1
#define DEFAULT_QUEUE_SIZE -1
#define DEFAULT_MAX_QUEUE_DELAY 50
#define DEFAULT_ALLOW_REORDERING TRUE
#define DEFAULT_REPLACE_DROPPED_WITH_EMPTY FALSE

static GstStaticPadTemplate gst_net_sim_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_net_sim_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

G_DEFINE_TYPE (GstNetSim, gst_net_sim, GST_TYPE_ELEMENT);
GST_ELEMENT_REGISTER_DEFINE (netsim, "netsim",
    GST_RANK_MARGINAL, GST_TYPE_NET_SIM);

typedef struct
{
  GstBuffer *buf;
  guint size_bits;
  GstClockTime arrival_time;
  GstClockTime delay;
  GstClockTime token_delay;
  guint seqnum;
} NetSimBuffer;

static NetSimBuffer *
net_sim_buffer_new (GstBuffer * buf,
    guint seqnum, GstClockTime arrival_time, GstClockTime delay)
{
  NetSimBuffer *nsbuf = g_slice_new0 (NetSimBuffer);
  nsbuf->buf = gst_buffer_ref (buf);
  nsbuf->size_bits = gst_buffer_get_size (buf) * 8;
  nsbuf->seqnum = seqnum;
  nsbuf->arrival_time = arrival_time;
  nsbuf->delay = delay;
  nsbuf->token_delay = 0;
  return nsbuf;
}

static GstClockTime
net_sim_buffer_get_sync_time (NetSimBuffer * nsbuf)
{
  return nsbuf->arrival_time + nsbuf->delay + nsbuf->token_delay;
}

static void
net_sim_buffer_free (NetSimBuffer * nsbuf)
{
  if (G_UNLIKELY (nsbuf == NULL))
    return;
  gst_buffer_unref (nsbuf->buf);
  g_slice_free (NetSimBuffer, nsbuf);
}

static GstFlowReturn
net_sim_buffer_push (NetSimBuffer * nsbuf, GstPad * srcpad)
{
  GstFlowReturn ret;
  ret = gst_pad_push (srcpad, nsbuf->buf);
  g_slice_free (NetSimBuffer, nsbuf);
  return ret;
}

static gint
nsbuf_compare_seqnum (gconstpointer a, gconstpointer b,
    G_GNUC_UNUSED gpointer user_data)
{
  const NetSimBuffer *buf_a = (NetSimBuffer *) a;
  const NetSimBuffer *buf_b = (NetSimBuffer *) b;
  return buf_a->seqnum - buf_b->seqnum;
}

static gint
nsbuf_compare_time (gconstpointer a, gconstpointer b,
    G_GNUC_UNUSED gpointer user_data)
{
  const NetSimBuffer *buf_a = (NetSimBuffer *) a;
  const NetSimBuffer *buf_b = (NetSimBuffer *) b;
  GstClockTime ts_a = buf_a->arrival_time + buf_a->delay;
  GstClockTime ts_b = buf_b->arrival_time + buf_b->delay;
  return ts_a - ts_b;
}

static gboolean
gst_net_sim_set_clock (GstElement * element, GstClock * clock)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (element);
  GST_DEBUG_OBJECT (netsim, "Setting clock %" GST_PTR_FORMAT, clock);
  if (clock == NULL)
    return TRUE;

  GST_NET_SIM_LOCK (netsim);
  if (netsim->clock)
    gst_object_unref (netsim->clock);
  netsim->clock = gst_object_ref (clock);
  GST_NET_SIM_SIGNAL (netsim);
  GST_NET_SIM_UNLOCK (netsim);

  return TRUE;
}

static gboolean
gst_net_sim_wait_for_clock (GstNetSim * netsim)
{
  while (netsim->clock == NULL) {
    if (!netsim->running)
      return FALSE;
    GST_INFO_OBJECT (netsim, "Waiting for a clock");
    GST_NET_SIM_WAIT (netsim);
  }
  return TRUE;
}

static gint
get_random_value_uniform (GRand * rand_seed, gint32 min_value, gint32 max_value)
{
  return g_rand_int_range (rand_seed, min_value, max_value + 1);
}

/* Use the Box-Muller transform. */
static gdouble
random_value_normal (GRand * rand_seed, gdouble mu, gdouble sigma,
    NormalDistributionState * state)
{
  gdouble u1, u2, t1, t2;

  state->generate = !state->generate;

  if (!state->generate)
    return state->z1 * sigma + mu;

  do {
    u1 = g_rand_double (rand_seed);
    u2 = g_rand_double (rand_seed);
  } while (u1 <= DBL_EPSILON);

  t1 = sqrt (-2.0 * log (u1));
  t2 = 2.0 * G_PI * u2;
  state->z0 = t1 * cos (t2);
  state->z1 = t1 * sin (t2);

  return state->z0 * sigma + mu;
}

/* Generate a value from a normal distributation with 95% confidense interval
 * between LOW and HIGH */
static gint
get_random_value_normal (GRand * rand_seed, gint32 low, gint32 high,
    NormalDistributionState * state)
{
  gdouble mu = (high + low) / 2.0;
  gdouble sigma = (high - low) / (2 * 1.96);    /* 95% confidence interval */
  gdouble z = random_value_normal (rand_seed, mu, sigma, state);

  return round (z);
}

/* Marsaglia and Tsang's method */
static gdouble
random_value_gamma (GRand * rand_seed, gdouble a, gdouble b,
    NormalDistributionState * state)
{
  const gdouble d = a - 1.0 / 3.0;
  const gdouble c = 1.0 / sqrt (9 * d);
  gdouble x, u, z, v;

  if (a >= 1.0) {
    while (TRUE) {
      z = random_value_normal (rand_seed, 0.0, 1.0, state);
      if (z > -1.0 / c) {
        u = g_rand_double (rand_seed);
        v = 1.0 + c * z;
        v = v * v * v;
        if (log (u) < (0.5 * z * z + d * (1 - v + log (v)))) {
          x = d * v;
          break;
        }
      }
    }
  } else {
    u = g_rand_double (rand_seed);
    x = random_value_gamma (rand_seed, a + 1, b, state) * pow (u, 1.0 / a);
  }

  return x * b;
}

static gint
get_random_value_gamma (GRand * rand_seed, gint32 low, gint32 high,
    NormalDistributionState * state)
{
  /* shape parameter 1.25 gives an OK simulation of wireless networks */
  /* Find the scale parameter so that P(0 < x < high-low) < 0.95 */
  /* We know: P(0 < x < R) < 0.95 for gamma(1.25, 1), R = 3.4640381 */
  gdouble shape = 1.25;
  gdouble scale = (high - low) / 3.4640381;
  gdouble x = random_value_gamma (rand_seed, shape, scale, state);
  /* Add offset so that low is the minimum possible value */
  return round (x + low);
}

static gint
gst_net_sim_get_tokens (GstNetSim * netsim, GstClockTime now)
{
  gint tokens = 0;
  GstClockTimeDiff elapsed_time = 0;
  GstClockTimeDiff token_time;
  guint max_bps;

  /* check for umlimited kbps and fill up the bucket if that is the case,
   * if not, calculate the number of tokens to add based on the elapsed time */
  if (netsim->max_kbps == -1)
    return netsim->max_bucket_size * 1000 - netsim->bucket_size;

  /* get the elapsed time */
  if (GST_CLOCK_TIME_IS_VALID (netsim->prev_time)) {
    if (now < netsim->prev_time) {
      GST_WARNING_OBJECT (netsim, "Clock is going backwards!!");
    } else {
      elapsed_time = GST_CLOCK_DIFF (netsim->prev_time, now);
    }
  } else {
    netsim->prev_time = now;
  }

  /* calculate number of tokens and how much time is "spent" by these tokens */
  max_bps = netsim->max_kbps * 1000;
  tokens = gst_util_uint64_scale_int (elapsed_time, max_bps, GST_SECOND);
  token_time = gst_util_uint64_scale_int (GST_SECOND, tokens, max_bps);

  GST_DEBUG_OBJECT (netsim,
      "Elapsed time: %" GST_TIME_FORMAT " produces %u tokens (" "token-time: %"
      GST_TIME_FORMAT ")", GST_TIME_ARGS (elapsed_time), tokens,
      GST_TIME_ARGS (token_time));

  /* increment the time with how much we spent in terms of whole tokens */
  netsim->prev_time += token_time;
  return tokens;
}

static guint
gst_net_sim_get_missing_tokens (GstNetSim * netsim,
    NetSimBuffer * nsbuf, GstClockTime now)
{
  gint tokens;

  /* with an unlimited bucket-size, we have nothing to do */
  if (netsim->max_bucket_size == -1)
    return 0;

  tokens = gst_net_sim_get_tokens (netsim, now);

  netsim->bucket_size =
      MIN (netsim->max_bucket_size * 1000, netsim->bucket_size + tokens);
  GST_LOG_OBJECT (netsim, "Added %d tokens to bucket (contains %u tokens)",
      tokens, netsim->bucket_size);

  if (nsbuf->size_bits > netsim->bucket_size) {
    GST_DEBUG_OBJECT (netsim, "Buffer size (%u) exeedes bucket size (%u)",
        nsbuf->size_bits, netsim->bucket_size);
    return nsbuf->size_bits - netsim->bucket_size;
  }

  netsim->bucket_size -= nsbuf->size_bits;
  GST_DEBUG_OBJECT (netsim, "Buffer taking %u tokens (%u left)",
      nsbuf->size_bits, netsim->bucket_size);
  return 0;
}

static void
gst_net_sim_drop_nsbuf (GstNetSim * netsim)
{
  NetSimBuffer *nsbuf;
  nsbuf = g_queue_pop_head (netsim->bqueue);
  GST_LOG_OBJECT (netsim, "Dropping buf #%u (tokens)", nsbuf->seqnum);
  netsim->bits_in_queue -= nsbuf->size_bits;
  net_sim_buffer_free (nsbuf);
}

/* must be called with GST_NET_SIM_LOCK */
static GstFlowReturn
gst_net_sim_push_unlocked (GstNetSim * netsim, GstClockTimeDiff jitter)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstClockTime now = gst_clock_get_time (netsim->clock);
  NetSimBuffer *nsbuf = g_queue_peek_head (netsim->bqueue);
  GstClockTime sync_time = net_sim_buffer_get_sync_time (nsbuf);
  guint missing_tokens;

  GST_DEBUG_OBJECT (netsim,
      "now: %" GST_TIME_FORMAT " jitter %" GST_STIME_FORMAT
      " netsim->max_delay %" GST_STIME_FORMAT, GST_TIME_ARGS (now),
      GST_STIME_ARGS (jitter),
      GST_STIME_ARGS (netsim->max_delay * GST_MSECOND));

  missing_tokens = gst_net_sim_get_missing_tokens (netsim, nsbuf, now);
  if (missing_tokens > 0) {
    GstClockTime token_delay = gst_util_uint64_scale_int (GST_SECOND,
        missing_tokens, netsim->max_kbps * 1000);
    GstClockTime new_synctime = now + token_delay;
    GstClockTime delta = new_synctime - net_sim_buffer_get_sync_time (nsbuf);
    nsbuf->token_delay += delta;

    GST_DEBUG_OBJECT (netsim,
        "Missing %u tokens, delaying buffer #%u additional %" GST_TIME_FORMAT
        " (total: %" GST_TIME_FORMAT ") for new sync_time: %" GST_TIME_FORMAT,
        missing_tokens, nsbuf->seqnum, GST_TIME_ARGS (delta),
        GST_TIME_ARGS (nsbuf->token_delay), GST_TIME_ARGS (sync_time));

    if (nsbuf->token_delay > netsim->max_queue_delay * GST_MSECOND) {
      gst_net_sim_drop_nsbuf (netsim);
    }
  } else {
    GST_DEBUG_OBJECT (netsim, "Pushing buffer #%u now", nsbuf->seqnum);
    nsbuf = g_queue_pop_head (netsim->bqueue);
    netsim->bits_in_queue -= nsbuf->size_bits;

    GST_NET_SIM_UNLOCK (netsim);
    ret = net_sim_buffer_push (nsbuf, netsim->srcpad);
    GST_NET_SIM_LOCK (netsim);
  }

  return ret;
}

static gint
gst_new_sim_get_delay_ms (GstNetSim * netsim)
{
  gint delay_ms = 0;

  if (netsim->delay_probability == 0 ||
      g_rand_double (netsim->rand_seed) > netsim->delay_probability)
    return delay_ms;

  switch (netsim->delay_distribution) {
    case DISTRIBUTION_UNIFORM:
      delay_ms = get_random_value_uniform (netsim->rand_seed, netsim->min_delay,
          netsim->max_delay);
      break;
    case DISTRIBUTION_NORMAL:
      delay_ms = get_random_value_normal (netsim->rand_seed, netsim->min_delay,
          netsim->max_delay, &netsim->delay_state);
      break;
    case DISTRIBUTION_GAMMA:
      delay_ms = get_random_value_gamma (netsim->rand_seed, netsim->min_delay,
          netsim->max_delay, &netsim->delay_state);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  return delay_ms;
}

static void
gst_net_sim_queue_buffer (GstNetSim * netsim, GstBuffer * buf)
{
  GstClockTime now = gst_clock_get_time (netsim->clock);
  gint delay_ms = gst_new_sim_get_delay_ms (netsim);
  NetSimBuffer *nsbuf = net_sim_buffer_new (buf, netsim->seqnum++,
      now, delay_ms * GST_MSECOND);

  if (delay_ms > 0) {
    GST_DEBUG_OBJECT (netsim, "Delaying buffer with %dms", delay_ms);
  }

  GST_DEBUG_OBJECT (netsim, "queue_size: %u, bits_in_queue: %u, bufsize: %u",
      netsim->queue_size * 1000, netsim->bits_in_queue, nsbuf->size_bits);

  if (netsim->bits_in_queue > 0 &&
      netsim->queue_size != -1 &&
      netsim->bits_in_queue + nsbuf->size_bits > netsim->queue_size * 1000) {
    GST_DEBUG_OBJECT (netsim, "dropping buf #%u", nsbuf->seqnum);
    net_sim_buffer_free (nsbuf);
  } else {
    GST_DEBUG_OBJECT (netsim, "queueing buf #%u", nsbuf->seqnum);
    GST_NET_SIM_LOCK (netsim);
    g_queue_insert_sorted (netsim->bqueue, nsbuf, netsim->compare_func, NULL);
    netsim->bits_in_queue += nsbuf->size_bits;
    GST_NET_SIM_SIGNAL (netsim);
    GST_NET_SIM_UNLOCK (netsim);
  }
}

static GstFlowReturn
gst_net_sim_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (parent);
  gboolean dropped = FALSE;

  if (netsim->drop_packets > 0) {
    netsim->drop_packets--;
    GST_DEBUG_OBJECT (netsim, "Dropping packet (%d left)",
        netsim->drop_packets);
    dropped = TRUE;
  } else if (netsim->drop_probability > 0
      && g_rand_double (netsim->rand_seed) <
      (gdouble) netsim->drop_probability) {
    GST_DEBUG_OBJECT (netsim, "Dropping packet");
    dropped = TRUE;
  } else if (netsim->duplicate_probability > 0 &&
      g_rand_double (netsim->rand_seed) <
      (gdouble) netsim->duplicate_probability) {
    GST_DEBUG_OBJECT (netsim, "Duplicating packet");
    gst_net_sim_queue_buffer (netsim, buf);
    gst_net_sim_queue_buffer (netsim, buf);
  } else {
    gst_net_sim_queue_buffer (netsim, buf);
  }

  if (dropped && netsim->replace_droppped_with_empty) {
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;

    if (gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp)) {
      guint header_len = gst_rtp_buffer_get_header_len (&rtp);
      gst_rtp_buffer_unmap (&rtp);

      buf = gst_buffer_make_writable (buf);
      gst_buffer_resize (buf, 0, header_len);
      gst_net_sim_queue_buffer (netsim, buf);
    }
  }

  gst_buffer_unref (buf);

  return GST_FLOW_OK;
}

static void
gst_net_sim_loop (gpointer data)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (data);
  GstFlowReturn ret;
  NetSimBuffer *nsbuf;
  GstClockTime sync_time;
  GstClockTimeDiff jitter;

  GST_NET_SIM_LOCK (netsim);
  if (!gst_net_sim_wait_for_clock (netsim))
    goto pause_task;

  if (!netsim->running)
    goto pause_task;

  while (netsim->running && g_queue_is_empty (netsim->bqueue))
    GST_NET_SIM_WAIT (netsim);

  if (!netsim->running)
    goto pause_task;

  nsbuf = g_queue_peek_head (netsim->bqueue);
  sync_time = net_sim_buffer_get_sync_time (nsbuf);
  netsim->clock_id = gst_clock_new_single_shot_id (netsim->clock, sync_time);

  GST_DEBUG_OBJECT (netsim, "Popped buf #%u with sync_time: %" GST_TIME_FORMAT,
      nsbuf->seqnum, GST_TIME_ARGS (sync_time));

  GST_NET_SIM_UNLOCK (netsim);
  gst_clock_id_wait (netsim->clock_id, &jitter);
  GST_NET_SIM_LOCK (netsim);

  gst_clock_id_unref (netsim->clock_id);
  netsim->clock_id = NULL;

  if (!netsim->running) {
    goto pause_task;
  }

  ret = gst_net_sim_push_unlocked (netsim, jitter);
  if (ret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (netsim, "pausing task because flow: %d", ret);
    goto pause_task;
  }

  GST_NET_SIM_UNLOCK (netsim);
  return;

pause_task:
  GST_INFO_OBJECT (netsim, "pausing task");
  gst_pad_pause_task (netsim->srcpad);
  GST_NET_SIM_UNLOCK (netsim);
  return;
}

static gboolean
gst_net_sim_src_activatemode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (parent);
  gboolean result;

  if (active) {
    GST_DEBUG_OBJECT (pad, "starting task on pad %s:%s",
        GST_DEBUG_PAD_NAME (pad));

    GST_NET_SIM_LOCK (netsim);
    netsim->running = TRUE;
    if (!gst_pad_start_task (netsim->srcpad, gst_net_sim_loop, netsim, NULL))
      g_assert_not_reached ();

    GST_NET_SIM_UNLOCK (netsim);
    result = TRUE;
  } else {
    GST_DEBUG_OBJECT (pad, "stopping task on pad %s:%s",
        GST_DEBUG_PAD_NAME (pad));

    GST_NET_SIM_LOCK (netsim);
    netsim->running = FALSE;
    if (netsim->clock_id)
      gst_clock_id_unschedule (netsim->clock_id);
    GST_NET_SIM_SIGNAL (netsim);
    GST_NET_SIM_UNLOCK (netsim);

    result = gst_pad_stop_task (pad);
  }

  return result;
}

static void
gst_net_sim_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (object);

  switch (prop_id) {
    case PROP_MIN_DELAY:
      netsim->min_delay = g_value_get_int (value);
      break;
    case PROP_MAX_DELAY:
      netsim->max_delay = g_value_get_int (value);
      break;
    case PROP_DELAY_DISTRIBUTION:
      netsim->delay_distribution = g_value_get_enum (value);
      break;
    case PROP_DELAY_PROBABILITY:
      netsim->delay_probability = g_value_get_float (value);
      break;
    case PROP_DROP_PROBABILITY:
      netsim->drop_probability = g_value_get_float (value);
      break;
    case PROP_DUPLICATE_PROBABILITY:
      netsim->duplicate_probability = g_value_get_float (value);
      break;
    case PROP_DROP_PACKETS:
      netsim->drop_packets = g_value_get_uint (value);
      break;
    case PROP_MAX_KBPS:
      netsim->max_kbps = g_value_get_int (value);
      break;
    case PROP_MAX_BUCKET_SIZE:
      netsim->max_bucket_size = g_value_get_int (value);
      if (netsim->max_bucket_size != -1)
        netsim->bucket_size = netsim->max_bucket_size * 1000;
      break;
    case PROP_QUEUE_SIZE:
      netsim->queue_size = g_value_get_int (value);
      break;
    case PROP_MAX_QUEUE_DELAY:
      netsim->max_queue_delay = g_value_get_int (value);
      break;
    case PROP_ALLOW_REORDERING:
      netsim->allow_reordering = g_value_get_boolean (value);
      if (netsim->allow_reordering)
        netsim->compare_func = nsbuf_compare_time;
      else
        netsim->compare_func = nsbuf_compare_seqnum;
      break;
    case PROP_REPLACE_DROPPED_WITH_EMPTY:
      netsim->replace_droppped_with_empty = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_net_sim_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (object);

  switch (prop_id) {
    case PROP_MIN_DELAY:
      g_value_set_int (value, netsim->min_delay);
      break;
    case PROP_MAX_DELAY:
      g_value_set_int (value, netsim->max_delay);
      break;
    case PROP_DELAY_DISTRIBUTION:
      g_value_set_enum (value, netsim->delay_distribution);
      break;
    case PROP_DELAY_PROBABILITY:
      g_value_set_float (value, netsim->delay_probability);
      break;
    case PROP_DROP_PROBABILITY:
      g_value_set_float (value, netsim->drop_probability);
      break;
    case PROP_DUPLICATE_PROBABILITY:
      g_value_set_float (value, netsim->duplicate_probability);
      break;
    case PROP_DROP_PACKETS:
      g_value_set_uint (value, netsim->drop_packets);
      break;
    case PROP_MAX_KBPS:
      g_value_set_int (value, netsim->max_kbps);
      break;
    case PROP_MAX_BUCKET_SIZE:
      g_value_set_int (value, netsim->max_bucket_size);
      break;
    case PROP_QUEUE_SIZE:
      g_value_set_int (value, netsim->queue_size);
      break;
    case PROP_MAX_QUEUE_DELAY:
      g_value_set_int (value, netsim->max_queue_delay);
      break;
    case PROP_ALLOW_REORDERING:
      g_value_set_boolean (value, netsim->allow_reordering);
      break;
    case PROP_REPLACE_DROPPED_WITH_EMPTY:
      g_value_set_boolean (value, netsim->replace_droppped_with_empty);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


static void
gst_net_sim_init (GstNetSim * netsim)
{
  netsim->srcpad =
      gst_pad_new_from_static_template (&gst_net_sim_src_template, "src");
  netsim->sinkpad =
      gst_pad_new_from_static_template (&gst_net_sim_sink_template, "sink");

  gst_element_add_pad (GST_ELEMENT (netsim), netsim->srcpad);
  gst_element_add_pad (GST_ELEMENT (netsim), netsim->sinkpad);

  netsim->bqueue = g_queue_new ();
  g_mutex_init (&netsim->mutex);
  g_cond_init (&netsim->cond);
  netsim->rand_seed = g_rand_new ();
  netsim->prev_time = GST_CLOCK_TIME_NONE;

  GST_OBJECT_FLAG_SET (netsim->sinkpad,
      GST_PAD_FLAG_PROXY_CAPS | GST_PAD_FLAG_PROXY_ALLOCATION);

  gst_pad_set_chain_function (netsim->sinkpad,
      GST_DEBUG_FUNCPTR (gst_net_sim_chain));
  gst_pad_set_activatemode_function (netsim->srcpad,
      GST_DEBUG_FUNCPTR (gst_net_sim_src_activatemode));
}

static void
gst_net_sim_finalize (GObject * object)
{
  GstNetSim *netsim = GST_NET_SIM_CAST (object);

  g_rand_free (netsim->rand_seed);
  g_mutex_clear (&netsim->mutex);
  g_cond_clear (&netsim->cond);

  gst_object_replace ((GstObject **) & netsim->clock, NULL);

  g_queue_free_full (netsim->bqueue, (GDestroyNotify) net_sim_buffer_free);

  G_OBJECT_CLASS (gst_net_sim_parent_class)->finalize (object);
}

static void
gst_net_sim_class_init (GstNetSimClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_net_sim_src_template);
  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_net_sim_sink_template);

  gst_element_class_set_metadata (gstelement_class,
      "Network Simulator",
      "Filter/Network",
      "An element that simulates network jitter, "
      "packet loss and packet duplication",
      "Philippe Kalaf <philippe.kalaf@collabora.co.uk>, "
      "Havard Graff <havard@pexip.com>");

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_net_sim_finalize);

  gobject_class->set_property = gst_net_sim_set_property;
  gobject_class->get_property = gst_net_sim_get_property;
  gstelement_class->set_clock = GST_DEBUG_FUNCPTR (gst_net_sim_set_clock);

  g_object_class_install_property (gobject_class, PROP_MIN_DELAY,
      g_param_spec_int ("min-delay", "Minimum delay (ms)",
          "The minimum delay in ms to apply to buffers",
          G_MININT, G_MAXINT, DEFAULT_MIN_DELAY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_DELAY,
      g_param_spec_int ("max-delay", "Maximum delay (ms)",
          "The maximum delay (inclusive) in ms to apply to buffers",
          G_MININT, G_MAXINT, DEFAULT_MAX_DELAY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:delay-distribution:
   *
   * Distribution for the amount of delay.
   *
   * Since: 1.14
   */
  g_object_class_install_property (gobject_class, PROP_DELAY_DISTRIBUTION,
      g_param_spec_enum ("delay-distribution", "Delay Distribution",
          "Distribution for the amount of delay",
          distribution_get_type (), DEFAULT_DELAY_DISTRIBUTION,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DELAY_PROBABILITY,
      g_param_spec_float ("delay-probability", "Delay Probability",
          "The Probability a buffer is delayed",
          0.0, 1.0, DEFAULT_DELAY_PROBABILITY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DROP_PROBABILITY,
      g_param_spec_float ("drop-probability", "Drop Probability",
          "The Probability a buffer is dropped",
          0.0, 1.0, DEFAULT_DROP_PROBABILITY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DUPLICATE_PROBABILITY,
      g_param_spec_float ("duplicate-probability", "Duplicate Probability",
          "The Probability a buffer is duplicated",
          0.0, 1.0, DEFAULT_DUPLICATE_PROBABILITY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DROP_PACKETS,
      g_param_spec_uint ("drop-packets", "Drop Packets",
          "Drop the next n packets",
          0, G_MAXUINT, DEFAULT_DROP_PACKETS,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_REPLACE_DROPPED_WITH_EMPTY,
      g_param_spec_boolean ("replace-dropped-with-empty-packets",
          "replace-dropped-with-empty-packets",
          "Insert packets with no payload instead of dropping",
          DEFAULT_REPLACE_DROPPED_WITH_EMPTY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:max-kbps:
   *
   * The maximum number of kilobits to let through per second. Setting this
   * property to a positive value enables network congestion simulation using
   * a token bucket algorithm. Also see the "max-bucket-size" property,
   *
   * Since: 1.14
   */
  g_object_class_install_property (gobject_class, PROP_MAX_KBPS,
      g_param_spec_int ("max-kbps", "Maximum Kbps",
          "The maximum number of kilobits to let through per second "
          "(-1 = unlimited)", -1, G_MAXINT, DEFAULT_MAX_KBPS,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:max-bucket-size:
   *
   * The size of the token bucket, related to burstiness resilience.
   *
   * Since: 1.14
   */
  g_object_class_install_property (gobject_class, PROP_MAX_BUCKET_SIZE,
      g_param_spec_int ("max-bucket-size", "Maximum Bucket Size (Kb)",
          "The size of the token bucket, related to burstiness resilience "
          "(-1 = unlimited)", -1, G_MAXINT, DEFAULT_MAX_BUCKET_SIZE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:queue-size:
   *
   * In case of insufficient tokens in the bucket, this property says how
   * much bits we can keep in the queue
   *
   * Since: 1.18
   */
  g_object_class_install_property (gobject_class, PROP_QUEUE_SIZE,
      g_param_spec_int ("queue-size", "Queue Size in bits",
          "The max number of bits in the internal queue "
          "(-1 = unlimited)",
          -1, G_MAXINT, DEFAULT_QUEUE_SIZE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:max-queue-delay:
   *
   * How long we will delay packets for in the queue
   *
   * Since: 1.18
   */
  g_object_class_install_property (gobject_class, PROP_MAX_QUEUE_DELAY,
      g_param_spec_int ("max-queue-delay", "Maximum queue delay (ms)",
          "The maximum delay a buffer can be given before being dropped",
          G_MININT, G_MAXINT, DEFAULT_MAX_QUEUE_DELAY,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstNetSim:allow-reordering:
   *
   * When delaying packets, are they allowed to be reordered or not. By
   * default this is enabled, but in the real world packet reordering is
   * fairly uncommon, yet the delay functions will always introduce reordering
   * if delay > packet-spacing, This property allows switching that off.
   *
   * Since: 1.14
   */
  g_object_class_install_property (gobject_class, PROP_ALLOW_REORDERING,
      g_param_spec_boolean ("allow-reordering", "Allow Reordering",
          "When delaying packets, are they allowed to be reordered or not",
          DEFAULT_ALLOW_REORDERING,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (netsim_debug, "netsim", 0, "Network simulator");

  gst_type_mark_as_plugin_api (distribution_get_type (), 0);
}

static gboolean
gst_net_sim_plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (netsim, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    netsim,
    "Network Simulator",
    gst_net_sim_plugin_init, PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
