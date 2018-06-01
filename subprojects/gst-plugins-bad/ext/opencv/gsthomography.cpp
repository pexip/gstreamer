/*
 * GStreamer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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
#  include <config.h>
#endif

#include "gsthomography.h"
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2//calib3d/calib3d.hpp>

GST_DEBUG_CATEGORY_STATIC (gst_homography_debug);
#define GST_CAT_DEFAULT gst_homography_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("RGB"))
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("RGB"))
    );

G_DEFINE_TYPE (GstHomography, gst_homography, GST_TYPE_OPENCV_VIDEO_FILTER);

static void gst_homography_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_homography_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_homography_transform (GstOpencvVideoFilter * filter,
    GstBuffer * buf, IplImage * img, GstBuffer * outbuf, IplImage * outimg);

static void
gst_homography_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstHomography *filter = GST_HOMOGRAPHY (object);
  (void)filter;
  (void)value;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_homography_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstHomography *filter = GST_HOMOGRAPHY (object);
  (void)filter;
  (void)value;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_homography_transform_caps (GstBaseTransform * trans,
    GstPadDirection dir, GstCaps * caps, GstCaps * filter)
{
  GstCaps * ret = gst_caps_copy (caps);
  guint i;

  GST_DEBUG_OBJECT (trans,
      "transforming caps %"GST_PTR_FORMAT" filter %"GST_PTR_FORMAT" in direction %s",
      caps, filter, (dir == GST_PAD_SINK) ? "sink" : "src");

  for (i = 0; i < gst_caps_get_size (ret); ++i) {
    GstStructure * s = gst_caps_get_structure (ret, i);

    gst_structure_set (s,
        "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
  }

  if (!gst_caps_is_empty (ret) && filter) {
    GstCaps *tmp = gst_caps_intersect_full (filter, ret,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_replace (&ret, tmp);
    gst_caps_unref (tmp);
  }

  GST_DEBUG_OBJECT (trans, "returning caps %" GST_PTR_FORMAT, ret);

  return ret;
}


static float
euclidean_distance (CvPoint2D32f * a, CvPoint2D32f * b)
{
  CvPoint2D32f d;
  d.x = a->x - b->x;
  d.y = a->y - b->y;

  return sqrtf (d.x * d.x + d.y * d.y);
}

static GstCaps *
gst_homography_fixate_caps (GstBaseTransform * trans,
    GstPadDirection dir, GstCaps * caps, GstCaps * othercaps)
{
  GstHomography *filter = GST_HOMOGRAPHY (trans);
  GstCaps * ret;
  GstStructure * out_s;

  g_return_val_if_fail (gst_caps_is_fixed (caps), NULL);

  GST_DEBUG_OBJECT (filter,
      "fixating othercaps %" GST_PTR_FORMAT " in direction %s based on %"GST_PTR_FORMAT,
      othercaps, (dir == GST_PAD_SINK) ? "sink" : "src", caps);

  ret = gst_caps_intersect (othercaps, caps);
  if (gst_caps_is_empty (ret)) {
    gst_caps_unref (ret);
    ret = gst_caps_ref (othercaps);
  }

  ret = gst_caps_make_writable (ret);
  out_s = gst_caps_get_structure (ret, 0);

  float top    = euclidean_distance (&filter->src_points[0], &filter->src_points[1]);
  float right  = euclidean_distance (&filter->src_points[1], &filter->src_points[2]);
  float bottom = euclidean_distance (&filter->src_points[2], &filter->src_points[3]);
  float left   = euclidean_distance (&filter->src_points[3], &filter->src_points[0]);

  //float aspect_ratio = (top + bottom) / 2.0f + (right + left) / 2.0f;

  gint max_width = (gint)round (MAX (top, bottom));
  gint max_height = (gint)round (MAX (right, left));

  gst_structure_set (out_s,
      "width", G_TYPE_INT, max_width,
      "height", G_TYPE_INT, max_height,
      NULL);

  return ret;
}

static GstFlowReturn
gst_homography_transform (GstOpencvVideoFilter * base, GstBuffer * buf,
    IplImage * img, GstBuffer * outbuf, IplImage * outimg)
{
  GstHomography *filter = GST_HOMOGRAPHY (base);

  CvPoint2D32f dst_points[4];

  dst_points[0].x = 0;
  dst_points[0].y = 0;
  dst_points[1].x = outimg->width - 1;
  dst_points[1].y = 0;
  dst_points[2].x = outimg->width - 1;
  dst_points[2].y = outimg->height - 1;
  dst_points[3].x = 0;
  dst_points[3].y = outimg->height - 1;

  CvMat src_mat = cvMat (1, 4, CV_32FC2, &filter->src_points);
  CvMat dst_mat = cvMat (1, 4, CV_32FC2, &dst_points);

  double H[9];
  CvMat res_mat = cvMat (3, 3, CV_64F, H);

  cvFindHomography(&src_mat, &dst_mat, &res_mat, CV_RANSAC);

  cvWarpPerspective (img, outimg, &res_mat);

  return GST_FLOW_OK;
}


/* initialize the homography's class */
static void
gst_homography_class_init (GstHomographyClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = (GstBaseTransformClass *) klass;
  GstOpencvVideoFilterClass *gstopencvbasefilter_class = (GstOpencvVideoFilterClass *) klass;

  gobject_class->set_property = gst_homography_set_property;
  gobject_class->get_property = gst_homography_get_property;

  trans_class->transform_caps = GST_DEBUG_FUNCPTR (gst_homography_transform_caps);
  trans_class->fixate_caps = GST_DEBUG_FUNCPTR (gst_homography_fixate_caps);

  gstopencvbasefilter_class->cv_trans_func = gst_homography_transform;

  gst_element_class_set_static_metadata (element_class,
      "homography",
      "Filter/Effect/Video",
      "Performs homography.",
      "Michael Sheldon <mike@mikeasoft.com>");

  gst_element_class_add_static_pad_template (element_class, &src_factory);
  gst_element_class_add_static_pad_template (element_class, &sink_factory);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_homography_init (GstHomography * filter)
{
  filter->src_points[0].x = 740;
  filter->src_points[0].y = 187;

  filter->src_points[1].x = 1043;
  filter->src_points[1].y = 165;

  filter->src_points[2].x = 1115;
  filter->src_points[2].y = 604;

  filter->src_points[3].x = 771;
  filter->src_points[3].y = 616;

  gst_opencv_video_filter_set_in_place (GST_OPENCV_VIDEO_FILTER_CAST (filter),
      FALSE);
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
gboolean
gst_homography_plugin_init (GstPlugin * plugin)
{
  /* debug category for fltering log messages */
  GST_DEBUG_CATEGORY_INIT (gst_homography_debug, "homography",
      0, "Performs homography on videos and images");

  return gst_element_register (plugin, "homography", GST_RANK_NONE,
      GST_TYPE_HOMOGRAPHY);
}
