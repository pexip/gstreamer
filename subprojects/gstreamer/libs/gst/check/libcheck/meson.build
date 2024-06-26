libcheck_files = files(
  'check.c',
  'check_error.c',
  'check_list.c',
  'check_log.c',
  'check_msg.c',
  'check_pack.c',
  'check_print.c',
  'check_run.c',
  'check_str.c',
  'libcompat/libcompat.c'
)

if not cdata.has('HAVE_ALARM')
  libcheck_files += ['libcompat/alarm.c']
endif

if not cdata.has('HAVE_GETTIMEOFDAY')
  libcheck_files += ['libcompat/gettimeofday.c']
endif

if not cdata.has('HAVE_CLOCK_GETTIME')
  libcheck_files += ['libcompat/clock_gettime.c']
endif

if not cdata.has('HAVE_DECL_LOCALTIME_R')
  libcheck_files += ['libcompat/localtime_r.c']
endif

if not cdata.has('HAVE_DECL_STRSIGNAL')
  libcheck_files += ['libcompat/strsignal.c']
endif

if not cdata.has('HAVE_DECL_STRDUP') and not cdata.has('HAVE__STRDUP')
  libcheck_files += ['libcompat/strdup.c']
endif

if not cdata.has('HAVE_GETLINE')
  libcheck_files += ['libcompat/getline.c']
endif

# FIXME: check that timer_create, timer_settime, timer_delete are in rt_lib
if not rt_lib.found() or get_option('default_library') == 'static'
  libcheck_files += files(
    'libcompat/timer_create.c',
    'libcompat/timer_settime.c',
    'libcompat/timer_delete.c'
  )
endif

configure_file(input : 'check.h.in',
  output : 'check.h',
  configuration : check_cdata)

internal_check_h_inc = include_directories('..')

# Must explicitly make symbols public if default visibility is hidden
if host_system in ['windows', 'cygwin'] and not static_build
  libcheck_visibility_args = ['-DCK_DLL_EXP=extern __declspec(dllexport)']
elif have_visibility_hidden
  libcheck_visibility_args = ['-DCK_DLL_EXP=extern __attribute__ ((visibility ("default")))']
else
  libcheck_visibility_args = ['-DCK_DLL_EXP=extern']
endif

no_warn_args = []
foreach arg : [
  '-Wno-undef',
  '-Wno-redundant-decls',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-old-style-definition',
  '-Wno-declaration-after-statement',
  '-Wno-format-nonliteral',
  '-Wno-tautological-constant-out-of-range-compare']
  if cc.has_argument(arg)
    no_warn_args += [arg]
  endif
endforeach

libcheck = static_library('check',
  libcheck_files,
  include_directories : [configinc, internal_check_h_inc],
  dependencies : [rt_lib, mathlib, glib_dep],
  c_args: gst_c_args + libcheck_visibility_args + no_warn_args +
	  # Don't want libcompat to think we don't have these and substitute
	  # replacements since we don't check for or define these. See libcompat.h
	  ['-DHAVE_VSNPRINTF', '-DHAVE_SNPRINTF', '-DHAVE_MALLOC', '-DHAVE_REALLOC'],
  pic: true)
