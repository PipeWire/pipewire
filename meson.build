project('pipewire', 'c',
  version : '0.1.9',
  meson_version : '>= 0.36.0',
  default_options : [ 'warning_level=1',
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

apiversion = '0.1'
soversion = 0
libversion = '@0@.@1@.0'.format(soversion, pipewire_version_minor.to_int() * 100 + pipewire_version_micro.to_int())

prefix = get_option('prefix')
pipewire_bindir = join_paths(prefix, get_option('bindir'))
pipewire_datadir = join_paths(prefix, get_option('datadir'))
pipewire_libdir = join_paths(prefix, get_option('libdir'))
pipewire_localedir = join_paths(prefix, get_option('localedir'))
pipewire_sysconfdir = join_paths(prefix, get_option('sysconfdir'))

modules_install_dir = join_paths(get_option('libdir'), 'pipewire-@0@'.format(apiversion))

gnome = import('gnome')

cc = meson.get_compiler('c')


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
cdata.set('PIPEWIRE_LICENSE', '"LGPL"')
cdata.set('PIPEWIRE_PACKAGE_ORIGIN', '"Unknown package origin"')
cdata.set('PIPEWIRE_PACKAGE_NAME', '"PipeWire source release"')
cdata.set('PACKAGE', '"pipewire"')
cdata.set('PACKAGE_NAME', '"PipeWire"')
cdata.set('PACKAGE_STRING', '"PipeWire @0@"'.format(pipewire_version))
cdata.set('PACKAGE_TARNAME', '"pipewire"')
cdata.set('PACKAGE_URL', '"http://pipewire.org"')
cdata.set('PACKAGE_VERSION', '"@0@"'.format(pipewire_version))
cdata.set('MODULEDIR', '"@0@/pipewire-@1@"'.format(pipewire_libdir,apiversion))
cdata.set('PIPEWIRE_CONFIG_DIR', '"@0@/pipewire"'.format(pipewire_sysconfdir))
cdata.set('VERSION', '"@0@"'.format(pipewire_version))
cdata.set('PLUGINDIR', '"@0@/spa"'.format(pipewire_libdir))
# FIXME: --with-memory-alignment],[8,N,malloc,pagesize (default is 32)]) option
cdata.set('MEMORY_ALIGNMENT_MALLOC', 1)


check_headers = [['dlfcn.h','HAVE_DLFCN_H'],
  ['inttypes.h', 'HAVE_INTTYPES_H'],
  ['memory.h', 'HAVE_MEMORY_H'],
  ['poll.h', 'HAVE_POLL_H'],
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

if cc.has_type('ptrdiff_t')
  cdata.set('HAVE_PTRDIFF_T')
endif

if cc.has_function('mkstemp', prefix : '#include <stdlib.h>')
  cdata.set('HAVE_MKSTEMP', 1)
endif

if cc.has_function('memfd_create', prefix : '#include <sys/mman.h>', args : [ '-D_GNU_SOURCE' ])
  cdata.set('HAVE_MEMFD_CREATE', 1)
endif

configure_file(input : 'config.h.meson',
  output : 'config.h',
  configuration : cdata)

configinc = include_directories('.')
pipewire_inc = include_directories('src')

makedata = configuration_data()
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

#optional dependencies
jack_dep = dependency('jack', version : '>= 1.9.10', required : false)

if get_option('enable_gstreamer')
  glib_dep = dependency('glib-2.0', version : '>=2.32.0')
  gobject_dep = dependency('gobject-2.0')
  gmodule_dep = dependency('gmodule-2.0')
  gio_dep = [dependency('gio-2.0'), dependency('gio-unix-2.0')]
  gst_dep = [dependency('gstreamer-1.0'),
             dependency('gstreamer-plugins-base-1.0'),
             dependency('gstreamer-video-1.0'),
             dependency('gstreamer-audio-1.0'),
             dependency('gstreamer-allocators-1.0'),]
endif

subdir('spa')
subdir('src')
subdir('pkgconfig')
subdir('pipewire-jack')
subdir('alsa-plugins')

if get_option('enable_docs')
  doxygen = find_program('doxygen', required : false)
  if doxygen.found()
    subdir('doc')
  else
    warning('Documentation was enabled, but doxygen is not available')
  endif
endif

if get_option('enable_man')
  xmltoman = find_program('xmltoman', required : false)
  if xmltoman.found()
    subdir('man')
  else
    warning('Man page generation was enabled, but xmltoman is not available')
  endif
endif
