/* GStreamer
 *
 * Unit tests for vp9dec.
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

#include <gst/check/gstcheck.h>

#include "vpxdec.h"

static Suite *
vp9dec_suite (void)
{
  Suite *s = suite_create ("vp9dec");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  vpx_dec_add_vp9_tests (tc_chain);

  return s;
}

GST_CHECK_MAIN (vp9dec);
