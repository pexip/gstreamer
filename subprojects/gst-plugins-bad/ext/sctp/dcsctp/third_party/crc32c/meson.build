cxx = meson.get_compiler('cpp')

crc32c_sources = [
  'src/crc32c.cc',
  'src/crc32c_portable.cc',
]

crc32c_core_conf = configuration_data()
crc32c_core_conf.set10('BYTE_ORDER_BIG_ENDIAN', (host_machine.endian() == 'big'))
crc32c_core_conf.set10('CRC32C_TESTS_BUILT_WITH_GLOG', false)

builtin_prefetch_code = '''
int main() {
  char data = 0;
  const char* address = &data;
  __builtin_prefetch(address, 0, 0);
  return 0;
}
  '''
crc32c_core_conf.set10('HAVE_BUILTIN_PREFETCH',
  cxx.compiles(builtin_prefetch_code, name : '__builtin_prefetch() test'))

mm_prefetch_code = '''
#if defined(_MSC_VER)
#include <intrin.h>
#else  // !defined(_MSC_VER)
#include <xmmintrin.h>
#endif  // defined(_MSC_VER)

int main() {
  char data = 0;
  const char* address = &data;
  _mm_prefetch(address, _MM_HINT_NTA);
  return 0;
}
  '''
crc32c_core_conf.set10('HAVE_MM_PREFETCH',
  cxx.compiles(mm_prefetch_code, name : '_mm_prefetch() test'))

sse4_code = '''
#if defined(_MSC_VER)
#include <intrin.h>
#else  // !defined(_MSC_VER)
#include <cpuid.h>
#include <nmmintrin.h>
#endif  // defined(_MSC_VER)

int main() {
  _mm_crc32_u8(0, 0); _mm_crc32_u32(0, 0);
#if defined(_M_X64) || defined(__x86_64__)
   _mm_crc32_u64(0, 0);
#endif // defined(_M_X64) || defined(__x86_64__)
  return 0;
}
  '''
have_sse4 = cxx.compiles(sse4_code, name : 'sse4() test')
crc32c_core_conf.set10('HAVE_SSE42', have_sse4)
if have_sse4
  crc32c_sources += ['src/crc32c_sse42.cc']
endif

arm64_crc32c_code = '''
#include <arm_acle.h>
#include <arm_neon.h>

int main() {
  __crc32cb(0, 0); __crc32ch(0, 0); __crc32cw(0, 0); __crc32cd(0, 0);
  vmull_p64(0, 0);
  return 0;
}
'''
have_arm64_crc32c = cxx.compiles(arm64_crc32c_code, name : 'arm_crc32c() test')
crc32c_core_conf.set10('HAVE_ARM64_CRC32C', have_arm64_crc32c)
if have_arm64_crc32c
  crc32c_sources += ['src/crc32c_arm64.cc']
endif

strong_getauxval_code = '''
#include <arm_acle.h>
#include <arm_neon.h>
#include <sys/auxv.h>

int main() {
  getauxval(AT_HWCAP);
  return 0;
}
'''
crc32c_core_conf.set10('HAVE_STRONG_GETAUXVAL',
  cxx.compiles(strong_getauxval_code, name : 'strong getauxval() test'))

weak_getauxval_code = '''
unsigned long getauxval(unsigned long type) __attribute__((weak));
#define AT_HWCAP 16

int main() {
  getauxval(AT_HWCAP);
  return 0;
}
'''
crc32c_core_conf.set10('HAVE_WEAK_GETAUXVAL',
  cxx.compiles(weak_getauxval_code, name : 'weak getauxval() test'))

configure_file(output : 'crc32c_config.h',
    input: 'crc32_config.h.meson',
    configuration : crc32c_core_conf)

crc32c_incdirs = [
  include_directories('..'),
  include_directories('src'),
  include_directories('src/include'),
]

crc32c_lib = static_library('crc32c',
  crc32c_sources,
  install: false,
  include_directories: crc32c_incdirs)

crc32c_dep = declare_dependency(link_with: crc32c_lib,
  include_directories : include_directories('src/include'))
