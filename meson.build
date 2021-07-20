project('pipewire', ['c' ],
  version : '0.3.32',
  license : [ 'MIT', 'LGPL-2.1-or-later', 'GPL-2.0-only' ],
  meson_version : '>= 0.54.0',
  default_options : [ 'warning_level=3',
                      'c_std=gnu99',
                      'b_pie=true',
		      #'b_sanitize=address,undefined',
                      'buildtype=debugoptimized' ])

pipewire_version = meson.project_version()
version_arr = pipewire_version.split('.')
pipewire_version_major = version_arr[0]
pipewire_version_minor = version_arr[1]
pipewire_version_micro = version_arr[2]
if version_arr.length() == 4
  pipewire_version_nano = version_arr[3]
else
  pipewire_version_nano = 0
endif

spaversion = '0.2'
apiversion = '0.3'
soversion = 0
libversion = '@0@.@1@.0'.format(soversion, pipewire_version_minor.to_int() * 100 + pipewire_version_micro.to_int())

pipewire_name = 'pipewire-@0@'.format(apiversion)
spa_name = 'spa-@0@'.format(spaversion)

prefix = get_option('prefix')
pipewire_bindir = prefix / get_option('bindir')
pipewire_datadir = prefix / get_option('datadir')
pipewire_libdir = prefix / get_option('libdir')
pipewire_libexecdir = prefix / get_option('libexecdir')
pipewire_localedir = prefix / get_option('localedir')
pipewire_sysconfdir = prefix / get_option('sysconfdir')

pipewire_configdir = pipewire_sysconfdir / 'pipewire'
pipewire_confdatadir = pipewire_datadir / 'pipewire'
modules_install_dir = pipewire_libdir / pipewire_name

if host_machine.system() == 'linux'
  # glibc ld.so interprets ${LIB} in a library loading path with an
  # appropriate value for the current architecture, typically something
  # like lib, lib64 or lib/x86_64-linux-gnu.
  # This allows the same pw-jack script to work for both 32- and 64-bit
  # applications on biarch/multiarch distributions, by setting something
  # like LD_LIBRARY_PATH='/usr/${LIB}/pipewire-0.3/jack'.
  # Note that ${LIB} is a special token expanded by the runtime linker,
  # not an environment variable, and must be passed through literally.
  modules_install_dir_dlopen = prefix / '${LIB}' / pipewire_name
else
  modules_install_dir_dlopen = modules_install_dir
endif

spa_plugindir = pipewire_libdir / spa_name

alsadatadir = pipewire_datadir / 'alsa-card-profile' / 'mixer'

pipewire_headers_dir = pipewire_name / 'pipewire'

gnome = import('gnome')
pkgconfig = import('pkgconfig')

cc = meson.get_compiler('c')

common_flags = [
  '-fvisibility=hidden',
  '-Werror=suggest-attribute=format',
  '-Wsign-compare',
  '-Wpointer-arith',
  '-Wpointer-sign',
  '-Wformat',
  '-Wformat-security',
  '-Wimplicit-fallthrough',
  '-Wmissing-braces',
  '-Wtype-limits',
  '-Wvariadic-macros',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-pedantic',
  '-Wold-style-declaration',
  '-Wunused-result',
]

cc_flags = common_flags + [
  '-D_GNU_SOURCE',
  '-DFASTPATH',
# '-DSPA_DEBUG_MEMCPY',
]
add_project_arguments(cc.get_supported_arguments(cc_flags), language: 'c')

have_cpp = add_languages('cpp', native: false, required : false)

if have_cpp
  cxx = meson.get_compiler('cpp')
  cxx_flags = common_flags
  add_project_arguments(cxx.get_supported_arguments(cxx_flags), language: 'cpp')
endif

sse_args = '-msse'
sse2_args = '-msse2'
ssse3_args = '-mssse3'
sse41_args = '-msse4.1'
fma_args = '-mfma'
avx_args = '-mavx'
avx2_args = '-mavx2'

have_sse = cc.has_argument(sse_args)
have_sse2 = cc.has_argument(sse2_args)
have_ssse3 = cc.has_argument(ssse3_args)
have_sse41 = cc.has_argument(sse41_args)
have_fma = cc.has_argument(fma_args)
have_avx = cc.has_argument(avx_args)
have_avx2 = cc.has_argument(avx2_args)

have_neon = false
if host_machine.cpu_family() == 'aarch64'
  if cc.compiles('''
    #include <arm_neon.h>
    int main () {
      float *s;
      asm volatile(
	"      ld1 { v0.4s }, [%[s]], #16\n"
        "      fcvtzs v0.4s, v0.4s, #31\n"
	: [s] "+r" (s) : :);
    }
    ''',
    name : 'aarch64 Neon Support')
      neon_args = []
      have_neon = true

  endif
elif cc.has_argument('-mfpu=neon')
  if cc.compiles('''
    #include <arm_neon.h>
    int main () {
      float *s;
      asm volatile(
        "      vld1.32 { q0 }, [%[s]]!\n"
        "      vcvt.s32.f32 q0, q0, #31\n"
	: [s] "+r" (s) : :);
    }
    ''',
    args: '-mfpu=neon',
    name : 'arm Neon Support')
      neon_args = ['-mfpu=neon']
      have_neon = true
  endif
endif

libatomic = cc.find_library('atomic', required : false)

test_8_byte_atomic = '''
#include <stdint.h>

int main(void)
{
  int64_t eight;
  __atomic_fetch_add(&eight, 123, __ATOMIC_SEQ_CST);
  return 0;
}
'''

# We currently assume that libatomic is unnecessary for 4-byte atomic
# operations on any reasonable architecture.
if cc.links(
  test_8_byte_atomic,
  name : '8-byte __atomic_fetch_add without libatomic')
  atomic_dep = dependency('', required: false)
elif cc.links(
  test_8_byte_atomic,
  dependencies : libatomic,
  name : '8-byte __atomic_fetch_add with libatomic')
  atomic_dep = libatomic
else
  error('8-byte atomic operations are required')
endif

versiondata = configuration_data()
versiondata.set('PIPEWIRE_VERSION_MAJOR', pipewire_version_major)
versiondata.set('PIPEWIRE_VERSION_MINOR', pipewire_version_minor)
versiondata.set('PIPEWIRE_VERSION_MICRO', pipewire_version_micro)
versiondata.set('PIPEWIRE_VERSION_NANO', pipewire_version_nano)
versiondata.set_quoted('PIPEWIRE_API_VERSION', apiversion)

cdata = configuration_data()
cdata.set_quoted('PIPEWIRE_CONFDATADIR', pipewire_confdatadir)
cdata.set_quoted('LOCALEDIR', pipewire_localedir)
cdata.set_quoted('LIBDIR', pipewire_libdir)
cdata.set_quoted('GETTEXT_PACKAGE', meson.project_name())
cdata.set('PACKAGE', '"pipewire"')
cdata.set('PACKAGE_NAME', '"PipeWire"')
cdata.set('PACKAGE_STRING', '"PipeWire @0@"'.format(pipewire_version))
cdata.set('PACKAGE_TARNAME', '"pipewire"')
cdata.set('PACKAGE_URL', '"http://pipewire.org"')
cdata.set_quoted('PACKAGE_VERSION', pipewire_version)
cdata.set_quoted('MODULEDIR', modules_install_dir)
cdata.set_quoted('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
cdata.set_quoted('PLUGINDIR', spa_plugindir)
# FIXME: --with-memory-alignment],[8,N,malloc,pagesize (default is 32)]) option
cdata.set('MEMORY_ALIGNMENT_MALLOC', 1)
cdata.set_quoted('PA_ALSA_PATHS_DIR', alsadatadir / 'paths')
cdata.set_quoted('PA_ALSA_PROFILE_SETS_DIR', alsadatadir / 'profile-sets')

if host_machine.endian() == 'big'
  cdata.set('WORDS_BIGENDIAN', 1)
endif

check_headers = [['dlfcn.h','HAVE_DLFCN_H'],
  ['inttypes.h', 'HAVE_INTTYPES_H'],
  ['memory.h', 'HAVE_MEMORY_H'],
  ['poll.h', 'HAVE_POLL_H'],
  ['stddef.h', 'HAVE_STDDEF_H'],
  ['stdint.h', 'HAVE_STDINT_H'],
  ['stdio_ext.h', 'HAVE_STDIO_EXT_H'],
  ['strings.h', 'HAVE_STRINGS_H'],
  ['string.h', 'HAVE_STRING_H'],
  ['sys/mount.h', 'HAVE_SYS_MOUNT_H'],
  ['sys/param.h', 'HAVE_SYS_PARAM_H'],
  ['sys/poll.h', 'HAVE_SYS_POLL_H'],
  ['sys/prctl.h', 'HAVE_SYS_PRCTL_H'],
  ['sys/random.h', 'HAVE_SYS_RANDOM_H'],
  ['sys/socket.h', 'HAVE_SYS_SOCKET_H'],
  ['sys/stat.h', 'HAVE_SYS_STAT_H'],
  ['sys/times.h', 'HAVE_SYS_TIMES_H'],
  ['sys/time.h', 'HAVE_SYS_TIME_H'],
  ['sys/types.h', 'HAVE_SYS_TYPES_H'],
  ['sys/utsname.h', 'HAVE_SYS_UTSNAME_H'],
  ['sys/vfs.h', 'HAVE_SYS_VFS_H'],
  ['sys/wait.h', 'HAVE_SYS_WAIT_H'],
  ['pwd.h', 'HAVE_PWD_H'],
  ['ucontext.h', 'HAVE_UCONTEXT_H'],
  ['unistd.h', 'HAVE_UNISTD_H'],
]

foreach h : check_headers
  if cc.has_header(h.get(0))
    cdata.set(h.get(1), 1)
  endif
endforeach

if cc.has_function('poll', prefix : '#include<poll.h>')
  cdata.set('HAVE_POLL', 1)
endif
if cc.has_function('pselect', prefix : '#include<sys/select.h>')
  cdata.set('HAVE_PSELECT', 1)
endif
cdata.set('HAVE_MMAP', 1)

if cc.has_function('posix_memalign', prefix : '#include<stdlib.h>')
  cdata.set('HAVE_POSIX_MEMALIGN', 1)
endif
if cc.has_function('getpagesize', prefix : '#include<unistd.h>')
  cdata.set('HAVE_GETPAGESIZE', 1)
endif
if cc.has_function('clock_gettime', prefix : '#include <time.h>')
  cdata.set('HAVE_CLOCK_GETTIME', 1)
endif

if cc.has_type('ptrdiff_t', prefix : '#include <stddef.h>')
  cdata.set('HAVE_PTRDIFF_T', 1)
endif

if cc.has_header_symbol('string.h', 'strndupa', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_STRNDUPA', 1)
endif

if cc.has_function('mkstemp', prefix : '#include <stdlib.h>')
  cdata.set('HAVE_MKSTEMP', 1)
endif

if cc.has_function('memfd_create', prefix : '#include <sys/mman.h>', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_MEMFD_CREATE', 1)
endif

if cc.has_function('getrandom', prefix : '#include <sys/random.h>', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_GETRANDOM', 1)
endif

if cc.has_function('sigabbrev_np', prefix : '#include <string.h>', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_SIGABBREV_NP', 1)
endif

if cc.get_define('SYS_pidfd_open', prefix : '#include <sys/syscall.h>') != ''
  cdata.set('HAVE_PIDFD_OPEN', 1)
endif

systemd = dependency('systemd', required: get_option('systemd'))
systemd_dep = dependency('libsystemd',required: get_option('systemd'))
summary({'systemd conf data': systemd.found()}, bool_yn: true)
summary({'libsystemd': systemd_dep.found()}, bool_yn: true)
if systemd.found() and systemd_dep.found()
  cdata.set('HAVE_SYSTEMD', 1)
endif

configinc = include_directories('.')
includes_inc = include_directories('include')
pipewire_inc = include_directories('src')

makedata = configuration_data()
  makedata.set('BUILD_ROOT', meson.build_root())
  makedata.set('SOURCE_ROOT', meson.source_root())
  makedata.set('VERSION', pipewire_version)
if version_arr.length() == 4
  makedata.set('TAG', 'HEAD')
else
  makedata.set('TAG', pipewire_version)
endif

configure_file(input : 'Makefile.in',
  output : 'Makefile',
  configuration : makedata)

# Find dependencies
mathlib = cc.find_library('m', required : false)
rt_lib = cc.find_library('rt', required : false) # clock_gettime
dl_lib = cc.find_library('dl', required : false)
pthread_lib = dependency('threads')
dbus_dep = dependency('dbus-1')
sdl_dep = dependency('sdl2', required : get_option('sdl2'))
summary({'SDL 2': sdl_dep.found()}, bool_yn: true, section: 'Misc dependencies')
ncurses_dep = dependency('ncursesw', required : false)
sndfile_dep = dependency('sndfile', version : '>= 1.0.20', required : get_option('sndfile'))
summary({'sndfile': sndfile_dep.found()}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
pulseaudio_dep = dependency('libpulse', required : get_option('libpulse'))
summary({'libpulse': pulseaudio_dep.found()}, bool_yn: true, section: 'Streaming between daemons')
avahi_dep = dependency('avahi-client', required : get_option('avahi'))
summary({'Avahi DNS-SD (Zeroconf)': avahi_dep.found()}, bool_yn: true,
  section: 'Streaming between daemons')

libusb_dep = dependency('libusb-1.0', required : get_option('libusb'))
summary({'libusb (Bluetooth quirks)': libusb_dep.found()}, bool_yn: true, section: 'Backend')
if libusb_dep.found()
  cdata.set('HAVE_LIBUSB', 1)
endif

cap_lib = dependency('libcap', required : false)
if cap_lib.found()
  cdata.set('HAVE_LIBCAP', 1)
endif

gst_option = get_option('gstreamer')
gst_deps_def = {
  'glib-2.0': {'version': '>=2.32.0'},
  'gobject-2.0': {},
  'gmodule-2.0': {},
  'gio-2.0': {},
  'gio-unix-2.0': {},
  'gstreamer-1.0': {'version': '>= 1.10.0'},
  'gstreamer-plugins-base-1.0': {},
  'gstreamer-video-1.0': {},
  'gstreamer-audio-1.0': {},
  'gstreamer-allocators-1.0': {},
}

gst_dep = []
foreach depname, kwargs: gst_deps_def
  dep = dependency(depname, required: gst_option, kwargs: kwargs)
  summary({depname: dep.found()}, bool_yn: true, section: 'GStreamer modules')
  if not dep.found()
    # Beware, there's logic below depending on the array clear here!
    gst_dep = []
    if get_option('gstreamer-device-provider').enabled()
      error('`gstreamer-device-provider` is enabled but `@0@` was not found.'.format(depname))
    endif
    break
  endif
  gst_dep += [dep]
endforeach

# This code relies on the array being empty if any dependency was not found
gst_dp_found = gst_dep.length() > 0
summary({'gstreamer-device-provider': gst_dp_found}, bool_yn: true, section: 'Backend')

if not get_option('gstreamer-device-provider').disabled()
  cdata.set('HAVE_GSTREAMER_DEVICE_PROVIDER', 1)
endif

webrtc_dep = dependency('webrtc-audio-processing',
  version : ['>= 0.2', '< 1.0'],
  required : get_option('echo-cancel-webrtc'))
summary({'WebRTC Echo Canceling': webrtc_dep.found()}, bool_yn: true, section: 'Misc dependencies')

if webrtc_dep.found()
  cdata.set('HAVE_WEBRTC', 1)
endif

# On FreeBSD, epoll-shim library is required for eventfd() and timerfd()
epoll_shim_dep = (build_machine.system() == 'freebsd'
    ? dependency('epoll-shim', required: true)
    : dependency('', required: false))

libinotify_dep = (build_machine.system() == 'freebsd'
    ? dependency('libinotify', required: true)
    : dependency('', required: false))

# On FreeBSD, libintl library is required for gettext
libintl_dep = cc.find_library('intl', required: false)

alsa_dep = dependency('alsa', version : '>=1.1.7', required: get_option('pipewire-alsa'))
summary({'pipewire-alsa': alsa_dep.found()}, bool_yn: true)

installed_tests_metadir = pipewire_datadir / 'installed-tests' / pipewire_name
installed_tests_execdir = pipewire_libexecdir / 'installed-tests' / pipewire_name
installed_tests_enabled = not get_option('installed_tests').disabled()
installed_tests_template = files('template.test.in')

if not get_option('tests').disabled()
  gstack = find_program('gstack', required : false)
  cdata.set10('HAVE_GSTACK', gstack.found())
endif

subdir('po')
subdir('spa')
subdir('src')

if not get_option('tests').disabled()
  subdir('test')
endif

configure_file(output : 'config.h',
               configuration : cdata)

if not get_option('pipewire-jack').disabled()
  subdir('pipewire-jack')
endif

if alsa_dep.found()
  subdir('pipewire-alsa/alsa-plugins')
  subdir('pipewire-alsa/conf')
endif

doxygen = find_program('doxygen', required : get_option('docs'))
if doxygen.found()
  subdir('doc')
endif

xmltoman = find_program('xmltoman', required : get_option('man'))
summary({'Manpage generation': xmltoman.found()}, bool_yn: true)
if xmltoman.found()
  subdir('man')
endif

setenv = find_program('pw-uninstalled.sh')
run_target('pw-uninstalled', command : [setenv,
    '-b@0@'.format(meson.build_root()), '-v@0@'.format(pipewire_version)])

if meson.version().version_compare('>=0.58.0')
  devenv = environment()

  builddir = meson.current_build_dir()
  srcdir = meson.current_source_dir()

  devenv.set('PIPEWIRE_CONFIG_DIR', builddir / 'src' / 'daemon')
  devenv.set('PIPEWIRE_MODULE_DIR', builddir / 'src' / 'modules')

  devenv.set('SPA_PLUGIN_DIR', builddir / 'spa' / 'plugins')

  devenv.set('GST_PLUGIN_PATH', builddir / 'src'/ 'gst')

  devenv.set('ALSA_PLUGIN_DIR', builddir / 'pipewire-alsa' / 'alsa-plugins')
  devenv.set('ACP_PATHS_DIR', srcdir / 'spa' / 'plugins' / 'alsa' / 'mixer' / 'paths')
  devenv.set('ACP_PROFILES_DIR', srcdir / 'spa' / 'plugins' / 'alsa' / 'mixer' / 'profile-sets')

  devenv.set('LD_LIBRARY_PATH', builddir / 'pipewire-jack' / 'src')

  devenv.set('PW_UNINSTALLED', '1')

  meson.add_devenv(devenv)
endif
