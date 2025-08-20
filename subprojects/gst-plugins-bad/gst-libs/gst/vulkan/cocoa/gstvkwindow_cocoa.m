/*
 * GStreamer
 * Copyright (C) 2019 Matthew Waters <matthew@centricular.com>
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

#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>

#include <gst/gst.h>

#include <gst/vulkan/vulkan.h>

#include "gstvkwindow_cocoa.h"
#include "gstvkdisplay_cocoa.h"

#include "gstvkcocoa_utils.h"

#define GET_PRIV(o) gst_vulkan_window_cocoa_get_instance_private (o)

#define GST_CAT_DEFAULT gst_vulkan_window_cocoa_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

static void
_init_debug (void)
{
  static gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "vulkanwindowmacos", 0,
        "Vulkan MacOS Window");
    g_once_init_leave (&_init, 1);
  }
}

gboolean gst_vulkan_window_cocoa_handle_event (GstVulkanWindowCocoa * window_cocoa);

enum
{
  PROP_0,
};

struct _GstVulkanWindowCocoaPrivate
{
  gpointer internal_win_id;
  gpointer internal_view;
  gpointer external_view;

  gint preferred_width;
  gint preferred_height;

  gboolean visible;
};

#define gst_vulkan_window_cocoa_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanWindowCocoa, gst_vulkan_window_cocoa,
    GST_TYPE_VULKAN_WINDOW, G_ADD_PRIVATE (GstVulkanWindowCocoa) _init_debug ());

static VkSurfaceKHR gst_vulkan_window_cocoa_get_surface (GstVulkanWindow * window,
    GError ** error);
static gboolean gst_vulkan_window_cocoa_get_presentation_support (GstVulkanWindow
    * window, GstVulkanDevice * device, guint32 queue_family_idx);
static gboolean gst_vulkan_window_cocoa_open (GstVulkanWindow * window,
    GError ** error);
static void gst_vulkan_window_cocoa_close (GstVulkanWindow * window);
static void gst_vulkan_window_cocoa_set_window_handle (GstVulkanWindow * window,
    guintptr window_handle);

static void
gst_vulkan_window_cocoa_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vulkan_window_cocoa_class_init (GstVulkanWindowCocoaClass * klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS (klass);
  GstVulkanWindowClass *window_class = (GstVulkanWindowClass *) klass;

  obj_class->finalize = gst_vulkan_window_cocoa_finalize;

  window_class->open = GST_DEBUG_FUNCPTR (gst_vulkan_window_cocoa_open);
  window_class->close = GST_DEBUG_FUNCPTR (gst_vulkan_window_cocoa_close);
  window_class->get_surface = gst_vulkan_window_cocoa_get_surface;
  window_class->get_presentation_support =
      gst_vulkan_window_cocoa_get_presentation_support;
  window_class->set_window_handle = gst_vulkan_window_cocoa_set_window_handle;
}

static void
gst_vulkan_window_cocoa_init (GstVulkanWindowCocoa * window)
{
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window);

  priv->preferred_width = 320;
  priv->preferred_height = 240;
}

/* Must be called in the gl thread */
GstVulkanWindowCocoa *
gst_vulkan_window_cocoa_new (GstVulkanDisplay * display)
{
  GstVulkanWindowCocoa *window;

  _init_debug ();

  if ((gst_vulkan_display_get_handle_type (display) &
          GST_VULKAN_DISPLAY_TYPE_COCOA)
      == GST_VULKAN_DISPLAY_TYPE_NONE) {
    GST_INFO ("Wrong display type %u for this window type %u", display->type,
        GST_VULKAN_DISPLAY_TYPE_COCOA);
    return NULL;
  }

  window = g_object_new (GST_TYPE_VULKAN_WINDOW_COCOA, NULL);
  gst_object_ref_sink (window);

  return window;
}

static void
_show_window (gpointer data)
{
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (data);
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);
  GstVulkanNSWindow *internal_win_id = (__bridge GstVulkanNSWindow *)priv->internal_win_id;

  GST_DEBUG_OBJECT (window_cocoa, "showing");
  [internal_win_id makeMainWindow];
  [internal_win_id orderFrontRegardless];
  [internal_win_id setViewsNeedDisplay:YES];

  priv->visible = TRUE;
}

static void
gst_vulkan_window_cocoa_show (GstVulkanWindow * window)
{
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);

  if (!priv->visible) {
    if (priv->external_view) {
      gst_vulkan_window_cocoa_set_window_handle (window, (guintptr) priv->external_view);
    } else {
      _gst_vk_invoke_on_main ((GstVulkanWindowFunc) _show_window,
          gst_object_ref (window), (GDestroyNotify) gst_object_unref);
    }
  }
}

static void
gst_vulkan_window_cocoa_hide (GstVulkanWindow * window)
{
//  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);

  /* FIXME: implement */
}

static void
_create_window (GstVulkanWindowCocoa * window_cocoa)
{
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);
  NSRect mainRect = [[NSScreen mainScreen] visibleFrame];
  gint h = priv->preferred_height;
  gint y = mainRect.size.height > h ? (mainRect.size.height - h) * 0.5 : 0;
  NSRect rect = NSMakeRect (0, y, priv->preferred_width, priv->preferred_height);
  GstVulkanNSWindow *internal_win_id;
  GstVulkanNSView *view;

  view = [[GstVulkanNSView alloc] initWithFrame:rect];
  view.wantsLayer = YES;

  internal_win_id = [[GstVulkanNSWindow alloc] initWithContentRect:rect styleMask:
      (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
      NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
      backing: NSBackingStoreBuffered defer: NO screen: nil gstWin: window_cocoa];

  [internal_win_id setContentView:view];

  priv->internal_win_id = (__bridge_retained gpointer)internal_win_id;
  priv->internal_view = (__bridge_retained gpointer)view;

  if (priv->external_view)
    gst_vulkan_window_cocoa_set_window_handle (GST_VULKAN_WINDOW (window_cocoa),
        (guintptr) priv->external_view);

  gst_vulkan_window_cocoa_show (GST_VULKAN_WINDOW (window_cocoa));
}

gboolean
gst_vulkan_window_cocoa_create_window (GstVulkanWindowCocoa * window_cocoa)
{
  _gst_vk_invoke_on_main ((GstVulkanWindowFunc) _create_window,
      gst_object_ref (window_cocoa), gst_object_unref);

  g_usleep(1000000);

  return TRUE;
}

typedef struct 
{
  VkResult result;
  VkSurfaceKHR surface;
  VkMacOSSurfaceCreateInfoMVK * info;
  GstVulkanWindow * window;

  GMutex mutex;
  GCond cond;
  gboolean called;
} GstVulkanCococa_CreateMacOSSurfaceCtx;

static void
_gst_vulkan_cocoa_wait_for_createmacossurface (GstVulkanCococa_CreateMacOSSurfaceCtx * ctx)
{
  g_mutex_lock (&ctx->mutex);
  while (!ctx->called)
    g_cond_wait(&ctx->cond, &ctx->mutex);
  g_mutex_unlock (&ctx->mutex);

  g_mutex_clear (&ctx->mutex);
  g_cond_clear (&ctx->cond);
}

static void
_gst_vulkan_cocoa_create_macos_surface_on_main_thread (gpointer data) 
{
  GstVulkanCococa_CreateMacOSSurfaceCtx * ctx = data;
  GstVulkanWindow *window = ctx->window;
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);

  ctx->result =
      window_cocoa->CreateMacOSSurface (window->display->instance->instance, ctx->info,
      NULL, &ctx->surface);

  g_mutex_lock (&ctx->mutex);
  ctx->called = TRUE;
  g_cond_signal (&ctx->cond);
  g_mutex_unlock (&ctx->mutex);
}

static VkResult
_gst_vulkan_cocoa_create_macos_surface (GstVulkanWindow *window, VkSurfaceKHR * surface)
{
  VkResult err;
  VkMacOSSurfaceCreateInfoMVK info = { 0, };

  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);

  GstVulkanCococa_CreateMacOSSurfaceCtx ctx;

  info.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
  info.pNext = NULL;
  info.flags = 0;
  info.pView = priv->internal_view;

  memset(&ctx, 0, sizeof(GstVulkanCococa_CreateMacOSSurfaceCtx));
  g_mutex_init (&ctx.mutex);
  g_cond_init (&ctx.cond);
  ctx.info = &info;
  ctx.window = window;

  _gst_vulkan_cocoa_invoke_on_main ((GstVulkanWindowFunc) _gst_vulkan_cocoa_create_macos_surface_on_main_thread, &ctx, NULL);
  _gst_vulkan_cocoa_wait_for_createmacossurface (&ctx);

  *surface = ctx.surface;

  return ctx.result;
}

static VkSurfaceKHR
gst_vulkan_window_cocoa_get_surface (GstVulkanWindow * window, GError ** error)
{
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);
  VkSurfaceKHR ret;
  VkResult err;

  if (!window_cocoa->CreateMacOSSurface)
    window_cocoa->CreateMacOSSurface =
        gst_vulkan_instance_get_proc_address (window->display->instance,
        "vkCreateMacOSSurfaceMVK");
  if (!window_cocoa->CreateMacOSSurface) {
    g_set_error_literal (error, GST_VULKAN_ERROR, VK_ERROR_FEATURE_NOT_PRESENT,
        "Could not retrieve \"vkCreateMacOSSurfaceMVK\" function pointer");
    return VK_NULL_HANDLE;
  }

  err = _gst_vulkan_cocoa_create_macos_surface (window, &ret);
  if (gst_vulkan_error_to_g_error (err, error, "vkCreateMacOSSurfaceMVK") < 0)
    return VK_NULL_HANDLE;

  return ret;
}

static gboolean
gst_vulkan_window_cocoa_get_presentation_support (GstVulkanWindow * window,
    GstVulkanDevice * device, guint32 queue_family_idx)
{
  return TRUE;
}

static void
_gst_vulkan_window_cocoa_insert_internal_win (gpointer data)
{
  GstVulkanWindowCocoa *window_cocoa;
  GstVulkanWindowCocoaPrivate *priv;

  window_cocoa = (GstVulkanWindowCocoa*) data;
  priv = GET_PRIV(window_cocoa);

  GstVulkanNSWindow *internal_win_id = (__bridge GstVulkanNSWindow *)priv->internal_win_id;
  NSView *external_view = (__bridge NSView *)priv->external_view;
  NSView *view = (__bridge NSView *)priv->internal_view;

  [internal_win_id orderOut:internal_win_id];
  [external_view addSubview: view];

  [external_view setAutoresizesSubviews: YES];
  [view setFrame: [external_view bounds]];
  [view setAutoresizingMask: NSViewWidthSizable|NSViewHeightSizable];
}

static void
gst_vulkan_window_cocoa_set_window_handle (GstVulkanWindow * window,
    guintptr handle)
{
  GstVulkanWindowCocoa *window_cocoa;
  GstVulkanWindowCocoaPrivate *priv;

  window_cocoa = GST_VULKAN_WINDOW_COCOA (window);
  priv = GET_PRIV(window_cocoa);

  if (priv->internal_win_id) {
    if (handle) {
      priv->external_view = (gpointer)handle;
      priv->visible = TRUE;
    } else {
      /* bring back our internal window */
      priv->external_view = 0;
      priv->visible = FALSE;
    }

    _gst_vulkan_cocoa_invoke_on_main ((GstVulkanWindowFunc) _gst_vulkan_window_cocoa_insert_internal_win,
      gst_object_ref (window), (GDestroyNotify) gst_object_unref);

  } else {
    /* no internal window yet so delay it to the next drawing */
    priv->external_view = (gpointer)handle;
    priv->visible = FALSE;
  }
}

static gboolean
gst_vulkan_window_cocoa_open (GstVulkanWindow * window, GError ** error)
{
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (window);

  if (!GST_VULKAN_WINDOW_CLASS (parent_class)->open (window, error))
    return FALSE;

  return gst_vulkan_window_cocoa_create_window (window_cocoa);
}

static void
_close_window (gpointer * data)
{
  GstVulkanWindowCocoa *window_cocoa = GST_VULKAN_WINDOW_COCOA (data);
  GstVulkanWindow *window = GST_VULKAN_WINDOW (window_cocoa);
  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);
  GstVulkanNSWindow *internal_win_id =
      (__bridge GstVulkanNSWindow *) priv->internal_win_id;

  gst_vulkan_window_cocoa_hide (window);

  [[internal_win_id contentView] removeFromSuperview];
  CFBridgingRelease (priv->internal_win_id);
  priv->internal_win_id = NULL;
  CFBridgingRelease (priv->internal_view);
  priv->internal_view = NULL;
}

static void
gst_vulkan_window_cocoa_close (GstVulkanWindow * window)
{
  _gst_vk_invoke_on_main ((GstVulkanWindowFunc) _close_window,
      gst_object_ref (window), (GDestroyNotify) gst_object_unref);

  GST_VULKAN_WINDOW_CLASS (parent_class)->close (window);
}

@implementation GstVulkanNSWindow

- (id) initWithContentRect: (NSRect) contentRect
        styleMask: (unsigned int) styleMask
    backing: (NSBackingStoreType) bufferingType
    defer: (BOOL) flag screen: (NSScreen *) aScreen
    gstWin: (GstVulkanWindowCocoa *) cocoa {

  m_isClosed = NO;
  window_cocoa = cocoa;

  self = [super initWithContentRect: contentRect
        styleMask: styleMask backing: bufferingType
        defer: flag screen:aScreen];

  GST_DEBUG ("initializing GstVulkanNSWindow");

  [self setReleasedWhenClosed:NO];
  [self setTitle:@"Vulkan renderer"];
  [self setBackgroundColor:[NSColor blackColor]];
  [self orderOut:self];

  return self;
}

- (void) setClosed {
  m_isClosed = YES;
}

- (BOOL) isClosed {
  return m_isClosed;
}

- (BOOL) canBecomeMainWindow {
  return YES;
}

- (BOOL) canBecomeKeyWindow {
  return YES;
}

- (BOOL) windowShouldClose:(id)sender {

  GstVulkanWindowCocoaPrivate *priv = GET_PRIV (window_cocoa);
  GstVulkanNSWindow *internal_win_id = (__bridge GstVulkanNSWindow *)priv->internal_win_id;
  GST_DEBUG ("user clicked the close button");
  [internal_win_id setClosed];
  return YES;
}

@end


@implementation GstVulkanNSView

-(BOOL) wantsUpdateLayer
{
   return YES;
}

+(Class) layerClass
{
  return [CAMetalLayer class];
}

-(CALayer*) makeBackingLayer
{
  CALayer* layer = [self.class.layerClass layer];
  CGSize viewScale = [self convertSizeToBacking: CGSizeMake(1.0, 1.0)];
  layer.contentsScale = MIN(viewScale.width, viewScale.height);
  return layer;
}

@end

void
_gst_vk_invoke_on_main (GstVulkanWindowFunc func, gpointer data, GDestroyNotify notify)
{
  if ([NSThread isMainThread]) {
    func (data);
    if (notify)
      notify (data);
  } else {
    dispatch_async (dispatch_get_main_queue (), ^{
      func (data);
      if (notify)
        notify (data);
    });
  }
}

