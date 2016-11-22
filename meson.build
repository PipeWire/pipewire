project('pinos', 'c',
  version : '0.0.1.1',
  meson_version : '>= 0.33.0',
  default_options : [ 'warning_level=1',
                      'c_std=gnu99',
                      'buildtype=debugoptimized' ])


pinos_version = meson.project_version()
version_arr = pinos_version.split('.')
pinos_version_major = version_arr[0]
pinos_version_minor = version_arr[1]
pinos_version_micro = version_arr[2]
if version_arr.length() == 4
  pinos_version_nano = version_arr[3]
else
  pinos_version_nano = 0
endif

apiversion = '0.1'
soversion = 0
libversion = '@0@.@1@.0'.format(soversion, pinos_version_minor.to_int() * 100 + pinos_version_micro.to_int())

prefix = get_option('prefix')

gnome = import('gnome')

cc = meson.get_compiler('c')


cdata = configuration_data()
cdata.set('PINOS_VERSION_MAJOR', pinos_version_major)
cdata.set('PINOS_VERSION_MINOR', pinos_version_minor)
cdata.set('PINOS_VERSION_MICRO', pinos_version_micro)
cdata.set('PINOS_VERSION_NANO', pinos_version_nano)
cdata.set('PINOS_API_VERSION', '"@0@"'.format(apiversion))
cdata.set('PINOS_DATADIR', '"@0@/@1@"'.format(prefix, get_option('datadir')))
cdata.set('LOCALEDIR', '"@0@/@1@"'.format(prefix, get_option('localedir')))
cdata.set('LIBDIR', '"@0@/@1@"'.format(prefix, get_option('libdir')))
cdata.set('GETTEXT_PACKAGE', '"pinos"')
cdata.set('PINOS_LICENSE', '"LGPL"')
cdata.set('PINOS_PACKAGE_ORIGIN', '"Unknown package origin"')
cdata.set('PINOS_PACKAGE_NAME', '"Pinos source release"')
cdata.set('PACKAGE', '"pinos"')
cdata.set('PACKAGE_NAME', '"Pinos"')
cdata.set('PACKAGE_STRING', '"Pinos @0@"'.format(pinos_version))
cdata.set('PACKAGE_TARNAME', '"pinos"')
cdata.set('PACKAGE_URL', '""')
cdata.set('PACKAGE_VERSION', '"@0@"'.format(pinos_version))
cdata.set('MODULEDIR', '"@0@/@1@/pinos-@2@"'.format(get_option('prefix'),get_option('libdir'),apiversion))
cdata.set('PINOS_CONFIG_DIR', '"@0@/etc/pinos"'.format(get_option('prefix')))
cdata.set('VERSION', '"@0@"'.format(pinos_version))
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

configure_file(input : 'config.h.meson',
  output : 'config.h',
  configuration : cdata)

configinc = include_directories('.')

# Find dependencies
glib_dep = dependency('glib-2.0', version : '>=2.32.0')
gobject_dep = dependency('gobject-2.0')
gmodule_dep = dependency('gmodule-2.0')
gio_dep = [dependency('gio-2.0'), dependency('gio-unix-2.0')]
mathlib = cc.find_library('m', required : false)
rt_lib = cc.find_library('rt', required : false) # clock_gettime
dl_lib = cc.find_library('dl', required : false)
pthread_lib = cc.find_library('pthread', required : true)
dbus_dep = dependency('dbus-1')
gst_dep = [dependency('gstreamer-1.0'),
           dependency('gstreamer-plugins-base-1.0'),
           dependency('gstreamer-video-1.0'),
           dependency('gstreamer-audio-1.0'),
           dependency('gstreamer-allocators-1.0'),]

subdir('spa')
subdir('pinos')
subdir('pkgconfig')

#gtkdoc = find_program('gtkdoc-scan', required : false)
#if gtkdoc.found()
#  subdir('docs')
#else
#  message('Not building documentation as gtk-doc was not found')
#endif
