/* VPX
 * Copyright (C) 2006 David Schleef <ds@schleef.org>
 * Copyright (C) 2010 Entropy Wave Inc
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
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstvpxelements.h"

#include <gst/tag/tag.h>

#define VP8_META_NAME "GstVP8Meta"

void
vpx_element_init (GstPlugin * plugin)
{
  static gsize res = FALSE;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&res)) {
    if (!gst_meta_get_info (VP8_META_NAME)) {
      gst_meta_register_custom (VP8_META_NAME, tags, NULL, NULL, NULL);
    }
    g_once_init_leave (&res, TRUE);
  }
}
