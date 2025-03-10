/* GStreamer
 * Copyright (C) 2023 Seungha Yang <seungha@centricular.com>
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
#include "config.h"
#endif

#include "gstd3d12.h"
#include "gstd3d12-private.h"
#include "gstd3d12converter-builder.h"
#include "gstd3d12converter-private.h"
#include "gstd3d12converter-pack.h"
#include "gstd3d12converter-unpack.h"
#include <directx/d3dx12.h>
#include <wrl.h>
#include <string.h>
#include <math.h>
#include <map>
#include <vector>
#include <memory>
#include <queue>

#ifndef HAVE_DIRECTX_MATH_SIMD
#define _XM_NO_INTRINSICS_
#endif
#include <DirectXMath.h>

GST_DEBUG_CATEGORY (gst_d3d12_converter_debug);
#define GST_CAT_DEFAULT gst_d3d12_converter_debug

GType
gst_d3d12_converter_sampler_filter_get_type (void)
{
  static GType filter_type = 0;
  static const GEnumValue filter_types[] = {
    {D3D12_FILTER_MIN_MAG_MIP_POINT,
        "D3D12_FILTER_MIN_MAG_MIP_POINT", "min-mag-mip-point"},
    {D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT,
        "D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT", "min-linear-mag-mip-point"},
    {D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
        "D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT", "min-mag-linear-mip-point"},
    {D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        "D3D12_FILTER_MIN_MAG_MIP_LINEAR", "min-mag-mip-linear"},
    {D3D12_FILTER_ANISOTROPIC, "D3D12_FILTER_ANISOTROPIC", "anisotropic"},
    {0, nullptr, nullptr},
  };

  GST_D3D12_CALL_ONCE_BEGIN {
    filter_type = g_enum_register_static ("GstD3D12ConverterSamplerFilter",
        filter_types);
  } GST_D3D12_CALL_ONCE_END;

  return filter_type;
}

GType
gst_d3d12_converter_alpha_mode_get_type (void)
{
  static GType type = 0;
  static const GEnumValue alpha_mode[] = {
    {GST_D3D12_CONVERTER_ALPHA_MODE_UNSPECIFIED,
        "GST_D3D12_CONVERTER_ALPHA_MODE_UNSPECIFIED", "unspecified"},
    {GST_D3D12_CONVERTER_ALPHA_MODE_PREMULTIPLIED,
        "GST_D3D12_CONVERTER_ALPHA_MODE_PREMULTIPLIED", "premultiplied"},
    {GST_D3D12_CONVERTER_ALPHA_MODE_STRAIGHT,
        "GST_D3D12_CONVERTER_ALPHA_MODE_STRAIGHT", "straight"},
    {0, nullptr, nullptr},
  };

  GST_D3D12_CALL_ONCE_BEGIN {
    type = g_enum_register_static ("GstD3D12ConverterAlphaMode", alpha_mode);
  } GST_D3D12_CALL_ONCE_END;

  return type;
}

/* *INDENT-OFF* */
using namespace Microsoft::WRL;
using namespace DirectX;
/* *INDENT-ON* */

#define GAMMA_LUT_SIZE 4096
#define DEFAULT_BUFFER_COUNT 2
#define DEFAULT_SAMPLER_FILTER D3D12_FILTER_MIN_MAG_MIP_LINEAR
static const WORD g_indices[6] = { 0, 1, 2, 3, 0, 2 };

struct PSColorSpace
{
  /* + 1 for 16bytes alignment  */
  FLOAT coeffX[4];
  FLOAT coeffY[4];
  FLOAT coeffZ[4];
  FLOAT offset[4];
  FLOAT min[4];
  FLOAT max[4];
};

struct PSConstBuffer
{
  PSColorSpace preCoeff;
  PSColorSpace postCoeff;
  PSColorSpace primariesCoeff;
};

struct VertexData
{
  struct
  {
    FLOAT x;
    FLOAT y;
    FLOAT z;
  } position;
  struct
  {
    FLOAT u;
    FLOAT v;
  } texture;
};

struct GammaLut
{
  guint16 lut[GAMMA_LUT_SIZE];
};

/* *INDENT-OFF* */
typedef std::shared_ptr<GammaLut> GammaLutPtr;

static const XMFLOAT4X4A g_matrix_identity = XMFLOAT4X4A (
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_90r = XMFLOAT4X4A (
    0.0f, -1.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_180 = XMFLOAT4X4A (
    -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_90l = XMFLOAT4X4A (
    0.0f, 1.0f, 0.0f, 0.0f,
    -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_horiz = XMFLOAT4X4A (
    -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_vert = XMFLOAT4X4A (
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_ul_lr = XMFLOAT4X4A (
    0.0f, -1.0f, 0.0f, 0.0f,
    -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

static const XMFLOAT4X4A g_matrix_ur_ll = XMFLOAT4X4A (
    0.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f);

constexpr UINT g_vertex_buf_size = sizeof (VertexData) * 4;
constexpr UINT g_index_buf_size = sizeof (g_indices);
constexpr UINT g_const_buf_size = sizeof (PSConstBuffer);
/* *INDENT-ON* */

enum
{
  PROP_0,
  PROP_SRC_X,
  PROP_SRC_Y,
  PROP_SRC_WIDTH,
  PROP_SRC_HEIGHT,
  PROP_DEST_X,
  PROP_DEST_Y,
  PROP_DEST_WIDTH,
  PROP_DEST_HEIGHT,
  PROP_ALPHA,
  PROP_FILL_BORDER,
  PROP_BORDER_COLOR,
  PROP_VIDEO_DIRECTION,
  PROP_SAMPLER_FILTER,
};

/* *INDENT-OFF* */
struct QuadData
{
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = { };
  ComPtr<ID3D12PipelineState> pso;
  guint num_rtv;
};

#define STATE_VERTEX_AND_INDEX \
  (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER)

struct _GstD3D12ConverterPrivate
{
  _GstD3D12ConverterPrivate ()
  {
    transform = g_matrix_identity;
    custom_transform = g_matrix_identity;
    blend_desc = CD3DX12_BLEND_DESC (D3D12_DEFAULT);
    for (guint i = 0; i < 4; i++)
      blend_factor[i] = 1.0f;

    sample_desc.Count = 1;
    sample_desc.Quality = 0;
  }

  ~_GstD3D12ConverterPrivate ()
  {
    if (fence_val > 0 && cq)
      gst_d3d12_command_queue_fence_wait (cq, fence_val);

    gst_clear_object (&srv_heap_pool);
    gst_clear_object (&cq);
    gst_clear_object (&pack);
    gst_clear_object (&unpack);
  }

  GstD3D12CommandQueue *cq = nullptr;
  GstD3D12Unpack *unpack = nullptr;
  GstD3D12Pack *pack = nullptr;

  GstVideoInfo in_info;
  GstVideoInfo out_info;

  CONVERT_TYPE convert_type = CONVERT_TYPE::IDENTITY;

  D3D12_VIEWPORT viewport[GST_VIDEO_MAX_PLANES];
  D3D12_RECT scissor_rect[GST_VIDEO_MAX_PLANES];

  D3D12_BLEND_DESC blend_desc;
  D3D12_INPUT_ELEMENT_DESC input_desc[2];
  FLOAT blend_factor[4];
  DXGI_SAMPLE_DESC sample_desc;
  gboolean update_pso = FALSE;
  gboolean update_sampler = FALSE;

  ConverterRootSignaturePtr crs;
  ComPtr<ID3D12RootSignature> rs;

  D3D12_VERTEX_BUFFER_VIEW vbv;
  D3D12_INDEX_BUFFER_VIEW idv;
  D3D12_GPU_VIRTUAL_ADDRESS const_buf_addr;
  ComPtr<ID3D12Resource> shader_buf;
  ComPtr<ID3D12Resource> vertex_upload;
  ComPtr<ID3D12Resource> gamma_dec_lut;
  ComPtr<ID3D12Resource> gamma_enc_lut;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT gamma_lut_layout;
  ComPtr<ID3D12DescriptorHeap> gamma_lut_heap;
  ComPtr<ID3D12DescriptorHeap> sampler_heap;

  std::vector<QuadData> quad_data;

  GstD3D12DescriptorPool *srv_heap_pool = nullptr;

  guint srv_inc_size;
  guint rtv_inc_size;
  guint sampler_inc_size;

  guint64 input_texture_width;
  guint input_texture_height;
  gboolean update_src_rect = FALSE;
  gboolean update_dest_rect = FALSE;
  gboolean update_transform = FALSE;
  XMFLOAT4X4A transform;
  XMFLOAT4X4A custom_transform;

  PSConstBuffer const_data;

  gboolean clear_background = FALSE;
  FLOAT clear_color[4][4];
  GstD3D12ColorMatrix clear_color_matrix;

  GstVideoOrientationMethod video_direction;

  std::mutex prop_lock;
  guint64 fence_val = 0;

  /* properties */
  gint src_x = 0;
  gint src_y = 0;
  gint src_width = 0;
  gint src_height = 0;
  gint dest_x = 0;
  gint dest_y = 0;
  gint dest_width = 0;
  gint dest_height = 0;
  FLOAT alpha = 1.0;
  gboolean fill_border = FALSE;
  guint64 border_color = 0;
  GstD3D12ConverterAlphaMode src_alpha_mode =
      GST_D3D12_CONVERTER_ALPHA_MODE_UNSPECIFIED;
  GstD3D12ConverterAlphaMode dst_alpha_mode =
      GST_D3D12_CONVERTER_ALPHA_MODE_UNSPECIFIED;
  D3D12_FILTER sampler_filter = DEFAULT_SAMPLER_FILTER;
};
/* *INDENT-ON* */

static void gst_d3d12_converter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_d3d12_converter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_d3d12_converter_finalize (GObject * object);
static void
gst_d3d12_converter_calculate_border_color (GstD3D12Converter * self);

#define gst_d3d12_converter_parent_class parent_class
G_DEFINE_TYPE (GstD3D12Converter, gst_d3d12_converter, GST_TYPE_OBJECT);

static void
gst_d3d12_converter_class_init (GstD3D12ConverterClass * klass)
{
  auto object_class = G_OBJECT_CLASS (klass);
  auto param_flags = (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  object_class->set_property = gst_d3d12_converter_set_property;
  object_class->get_property = gst_d3d12_converter_get_property;
  object_class->finalize = gst_d3d12_converter_finalize;

  g_object_class_install_property (object_class, PROP_SRC_X,
      g_param_spec_int ("src-x", "Src-X",
          "Source x poisition to start conversion", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_SRC_Y,
      g_param_spec_int ("src-y", "Src-Y",
          "Source y poisition to start conversion", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_SRC_WIDTH,
      g_param_spec_int ("src-width", "Src-Width",
          "Source width to convert", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_SRC_HEIGHT,
      g_param_spec_int ("src-height", "Src-Height",
          "Source height to convert", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_DEST_X,
      g_param_spec_int ("dest-x", "Dest-X",
          "x poisition in the destination frame", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_DEST_Y,
      g_param_spec_int ("dest-y", "Dest-Y",
          "y poisition in the destination frame", G_MININT, G_MAXINT, 0,
          param_flags));
  g_object_class_install_property (object_class, PROP_DEST_WIDTH,
      g_param_spec_int ("dest-width", "Dest-Width",
          "Width in the destination frame", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_DEST_HEIGHT,
      g_param_spec_int ("dest-height", "Dest-Height",
          "Height in the destination frame", 0, G_MAXINT, 0, param_flags));
  g_object_class_install_property (object_class, PROP_ALPHA,
      g_param_spec_double ("alpha", "Alpha",
          "The alpha color value to use", 0, 1.0, 1.0, param_flags));
  g_object_class_install_property (object_class, PROP_FILL_BORDER,
      g_param_spec_boolean ("fill-border", "Fill border",
          "Fill border with \"border-color\" if destination rectangle does not "
          "fill the complete destination image", FALSE, param_flags));
  g_object_class_install_property (object_class, PROP_BORDER_COLOR,
      g_param_spec_uint64 ("border-color", "Border Color",
          "ARGB representation of the border color to use",
          0, G_MAXUINT64, 0xffff000000000000, param_flags));
  g_object_class_install_property (object_class, PROP_VIDEO_DIRECTION,
      g_param_spec_enum ("video-direction", "Video Direction",
          "Video direction", GST_TYPE_VIDEO_ORIENTATION_METHOD,
          GST_VIDEO_ORIENTATION_IDENTITY, param_flags));
  g_object_class_install_property (object_class, PROP_SAMPLER_FILTER,
      g_param_spec_enum ("sampler-filter", "Sampler Filter",
          "Sampler Filter", GST_TYPE_D3D12_CONVERTER_SAMPLER_FILTER,
          DEFAULT_SAMPLER_FILTER, param_flags));

  GST_DEBUG_CATEGORY_INIT (gst_d3d12_converter_debug,
      "d3d12converter", 0, "d3d12converter");
}

static void
gst_d3d12_converter_init (GstD3D12Converter * self)
{
  self->priv = new GstD3D12ConverterPrivate ();
}

static void
gst_d3d12_converter_finalize (GObject * object)
{
  auto self = GST_D3D12_CONVERTER (object);

  delete self->priv;
  gst_clear_object (&self->device);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
update_src_rect (GstD3D12Converter * self, gint * old_val,
    const GValue * new_val)
{
  auto priv = self->priv;
  gint tmp;

  tmp = g_value_get_int (new_val);
  if (tmp != *old_val) {
    priv->update_src_rect = TRUE;
    *old_val = tmp;
  }
}

static void
update_dest_rect (GstD3D12Converter * self, gint * old_val,
    const GValue * new_val)
{
  auto priv = self->priv;
  gint tmp;

  tmp = g_value_get_int (new_val);
  if (tmp != *old_val) {
    priv->update_dest_rect = TRUE;
    *old_val = tmp;
  }
}

static void
gst_d3d12_converter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  auto self = GST_D3D12_CONVERTER (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->prop_lock);
  switch (prop_id) {
    case PROP_SRC_X:
      update_src_rect (self, &priv->src_x, value);
      break;
    case PROP_SRC_Y:
      update_src_rect (self, &priv->src_y, value);
      break;
    case PROP_SRC_WIDTH:
      update_src_rect (self, &priv->src_width, value);
      break;
    case PROP_SRC_HEIGHT:
      update_src_rect (self, &priv->src_height, value);
      break;
    case PROP_DEST_X:
      update_dest_rect (self, &priv->dest_x, value);
      break;
    case PROP_DEST_Y:
      update_dest_rect (self, &priv->dest_y, value);
      break;
    case PROP_DEST_WIDTH:
      update_dest_rect (self, &priv->dest_width, value);
      break;
    case PROP_DEST_HEIGHT:
      update_dest_rect (self, &priv->dest_height, value);
      break;
    case PROP_ALPHA:
      priv->alpha = g_value_get_double (value);
      break;
    case PROP_FILL_BORDER:
    {
      gboolean fill_border = g_value_get_boolean (value);

      if (fill_border != priv->fill_border) {
        priv->update_dest_rect = TRUE;
        priv->fill_border = fill_border;
      }
      break;
    }
    case PROP_BORDER_COLOR:
    {
      guint64 border_color = g_value_get_uint64 (value);

      if (border_color != priv->border_color) {
        priv->border_color = border_color;
        gst_d3d12_converter_calculate_border_color (self);
      }
      break;
    }
    case PROP_VIDEO_DIRECTION:
    {
      GstVideoOrientationMethod video_direction =
          (GstVideoOrientationMethod) g_value_get_enum (value);
      if (video_direction != priv->video_direction) {
        priv->video_direction = video_direction;
        priv->update_transform = TRUE;
      }
      break;
    }
    case PROP_SAMPLER_FILTER:
    {
      auto filter = (D3D12_FILTER) g_value_get_enum (value);
      if (filter != priv->sampler_filter) {
        priv->sampler_filter = filter;
        priv->update_sampler = TRUE;
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d12_converter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  auto self = GST_D3D12_CONVERTER (object);
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->prop_lock);
  switch (prop_id) {
    case PROP_SRC_X:
      g_value_set_int (value, priv->src_x);
      break;
    case PROP_SRC_Y:
      g_value_set_int (value, priv->src_y);
      break;
    case PROP_SRC_WIDTH:
      g_value_set_int (value, priv->src_width);
      break;
    case PROP_SRC_HEIGHT:
      g_value_set_int (value, priv->src_height);
      break;
    case PROP_DEST_X:
      g_value_set_int (value, priv->dest_x);
      break;
    case PROP_DEST_Y:
      g_value_set_int (value, priv->dest_y);
      break;
    case PROP_DEST_WIDTH:
      g_value_set_int (value, priv->dest_width);
      break;
    case PROP_DEST_HEIGHT:
      g_value_set_int (value, priv->dest_height);
      break;
    case PROP_ALPHA:
      g_value_set_double (value, priv->alpha);
      break;
    case PROP_FILL_BORDER:
      g_value_set_boolean (value, priv->fill_border);
      break;
    case PROP_BORDER_COLOR:
      g_value_set_uint64 (value, priv->border_color);
      break;
    case PROP_VIDEO_DIRECTION:
      g_value_set_enum (value, priv->video_direction);
      break;
    case PROP_SAMPLER_FILTER:
      g_value_set_enum (value, priv->sampler_filter);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GammaLutPtr
gst_d3d12_converter_get_gamma_dec_table (GstVideoTransferFunction func)
{
  static std::mutex lut_lock;
  static std::map < GstVideoTransferFunction, GammaLutPtr > g_gamma_dec_table;

  std::lock_guard < std::mutex > lk (lut_lock);
  auto lut = g_gamma_dec_table.find (func);
  if (lut != g_gamma_dec_table.end ())
    return lut->second;

  const gdouble scale = (gdouble) 1 / (GAMMA_LUT_SIZE - 1);
  auto table = std::make_shared < GammaLut > ();
  for (guint i = 0; i < GAMMA_LUT_SIZE; i++) {
    gdouble val = gst_video_transfer_function_decode (func, i * scale);
    val = rint (val * 65535);
    val = CLAMP (val, 0, 65535);
    table->lut[i] = (guint16) val;
  }

  g_gamma_dec_table[func] = table;
  return table;
}

static GammaLutPtr
gst_d3d12_converter_get_gamma_enc_table (GstVideoTransferFunction func)
{
  static std::mutex lut_lock;
  static std::map < GstVideoTransferFunction, GammaLutPtr > g_gamma_enc_table;

  std::lock_guard < std::mutex > lk (lut_lock);
  auto lut = g_gamma_enc_table.find (func);
  if (lut != g_gamma_enc_table.end ())
    return lut->second;

  const gdouble scale = (gdouble) 1 / (GAMMA_LUT_SIZE - 1);
  auto table = std::make_shared < GammaLut > ();
  for (guint i = 0; i < GAMMA_LUT_SIZE; i++) {
    gdouble val = gst_video_transfer_function_encode (func, i * scale);
    val = rint (val * 65535);
    val = CLAMP (val, 0, 65535);
    table->lut[i] = (guint16) val;
  }

  g_gamma_enc_table[func] = table;
  return table;
}

static guint
reorder_rtv_index (GstVideoFormat output_format, guint index)
{
  switch (output_format) {
    case GST_VIDEO_FORMAT_A420:
    case GST_VIDEO_FORMAT_A420_10LE:
    case GST_VIDEO_FORMAT_A420_12LE:
    case GST_VIDEO_FORMAT_A420_16LE:
    case GST_VIDEO_FORMAT_A422:
    case GST_VIDEO_FORMAT_A422_10LE:
    case GST_VIDEO_FORMAT_A422_12LE:
    case GST_VIDEO_FORMAT_A422_16LE:
    {
      switch (index) {
        case 0:
          return 0;
        case 1:
          return 3;
        case 2:
          return 1;
        case 3:
          return 2;
        default:
          g_assert_not_reached ();
          break;
      }
      return 0;
    }
    case GST_VIDEO_FORMAT_AV12:
    {
      switch (index) {
        case 0:
          return 0;
        case 1:
          return 2;
        case 2:
          return 1;
        case 3:
          return 3;
        default:
          g_assert_not_reached ();
          break;
      }
      return 0;
    }
    default:
      break;
  }

  return index;
}

static gboolean
gst_d3d12_converter_create_sampler (GstD3D12Converter * self,
    D3D12_FILTER filter, ID3D12DescriptorHeap ** heap)
{
  auto priv = self->priv;

  ComPtr < ID3D12DescriptorHeap > sampler_heap;
  auto hr = gst_d3d12_device_get_sampler_state (self->device, filter,
      &sampler_heap);
  if (!gst_d3d12_result (hr, self->device))
    return FALSE;

  if (priv->crs->HaveLut ()) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { };
    heap_desc.NumDescriptors = 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ComPtr < ID3D12DescriptorHeap > new_heap;
    auto device = gst_d3d12_device_get_device_handle (self->device);
    hr = device->CreateDescriptorHeap (&heap_desc, IID_PPV_ARGS (&new_heap));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create sampler heap");
      return FALSE;
    }

    auto dst_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE
        (GetCPUDescriptorHandleForHeapStart (new_heap));
    device->CopyDescriptorsSimple (1, dst_handle,
        GetCPUDescriptorHandleForHeapStart (sampler_heap),
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    if (filter != DEFAULT_SAMPLER_FILTER) {
      hr = gst_d3d12_device_get_sampler_state (self->device,
          DEFAULT_SAMPLER_FILTER, sampler_heap.ReleaseAndGetAddressOf ());

      if (!gst_d3d12_result (hr, self->device)) {
        GST_ERROR_OBJECT (self, "Couldn't create sampler heap");
        return FALSE;
      }
    }

    dst_handle.Offset (priv->sampler_inc_size);
    device->CopyDescriptorsSimple (1, dst_handle,
        GetCPUDescriptorHandleForHeapStart (sampler_heap),
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    *heap = new_heap.Detach ();
  } else {
    *heap = sampler_heap.Detach ();
  }

  return TRUE;
}

static gboolean
gst_d3d12_converter_setup_resource (GstD3D12Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info,
    const GstD3D12Format * in_format, const GstD3D12Format * out_format,
    D3D12_FILTER sampler_filter, guint sample_count, guint sample_quality)
{
  auto priv = self->priv;
  HRESULT hr;
  VertexData vertex_data[4];
  ComPtr < ID3D12Resource > upload_buf;
  ComPtr < ID3D12Resource > gamma_dec_lut_upload;
  ComPtr < ID3D12Resource > gamma_enc_lut_upload;

  auto device = gst_d3d12_device_get_device_handle (self->device);

  priv->srv_inc_size = device->GetDescriptorHandleIncrementSize
      (D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  priv->rtv_inc_size = device->GetDescriptorHandleIncrementSize
      (D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  priv->sampler_inc_size = device->GetDescriptorHandleIncrementSize
      (D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

  ComPtr < ID3DBlob > rs_blob;
  priv->crs =
      gst_d3d12_get_converter_root_signature (self->device,
      GST_VIDEO_INFO_FORMAT (in_info), priv->convert_type);
  if (!priv->crs) {
    GST_ERROR_OBJECT (self, "Couldn't get root signature blob");
    return FALSE;
  }

  priv->crs->GetBlob (&rs_blob);
  hr = device->CreateRootSignature (0, rs_blob->GetBufferPointer (),
      rs_blob->GetBufferSize (), IID_PPV_ARGS (&priv->rs));
  if (!gst_d3d12_result (hr, self->device)) {
    GST_ERROR_OBJECT (self, "Couldn't create root signature");
    return FALSE;
  }

  if (!gst_d3d12_converter_create_sampler (self, sampler_filter,
          &priv->sampler_heap)) {
    if (sampler_filter != DEFAULT_SAMPLER_FILTER) {
      sampler_filter = DEFAULT_SAMPLER_FILTER;
      if (!gst_d3d12_converter_create_sampler (self, sampler_filter,
              &priv->sampler_heap)) {
        return FALSE;
      }
    } else {
      return FALSE;
    }
  }

  priv->sampler_filter = sampler_filter;

  auto psblob_list =
      gst_d3d12_get_converter_pixel_shader_blob (GST_VIDEO_INFO_FORMAT
      (in_info), GST_VIDEO_INFO_FORMAT (out_info),
      priv->src_alpha_mode == GST_D3D12_CONVERTER_ALPHA_MODE_PREMULTIPLIED,
      priv->dst_alpha_mode == GST_D3D12_CONVERTER_ALPHA_MODE_PREMULTIPLIED,
      priv->convert_type);
  if (psblob_list.empty ()) {
    GST_ERROR_OBJECT (self, "Couldn't get pixel shader blob");
    return FALSE;
  }

  D3D12_SHADER_BYTECODE vs_blob;
  hr = gst_d3d12_get_converter_vertex_shader_blob (&vs_blob, priv->input_desc);
  if (!gst_d3d12_result (hr, self->device)) {
    GST_ERROR_OBJECT (self, "Couldn't get vertex shader blob");
    return FALSE;
  }

  std::queue < DXGI_FORMAT > rtv_formats;
  auto output_format = GST_VIDEO_INFO_FORMAT (out_info);
  for (guint i = 0; i < 4; i++) {
    auto index = reorder_rtv_index (output_format, i);
    auto format = out_format->resource_format[index];
    if (format == DXGI_FORMAT_UNKNOWN)
      break;

    rtv_formats.push (format);
  }

  priv->quad_data.resize (psblob_list.size ());

  for (size_t i = 0; i < psblob_list.size (); i++) {
    auto & pso_desc = priv->quad_data[i].desc;
    pso_desc.pRootSignature = priv->rs.Get ();
    pso_desc.VS = vs_blob;
    pso_desc.PS = psblob_list[i].bytecode;
    pso_desc.BlendState = priv->blend_desc;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC (D3D12_DEFAULT);
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.InputLayout.pInputElementDescs = priv->input_desc;
    pso_desc.InputLayout.NumElements = 2;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = psblob_list[i].num_rtv;
    for (UINT j = 0; j < pso_desc.NumRenderTargets; j++) {
      pso_desc.RTVFormats[j] = rtv_formats.front ();
      rtv_formats.pop ();
    }
    pso_desc.SampleDesc.Count = sample_count;
    pso_desc.SampleDesc.Quality = sample_quality;

    ComPtr < ID3D12PipelineState > pso;
    hr = device->CreateGraphicsPipelineState (&pso_desc, IID_PPV_ARGS (&pso));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create PSO");
      return FALSE;
    }

    priv->quad_data[i].pso = pso;
    priv->quad_data[i].num_rtv = psblob_list[i].num_rtv;
  }

  D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = { };
  srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_heap_desc.NumDescriptors = priv->crs->GetNumSrv ();
  if (priv->crs->HaveLut ())
    srv_heap_desc.NumDescriptors += 2;

  srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  priv->srv_heap_pool = gst_d3d12_descriptor_pool_new (device, &srv_heap_desc);

  /* bottom left */
  vertex_data[0].position.x = -1.0f;
  vertex_data[0].position.y = -1.0f;
  vertex_data[0].position.z = 0.0f;
  vertex_data[0].texture.u = 0.0f;
  vertex_data[0].texture.v = 1.0f;

  /* top left */
  vertex_data[1].position.x = -1.0f;
  vertex_data[1].position.y = 1.0f;
  vertex_data[1].position.z = 0.0f;
  vertex_data[1].texture.u = 0.0f;
  vertex_data[1].texture.v = 0.0f;

  /* top right */
  vertex_data[2].position.x = 1.0f;
  vertex_data[2].position.y = 1.0f;
  vertex_data[2].position.z = 0.0f;
  vertex_data[2].texture.u = 1.0f;
  vertex_data[2].texture.v = 0.0f;

  /* bottom right */
  vertex_data[3].position.x = 1.0f;
  vertex_data[3].position.y = -1.0f;
  vertex_data[3].position.z = 0.0f;
  vertex_data[3].texture.u = 1.0f;
  vertex_data[3].texture.v = 1.0f;

  /* vertex, index and constant buffers */
  D3D12_HEAP_PROPERTIES heap_prop;
  D3D12_RESOURCE_DESC resource_desc;
  CD3DX12_RANGE range (0, 0);
  guint8 *data;
  D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
  if (gst_d3d12_device_non_zeroed_supported (self->device))
    heap_flags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

  {
    guint vertex_index_size = g_vertex_buf_size + g_index_buf_size;
    vertex_index_size = GST_ROUND_UP_N (vertex_index_size,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    guint const_size = GST_ROUND_UP_N (g_const_buf_size,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    heap_prop = CD3DX12_HEAP_PROPERTIES (D3D12_HEAP_TYPE_DEFAULT);
    resource_desc =
        CD3DX12_RESOURCE_DESC::Buffer (vertex_index_size + const_size);
    hr = device->CreateCommittedResource (&heap_prop, heap_flags,
        &resource_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS (&priv->shader_buf));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create vertex buffer");
      return FALSE;
    }

    priv->vbv.BufferLocation = priv->shader_buf->GetGPUVirtualAddress ();
    priv->vbv.SizeInBytes = g_vertex_buf_size;
    priv->vbv.StrideInBytes = sizeof (VertexData);

    priv->idv.BufferLocation = priv->vbv.BufferLocation + g_vertex_buf_size;
    priv->idv.SizeInBytes = g_index_buf_size;
    priv->idv.Format = DXGI_FORMAT_R16_UINT;

    priv->const_buf_addr = priv->vbv.BufferLocation + vertex_index_size;

    heap_prop = CD3DX12_HEAP_PROPERTIES (D3D12_HEAP_TYPE_UPLOAD);
    hr = device->CreateCommittedResource (&heap_prop,
        heap_flags, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS (&upload_buf));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create vertex buffer upload");
      return FALSE;
    }

    hr = upload_buf->Map (0, &range, (void **) &data);
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't map vertext buffer");
      return FALSE;
    }

    memcpy (data, vertex_data, g_vertex_buf_size);
    memcpy (data + g_vertex_buf_size, g_indices, g_index_buf_size);
    memcpy (data + vertex_index_size, &priv->const_data, g_const_buf_size);
    upload_buf->Unmap (0, nullptr);
  }

  if (priv->crs->HaveLut ()) {
    heap_prop = CD3DX12_HEAP_PROPERTIES (D3D12_HEAP_TYPE_DEFAULT);
    resource_desc = CD3DX12_RESOURCE_DESC::Tex1D (DXGI_FORMAT_R16_UNORM,
        GAMMA_LUT_SIZE, 1, 1);

    hr = device->CreateCommittedResource (&heap_prop,
        heap_flags, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS (&priv->gamma_dec_lut));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create gamma decoding LUT");
      return FALSE;
    }

    hr = device->CreateCommittedResource (&heap_prop,
        heap_flags, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS (&priv->gamma_enc_lut));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create gamma encoding LUT");
      return FALSE;
    }

    UINT64 gamma_lut_size;
    device->GetCopyableFootprints (&resource_desc, 0, 1, 0,
        &priv->gamma_lut_layout, nullptr, nullptr, &gamma_lut_size);

    heap_prop = CD3DX12_HEAP_PROPERTIES (D3D12_HEAP_TYPE_UPLOAD);
    resource_desc = CD3DX12_RESOURCE_DESC::Buffer (gamma_lut_size);

    hr = device->CreateCommittedResource (&heap_prop,
        heap_flags, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS (&gamma_dec_lut_upload));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create gamma decoding LUT upload");
      return FALSE;
    }

    hr = device->CreateCommittedResource (&heap_prop,
        heap_flags, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS (&gamma_enc_lut_upload));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create gamma encoding LUT upload");
      return FALSE;
    }

    auto in_trc = in_info->colorimetry.transfer;
    auto out_trc = out_info->colorimetry.transfer;

    auto gamma_dec_table = gst_d3d12_converter_get_gamma_dec_table (in_trc);
    auto gamma_enc_table = gst_d3d12_converter_get_gamma_enc_table (out_trc);

    hr = gamma_dec_lut_upload->Map (0, &range, (void **) &data);
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't map gamma lut upload buffer");
      return FALSE;
    }

    memcpy (data, gamma_dec_table->lut, GAMMA_LUT_SIZE * sizeof (guint16));
    gamma_dec_lut_upload->Unmap (0, nullptr);

    hr = gamma_enc_lut_upload->Map (0, &range, (void **) &data);
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't map gamma lut upload buffer");
      return FALSE;
    }

    memcpy (data, gamma_enc_table->lut, GAMMA_LUT_SIZE * sizeof (guint16));
    gamma_enc_lut_upload->Unmap (0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC desc = { };
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 2;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    auto hr = device->CreateDescriptorHeap (&desc,
        IID_PPV_ARGS (&priv->gamma_lut_heap));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't map gamma lut upload buffer");
      return FALSE;
    }

    auto cpu_handle =
        CD3DX12_CPU_DESCRIPTOR_HANDLE (GetCPUDescriptorHandleForHeapStart
        (priv->gamma_lut_heap));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = { };
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture1D.MipLevels = 1;

    device->CreateShaderResourceView (priv->gamma_dec_lut.Get (), &srv_desc,
        cpu_handle);
    cpu_handle.Offset (priv->srv_inc_size);

    device->CreateShaderResourceView (priv->gamma_enc_lut.Get (), &srv_desc,
        cpu_handle);
  }

  priv->input_texture_width = GST_VIDEO_INFO_WIDTH (in_info);
  priv->input_texture_height = GST_VIDEO_INFO_HEIGHT (in_info);

  for (guint i = 0; i < GST_VIDEO_INFO_N_PLANES (out_info); i++) {
    priv->viewport[i].TopLeftX = 0;
    priv->viewport[i].TopLeftY = 0;
    priv->viewport[i].Width = GST_VIDEO_INFO_COMP_WIDTH (out_info, i);
    priv->viewport[i].Height = GST_VIDEO_INFO_COMP_HEIGHT (out_info, i);
    priv->viewport[i].MinDepth = 0.0f;
    priv->viewport[i].MaxDepth = 1.0f;

    priv->scissor_rect[i].left = 0;
    priv->scissor_rect[i].top = 0;
    priv->scissor_rect[i].right = GST_VIDEO_INFO_COMP_WIDTH (out_info, i);
    priv->scissor_rect[i].bottom = GST_VIDEO_INFO_COMP_HEIGHT (out_info, i);
  }

  ComPtr < ID3D12CommandAllocator > ca;
  hr = device->CreateCommandAllocator (D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS (&ca));
  if (!gst_d3d12_result (hr, self->device))
    return FALSE;

  ComPtr < ID3D12GraphicsCommandList > cl;
  hr = device->CreateCommandList (0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      ca.Get (), nullptr, IID_PPV_ARGS (&cl));
  if (!gst_d3d12_result (hr, self->device))
    return FALSE;

  std::vector < D3D12_RESOURCE_BARRIER > barriers;
  cl->CopyResource (priv->shader_buf.Get (), upload_buf.Get ());

  barriers.push_back (CD3DX12_RESOURCE_BARRIER::Transition (priv->shader_buf.
          Get (), D3D12_RESOURCE_STATE_COPY_DEST, STATE_VERTEX_AND_INDEX));

  if (priv->crs->HaveLut ()) {
    D3D12_TEXTURE_COPY_LOCATION src;
    D3D12_TEXTURE_COPY_LOCATION dst;
    src =
        CD3DX12_TEXTURE_COPY_LOCATION (gamma_dec_lut_upload.Get (),
        priv->gamma_lut_layout);
    dst = CD3DX12_TEXTURE_COPY_LOCATION (priv->gamma_dec_lut.Get ());
    cl->CopyTextureRegion (&dst, 0, 0, 0, &src, nullptr);

    src =
        CD3DX12_TEXTURE_COPY_LOCATION (gamma_enc_lut_upload.Get (),
        priv->gamma_lut_layout);
    dst = CD3DX12_TEXTURE_COPY_LOCATION (priv->gamma_enc_lut.Get ());
    cl->CopyTextureRegion (&dst, 0, 0, 0, &src, nullptr);

    barriers.
        push_back (CD3DX12_RESOURCE_BARRIER::Transition (priv->gamma_dec_lut.
            Get (), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    barriers.
        push_back (CD3DX12_RESOURCE_BARRIER::Transition (priv->gamma_enc_lut.
            Get (), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
  }

  cl->ResourceBarrier (barriers.size (), barriers.data ());

  hr = cl->Close ();
  if (!gst_d3d12_result (hr, self->device)) {
    GST_ERROR_OBJECT (self, "Couldn't close upload command list");
    return FALSE;
  }

  ID3D12CommandList *cmd_list[] = { cl.Get () };

  hr = gst_d3d12_command_queue_execute_command_lists (priv->cq, 1, cmd_list,
      &priv->fence_val);
  if (!gst_d3d12_result (hr, self->device)) {
    GST_ERROR_OBJECT (self, "Couldn't execute command list");
    return FALSE;
  }

  GstD3D12FenceData *fence_data;
  gst_d3d12_device_acquire_fence_data (self->device, &fence_data);
  gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_COM (cl.Detach ()));
  gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_COM (ca.Detach ()));
  gst_d3d12_fence_data_push (fence_data,
      FENCE_NOTIFY_COM (upload_buf.Detach ()));
  if (gamma_dec_lut_upload) {
    gst_d3d12_fence_data_push (fence_data,
        FENCE_NOTIFY_COM (gamma_dec_lut_upload.Detach ()));
  }

  if (gamma_enc_lut_upload) {
    gst_d3d12_fence_data_push (fence_data,
        FENCE_NOTIFY_COM (gamma_enc_lut_upload.Detach ()));
  }

  gst_d3d12_command_queue_set_notify (priv->cq, priv->fence_val,
      FENCE_NOTIFY_MINI_OBJECT (fence_data));

  return TRUE;
}

static void
gst_d3d12_converter_update_clear_background (GstD3D12Converter * self)
{
  auto priv = self->priv;
  const GstVideoInfo *out_info = &priv->out_info;

  if (priv->fill_border && (priv->dest_x != 0 || priv->dest_y != 0 ||
          priv->dest_width != out_info->width ||
          priv->dest_height != out_info->height ||
          priv->video_direction == GST_VIDEO_ORIENTATION_CUSTOM)) {
    GST_DEBUG_OBJECT (self, "Enable background color");
    priv->clear_background = TRUE;
  } else {
    GST_DEBUG_OBJECT (self, "Disable background color");
    priv->clear_background = FALSE;
  }
}

static gboolean
gst_d3d12_converter_apply_orientation (GstD3D12Converter * self)
{
  auto priv = self->priv;

  switch (priv->video_direction) {
    case GST_VIDEO_ORIENTATION_IDENTITY:
    case GST_VIDEO_ORIENTATION_AUTO:
    default:
      priv->transform = g_matrix_identity;
      break;
    case GST_VIDEO_ORIENTATION_90R:
      priv->transform = g_matrix_90r;
      break;
    case GST_VIDEO_ORIENTATION_180:
      priv->transform = g_matrix_180;
      break;
    case GST_VIDEO_ORIENTATION_90L:
      priv->transform = g_matrix_90l;
      break;
    case GST_VIDEO_ORIENTATION_HORIZ:
      priv->transform = g_matrix_horiz;
      break;
    case GST_VIDEO_ORIENTATION_VERT:
      priv->transform = g_matrix_vert;
      break;
    case GST_VIDEO_ORIENTATION_UL_LR:
      priv->transform = g_matrix_ul_lr;
      break;
    case GST_VIDEO_ORIENTATION_UR_LL:
      priv->transform = g_matrix_ur_ll;
      break;
    case GST_VIDEO_ORIENTATION_CUSTOM:
      priv->transform = priv->custom_transform;
  }

  return TRUE;
}

static gboolean
gst_d3d12_converter_update_transform (GstD3D12Converter * self)
{
  auto priv = self->priv;

  if (!priv->update_transform)
    return TRUE;

  priv->update_transform = FALSE;

  gst_d3d12_converter_update_clear_background (self);

  return gst_d3d12_converter_apply_orientation (self);
}

static gboolean
gst_d3d12_converter_update_src_rect (GstD3D12Converter * self)
{
  auto priv = self->priv;
  VertexData vertex_data[4];
  HRESULT hr;
  FLOAT u0, u1, v0, v1, off_u, off_v;
  gint texture_width = priv->input_texture_width;
  gint texture_height = priv->input_texture_height;

  if (!priv->update_src_rect)
    return TRUE;

  priv->update_src_rect = FALSE;

  GST_DEBUG_OBJECT (self, "Updating vertex buffer");

  if (!priv->vertex_upload) {
    D3D12_HEAP_PROPERTIES heap_prop =
        CD3DX12_HEAP_PROPERTIES (D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC buffer_desc =
        CD3DX12_RESOURCE_DESC::Buffer (g_vertex_buf_size);
    auto device = gst_d3d12_device_get_device_handle (self->device);
    hr = device->CreateCommittedResource (&heap_prop, D3D12_HEAP_FLAG_NONE,
        &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS (&priv->vertex_upload));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create vertex buffer upload");
      return FALSE;
    }
  }

  /*
   *  (u0, v0) -- (u1, v0)
   *     |            |
   *  (u0, v1) -- (u1, v1)
   */
  off_u = 0.5f / texture_width;
  off_v = 0.5f / texture_height;

  if (priv->src_x > 0)
    u0 = (priv->src_x / (gfloat) texture_width) + off_u;
  else
    u0 = 0.0f;

  if ((priv->src_x + priv->src_width) != texture_width)
    u1 = ((priv->src_x + priv->src_width) / (gfloat) texture_width) - off_u;
  else
    u1 = 1.0f;

  if (priv->src_y > 0)
    v0 = (priv->src_y / (gfloat) texture_height) + off_v;
  else
    v0 = 0.0;

  if ((priv->src_y + priv->src_height) != texture_height)
    v1 = ((priv->src_y + priv->src_height) / (gfloat) texture_height) - off_v;
  else
    v1 = 1.0f;

  /* bottom left */
  vertex_data[0].position.x = -1.0f;
  vertex_data[0].position.y = -1.0f;
  vertex_data[0].position.z = 0.0f;
  vertex_data[0].texture.u = u0;
  vertex_data[0].texture.v = v1;

  /* top left */
  vertex_data[1].position.x = -1.0f;
  vertex_data[1].position.y = 1.0f;
  vertex_data[1].position.z = 0.0f;
  vertex_data[1].texture.u = u0;
  vertex_data[1].texture.v = v0;

  /* top right */
  vertex_data[2].position.x = 1.0f;
  vertex_data[2].position.y = 1.0f;
  vertex_data[2].position.z = 0.0f;
  vertex_data[2].texture.u = u1;
  vertex_data[2].texture.v = v0;

  /* bottom right */
  vertex_data[3].position.x = 1.0f;
  vertex_data[3].position.y = -1.0f;
  vertex_data[3].position.z = 0.0f;
  vertex_data[3].texture.u = u1;
  vertex_data[3].texture.v = v1;

  guint8 *data;
  CD3DX12_RANGE range (0, 0);
  hr = priv->vertex_upload->Map (0, &range, (void **) &data);
  if (!gst_d3d12_result (hr, self->device)) {
    GST_ERROR_OBJECT (self, "Couldn't map vertex buffer, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  memcpy (data, vertex_data, g_vertex_buf_size);
  priv->vertex_upload->Unmap (0, nullptr);

  return TRUE;
}

static gboolean
gst_d3d12_converter_update_dest_rect (GstD3D12Converter * self)
{
  auto priv = self->priv;

  if (!priv->update_dest_rect)
    return TRUE;

  priv->viewport[0].TopLeftX = priv->dest_x;
  priv->viewport[0].TopLeftY = priv->dest_y;
  priv->viewport[0].Width = priv->dest_width;
  priv->viewport[0].Height = priv->dest_height;

  priv->scissor_rect[0].left = priv->dest_x;
  priv->scissor_rect[0].top = priv->dest_y;
  priv->scissor_rect[0].right = priv->dest_width + priv->dest_x;
  priv->scissor_rect[0].bottom = priv->dest_height + priv->dest_y;

  GST_DEBUG_OBJECT (self,
      "Update viewport, TopLeftX: %f, TopLeftY: %f, Width: %f, Height %f",
      priv->viewport[0].TopLeftX, priv->viewport[0].TopLeftY,
      priv->viewport[0].Width, priv->viewport[0].Height);

  gst_d3d12_converter_update_clear_background (self);

  auto format = GST_VIDEO_INFO_FORMAT (&priv->out_info);
  switch (format) {
    case GST_VIDEO_FORMAT_YUV9:
    case GST_VIDEO_FORMAT_YVU9:
      priv->viewport[1].TopLeftX = priv->viewport[0].TopLeftX / 4;
      priv->viewport[1].TopLeftY = priv->viewport[0].TopLeftY / 4;
      priv->viewport[1].Width = priv->viewport[0].Width / 4;
      priv->viewport[1].Height = priv->viewport[0].Height / 4;

      priv->scissor_rect[1].left = priv->scissor_rect[0].left / 4;
      priv->scissor_rect[1].top = priv->scissor_rect[0].top / 4;
      priv->scissor_rect[1].right = priv->scissor_rect[0].right / 4;
      priv->scissor_rect[1].bottom = priv->scissor_rect[0].bottom / 4;
      for (guint i = 2; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
        priv->viewport[i] = priv->viewport[1];
        priv->scissor_rect[i] = priv->scissor_rect[1];
      }
      break;
    case GST_VIDEO_FORMAT_Y41B:
      priv->viewport[1].TopLeftX = priv->viewport[0].TopLeftX / 4;
      priv->viewport[1].TopLeftY = priv->viewport[0].TopLeftY;
      priv->viewport[1].Width = priv->viewport[0].Width / 4;
      priv->viewport[1].Height = priv->viewport[0].Height;

      priv->scissor_rect[1].left = priv->scissor_rect[0].left / 4;
      priv->scissor_rect[1].top = priv->scissor_rect[0].top;
      priv->scissor_rect[1].right = priv->scissor_rect[0].right / 4;
      priv->scissor_rect[1].bottom = priv->scissor_rect[0].bottom;
      for (guint i = 2; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
        priv->viewport[i] = priv->viewport[1];
        priv->scissor_rect[i] = priv->scissor_rect[1];
      }
      break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P012_LE:
    case GST_VIDEO_FORMAT_P016_LE:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
    case GST_VIDEO_FORMAT_I420_10LE:
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_A420:
    case GST_VIDEO_FORMAT_A420_10LE:
    case GST_VIDEO_FORMAT_A420_12LE:
    case GST_VIDEO_FORMAT_A420_16LE:
    case GST_VIDEO_FORMAT_AV12:
      priv->viewport[1].TopLeftX = priv->viewport[0].TopLeftX / 2;
      priv->viewport[1].TopLeftY = priv->viewport[0].TopLeftY / 2;
      priv->viewport[1].Width = priv->viewport[0].Width / 2;
      priv->viewport[1].Height = priv->viewport[0].Height / 2;

      priv->scissor_rect[1].left = priv->scissor_rect[0].left / 2;
      priv->scissor_rect[1].top = priv->scissor_rect[0].top / 2;
      priv->scissor_rect[1].right = priv->scissor_rect[0].right / 2;
      priv->scissor_rect[1].bottom = priv->scissor_rect[0].bottom / 2;

      for (guint i = 2; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
        priv->viewport[i] = priv->viewport[1];
        priv->scissor_rect[i] = priv->scissor_rect[1];
      }
      break;
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_I422_10LE:
    case GST_VIDEO_FORMAT_I422_12LE:
    case GST_VIDEO_FORMAT_A422:
    case GST_VIDEO_FORMAT_A422_10LE:
    case GST_VIDEO_FORMAT_A422_12LE:
    case GST_VIDEO_FORMAT_A422_16LE:
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_NV61:
      priv->viewport[1].TopLeftX = priv->viewport[0].TopLeftX / 2;
      priv->viewport[1].TopLeftY = priv->viewport[0].TopLeftY;
      priv->viewport[1].Width = priv->viewport[0].Width / 2;
      priv->viewport[1].Height = priv->viewport[0].Height;

      priv->scissor_rect[1].left = priv->scissor_rect[0].left / 2;
      priv->scissor_rect[1].top = priv->scissor_rect[0].top;
      priv->scissor_rect[1].right = priv->scissor_rect[0].right / 2;
      priv->scissor_rect[1].bottom = priv->scissor_rect[0].bottom;

      for (guint i = 2; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
        priv->viewport[i] = priv->viewport[1];
        priv->scissor_rect[i] = priv->scissor_rect[1];
      }
      break;
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_Y444_16LE:
    case GST_VIDEO_FORMAT_RGBP:
    case GST_VIDEO_FORMAT_BGRP:
    case GST_VIDEO_FORMAT_GBR:
    case GST_VIDEO_FORMAT_GBR_10LE:
    case GST_VIDEO_FORMAT_GBR_12LE:
    case GST_VIDEO_FORMAT_GBR_16LE:
    case GST_VIDEO_FORMAT_GBRA:
    case GST_VIDEO_FORMAT_GBRA_10LE:
    case GST_VIDEO_FORMAT_GBRA_12LE:
    case GST_VIDEO_FORMAT_A444:
    case GST_VIDEO_FORMAT_A444_10LE:
    case GST_VIDEO_FORMAT_A444_12LE:
    case GST_VIDEO_FORMAT_A444_16LE:
    case GST_VIDEO_FORMAT_NV24:
      for (guint i = 1; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
        priv->viewport[i] = priv->viewport[0];
        priv->scissor_rect[i] = priv->scissor_rect[0];
      }
      break;
    default:
      break;
  }

  priv->update_dest_rect = FALSE;

  return TRUE;
}

static void
convert_info_gray_to_yuv (const GstVideoInfo * gray, GstVideoInfo * yuv)
{
  GstVideoInfo tmp;

  if (GST_VIDEO_INFO_IS_YUV (gray)) {
    *yuv = *gray;
    return;
  }

  if (gray->finfo->depth[0] == 8) {
    gst_video_info_set_format (&tmp,
        GST_VIDEO_FORMAT_Y444, gray->width, gray->height);
  } else {
    gst_video_info_set_format (&tmp,
        GST_VIDEO_FORMAT_Y444_16LE, gray->width, gray->height);
  }

  tmp.colorimetry.range = gray->colorimetry.range;
  if (tmp.colorimetry.range == GST_VIDEO_COLOR_RANGE_UNKNOWN)
    tmp.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

  tmp.colorimetry.primaries = gray->colorimetry.primaries;
  if (tmp.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN)
    tmp.colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;

  tmp.colorimetry.transfer = gray->colorimetry.transfer;
  if (tmp.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN)
    tmp.colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;

  tmp.colorimetry.matrix = gray->colorimetry.matrix;
  if (tmp.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN)
    tmp.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;

  *yuv = tmp;
}

static gboolean
gst_d3d12_converter_calculate_matrix (GstD3D12Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  auto priv = self->priv;
  GstD3D12ColorMatrix pre_coeff;
  GstD3D12ColorMatrix post_coeff;
  GstD3D12ColorMatrix primaries_coeff;
  GstVideoInfo rgb_info;

  gst_d3d12_color_matrix_init (&pre_coeff);
  gst_d3d12_color_matrix_init (&post_coeff);
  gst_d3d12_color_matrix_init (&primaries_coeff);

  switch (priv->convert_type) {
    case CONVERT_TYPE::RANGE:
      gst_d3d12_color_range_adjust_matrix_unorm (in_info, out_info,
          &post_coeff);
      break;
    case CONVERT_TYPE::SIMPLE:
      if (GST_VIDEO_INFO_IS_RGB (in_info)) {
        gst_d3d12_rgb_to_yuv_matrix_unorm (in_info, out_info, &post_coeff);
      } else {
        gst_d3d12_yuv_to_rgb_matrix_unorm (in_info, out_info, &post_coeff);
      }
      break;
    case CONVERT_TYPE::GAMMA:
    case CONVERT_TYPE::PRIMARY:
      if (GST_VIDEO_INFO_IS_RGB (in_info)) {
        rgb_info = *in_info;
        if (in_info->colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
          rgb_info.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

          gst_d3d12_color_range_adjust_matrix_unorm (in_info,
              &rgb_info, &pre_coeff);
        }
      } else {
        gst_video_info_set_format (&rgb_info,
            in_info->finfo->depth[0] == 8 ? GST_VIDEO_FORMAT_RGBA :
            GST_VIDEO_FORMAT_RGBA64_LE, in_info->width, in_info->height);
        rgb_info.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
        rgb_info.colorimetry.transfer = in_info->colorimetry.transfer;
        rgb_info.colorimetry.primaries = in_info->colorimetry.primaries;

        gst_d3d12_yuv_to_rgb_matrix_unorm (in_info, &rgb_info, &pre_coeff);
      }

      if (priv->convert_type == CONVERT_TYPE::PRIMARY) {
        const GstVideoColorPrimariesInfo *in_pinfo;
        const GstVideoColorPrimariesInfo *out_pinfo;

        in_pinfo =
            gst_video_color_primaries_get_info (in_info->colorimetry.primaries);
        out_pinfo =
            gst_video_color_primaries_get_info (out_info->
            colorimetry.primaries);

        gst_d3d12_color_primaries_matrix_unorm (in_pinfo, out_pinfo,
            &primaries_coeff);
      }

      if (GST_VIDEO_INFO_IS_RGB (out_info)) {
        if (out_info->colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235) {
          rgb_info = *out_info;
          rgb_info.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;

          gst_d3d12_color_range_adjust_matrix_unorm (&rgb_info,
              out_info, &post_coeff);
        }
      } else {
        gst_d3d12_rgb_to_yuv_matrix_unorm (&rgb_info, out_info, &post_coeff);
      }
      break;
    default:
      break;
  }

  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
    gchar *matrix_dump;
    matrix_dump = gst_d3d12_dump_color_matrix (&pre_coeff);
    GST_DEBUG_OBJECT (self, "PreCoeff \n%s", matrix_dump);
    g_free (matrix_dump);

    matrix_dump = gst_d3d12_dump_color_matrix (&primaries_coeff);
    GST_DEBUG_OBJECT (self, "PrimaryCoeff \n%s", matrix_dump);
    g_free (matrix_dump);

    matrix_dump = gst_d3d12_dump_color_matrix (&post_coeff);
    GST_DEBUG_OBJECT (self, "PostCoeff \n%s", matrix_dump);
    g_free (matrix_dump);
  }

  PSColorSpace *preCoeff = &priv->const_data.preCoeff;
  PSColorSpace *postCoeff = &priv->const_data.postCoeff;
  PSColorSpace *primariesCoeff = &priv->const_data.primariesCoeff;

  for (guint i = 0; i < 3; i++) {
    preCoeff->coeffX[i] = pre_coeff.matrix[0][i];
    preCoeff->coeffY[i] = pre_coeff.matrix[1][i];
    preCoeff->coeffZ[i] = pre_coeff.matrix[2][i];
    preCoeff->offset[i] = pre_coeff.offset[i];
    preCoeff->min[i] = pre_coeff.min[i];
    preCoeff->max[i] = pre_coeff.max[i];

    postCoeff->coeffX[i] = post_coeff.matrix[0][i];
    postCoeff->coeffY[i] = post_coeff.matrix[1][i];
    postCoeff->coeffZ[i] = post_coeff.matrix[2][i];
    postCoeff->offset[i] = post_coeff.offset[i];
    postCoeff->min[i] = post_coeff.min[i];
    postCoeff->max[i] = post_coeff.max[i];

    primariesCoeff->coeffX[i] = primaries_coeff.matrix[0][i];
    primariesCoeff->coeffY[i] = primaries_coeff.matrix[1][i];
    primariesCoeff->coeffZ[i] = primaries_coeff.matrix[2][i];
    primariesCoeff->offset[i] = primaries_coeff.offset[i];
    primariesCoeff->min[i] = primaries_coeff.min[i];
    primariesCoeff->max[i] = primaries_coeff.max[i];
  }

  return TRUE;
}

static gboolean
is_custom_format (GstVideoFormat format)
{
  switch (format) {
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_BGRA64_LE:
    case GST_VIDEO_FORMAT_BGR10A2_LE:
    case GST_VIDEO_FORMAT_RBGA:
    case GST_VIDEO_FORMAT_ARGB64_LE:
      return TRUE;
    default:
      break;
  }

  return FALSE;
}

static void
gst_d3d12_converter_calculate_border_color (GstD3D12Converter * self)
{
  auto priv = self->priv;
  GstD3D12ColorMatrix *m = &priv->clear_color_matrix;
  const GstVideoInfo *out_info = &priv->out_info;
  gdouble a;
  gdouble rgb[3];
  gdouble converted[3];
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (out_info);

  a = ((priv->border_color & 0xffff000000000000) >> 48) / (gdouble) G_MAXUINT16;
  rgb[0] =
      ((priv->border_color & 0x0000ffff00000000) >> 32) / (gdouble) G_MAXUINT16;
  rgb[1] =
      ((priv->border_color & 0x00000000ffff0000) >> 16) / (gdouble) G_MAXUINT16;
  rgb[2] = (priv->border_color & 0x000000000000ffff) / (gdouble) G_MAXUINT16;

  for (guint i = 0; i < 3; i++) {
    converted[i] = 0;
    for (guint j = 0; j < 3; j++) {
      converted[i] += m->matrix[i][j] * rgb[j];
    }
    converted[i] += m->offset[i];
    converted[i] = CLAMP (converted[i], m->min[i], m->max[i]);
  }

  GST_DEBUG_OBJECT (self, "Calculated background color ARGB: %f, %f, %f, %f",
      a, converted[0], converted[1], converted[2]);

  /* scale down if output is planar high bitdepth format */
  switch (format) {
    case GST_VIDEO_FORMAT_I420_10LE:
    case GST_VIDEO_FORMAT_I422_10LE:
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_GBR_10LE:
    case GST_VIDEO_FORMAT_GBRA_10LE:
    case GST_VIDEO_FORMAT_A420_10LE:
    case GST_VIDEO_FORMAT_A422_10LE:
    case GST_VIDEO_FORMAT_A444_10LE:
      for (guint i = 0; i < 3; i++) {
        converted[i] /= 64.0;
      }
      a /= 64.0;
      break;
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_I422_12LE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_GBR_12LE:
    case GST_VIDEO_FORMAT_GBRA_12LE:
    case GST_VIDEO_FORMAT_A420_12LE:
    case GST_VIDEO_FORMAT_A422_12LE:
    case GST_VIDEO_FORMAT_A444_12LE:
      for (guint i = 0; i < 3; i++) {
        converted[i] /= 16.0;
      }
      a /= 16.0;
      break;
    default:
      break;
  }

  if ((GST_VIDEO_INFO_IS_RGB (out_info) &&
          GST_VIDEO_INFO_N_PLANES (out_info) == 1 &&
          !is_custom_format (format)) || GST_VIDEO_INFO_IS_GRAY (out_info)) {
    for (guint i = 0; i < 3; i++)
      priv->clear_color[0][i] = converted[i];
    priv->clear_color[0][3] = a;
  } else {
    switch (format) {
      case GST_VIDEO_FORMAT_VUYA:
        priv->clear_color[0][0] = converted[2];
        priv->clear_color[0][1] = converted[1];
        priv->clear_color[0][2] = converted[0];
        priv->clear_color[0][3] = a;
        break;
      case GST_VIDEO_FORMAT_AYUV:
      case GST_VIDEO_FORMAT_AYUV64:
        priv->clear_color[0][0] = a;
        priv->clear_color[0][1] = converted[0];
        priv->clear_color[0][2] = converted[1];
        priv->clear_color[0][3] = converted[2];
        break;
      case GST_VIDEO_FORMAT_ARGB:
      case GST_VIDEO_FORMAT_xRGB:
      case GST_VIDEO_FORMAT_ARGB64_LE:
        priv->clear_color[0][0] = a;
        priv->clear_color[0][1] = converted[0];
        priv->clear_color[0][2] = converted[1];
        priv->clear_color[0][3] = converted[2];
        break;
      case GST_VIDEO_FORMAT_ABGR:
      case GST_VIDEO_FORMAT_xBGR:
        priv->clear_color[0][0] = a;
        priv->clear_color[0][1] = converted[2];
        priv->clear_color[0][2] = converted[1];
        priv->clear_color[0][3] = converted[0];
        break;
      case GST_VIDEO_FORMAT_RBGA:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[0][1] = converted[2];
        priv->clear_color[0][2] = converted[1];
        priv->clear_color[0][3] = a;
        break;
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_NV21:
      case GST_VIDEO_FORMAT_NV16:
      case GST_VIDEO_FORMAT_NV61:
      case GST_VIDEO_FORMAT_NV24:
      case GST_VIDEO_FORMAT_P010_10LE:
      case GST_VIDEO_FORMAT_P012_LE:
      case GST_VIDEO_FORMAT_P016_LE:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[0][1] = 0;
        priv->clear_color[0][2] = 0;
        priv->clear_color[0][3] = 1.0;
        if (format == GST_VIDEO_FORMAT_NV21 || format == GST_VIDEO_FORMAT_NV61) {
          priv->clear_color[1][0] = converted[2];
          priv->clear_color[1][1] = converted[1];
        } else {
          priv->clear_color[1][0] = converted[1];
          priv->clear_color[1][1] = converted[2];
        }
        priv->clear_color[1][2] = 0;
        priv->clear_color[1][3] = 1.0;
        break;
      case GST_VIDEO_FORMAT_AV12:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[1][0] = converted[1];
        priv->clear_color[1][1] = converted[2];
        priv->clear_color[2][0] = a;
        break;
      case GST_VIDEO_FORMAT_YUV9:
      case GST_VIDEO_FORMAT_YVU9:
      case GST_VIDEO_FORMAT_Y41B:
      case GST_VIDEO_FORMAT_I420:
      case GST_VIDEO_FORMAT_YV12:
      case GST_VIDEO_FORMAT_I420_10LE:
      case GST_VIDEO_FORMAT_I420_12LE:
      case GST_VIDEO_FORMAT_Y42B:
      case GST_VIDEO_FORMAT_I422_10LE:
      case GST_VIDEO_FORMAT_I422_12LE:
      case GST_VIDEO_FORMAT_Y444:
      case GST_VIDEO_FORMAT_Y444_10LE:
      case GST_VIDEO_FORMAT_Y444_12LE:
      case GST_VIDEO_FORMAT_Y444_16LE:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[0][1] = 0;
        priv->clear_color[0][2] = 0;
        priv->clear_color[0][3] = 1.0;
        if (format == GST_VIDEO_FORMAT_YV12 || format == GST_VIDEO_FORMAT_YVU9) {
          priv->clear_color[1][0] = converted[2];
          priv->clear_color[2][0] = converted[1];
        } else {
          priv->clear_color[1][0] = converted[1];
          priv->clear_color[2][0] = converted[2];
        }
        priv->clear_color[1][1] = 0;
        priv->clear_color[1][2] = 0;
        priv->clear_color[1][3] = 1.0;
        priv->clear_color[2][1] = 0;
        priv->clear_color[2][2] = 0;
        priv->clear_color[2][3] = 1.0;
        break;
      case GST_VIDEO_FORMAT_A420:
      case GST_VIDEO_FORMAT_A420_10LE:
      case GST_VIDEO_FORMAT_A420_12LE:
      case GST_VIDEO_FORMAT_A420_16LE:
      case GST_VIDEO_FORMAT_A422:
      case GST_VIDEO_FORMAT_A422_10LE:
      case GST_VIDEO_FORMAT_A422_12LE:
      case GST_VIDEO_FORMAT_A422_16LE:
      case GST_VIDEO_FORMAT_A444:
      case GST_VIDEO_FORMAT_A444_10LE:
      case GST_VIDEO_FORMAT_A444_12LE:
      case GST_VIDEO_FORMAT_A444_16LE:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[1][0] = converted[1];
        priv->clear_color[2][0] = converted[2];
        priv->clear_color[3][0] = a;
        break;
      case GST_VIDEO_FORMAT_RGBP:
        priv->clear_color[0][0] = converted[0];
        priv->clear_color[1][0] = converted[1];
        priv->clear_color[2][0] = converted[2];
        break;
      case GST_VIDEO_FORMAT_BGRP:
        priv->clear_color[0][0] = converted[2];
        priv->clear_color[1][0] = converted[1];
        priv->clear_color[2][0] = converted[0];
        break;
      case GST_VIDEO_FORMAT_GBR:
      case GST_VIDEO_FORMAT_GBR_10LE:
      case GST_VIDEO_FORMAT_GBR_12LE:
      case GST_VIDEO_FORMAT_GBR_16LE:
        priv->clear_color[0][0] = converted[1];
        priv->clear_color[1][0] = converted[2];
        priv->clear_color[2][0] = converted[0];
        break;
      case GST_VIDEO_FORMAT_GBRA:
      case GST_VIDEO_FORMAT_GBRA_10LE:
      case GST_VIDEO_FORMAT_GBRA_12LE:
        priv->clear_color[0][0] = converted[1];
        priv->clear_color[1][0] = converted[2];
        priv->clear_color[2][0] = converted[0];
        priv->clear_color[3][0] = a;
        break;
      default:
        g_assert_not_reached ();
        break;
    }
  }
}

/**
 * gst_d3d12_converter_new:
 * @device: a #GstD3D12Device
 * @queue: (allow-none): a #GstD3D12CommandQueue
 * @in_info: a #GstVideoInfo
 * @out_info: a #GstVideoInfo
 * @blend_desc: (nullable): D3D12_BLEND_DESC
 * @blend_factor: (nullable): blend factor value
 * @config: (nullable): converter config
 *
 * Creates a new converter instance
 *
 * Returns: (transfer full) (nullable): a new #GstD3D12Converter instance
 * or %NULL if conversion is not supported
 *
 * Since: 1.26
 */
GstD3D12Converter *
gst_d3d12_converter_new (GstD3D12Device * device, GstD3D12CommandQueue * queue,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info,
    const D3D12_BLEND_DESC * blend_desc, const gfloat blend_factor[4],
    GstStructure * config)
{
  GstD3D12Converter *self;
  GstD3D12Format in_d3d12_format;
  GstD3D12Format out_d3d12_format;
  gboolean allow_gamma = FALSE;
  gboolean allow_primaries = FALSE;
  D3D12_FILTER sampler_filter = DEFAULT_SAMPLER_FILTER;
  GstVideoInfo matrix_in_info;
  GstVideoInfo matrix_out_info;
  guint sample_count = 1;
  guint sample_quality = 0;

  g_return_val_if_fail (GST_IS_D3D12_DEVICE (device), nullptr);
  g_return_val_if_fail (in_info != nullptr, nullptr);
  g_return_val_if_fail (out_info != nullptr, nullptr);
  g_return_val_if_fail (queue == nullptr || GST_IS_D3D12_COMMAND_QUEUE (queue),
      nullptr);

  self = (GstD3D12Converter *) g_object_new (GST_TYPE_D3D12_CONVERTER, nullptr);
  gst_object_ref_sink (self);
  auto priv = self->priv;
  priv->cq = queue;
  if (!priv->cq) {
    priv->cq = gst_d3d12_device_get_command_queue (device,
        D3D12_COMMAND_LIST_TYPE_DIRECT);
  }
  gst_object_ref (priv->cq);

  priv->unpack = gst_d3d12_unpack_new (device, in_info);
  if (!priv->unpack) {
    GST_ERROR_OBJECT (self, "Couldn't create unpack object");
    gst_object_unref (self);
    return nullptr;
  }

  priv->pack = gst_d3d12_pack_new (device, out_info);
  if (!priv->pack) {
    GST_ERROR_OBJECT (self, "Couldn't create pack object");
    gst_object_unref (self);
    return nullptr;
  }

  if (blend_desc)
    priv->blend_desc = *blend_desc;

  if (blend_factor) {
    for (guint i = 0; i < 4; i++)
      priv->blend_factor[i] = blend_factor[i];
  }

  if (config) {
    gint value;
    if (gst_structure_get_enum (config, GST_D3D12_CONVERTER_OPT_GAMMA_MODE,
            GST_TYPE_VIDEO_GAMMA_MODE, &value) &&
        (GstVideoGammaMode) value != GST_VIDEO_GAMMA_MODE_NONE) {
      allow_gamma = TRUE;
    }

    if (gst_structure_get_enum (config, GST_D3D12_CONVERTER_OPT_PRIMARIES_MODE,
            GST_TYPE_VIDEO_PRIMARIES_MODE, &value) &&
        (GstVideoPrimariesMode) value != GST_VIDEO_PRIMARIES_MODE_NONE) {
      allow_primaries = TRUE;
    }

    gst_structure_get_enum (config, GST_D3D12_CONVERTER_OPT_SAMPLER_FILTER,
        GST_TYPE_D3D12_CONVERTER_SAMPLER_FILTER, (int *) &sampler_filter);

    gst_structure_get_enum (config, GST_D3D12_CONVERTER_OPT_SRC_ALPHA_MODE,
        GST_TYPE_D3D12_CONVERTER_ALPHA_MODE, (int *) &priv->src_alpha_mode);
    gst_structure_get_enum (config, GST_D3D12_CONVERTER_OPT_DEST_ALPHA_MODE,
        GST_TYPE_D3D12_CONVERTER_ALPHA_MODE, (int *) &priv->dst_alpha_mode);

    gst_structure_get_uint (config,
        GST_D3D12_CONVERTER_OPT_PSO_SAMPLE_DESC_COUNT, &sample_count);
    gst_structure_get_uint (config,
        GST_D3D12_CONVERTER_OPT_PSO_SAMPLE_DESC_QUALITY, &sample_quality);

    priv->sample_desc.Count = sample_count;
    priv->sample_desc.Quality = sample_quality;

    gst_structure_free (config);
  }

  GST_DEBUG_OBJECT (self,
      "Setup converter with format %s -> %s, "
      "allow gamma conversion: %d, allow primaries conversion: %d ",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (out_info)),
      allow_gamma, allow_primaries);

  self->device = (GstD3D12Device *) gst_object_ref (device);
  gst_d3d12_unpack_get_video_info (priv->unpack, &priv->in_info);
  gst_d3d12_pack_get_video_info (priv->pack, &priv->out_info);

  auto in_format = GST_VIDEO_INFO_FORMAT (&priv->in_info);
  auto out_format = GST_VIDEO_INFO_FORMAT (&priv->out_info);
  if (!gst_d3d12_device_get_format (device, in_format, &in_d3d12_format)) {
    GST_ERROR_OBJECT (self, "%s couldn't be converted to d3d12 format",
        gst_video_format_to_string (in_format));
    gst_object_unref (self);
    return nullptr;
  }

  if (!gst_d3d12_device_get_format (device, out_format, &out_d3d12_format)) {
    GST_ERROR_OBJECT (self, "%s couldn't be converted to d3d12 format",
        gst_video_format_to_string (out_format));
    gst_object_unref (self);
    return nullptr;
  }

  /* Init properties */
  priv->src_width = GST_VIDEO_INFO_WIDTH (in_info);
  priv->src_height = GST_VIDEO_INFO_HEIGHT (in_info);
  priv->dest_width = GST_VIDEO_INFO_WIDTH (out_info);
  priv->dest_height = GST_VIDEO_INFO_HEIGHT (out_info);
  priv->alpha = 1.0;
  priv->border_color = 0xffff000000000000;

  if (GST_VIDEO_INFO_IS_RGB (&priv->out_info)) {
    GstVideoInfo rgb_info = priv->out_info;
    rgb_info.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
    gst_d3d12_color_range_adjust_matrix_unorm (&rgb_info, &priv->out_info,
        &priv->clear_color_matrix);
  } else {
    GstVideoInfo rgb_info;
    GstVideoInfo yuv_info;

    gst_video_info_set_format (&rgb_info, GST_VIDEO_FORMAT_RGBA64_LE,
        priv->out_info.width, priv->out_info.height);
    convert_info_gray_to_yuv (&priv->out_info, &yuv_info);

    if (yuv_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
        yuv_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB) {
      GST_WARNING_OBJECT (self, "Invalid matrix is detected");
      yuv_info.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
    }

    gst_d3d12_rgb_to_yuv_matrix_unorm (&rgb_info,
        &yuv_info, &priv->clear_color_matrix);
  }

  gst_d3d12_converter_calculate_border_color (self);

  priv->convert_type = CONVERT_TYPE::IDENTITY;
  if (GST_VIDEO_INFO_IS_RGB (in_info) != GST_VIDEO_INFO_IS_RGB (out_info)) {
    priv->convert_type = CONVERT_TYPE::SIMPLE;
  } else if (in_info->colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
      out_info->colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
      in_info->colorimetry.range != out_info->colorimetry.range) {
    priv->convert_type = CONVERT_TYPE::RANGE;
  }

  if (allow_gamma &&
      in_info->colorimetry.transfer != GST_VIDEO_TRANSFER_UNKNOWN &&
      out_info->colorimetry.transfer != GST_VIDEO_TRANSFER_UNKNOWN &&
      !gst_video_transfer_function_is_equivalent (in_info->colorimetry.transfer,
          GST_VIDEO_INFO_COMP_DEPTH (in_info, 0),
          out_info->colorimetry.transfer, GST_VIDEO_INFO_COMP_DEPTH (out_info,
              0))) {
    GST_DEBUG_OBJECT (self, "Different transfer function %d -> %d",
        in_info->colorimetry.transfer, out_info->colorimetry.transfer);
    priv->convert_type = CONVERT_TYPE::GAMMA;
  }

  if (allow_primaries &&
      in_info->colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_UNKNOWN &&
      out_info->colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_UNKNOWN &&
      in_info->colorimetry.transfer != GST_VIDEO_TRANSFER_UNKNOWN &&
      out_info->colorimetry.transfer != GST_VIDEO_TRANSFER_UNKNOWN &&
      !gst_video_color_primaries_is_equivalent (in_info->colorimetry.primaries,
          out_info->colorimetry.primaries)) {
    GST_DEBUG_OBJECT (self, "Different primaries %d -> %d",
        in_info->colorimetry.primaries, out_info->colorimetry.primaries);
    priv->convert_type = CONVERT_TYPE::PRIMARY;
  }

  if (GST_VIDEO_INFO_IS_RGB (&priv->in_info)) {
    matrix_in_info = priv->in_info;
  } else {
    convert_info_gray_to_yuv (&priv->in_info, &matrix_in_info);
    if (matrix_in_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
        matrix_in_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB) {
      matrix_in_info.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
    }
  }

  if (GST_VIDEO_INFO_IS_RGB (&priv->out_info)) {
    matrix_out_info = priv->out_info;
  } else {
    convert_info_gray_to_yuv (&priv->out_info, &matrix_out_info);
    if (matrix_out_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
        matrix_out_info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB) {
      matrix_out_info.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
    }
  }

  if (!gst_d3d12_converter_calculate_matrix (self,
          &matrix_in_info, &matrix_out_info)) {
    gst_object_unref (self);
    return nullptr;
  }

  if (!gst_d3d12_converter_setup_resource (self, &priv->in_info,
          &priv->out_info, &in_d3d12_format, &out_d3d12_format,
          sampler_filter, sample_count, sample_quality)) {
    gst_object_unref (self);
    return nullptr;
  }

  return self;
}

static gboolean
gst_d3d12_converter_update_pso (GstD3D12Converter * self)
{
  auto priv = self->priv;
  if (!priv->update_pso)
    return TRUE;

  std::vector < QuadData > quad_data;
  quad_data.resize (priv->quad_data.size ());

  auto device = gst_d3d12_device_get_device_handle (self->device);

  for (size_t i = 0; i < quad_data.size (); i++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = priv->quad_data[i].desc;
    pso_desc.BlendState = priv->blend_desc;
    pso_desc.SampleDesc = priv->sample_desc;

    ComPtr < ID3D12PipelineState > pso;
    auto hr =
        device->CreateGraphicsPipelineState (&pso_desc, IID_PPV_ARGS (&pso));
    if (!gst_d3d12_result (hr, self->device)) {
      GST_ERROR_OBJECT (self, "Couldn't create pso");
      return FALSE;
    }

    quad_data[i].desc = pso_desc;
    quad_data[i].pso = pso;
    quad_data[i].num_rtv = priv->quad_data[i].num_rtv;
  }

  priv->update_pso = FALSE;
  priv->quad_data = quad_data;

  return TRUE;
}

static void
gst_d3d12_converter_update_sampler (GstD3D12Converter * self)
{
  auto priv = self->priv;
  if (!priv->update_sampler)
    return;

  priv->update_sampler = FALSE;
  ComPtr < ID3D12DescriptorHeap > sampler_heap;
  if (gst_d3d12_converter_create_sampler (self, priv->sampler_filter,
          &sampler_heap)) {
    priv->sampler_heap = sampler_heap;
  }
}

static void
reorder_rtv_handles (GstVideoFormat output_format,
    D3D12_CPU_DESCRIPTOR_HANDLE * src, D3D12_CPU_DESCRIPTOR_HANDLE * dst)
{
  for (guint i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
    auto index = reorder_rtv_index (output_format, i);
    dst[i] = src[index];
  }
}

static gboolean
gst_d3d12_converter_execute (GstD3D12Converter * self, GstD3D12Frame * in_frame,
    GstD3D12Frame * out_frame, GstD3D12FenceData * fence_data,
    ID3D12GraphicsCommandList * cl)
{
  auto priv = self->priv;

  std::lock_guard < std::mutex > lk (priv->prop_lock);
  auto desc = GetDesc (in_frame->data[0]);
  if (desc.Width != priv->input_texture_width ||
      desc.Height != priv->input_texture_height) {
    GST_DEBUG_OBJECT (self, "Texture resolution changed %ux%u -> %ux%u",
        (guint) priv->input_texture_width, priv->input_texture_height,
        (guint) desc.Width, desc.Height);
    priv->input_texture_width = desc.Width;
    priv->input_texture_height = desc.Height;
    priv->update_src_rect = TRUE;
  }

  desc = GetDesc (out_frame->data[0]);
  if (desc.SampleDesc.Count != priv->sample_desc.Count ||
      desc.SampleDesc.Quality != priv->sample_desc.Quality) {
    GST_DEBUG_OBJECT (self, "Sample desc updated");
    priv->sample_desc = desc.SampleDesc;
    priv->update_pso = TRUE;
  }

  if (!gst_d3d12_converter_update_dest_rect (self)) {
    GST_ERROR_OBJECT (self, "Failed to update dest rect");
    return FALSE;
  }

  if (!gst_d3d12_converter_update_src_rect (self)) {
    GST_ERROR_OBJECT (self, "Failed to update src rect");
    return FALSE;
  }

  if (!gst_d3d12_converter_update_transform (self)) {
    GST_ERROR_OBJECT (self, "Failed to update transform matrix");
    return FALSE;
  }

  if (!gst_d3d12_converter_update_pso (self)) {
    GST_ERROR_OBJECT (self, "Failed to update pso");
    return FALSE;
  }

  gst_d3d12_converter_update_sampler (self);

  if (priv->vertex_upload) {
    auto barrier =
        CD3DX12_RESOURCE_BARRIER::Transition (priv->shader_buf.Get (),
        STATE_VERTEX_AND_INDEX, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier (1, &barrier);

    cl->CopyBufferRegion (priv->shader_buf.Get (), 0,
        priv->vertex_upload.Get (), 0, g_vertex_buf_size);
    barrier =
        CD3DX12_RESOURCE_BARRIER::Transition (priv->shader_buf.Get (),
        D3D12_RESOURCE_STATE_COPY_DEST, STATE_VERTEX_AND_INDEX);
    cl->ResourceBarrier (1, &barrier);

    GST_DEBUG_OBJECT (self, "Vertex updated");
  }

  auto device = gst_d3d12_device_get_device_handle (self->device);

  GstD3D12Descriptor *descriptor;
  if (!gst_d3d12_descriptor_pool_acquire (priv->srv_heap_pool, &descriptor)) {
    GST_ERROR_OBJECT (self, "Couldn't acquire srv heap");
    return FALSE;
  }

  auto srv_heap = gst_d3d12_descriptor_get_handle (descriptor);
  gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_MINI_OBJECT (descriptor));

  auto cpu_handle =
      CD3DX12_CPU_DESCRIPTOR_HANDLE (GetCPUDescriptorHandleForHeapStart
      (srv_heap));

  for (guint i = 0; i < GST_VIDEO_INFO_N_PLANES (&priv->in_info); i++) {
    device->CopyDescriptorsSimple (1, cpu_handle, in_frame->srv_desc_handle[i],
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu_handle.Offset (priv->srv_inc_size);
  }

  if (priv->crs->HaveLut ()) {
    device->CopyDescriptorsSimple (2, cpu_handle,
        GetCPUDescriptorHandleForHeapStart (priv->gamma_lut_heap),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  if (priv->clear_background) {
    for (guint i = 0; i < GST_VIDEO_INFO_N_PLANES (&priv->out_info); i++) {
      cl->ClearRenderTargetView (out_frame->rtv_desc_handle[i],
          priv->clear_color[i], 1, &out_frame->plane_rect[i]);
    }
  }

  D3D12_CPU_DESCRIPTOR_HANDLE reordered_rtv_handle[GST_VIDEO_MAX_PLANES];
  reorder_rtv_handles (GST_VIDEO_INFO_FORMAT (&priv->out_info),
      out_frame->rtv_desc_handle, reordered_rtv_handle);

  auto pso = priv->quad_data[0].pso.Get ();

  cl->SetGraphicsRootSignature (priv->rs.Get ());
  cl->SetPipelineState (pso);

  ID3D12DescriptorHeap *heaps[] = { srv_heap, priv->sampler_heap.Get () };
  cl->SetDescriptorHeaps (2, heaps);
  cl->SetGraphicsRootDescriptorTable (priv->crs->GetPsSrvIdx (),
      GetGPUDescriptorHandleForHeapStart (srv_heap));
  cl->SetGraphicsRootDescriptorTable (priv->crs->GetPsSamplerIdx (),
      GetGPUDescriptorHandleForHeapStart (priv->sampler_heap));
  cl->SetGraphicsRoot32BitConstants (priv->crs->GetVsRootConstIdx (),
      16, &priv->transform, 0);
  cl->SetGraphicsRoot32BitConstants (priv->crs->GetPsRootConstIdx (),
      1, &priv->alpha, 0);
  cl->SetGraphicsRootConstantBufferView (priv->crs->GetPsCbvIdx (),
      priv->const_buf_addr);

  cl->IASetIndexBuffer (&priv->idv);
  cl->IASetVertexBuffers (0, 1, &priv->vbv);
  cl->IASetPrimitiveTopology (D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cl->RSSetViewports (1, priv->viewport);
  cl->RSSetScissorRects (1, priv->scissor_rect);
  cl->OMSetRenderTargets (priv->quad_data[0].num_rtv,
      reordered_rtv_handle, FALSE, nullptr);
  cl->OMSetBlendFactor (priv->blend_factor);
  cl->DrawIndexedInstanced (6, 1, 0, 0, 0);

  pso->AddRef ();
  gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_COM (pso));

  auto offset = priv->quad_data[0].num_rtv;
  if (priv->quad_data.size () == 2) {
    pso = priv->quad_data[1].pso.Get ();

    cl->SetPipelineState (pso);
    cl->RSSetViewports (1, &priv->viewport[offset]);
    cl->RSSetScissorRects (1, &priv->scissor_rect[offset]);
    cl->OMSetRenderTargets (priv->quad_data[1].num_rtv,
        reordered_rtv_handle + offset, FALSE, nullptr);
    cl->DrawIndexedInstanced (6, 1, 0, 0, 0);

    pso->AddRef ();
    gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_COM (pso));
  }

  gst_d3d12_fence_data_push (fence_data,
      FENCE_NOTIFY_MINI_OBJECT (gst_buffer_ref (in_frame->buffer)));
  if (priv->vertex_upload) {
    gst_d3d12_fence_data_push (fence_data,
        FENCE_NOTIFY_COM (priv->vertex_upload.Detach ()));
  }

  auto sampler = priv->sampler_heap.Get ();
  sampler->AddRef ();
  gst_d3d12_fence_data_push (fence_data, FENCE_NOTIFY_COM (sampler));

  return TRUE;
}

/**
 * gst_d3d12_converter_convert_buffer:
 * @converter: a #GstD3D12Converter
 * @in_buf: a #GstBuffer
 * @out_buf: a #GstBuffer
 * @fence_data: a #GstD3D12FenceData
 * @command_list: a ID3D12GraphicsCommandList
 * @execute_gpu_wait: Executes wait operation against @queue
 *
 * Records command list for conversion operation. converter will attach
 * conversion command associated resources such as command allocator
 * to @fence_data.
 *
 * If @execute_wait is %TRUE and buffers are associated with external fences,
 * this method will schedule GPU wait operation against @queue.
 *
 * Returns: %TRUE if successful
 *
 * Since: 1.26
 */
gboolean
gst_d3d12_converter_convert_buffer (GstD3D12Converter * converter,
    GstBuffer * in_buf, GstBuffer * out_buf, GstD3D12FenceData * fence_data,
    ID3D12GraphicsCommandList * command_list, gboolean execute_gpu_wait)
{
  g_return_val_if_fail (GST_IS_D3D12_CONVERTER (converter), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (in_buf), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (out_buf), FALSE);
  g_return_val_if_fail (fence_data, FALSE);
  g_return_val_if_fail (command_list, FALSE);

  GstD3D12Frame in_frame;
  GstD3D12Frame out_frame;

  auto priv = converter->priv;

  auto render_target = gst_d3d12_pack_acquire_render_target (priv->pack,
      out_buf);
  if (!render_target) {
    GST_ERROR_OBJECT (converter, "Couldn't get render target buffer");
    return FALSE;
  }

  in_buf = gst_d3d12_unpack_execute (priv->unpack, in_buf, fence_data,
      command_list);
  if (!in_buf) {
    GST_ERROR_OBJECT (converter, "Preprocessing failed");
    gst_buffer_unref (render_target);
    return FALSE;
  }

  /* Don't map output memory, we don't actually update output memory here */
  if (!gst_d3d12_frame_map (&out_frame, &priv->out_info, render_target,
          (GstMapFlags) GST_MAP_D3D12, GST_D3D12_FRAME_MAP_FLAG_RTV)) {
    GST_ERROR_OBJECT (converter, "Couldn't map output buffer");
    gst_buffer_unref (render_target);
    gst_buffer_unref (in_buf);
    return FALSE;
  }

  if (!gst_d3d12_frame_map (&in_frame, &priv->in_info,
          in_buf, GST_MAP_READ_D3D12, GST_D3D12_FRAME_MAP_FLAG_SRV)) {
    GST_ERROR_OBJECT (converter, "Couldn't map fallback input");
    gst_d3d12_frame_unmap (&out_frame);
    gst_buffer_unref (render_target);
    gst_buffer_unref (in_buf);
    return FALSE;
  }

  auto ret = gst_d3d12_converter_execute (converter,
      &in_frame, &out_frame, fence_data, command_list);

  if (ret) {
    ret = gst_d3d12_pack_execute (priv->pack, render_target, out_buf,
        fence_data, command_list);
  }

  if (ret && execute_gpu_wait) {
    gst_d3d12_frame_fence_gpu_wait (&in_frame, priv->cq);
    gst_d3d12_frame_fence_gpu_wait (&out_frame, priv->cq);
  }

  gst_d3d12_frame_unmap (&in_frame);
  gst_d3d12_frame_unmap (&out_frame);

  gst_buffer_unref (in_buf);
  gst_buffer_unref (render_target);

  return ret;
}

/**
 * gst_d3d12_converter_update_blend_state:
 * @converter: a #GstD3D12Converter
 * @blend_desc: (nullable): D3D12_BLEND_DESC
 * @blend_factor: (nullable): blend factor values
 *
 * Updates pipeline state object with new @blend_desc. If @blend_desc is %NULL,
 * pipeline state object will be updated with default blend state
 *
 * Returns: %TRUE if successful
 *
 * Since: 1.26
 */
gboolean
gst_d3d12_converter_update_blend_state (GstD3D12Converter * converter,
    const D3D12_BLEND_DESC * blend_desc, const gfloat blend_factor[4])
{
  g_return_val_if_fail (GST_IS_D3D12_CONVERTER (converter), FALSE);

  auto priv = converter->priv;
  std::lock_guard < std::mutex > lk (priv->prop_lock);
  D3D12_BLEND_DESC new_blend = CD3DX12_BLEND_DESC (D3D12_DEFAULT);

  if (blend_desc)
    new_blend = *blend_desc;

  if (memcmp (&priv->blend_desc, &new_blend, sizeof (D3D12_BLEND_DESC)) != 0)
    priv->update_pso = TRUE;

  if (blend_factor) {
    for (guint i = 0; i < 4; i++)
      priv->blend_factor[i] = blend_factor[i];
  } else {
    for (guint i = 0; i < 4; i++)
      priv->blend_factor[i] = 1.0f;
  }

  return TRUE;
}

gboolean
gst_d3d12_converter_apply_transform (GstD3D12Converter * converter,
    GstVideoOrientationMethod orientation, gfloat viewport_width,
    gfloat viewport_height, gfloat fov, gboolean ortho, gfloat rotation_x,
    gfloat rotation_y, gfloat rotation_z, gfloat scale_x, gfloat scale_y)
{
  g_return_val_if_fail (GST_IS_D3D12_CONVERTER (converter), FALSE);

  auto priv = converter->priv;
  std::lock_guard < std::mutex > lk (priv->prop_lock);

  gfloat aspect_ratio;
  gboolean rotated = FALSE;
  XMMATRIX rotate_matrix = XMMatrixIdentity ();

  switch (orientation) {
    case GST_VIDEO_ORIENTATION_IDENTITY:
    case GST_VIDEO_ORIENTATION_AUTO:
    case GST_VIDEO_ORIENTATION_CUSTOM:
    default:
      break;
    case GST_VIDEO_ORIENTATION_90R:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_90r);
      rotated = TRUE;
      break;
    case GST_VIDEO_ORIENTATION_180:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_180);
      break;
    case GST_VIDEO_ORIENTATION_90L:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_90l);
      rotated = TRUE;
      break;
    case GST_VIDEO_ORIENTATION_HORIZ:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_horiz);
      break;
    case GST_VIDEO_ORIENTATION_VERT:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_vert);
      break;
    case GST_VIDEO_ORIENTATION_UL_LR:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_ul_lr);
      rotated = TRUE;
      break;
    case GST_VIDEO_ORIENTATION_UR_LL:
      rotate_matrix = XMLoadFloat4x4A (&g_matrix_ur_ll);
      rotated = TRUE;
      break;
  }

  if (rotated)
    aspect_ratio = viewport_height / viewport_width;
  else
    aspect_ratio = viewport_width / viewport_height;

  /* Apply user specified transform matrix first, then rotate-method */
  XMMATRIX scale = XMMatrixScaling (scale_x * aspect_ratio, scale_y, 1.0);

  XMMATRIX rotate =
      XMMatrixRotationX (XMConvertToRadians (rotation_x)) *
      XMMatrixRotationY (XMConvertToRadians (-rotation_y)) *
      XMMatrixRotationZ (XMConvertToRadians (-rotation_z));

  XMMATRIX view = XMMatrixLookAtLH (XMVectorSet (0.0, 0.0, -1.0, 0.0),
      XMVectorSet (0.0, 0.0, 0.0, 0.0), XMVectorSet (0.0, 1.0, 0.0, 0.0));

  XMMATRIX proj;
  if (ortho) {
    proj = XMMatrixOrthographicOffCenterLH (-aspect_ratio,
        aspect_ratio, -1.0, 1.0, 0.1, 100.0);
  } else {
    proj = XMMatrixPerspectiveFovLH (XMConvertToRadians (fov),
        aspect_ratio, 0.1, 100.0);
  }

  XMMATRIX mvp = scale * rotate * view * proj * rotate_matrix;
  XMStoreFloat4x4A (&priv->custom_transform, mvp);
  priv->update_transform = TRUE;
  priv->video_direction = GST_VIDEO_ORIENTATION_CUSTOM;

  return TRUE;
}
