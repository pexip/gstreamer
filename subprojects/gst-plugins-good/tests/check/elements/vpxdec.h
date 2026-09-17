/* GStreamer
 *
 * Registration hooks for the shared VP8/VP9 decoder error path tests
 * implemented in vpxdec.c. The tests are compiled into the vp8dec and the
 * vp9dec test binaries so that each codec can be exercised on its own when
 * only one of the two is built.
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

#ifndef __TEST_VPXDEC_H__
#define __TEST_VPXDEC_H__

#include <gst/check/gstcheck.h>

G_BEGIN_DECLS
/* Both add the shared decoder error path tests to @tc. Each one needs the
 * matching encoder as well, because the fixtures are generated at run time,
 * so nothing is added when the encoder is not available. */
void vpx_dec_add_vp8_tests (TCase * tc);
void vpx_dec_add_vp9_tests (TCase * tc);

G_END_DECLS
#endif /* __TEST_VPXDEC_H__ */
