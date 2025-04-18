gstvulkan_dep = dependency('', required: false)
gstvulkanxcb_dep = dependency('', required: false)
gstvulkanwyland_dep = dependency('', required: false)
vulkan_windowing_opt = get_option('vulkan-windowing')
vulkan_opt = get_option('vulkan')
if vulkan_opt.disabled()
  subdir_done()
endif

vulkan_sources = files(
  'gstvkbuffermemory.c',
  'gstvkbufferpool.c',
  'gstvkcommandbuffer.c',
  'gstvkcommandpool.c',
  'gstvkdescriptorcache.c',
  'gstvkdescriptorset.c',
  'gstvkdescriptorpool.c',
  'gstvkdevice.c',
  'gstvkdebug.c',
  'gstvkdisplay.c',
  'gstvkerror.c',
  'gstvkfence.c',
  'gstvkformat.c',
  'gstvkfullscreenquad.c',
  'gstvkhandle.c',
  'gstvkhandlepool.c',
  'gstvkimagememory.c',
  'gstvkimagebufferpool.c',
  'gstvkimageview.c',
  'gstvkinstance.c',
  'gstvkmemory.c',
  'gstvkoperation.c',
  'gstvkphysicaldevice.c',
  'gstvkqueue.c',
  'gstvkswapper.c',
  'gstvktrash.c',
  'gstvkvideofilter.c',
  'gstvkvideoutils.c',
  'gstvkutils.c',
  'gstvkwindow.c',
)

vulkan_headers = files(
  'gstvkapi.h',
  'gstvkbarrier.h',
  'gstvkbuffermemory.h',
  'gstvkbufferpool.h',
  'gstvkcommandbuffer.h',
  'gstvkcommandpool.h',
  'gstvkdescriptorcache.h',
  'gstvkdescriptorset.h',
  'gstvkdescriptorpool.h',
  'gstvkdebug.h',
  'gstvkdevice.h',
  'gstvkdisplay.h',
  'gstvkerror.h',
  'gstvkfence.h',
  'gstvkformat.h',
  'gstvkfullscreenquad.h',
  'gstvkhandle.h',
  'gstvkhandlepool.h',
  'gstvkimagememory.h',
  'gstvkimagebufferpool.h',
  'gstvkimageview.h',
  'gstvkinstance.h',
  'gstvkmemory.h',
  'gstvkoperation.h',
  'gstvkphysicaldevice.h',
  'gstvkqueue.h',
  'gstvkswapper.h',
  'gstvktrash.h',
  'gstvkutils.h',
  'gstvkvideofilter.h',
  'gstvkvideoutils.h',
  'gstvkwindow.h',
  'vulkan-prelude.h',
  'vulkan_fwd.h',
  'vulkan.h',
)

vulkan_priv_sources = []
vulkan_xcb_sources = []
vulkan_xcb_headers = []
vulkan_wayland_sources = []
vulkan_wayland_headers = []

vulkan_windowing = false
vulkan_objc_args = []
vulkan_defines = []
optional_deps = []
has_vulkan_header = false
vulkan_dep = dependency('', required: false)
vulkan_inc_dir = ''
enabled_vulkan_winsys = []
is_ios_simulator = host_system == 'ios' and meson.get_external_property('needs_exe_wrapper', false)

vulkan_conf = configuration_data()
vulkan_conf_options = [
    'GST_VULKAN_HAVE_WINDOW_XCB',
    'GST_VULKAN_HAVE_WINDOW_WAYLAND',
    'GST_VULKAN_HAVE_WINDOW_COCOA',
    'GST_VULKAN_HAVE_WINDOW_IOS',
    'GST_VULKAN_HAVE_WINDOW_WIN32',
    'GST_VULKAN_HAVE_WINDOW_ANDROID',
    'GST_VULKAN_HAVE_VIDEO_EXTENSIONS',
]

foreach option : vulkan_conf_options
  vulkan_conf.set(option, 0)
endforeach

if ['ios', 'darwin'].contains(host_system)
  # - ios does not support the loader/validation layers
  # - We need to link directly to MoltenVK to be able to use
  #   MoltenVK-specific functions that use dispatchable handles (like
  #   retrieving the metal device from the VkDevice) which is currently waiting
  #   on implementing a proper Metal extension for Vulkan
  #   https://github.com/KhronosGroup/MoltenVK/issues/492

  # 1. Download and install the Vulkan SDK for macOS:
  #   https://vulkan.lunarg.com/sdk/home#mac
  # 2. Set a shell environment variable to point to the SDK:
  #   `export VK_SDK_PATH="/path/to/VulkanSDK/1.3.231.1"`
  vulkan_root = run_command(python3, '-c', 'import os; print(os.environ.get("VK_SDK_PATH"))', check: false).stdout().strip()

  if vulkan_root != '' and vulkan_root != 'None'
    molten_vk_root = join_paths(vulkan_root, 'MoltenVK')

    if is_ios_simulator
      platform_dir = 'ios-arm64_x86_64-simulator'
    elif host_system == 'ios'
      platform_dir = 'ios-arm64'
    else
      platform_dir = 'macos-arm64_x86_64'
    endif

    vulkan_lib_dir = join_paths(molten_vk_root, 'MoltenVK.xcframework', platform_dir)
    vulkan_inc_dir = join_paths(molten_vk_root, 'include')
    vulkan_lib = cc.find_library('MoltenVK', dirs: vulkan_lib_dir,
                                 required : get_option('vulkan'))
    has_vulkan_header = cc.has_header('vulkan/vulkan_core.h',
                                      include_directories: include_directories(vulkan_inc_dir))
    vulkan_dep = declare_dependency(include_directories: include_directories(vulkan_inc_dir),
                                    dependencies: vulkan_lib,
                                    link_args: ['-lc++'])
  else
    subdir_done()
  endif
elif host_system == 'windows'
  vulkan_root = run_command(python3, '-c', 'import os; print(os.environ.get("VK_SDK_PATH"))', check: false).stdout().strip()
  if vulkan_root != '' and vulkan_root != 'None'
    vulkan_lib_dir = ''
    if build_machine.cpu_family() == 'x86_64'
      vulkan_lib_dir = join_paths(vulkan_root, 'Lib')
    else
      vulkan_lib_dir = join_paths(vulkan_root, 'Lib32')
    endif

    vulkan_lib = cc.find_library('vulkan-1', dirs: vulkan_lib_dir,
                                 required : vulkan_opt)

    vulkan_inc_dir = join_paths(vulkan_root, 'Include')
    has_vulkan_header = cc.has_header('vulkan/vulkan_core.h',
                                      args: '-I' + vulkan_inc_dir,
                                      required: vulkan_opt)

    if vulkan_lib.found() and has_vulkan_header
      vulkan_dep = declare_dependency(include_directories: include_directories(vulkan_inc_dir),
                                      dependencies: vulkan_lib)
    endif
  endif
else
  vulkan_dep = dependency('vulkan', method: 'pkg-config', required: false)
  if not vulkan_dep.found()
    vulkan_dep = cc.find_library('vulkan', required: vulkan_opt)
  endif
endif

if not ['ios', 'darwin', 'windows'].contains(host_system)
  has_vulkan_header = cc.has_header('vulkan/vulkan_core.h', dependencies: vulkan_dep, required: vulkan_opt)
endif

if 'x11' in vulkan_windowing_opt or 'auto' in vulkan_windowing_opt
  vulkan_xcb_required = vulkan_opt.enabled() and 'x11' in vulkan_windowing_opt
  xcb_dep = dependency('xcb', version : '>=1.10', required: vulkan_xcb_required)
  xkbcommon_dep = dependency('xkbcommon', required: vulkan_xcb_required)
  xkbcommon_x11_dep = dependency('xkbcommon-x11', required: vulkan_xcb_required)

  if xcb_dep.found() and xkbcommon_dep.found() and xkbcommon_x11_dep.found() and \
     cc.has_header('vulkan/vulkan_xcb.h', dependencies : vulkan_dep, required: vulkan_xcb_required)
    vulkan_priv_sources += files(
      'xcb/gstvkwindow_xcb.c',
      'xcb/xcb_event_source.c',
    )
    vulkan_xcb_sources += files(
      'xcb/gstvkdisplay_xcb.c',
    )
    vulkan_xcb_headers += files(
      'xcb/xcb.h',
      'xcb/gstvkdisplay_xcb.h'
    )

    optional_deps += [xcb_dep, xkbcommon_dep, xkbcommon_x11_dep]
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_XCB', 1)
    enabled_vulkan_winsys += ['xcb']
  endif
endif

if 'wayland' in vulkan_windowing_opt or 'auto' in vulkan_windowing_opt
  vulkan_wayland_required = vulkan_opt.enabled() and 'wayland' in vulkan_windowing_opt
  wayland_client_dep = dependency('wayland-client', version : '>=1.4', required: vulkan_wayland_required)
  wayland_protocols_dep = dependency('wayland-protocols', version : '>= 1.15', required: vulkan_wayland_required)
  wayland_scanner = find_program('wayland-scanner', required: vulkan_wayland_required)

  if wayland_client_dep.found() and wayland_protocols_dep.found() and wayland_scanner.found() and \
     cc.has_header('vulkan/vulkan_wayland.h', dependencies: vulkan_dep, required: vulkan_wayland_required)
    # Generate the XDG shell interface
    wayland_protocols_basedir = wayland_protocols_dep.get_variable('pkgdatadir')
    xdg_shell_xml_spec = join_paths(wayland_protocols_basedir, 'stable', 'xdg-shell', 'xdg-shell.xml')
    xdg_shell_header = custom_target('xdg-shell-client-header',
      command: [ wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@' ],
      input: xdg_shell_xml_spec,
      output: 'xdg-shell-client-protocol.h',
    )
    xdg_shell_code = custom_target('xdg-shell-client-code',
      command: [ wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@' ],
      input: xdg_shell_xml_spec,
      output: 'xdg-shell-client-protocol.c',
    )

    vulkan_priv_sources += files(
      'wayland/gstvkdisplay_wayland.c',
      'wayland/gstvkwindow_wayland.c',
      'wayland/wayland_event_source.c',
    )
    vulkan_priv_sources += [
      xdg_shell_header,
      xdg_shell_code,
    ]
    vulkan_wayland_sources += files(
      'wayland/gstvkdisplay_wayland.c',
    )
    vulkan_wayland_headers += files(
      'wayland/wayland.h',
      'wayland/gstvkdisplay_wayland.h'
    )

    optional_deps += wayland_client_dep
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_WAYLAND', 1)
    enabled_vulkan_winsys += ['wayland']
  endif
endif

if ['darwin', 'ios'].contains(host_system)
  objc = meson.get_compiler('objc')
  if not objc.has_argument('-fobjc-arc')
    error('ARC is required for building')
  endif

  vulkan_objc_args += ['-fobjc-arc']

  apple_deps = [
    dependency('appleframeworks', modules : ['Foundation'], required : vulkan_opt),
    dependency('appleframeworks', modules : ['QuartzCore'], required : vulkan_opt),
    dependency('appleframeworks', modules : ['CoreFoundation'], required : vulkan_opt),
    dependency('appleframeworks', modules : ['Metal'], required : vulkan_opt),
    dependency('appleframeworks', modules : ['IOSurface'], required : vulkan_opt),
  ]

  foreach dep : apple_deps
    if dep.found()
      optional_deps += [dep]
    endif
  endforeach
endif

if host_system == 'darwin'
  cocoa_dep = dependency('appleframeworks', modules : ['Cocoa'], required : vulkan_opt)

  if cocoa_dep.found() and cc.has_header('vulkan/vulkan_macos.h', include_directories: include_directories(vulkan_inc_dir), dependencies: vulkan_dep, required: vulkan_opt)
    vulkan_priv_sources += files(
      'cocoa/gstvkdisplay_cocoa.m',
      'cocoa/gstvkwindow_cocoa.m',
    )
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_COCOA', 1)
    enabled_vulkan_winsys += ['cocoa']

    iokit_dep = dependency('appleframeworks', modules : ['IOKit'], required : get_option('vulkan'))
    if cocoa_dep.found()
      optional_deps += [iokit_dep]
    endif

    optional_deps += [cocoa_dep]
  endif
endif

if host_system == 'ios'
  uikit_dep = dependency('appleframeworks', modules : ['UIKit', 'IOSurface', 'CoreGraphics', 'Metal'], required : vulkan_opt)

  if uikit_dep.found() and cc.has_header('vulkan/vulkan_ios.h', include_directories: include_directories(vulkan_inc_dir), dependencies : vulkan_dep, required: vulkan_opt)
    vulkan_priv_sources += files(
      'ios/gstvkdisplay_ios.m',
      'ios/gstvkwindow_ios.m',
    )
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_IOS', 1)
    enabled_vulkan_winsys += ['ios']

    coregraphics_dep = dependency('appleframeworks', modules : ['CoreGraphics'], required : get_option('vulkan'))
    if coregraphics_dep.found()
      optional_deps += [coregraphics_dep]
    endif

    optional_deps += [uikit_dep]
  endif
endif

if host_system == 'windows'
  gdi_dep = cc.find_library('gdi32', required : vulkan_opt)

  # Cannot use internal dependency object with cc.has_header()
  if gdi_dep.found() and cc.has_header('vulkan/vulkan_win32.h', args: '-I' + vulkan_inc_dir, required: vulkan_opt)
    vulkan_priv_sources += ['win32/gstvkwindow_win32.c']
    optional_deps += [gdi_dep]
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_WIN32', 1)
    enabled_vulkan_winsys += ['win32']
  endif
endif

if host_system == 'android'
  if cc.has_header('vulkan/vulkan_android.h', dependencies : vulkan_dep, required: vulkan_opt)
    vulkan_priv_sources += files(
      'android/gstvkdisplay_android.c',
      'android/gstvkwindow_android.c',
    )
    vulkan_windowing = true
    vulkan_conf.set('GST_VULKAN_HAVE_WINDOW_ANDROID', 1)
    enabled_vulkan_winsys += ['android']
  endif
endif

if not vulkan_windowing
  if vulkan_opt.enabled()
    error('No Windowing system found. vulkansink will not work')
  else
    message('No Windowing system found. vulkansink will not work')
  endif
endif

if not vulkan_dep.found() or not has_vulkan_header
  subdir_done()
endif

if get_option('vulkan-video').allowed()
  video_test = '''
#include <vulkan/vulkan.h>

#if !(defined(VK_VERSION_1_4) || (defined(VK_VERSION_1_3) && VK_HEADER_VERSION >= 275))
#error "Need at least Vulkan 1.3.275"
#endif

/* vk_video/vulkan_video_codec_h264std.h */
static StdVideoH264SequenceParameterSet h264_sps;
static StdVideoH264PictureParameterSet h264_pps;

/* vk_video/vulkan_video_codec_h265std.h */
static StdVideoH265SequenceParameterSet h265_sps;
static StdVideoH265PictureParameterSet h265_pps;

static VkVideoEncodeH264ProfileInfoKHR h264_profile;
static VkVideoEncodeH265ProfileInfoKHR h265_profile;
'''

  if vulkan_inc_dir != ''
    have_vk_video = cc.compiles(video_test, dependencies : vulkan_dep,  args: '-I' + vulkan_inc_dir)
  else
    have_vk_video = cc.compiles(video_test, dependencies : vulkan_dep)
  endif
  if have_vk_video
    vulkan_conf.set('GST_VULKAN_HAVE_VIDEO_EXTENSIONS', 1)
    vulkan_priv_sources += files(
      'gstvkvideo-private.c',
      'gstvkdecoder-private.c',
      'gstvkencoder-private.c',
    )
  elif get_option('vulkan-video').enabled()
    error('Vulkan Video extensions headers not found')
  endif
endif

gen_sources = []

install_headers(vulkan_headers, subdir : 'gstreamer-1.0/gst/vulkan')

configure_file(input : 'gstvkconfig.h.meson',
  output : 'gstvkconfig.h',
  install_dir : get_option('includedir') / 'gstreamer-1.0/gst/vulkan',
  configuration : vulkan_conf)

glib_mkenums = find_program('glib-mkenums')
mkenums = find_program('vulkan_mkenum.py')
vulkan_enumtypes_h = custom_target('gstvulkanenum_h',
  output : 'vulkan-enumtypes.h',
  input : vulkan_headers,
  install : true,
  install_dir : get_option('includedir') / 'gstreamer-1.0/gst/vulkan',
  command : [mkenums, glib_mkenums, '@OUTPUT@', '@INPUT@'])

vulkan_enumtypes_c = custom_target('gstvulkanenum_c',
  output : 'vulkan-enumtypes.c',
  input : vulkan_headers,
  depends : [vulkan_enumtypes_h],
  command : [mkenums, glib_mkenums, '@OUTPUT@', '@INPUT@'])
gen_sources += [vulkan_enumtypes_h]

gstvulkan = library('gstvulkan-' + api_version,
  vulkan_sources, vulkan_priv_sources, vulkan_wayland_sources, vulkan_xcb_sources, vulkan_enumtypes_c, vulkan_enumtypes_h,
  c_args : gst_plugins_bad_args + vulkan_defines + ['-DBUILDING_GST_VULKAN', '-DG_LOG_DOMAIN="GStreamer-Vulkan"'],
  objc_args : gst_plugins_bad_args + vulkan_defines + vulkan_objc_args + ['-DBUILDING_GST_VULKAN', '-DG_LOG_DOMAIN="GStreamer-Vulkan"'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  darwin_versions : osxversion,
  install : true,
  dependencies : [gstbase_dep, gstvideo_dep, vulkan_dep] + optional_deps,
  # don't confuse gst/vulkan/xcb/xcb.h with xcb/xcb.h
  implicit_include_directories : false)

library_def = {'lib': gstvulkan}
pkg_name = 'gstreamer-vulkan-1.0'
pkgconfig.generate(gstvulkan,
  libraries : [gst_dep, gstbase_dep, gstvideo_dep],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : pkg_name,
  description : 'GStreamer Vulkan support',
)

if build_gir
  extra_gir_includes = []
  gobject_introspection_dep = dependency('gobject-introspection-1.0')
  if gobject_introspection_dep.version().version_compare('>=1.61.1')
    # This is the first version that contains Vulkan-1.0.gir
    extra_gir_includes += ['Vulkan-1.0']
  endif

  gir = {
    'sources' : vulkan_sources + vulkan_headers + [vulkan_enumtypes_h, vulkan_enumtypes_c],
    'namespace' : 'GstVulkan',
    'nsversion' : api_version,
    'identifier_prefix' : 'Gst',
    'symbol_prefix' : 'gst',
    'export_packages' : pkg_name,
    'includes' : ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0'] + extra_gir_includes,
    'install' : true,
    'extra_args' : gir_init_section + ['--c-include=gst/vulkan/vulkan.h'],
    'dependencies' : [gstvideo_dep, gst_dep, gstbase_dep] + optional_deps
  }

  library_def += {'gir': [gir]}
  if not static_build
    vulkan_gir = gnome.generate_gir(gstvulkan, kwargs: gir)
    library_def += {'gir_targets':  library_def.get('gir_targets', []) + [vulkan_gir]}
    gen_sources += vulkan_gir
  endif
endif
gst_libraries += [[pkg_name, library_def]]

gstvulkan_dep = declare_dependency(link_with : gstvulkan,
  include_directories : [libsinc],
  sources: gen_sources,
  dependencies : [gstvideo_dep, gstbase_dep, vulkan_dep] + optional_deps)

meson.override_dependency(pkg_name, gstvulkan_dep)

if enabled_vulkan_winsys.contains('xcb')
  install_headers(vulkan_xcb_headers, subdir : 'gstreamer-1.0/gst/vulkan/xcb')
  pkgconfig.generate(
    libraries : [gstvulkan],
    requires : ['xcb'],
    subdirs : pkgconfig_subdirs,
    name : 'gstreamer-vulkan-xcb-1.0',
    description : 'GStreamer Vulkan support (XCB Specifics)',
  )
  vulkan_xcb_gir = []
  if build_gir
    gir = {
      'sources' : vulkan_xcb_sources + vulkan_xcb_headers,
      'namespace' : 'GstVulkanXCB',
      'nsversion' : api_version,
      'identifier_prefix' : 'Gst',
      'symbol_prefix' : 'gst',
      'export_packages' : 'gstreamer-vulkan-xcb-1.0',
      'install' : true,
      'extra_args' : gir_init_section + ['--c-include=gst/vulkan/xcb/xcb.h'],
      'dependencies' : [gstvideo_dep, gst_dep, gstbase_dep] + optional_deps
    }

    if not static_build
      gir += {'includes' : ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0', vulkan_gir[0]] + extra_gir_includes}
      vulkan_xcb_gir = gnome.generate_gir(gstvulkan, kwargs: gir)
      library_def += {'gir_targets':  library_def.get('gir_targets', []) + [vulkan_xcb_gir]}
    endif

    gir += {'includes' :['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0', 'GstVulkan-1.0'] + extra_gir_includes}
    library_def += {'gir':  library_def['gir'] + [gir]}
  endif
  gstvulkanxcb_dep = declare_dependency(dependencies : [gstvulkan_dep],
      sources : vulkan_xcb_gir)
  meson.override_dependency('gstreamer-vulkan-xcb-1.0', gstvulkanxcb_dep)
endif

if enabled_vulkan_winsys.contains('wayland')
  install_headers(vulkan_wayland_headers, subdir : 'gstreamer-1.0/gst/vulkan/wayland')
  pkgconfig.generate(
    libraries : [gstvulkan],
    requires : ['wayland-client'],
    subdirs : pkgconfig_subdirs,
    name : 'gstreamer-vulkan-wayland-1.0',
    description : 'GStreamer Vulkan support (Wayland Specifics)',
  )
  vulkan_wayland_gir = []
  if build_gir
    gir = {
      'sources' : vulkan_wayland_sources + vulkan_wayland_headers,
      'namespace' : 'GstVulkanWayland',
      'nsversion' : api_version,
      'identifier_prefix' : 'Gst',
      'symbol_prefix' : 'gst',
      'export_packages' : 'gstreamer-vulkan-wayland-1.0',
      'install' : true,
      'extra_args' : gir_init_section + ['--c-include=gst/vulkan/wayland/wayland.h'],
      'dependencies' : [gstvideo_dep, gst_dep, gstbase_dep] + optional_deps
    }
    if not static_build
      gir += {'includes' : ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0', vulkan_gir[0]] + extra_gir_includes}
      vulkan_wayland_gir += gnome.generate_gir(gstvulkan, kwargs: gir)
      library_def += {'gir_targets':  library_def.get('gir_targets', []) + [vulkan_wayland_gir]}
    endif
    gir += {'includes' :['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0', 'GstVulkan-1.0'] + extra_gir_includes}
    library_def += {'gir':  library_def['gir'] + [gir]}
  endif
  gstvulkanwayland_dep = declare_dependency(dependencies : [gstvulkan_dep],
      sources : vulkan_wayland_gir)
  meson.override_dependency('gstreamer-vulkan-wayland-1.0', gstvulkanwayland_dep)
endif
