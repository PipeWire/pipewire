project('pipewire', ['c' ],
  version : '0.3.2',
  license : 'MIT',
  meson_version : '>= 0.50.0',
  default_options : [ 'warning_level=3',
                      'c_std=gnu99',
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
pipewire_bindir = join_paths(prefix, get_option('bindir'))
pipewire_datadir = join_paths(prefix, get_option('datadir'))
pipewire_libdir = join_paths(prefix, get_option('libdir'))
pipewire_localedir = join_paths(prefix, get_option('localedir'))
pipewire_sysconfdir = join_paths(prefix, get_option('sysconfdir'))

pipewire_configdir = join_paths(pipewire_sysconfdir, 'pipewire')
modules_install_dir = join_paths(pipewire_libdir, pipewire_name)
spa_plugindir = join_paths(pipewire_libdir, spa_name)

pipewire_headers_dir = join_paths(pipewire_name, 'pipewire')

gnome = import('gnome')
pkgconfig = import('pkgconfig')

have_cpp = add_languages('cpp', required : false)

cc = meson.get_compiler('c')

if cc.get_id() == 'gcc'
  add_global_arguments('-fvisibility=hidden',
			'-Wsign-compare',
			'-Wimplicit-fallthrough',
			'-Wpointer-arith',
			'-Wformat-security',
			'-Werror=suggest-attribute=format',
			'-Wmissing-braces',
			'-Wtype-limits',
			'-Wold-style-declaration',
			'-Wvariadic-macros',
		        '-Wno-missing-field-initializers',
		        '-Wno-unused-parameter',
			'-Wno-pedantic',
			'-DFASTPATH',
#			'-DSPA_DEBUG_MEMCPY',
			language : 'c')
  add_global_arguments('-fvisibility=hidden',
			'-Wsign-compare',
			'-Wimplicit-fallthrough',
			'-Wpointer-arith',
			'-Wformat-security',
			'-Werror=suggest-attribute=format',
			'-Wmissing-braces',
			'-Wtype-limits',
			'-Wvariadic-macros',
		        '-Wno-missing-field-initializers',
		        '-Wno-unused-parameter',
			'-Wno-pedantic',
			language : 'cpp')
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
if host_machine.cpu_family() == 'arm' or host_machine.cpu_family() == 'aarch64'
  if cc.compiles(
'''
#include <arm_neon.h>
int32x4_t testfunc(int16_t *a, int16_t *b) {
return vmull_s16(vld1_s16(a), vld1_s16(b));
}
''',
   name : 'NEON support')
    have_neon = true
  endif
endif


cdata = configuration_data()
cdata.set('PIPEWIRE_VERSION_MAJOR', pipewire_version_major)
cdata.set('PIPEWIRE_VERSION_MINOR', pipewire_version_minor)
cdata.set('PIPEWIRE_VERSION_MICRO', pipewire_version_micro)
cdata.set('PIPEWIRE_VERSION_NANO', pipewire_version_nano)
cdata.set('PIPEWIRE_API_VERSION', '"@0@"'.format(apiversion))
cdata.set('PIPEWIRE_DATADIR', '"@0@"'.format(pipewire_datadir))
cdata.set('LOCALEDIR', '"@0@"'.format(pipewire_localedir))
cdata.set('LIBDIR', '"@0@"'.format(pipewire_libdir))
cdata.set('GETTEXT_PACKAGE', '"pipewire"')
cdata.set('PIPEWIRE_LICENSE', '"MIT"')
cdata.set('PIPEWIRE_PACKAGE_ORIGIN', '"Unknown package origin"')
cdata.set('PIPEWIRE_PACKAGE_NAME', '"PipeWire source release"')
cdata.set('PACKAGE', '"pipewire"')
cdata.set('PACKAGE_NAME', '"PipeWire"')
cdata.set('PACKAGE_STRING', '"PipeWire @0@"'.format(pipewire_version))
cdata.set('PACKAGE_TARNAME', '"pipewire"')
cdata.set('PACKAGE_URL', '"http://pipewire.org"')
cdata.set('PACKAGE_VERSION', '"@0@"'.format(pipewire_version))
cdata.set('MODULEDIR', '"@0@"'.format(modules_install_dir))
cdata.set('PIPEWIRE_CONFIG_DIR', '"@0@"'.format(pipewire_configdir))
cdata.set('VERSION', '"@0@"'.format(pipewire_version))
cdata.set('PLUGINDIR', '"@0@"'.format(spa_plugindir))
# FIXME: --with-memory-alignment],[8,N,malloc,pagesize (default is 32)]) option
cdata.set('MEMORY_ALIGNMENT_MALLOC', 1)


check_headers = [['dlfcn.h','HAVE_DLFCN_H'],
  ['inttypes.h', 'HAVE_INTTYPES_H'],
  ['memory.h', 'HAVE_MEMORY_H'],
  ['poll.h', 'HAVE_POLL_H'],
  ['stddef.h', 'HAVE_STDDEF_H'],
  ['stdint.h', 'HAVE_STDINT_H'],
  ['stdio_ext.h', 'HAVE_STDIO_EXT_H'],
  ['strings.h', 'HAVE_STRINGS_H'],
  ['string.h', 'HAVE_STRING_H'],
  ['sys/param.h', 'HAVE_SYS_PARAM_H'],
  ['sys/poll.h', 'HAVE_SYS_POLL_H'],
  ['sys/prctl.h', 'HAVE_SYS_PRCTL_H'],
  ['sys/socket.h', 'HAVE_SYS_SOCKET_H'],
  ['sys/stat.h', 'HAVE_SYS_STAT_H'],
  ['sys/times.h', 'HAVE_SYS_TIMES_H'],
  ['sys/time.h', 'HAVE_SYS_TIME_H'],
  ['sys/types.h', 'HAVE_SYS_TYPES_H'],
  ['sys/utsname.h', 'HAVE_SYS_UTSNAME_H'],
  ['sys/wait.h', 'HAVE_SYS_WAIT_H'],
  ['ucontext.h', 'HAVE_UCONTEXT_H'],
  ['unistd.h', 'HAVE_UNISTD_H'],
  ['valgrind/valgrind.h', 'HAVE_VALGRIND_VALGRIND_H'],
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

if cc.has_function('mkstemp', prefix : '#include <stdlib.h>')
  cdata.set('HAVE_MKSTEMP', 1)
endif

if cc.has_function('memfd_create', prefix : '#include <sys/mman.h>', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_MEMFD_CREATE', 1)
endif

if get_option('systemd')
  systemd = dependency('systemd', required: false)
  systemd_dep = dependency('libsystemd', required: false)
  if systemd.found() and systemd_dep.found()
    cdata.set('HAVE_SYSTEMD_DAEMON', 1)
  else
    warning('Systemd integration was enabled, but systemd is not available')
  endif
endif

configure_file(input : 'config.h.meson',
  output : 'config.h',
  configuration : cdata)

configinc = include_directories('.')
pipewire_inc = include_directories('src')

makedata = configuration_data()
  makedata.set('BUILD_ROOT', meson.build_root())
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
sdl_dep = dependency('sdl2', required : false)
sndfile_dep = dependency('sndfile', version : '>= 1.0.20', required : false)

if get_option('gstreamer') or get_option('pipewire-pulseaudio')
  glib_dep = dependency('glib-2.0', version : '>=2.32.0')
endif

if get_option('gstreamer')
  gobject_dep = dependency('gobject-2.0')
  gmodule_dep = dependency('gmodule-2.0')
  gio_dep = [dependency('gio-2.0'), dependency('gio-unix-2.0')]
  gst_dep = [dependency('gstreamer-1.0'),
             dependency('gstreamer-plugins-base-1.0'),
             dependency('gstreamer-video-1.0'),
             dependency('gstreamer-audio-1.0'),
             dependency('gstreamer-allocators-1.0'),]
endif

# On FreeBSD, epoll-shim library is required for eventfd() and timerfd()
epoll_shim_dep = (build_machine.system() == 'freebsd'
    ? dependency('epoll-shim', required: true)
    : dependency('', required: false))

alsa_dep = (get_option('pipewire-alsa')
    ? dependency('alsa')
    : dependency('', required: false))

subdir('spa')
subdir('src')

if get_option('pipewire-jack')
  jack_dep = dependency('jack', version : '>= 1.9.10')
  subdir('pipewire-jack/src')
endif

if get_option('pipewire-pulseaudio')
  pulseaudio_dep = dependency('libpulse', version : '>= 11.1')
  subdir('pipewire-pulseaudio/src')
endif

if get_option('pipewire-alsa')
  subdir('pipewire-alsa/alsa-plugins')
endif

if get_option('docs')
  doxygen = find_program('doxygen', required : false)
  if doxygen.found()
    subdir('doc')
  else
    warning('Documentation was enabled, but doxygen is not available')
  endif
endif

if get_option('man')
  xmltoman = find_program('xmltoman', required : false)
  if xmltoman.found()
    subdir('man')
  else
    warning('Man page generation was enabled, but xmltoman is not available')
  endif
endif

setenv = find_program('pw-uninstalled.sh')
run_target('uninstalled', command : [setenv,
    '-b@0@'.format(meson.build_root()), '-v@0@'.format(pipewire_version)])
