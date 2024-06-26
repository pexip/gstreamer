/* GStreamer android.hardware.Camera2 Source
 * Copyright (C) 2017, Collabora Ltd.
 *   Author:Justin Kim <justin.kim@collabora.com>
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

#ifndef __GST_AHC2_SRC_H__
#define __GST_AHC2_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define GST_TYPE_AHC2_SRC              (gst_ahc2_src_get_type ())
#define GST_IS_AHC2_SRC(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_AHC2_SRC))
#define GST_IS_AHC2_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_AHC2_SRC))
#define GST_AHC2_SRC_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_AHC2_SRC, GstAHC2SrcClass))
#define GST_AHC2_SRC(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_AHC2_SRC, GstAHC2Src))
#define GST_AHC2_SRC_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_AHC2_SRC, GstAHC2SrcClass))
#define GST_AHC2_SRC_CAST(obj)         ((GstAHC2Src*)(obj))
#define GST_AHC2_SRC_CLASS_CAST(klass) ((GstAHC2SrcClass*)(klass))

typedef struct _GstAHC2Src GstAHC2Src;
typedef struct _GstAHC2SrcClass GstAHC2SrcClass;
typedef struct _GstAHC2SrcPrivate GstAHC2SrcPrivate;

struct _GstAHC2SrcClass {
  GstPushSrcClass parent_class;

  /* actions */
  const gchar*  (*get_camera_id_by_index)       (GstAHC2Src *self, gint idx);

  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GST_EXPORT
GType gst_ahc2_src_get_type (void);

G_END_DECLS

#endif /* __GST_AHC2_SRC_H__ */
