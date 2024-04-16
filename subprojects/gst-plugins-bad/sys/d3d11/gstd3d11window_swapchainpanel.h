/*
 * GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2024 Tulio Beloqui <tulio@pexip.com>
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

#ifndef __GST_D3D11_WINDOW_SWAP_CHAIN_PANEL_H__
#define __GST_D3D11_WINDOW_SWAP_CHAIN_PANEL_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include "gstd3d11window.h"

G_BEGIN_DECLS

#define GST_TYPE_D3D11_WINDOW_SWAP_CHAIN_PANEL (gst_d3d11_window_swap_chain_panel_get_type())
G_DECLARE_FINAL_TYPE (GstD3D11WindowSwapChainPanel,
    gst_d3d11_window_swap_chain_panel,
    GST, D3D11_WINDOW_SWAP_CHAIN_PANEL, GstD3D11Window);

GstD3D11Window * gst_d3d11_window_swap_chain_panel_new (GstD3D11Device * device,
                                                        guintptr handle);

G_END_DECLS

#endif /* __GST_D3D11_WINDOW_SWAP_CHAIN_PANEL_H__ */
