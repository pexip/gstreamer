/* GStreamer - GParamSpecs for some of our types
 * Copyright (C) 2007 Tim-Philipp Müller  <tim centricular net>
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

#ifndef __GST_PARAMSPECS_H__
#define __GST_PARAMSPECS_H__

#include <gst/gstvalue.h>

G_BEGIN_DECLS

/* --- paramspec flags */

/**
 * GST_PARAM_CONTROLLABLE:
 *
 * Use this flag on GObject properties to signal they can make sense to be.
 * controlled over time. This hint is used by the GstController.
 */
#define	GST_PARAM_CONTROLLABLE	(1 << (G_PARAM_USER_SHIFT + 1))

/**
 * GST_PARAM_MUTABLE_READY:
 *
 * Use this flag on GObject properties of GstElements to indicate that
 * they can be changed when the element is in the READY or lower state.
 */
#define GST_PARAM_MUTABLE_READY  (1 << (G_PARAM_USER_SHIFT + 2))

/**
 * GST_PARAM_MUTABLE_PAUSED:
 *
 * Use this flag on GObject properties of GstElements to indicate that
 * they can be changed when the element is in the PAUSED or lower state.
 * This flag implies GST_PARAM_MUTABLE_READY.
 */
#define GST_PARAM_MUTABLE_PAUSED  (1 << (G_PARAM_USER_SHIFT + 3))

/**
 * GST_PARAM_MUTABLE_PLAYING:
 *
 * Use this flag on GObject properties of GstElements to indicate that
 * they can be changed when the element is in the PLAYING or lower state.
 * This flag implies GST_PARAM_MUTABLE_PAUSED.
 */
#define GST_PARAM_MUTABLE_PLAYING  (1 << (G_PARAM_USER_SHIFT + 4))

/**
 * GST_PARAM_USER_SHIFT:
 *
 * Bits based on GST_PARAM_USER_SHIFT can be used by 3rd party applications.
 */
#define	GST_PARAM_USER_SHIFT	(1 << (G_PARAM_USER_SHIFT + 8))


/* --- type macros --- */

#define GST_TYPE_PARAM_FRACTION           (gst_param_spec_fraction_get_type ())
#define GST_IS_PARAM_SPEC_FRACTION(pspec) (G_TYPE_CHECK_INSTANCE_TYPE ((pspec), GST_TYPE_PARAM_FRACTION))
#define GST_PARAM_SPEC_FRACTION(pspec)    (G_TYPE_CHECK_INSTANCE_CAST ((pspec), GST_TYPE_PARAM_FRACTION, GstParamSpecFraction))

#define GST_TYPE_PARAM_INT_RANGE           (gst_param_spec_int_range_get_type ())
#define GST_IS_PARAM_SPEC_INT_RANGE(pspec) (G_TYPE_CHECK_INSTANCE_TYPE ((pspec), GST_TYPE_PARAM_INT_RANGE))
#define GST_PARAM_SPEC_INT_RANGE(pspec)    (G_TYPE_CHECK_INSTANCE_CAST ((pspec), GST_TYPE_PARAM_INT_RANGE, GstParamSpecIntRange))


/* --- get_type functions --- */

GType  gst_param_spec_fraction_get_type (void);
GType  gst_param_spec_int_range_get_type (void);


/* --- typedefs & structures --- */

typedef struct _GstParamSpecFraction GstParamSpecFraction;
typedef struct _GstParamSpecIntRange GstParamSpecIntRange;

/**
 * GstParamSpecFraction:
 * @parent_instance: super class
 * @min_num: minimal numerator
 * @min_den: minimal denominator
 * @max_num: maximal numerator
 * @max_den: maximal denominator
 * @def_num: default numerator
 * @def_den: default denominator
 *
 * A GParamSpec derived structure that contains the meta data for fractional
 * properties.
 */
struct _GstParamSpecFraction {
  GParamSpec    parent_instance;

  gint          min_num, min_den;
  gint          max_num, max_den;
  gint          def_num, def_den;
};

/**
 * GstParamSpecIntRange:
 * @parent_instance: super class
 * @min_min:  minimal lowest value in range
 * @min_max:  minimal hightst value in range
 * @min_step: minimal step
 * @max_min:  maximal lowest value in range
 * @max_max:  maximal hightst value in range
 * @max_step: maximal step
 * @def_min:  default lowest value in range
 * @def_max:  default hightst value in range
 * @def_step: default step
 *
 * A GParamSpec derived structure that contains the meta data for int range
 * properties.
 */
struct _GstParamSpecIntRange {
  GParamSpec    parent_instance;

  gint          min_min, min_max, min_step;
  gint          max_min, max_max, max_step;
  gint          def_min, def_max, def_step;
};

/* --- GParamSpec prototypes --- */

GParamSpec  * gst_param_spec_fraction (const gchar * name,
                                       const gchar * nick,
                                       const gchar * blurb,
                                       gint min_num, gint min_denom,
                                       gint max_num, gint max_denom,
                                       gint default_num, gint default_denom,
                                       GParamFlags flags) G_GNUC_MALLOC;

GParamSpec * gst_param_spec_int_range (const gchar * name,
                                       const gchar * nick,
                                       const gchar * blurb,
                                       gint min_min, gint min_max, gint min_step,
                                       gint max_min, gint max_max, gint max_step,
                                       gint def_min, gint def_max, gint def_step,
                                       GParamFlags flags) G_GNUC_MALLOC;


G_END_DECLS

#endif /* __GST_PARAMSPECS_H__ */

