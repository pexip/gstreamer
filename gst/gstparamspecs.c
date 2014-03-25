/* GStreamer - GParamSpecs for some of our types
 * Copyright (C) 2007 Tim-Philipp Müller  <tim centricular net>
 * Copyright (C) 2014 Haakon Sporsheim  <haakon pexip com>
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
/**
 * SECTION:gstparamspec
 * @short_description: GParamSpec implementations specific
 * to GStreamer
 *
 * GParamSpec implementations specific to GStreamer.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_private.h"
#include "glib-compat-private.h"
#include "gstparamspecs.h"

/* --- GstParamSpecFraction --- */

static void
_gst_param_fraction_init (GParamSpec * pspec)
{
  GstParamSpecFraction *fspec = GST_PARAM_SPEC_FRACTION (pspec);

  fspec->min_num = 0;
  fspec->min_den = 1;
  fspec->max_num = G_MAXINT;
  fspec->max_den = 1;
  fspec->def_num = 1;
  fspec->def_den = 1;
}

static void
_gst_param_fraction_set_default (GParamSpec * pspec, GValue * value)
{
  value->data[0].v_int = GST_PARAM_SPEC_FRACTION (pspec)->def_num;
  value->data[1].v_int = GST_PARAM_SPEC_FRACTION (pspec)->def_den;
}

static gboolean
_gst_param_fraction_validate (GParamSpec * pspec, GValue * value)
{
  GstParamSpecFraction *fspec = GST_PARAM_SPEC_FRACTION (pspec);
  gboolean within_range = FALSE;
  GValue f_this = { 0, };
  GValue f_min = { 0, };
  GValue f_max = { 0, };
  gint res;

  g_value_init (&f_this, GST_TYPE_FRACTION);
  gst_value_set_fraction (&f_this, value->data[0].v_int, value->data[1].v_int);

  g_value_init (&f_min, GST_TYPE_FRACTION);
  gst_value_set_fraction (&f_min, fspec->min_num, fspec->min_den);

  g_value_init (&f_max, GST_TYPE_FRACTION);
  gst_value_set_fraction (&f_max, fspec->max_num, fspec->max_den);

  res = gst_value_compare (&f_min, &f_this);
#ifndef GST_DISABLE_GST_DEBUG
  GST_LOG ("comparing %d/%d to %d/%d, result = %d", fspec->min_num,
      fspec->min_den, value->data[0].v_int, value->data[1].v_int, res);
#endif
  if (res != GST_VALUE_LESS_THAN && res != GST_VALUE_EQUAL)
    goto out;

#ifndef GST_DISABLE_GST_DEBUG
  GST_LOG ("comparing %d/%d to %d/%d, result = %d", value->data[0].v_int,
      value->data[1].v_int, fspec->max_num, fspec->max_den, res);
#endif
  res = gst_value_compare (&f_this, &f_max);
  if (res != GST_VALUE_LESS_THAN && res != GST_VALUE_EQUAL)
    goto out;

  within_range = TRUE;

out:

  g_value_unset (&f_min);
  g_value_unset (&f_max);
  g_value_unset (&f_this);

#ifndef GST_DISABLE_GST_DEBUG
  GST_LOG ("%swithin range", (within_range) ? "" : "not ");
#endif

  /* return FALSE if everything ok, otherwise TRUE */
  return !within_range;
}

static gint
_gst_param_fraction_values_cmp (GParamSpec * pspec, const GValue * value1,
    const GValue * value2)
{
  gint res;

  res = gst_value_compare (value1, value2);

  g_assert (res != GST_VALUE_UNORDERED);

  /* GST_VALUE_LESS_THAN is -1, EQUAL is 0, and GREATER_THAN is 1 */
  return res;
}

GType
gst_param_spec_fraction_get_type (void)
{
  static GType type;            /* 0 */

  /* register GST_TYPE_PARAM_FRACTION */
  if (type == 0) {
    static GParamSpecTypeInfo pspec_info = {
      sizeof (GstParamSpecFraction),    /* instance_size     */
      0,                        /* n_preallocs       */
      _gst_param_fraction_init, /* instance_init     */
      G_TYPE_INVALID,           /* value_type        */
      NULL,                     /* finalize          */
      _gst_param_fraction_set_default,  /* value_set_default */
      _gst_param_fraction_validate,     /* value_validate    */
      _gst_param_fraction_values_cmp,   /* values_cmp        */
    };
    pspec_info.value_type = GST_TYPE_FRACTION;
    type = g_param_type_register_static ("GstParamFraction", &pspec_info);
  }
  return type;
}

/**
 * gst_param_spec_fraction:
 * @name: canonical name of the property specified
 * @nick: nick name for the property specified
 * @blurb: description of the property specified
 * @min_num: minimum value (fraction numerator)
 * @min_denom: minimum value (fraction denominator)
 * @max_num: maximum value (fraction numerator)
 * @max_denom: maximum value (fraction denominator)
 * @default_num: default value (fraction numerator)
 * @default_denom: default value (fraction denominator)
 * @flags: flags for the property specified
 *
 * This function creates a fraction GParamSpec for use by objects/elements
 * that want to expose properties of fraction type. This function is typically
 * used in connection with g_object_class_install_property() in a GObjects's
 * instance_init function.
 *
 * Returns: (transfer full): a newly created parameter specification
 */
GParamSpec *
gst_param_spec_fraction (const gchar * name, const gchar * nick,
    const gchar * blurb, gint min_num, gint min_denom, gint max_num,
    gint max_denom, gint default_num, gint default_denom, GParamFlags flags)
{
  GstParamSpecFraction *fspec;
  GParamSpec *pspec;
  GValue default_val = { 0, };

  fspec =
      g_param_spec_internal (GST_TYPE_PARAM_FRACTION, name, nick, blurb, flags);

  fspec->min_num = min_num;
  fspec->min_den = min_denom;
  fspec->max_num = max_num;
  fspec->max_den = max_denom;
  fspec->def_num = default_num;
  fspec->def_den = default_denom;

  pspec = G_PARAM_SPEC (fspec);

  /* check that min <= default <= max */
  g_value_init (&default_val, GST_TYPE_FRACTION);
  gst_value_set_fraction (&default_val, default_num, default_denom);
  /* validate returns TRUE if the validation fails */
  if (_gst_param_fraction_validate (pspec, &default_val)) {
    g_critical ("GstParamSpec of type 'fraction' for property '%s' has a "
        "default value of %d/%d, which is not within the allowed range of "
        "%d/%d to %d/%d", name, default_num, default_denom, min_num,
        min_denom, max_num, max_denom);
    g_param_spec_ref (pspec);
    g_param_spec_sink (pspec);
    g_param_spec_unref (pspec);
    pspec = NULL;
  }
  g_value_unset (&default_val);

  return pspec;
}

/* --- GstParamSpecIntRange --- */

static void
_gst_param_int_range_init (GParamSpec * pspec)
{
  GstParamSpecIntRange *irspec = GST_PARAM_SPEC_INT_RANGE (pspec);

  irspec->min_min  = G_MININT;
  irspec->min_max  = G_MININT;
  irspec->min_step = G_MININT;
  irspec->max_min  = G_MAXINT;
  irspec->max_max  = G_MAXINT;
  irspec->max_step = G_MAXINT;

  irspec->def_min  = G_MININT;
  irspec->def_max  = G_MAXINT;
  irspec->def_step = 1;
}

static void
_gst_param_int_range_set_default (GParamSpec * pspec, GValue * value)
{
  GstParamSpecIntRange *irspec = GST_PARAM_SPEC_INT_RANGE (pspec);

  gst_value_set_int_range_step (value,
      irspec->def_min, irspec->def_max, irspec->def_step);
}

static gboolean
_gst_param_int_range_validate (GParamSpec * pspec, GValue * value)
{
  (void)pspec;

  g_return_val_if_fail (GST_VALUE_HOLDS_INT_RANGE (value), TRUE);
  g_return_val_if_fail (gst_value_get_int_range_step (value) > 0, TRUE);

  if (gst_value_get_int_range_max (value) < gst_value_get_int_range_min (value)) {
    gst_value_set_int_range (value, gst_value_get_int_range_min (value),
        gst_value_get_int_range_min (value));
    return TRUE;
  }

  return FALSE;
}

static gint
_gst_param_int_range_values_cmp (GParamSpec * pspec, const GValue * value1,
    const GValue * value2)
{
  /* GST_VALUE_LESS_THAN is -1, EQUAL is 0, and GREATER_THAN is 1 */
  return gst_value_compare (value1, value2);
}

GType
gst_param_spec_int_range_get_type (void)
{
  static GType type;            /* 0 */

  /* register GST_TYPE_PARAM_INT_RANGE */
  if (type == 0) {
    static GParamSpecTypeInfo pspec_info = {
      sizeof (GstParamSpecIntRange),     /* instance_size     */
      0,                                 /* n_preallocs       */
      _gst_param_int_range_init,         /* instance_init     */
      G_TYPE_INVALID,                    /* value_type        */
      NULL,                              /* finalize          */
      _gst_param_int_range_set_default,  /* value_set_default */
      _gst_param_int_range_validate,     /* value_validate    */
      _gst_param_int_range_values_cmp,   /* values_cmp        */
    };
    pspec_info.value_type = GST_TYPE_INT_RANGE;
    type = g_param_type_register_static ("GstParamIntRange", &pspec_info);
  }
  return type;
}

/**
 * gst_param_spec_int_range:
 * @name: canonical name of the property specified
 * @nick: nick name for the property specified
 * @blurb: description of the property specified
 * @min_min:  minimum min value
 * @min_max:  minimum max value
 * @min_step: minimum step
 * @max_min:  maximum min value
 * @max_max:  maximum max value
 * @max_step: maximum step
 * @def_min:  default min value
 * @def_max:  default max value
 * @def_step: default step
 * @flags: flags for the property specified
 *
 * This function creates an int range GParamSpec for use by objects/elements
 * that want to expose properties of int range type. This function is typically
 * used in connection with g_object_class_install_property() in a GObjects's
 * instance_init function.
 *
 * Returns: (transfer full): a newly created parameter specification
 */
GParamSpec *
gst_param_spec_int_range (const gchar * name, const gchar * nick, const gchar * blurb,
    gint min_min, gint min_max, gint min_step,
    gint max_min, gint max_max, gint max_step,
    gint def_min, gint def_max, gint def_step,
    GParamFlags flags)
{
  GstParamSpecIntRange *irspec;
  GParamSpec *pspec;
  GValue default_val = G_VALUE_INIT;

  irspec =
      g_param_spec_internal (GST_TYPE_PARAM_INT_RANGE, name, nick, blurb, flags);

  irspec->min_min  = min_min;
  irspec->min_max  = min_max;
  irspec->min_step = min_step;
  irspec->max_min  = max_min;
  irspec->max_max  = max_max;
  irspec->max_step = max_step;
  irspec->def_min  = def_min;
  irspec->def_max  = def_max;
  irspec->def_step = def_step;

  pspec = G_PARAM_SPEC (irspec);

  /* check that min <= default <= max */
  g_value_init (&default_val, GST_TYPE_INT_RANGE);
  gst_value_set_int_range_step (&default_val, def_min, def_max, def_step);

  if (_gst_param_int_range_validate (pspec, &default_val)) {
    g_critical ("GstParamSpec of type 'int_range' for property '%s' has a "
        "default value of %d->%d (%d), which is not within the allowed range of "
        "%d->%d to %d->%d",
        name, def_min, def_max, def_step, min_min, max_min, min_max, max_max);
    g_param_spec_ref (pspec);
    g_param_spec_sink (pspec);
    g_param_spec_unref (pspec);
    pspec = NULL;
  }
  g_value_unset (&default_val);

  return pspec;
}

