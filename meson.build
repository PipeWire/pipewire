project('pipewire', ['c' ],
  version : '1.0.5',
  license : [ 'MIT', 'LGPL-2.1-or-later', 'GPL-2.0-only' ],
  meson_version : '>= 0.61.1',
  default_options : [ 'warning_level=3',
                      'c_std=gnu11',
                      'cpp_std=c++17',
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
libversion_minor = pipewire_version_major.to_int() * 1000 + pipewire_version_minor.to_int() * 100 + pipewire_version_micro.to_int()
libversion = '@0@.@1@.0'.format(soversion, libversion_minor)

# LADI/jack
# 3, for PipeWire being the third JACK implementation, after JACK1 and jackdmp/JACK2)
jack_version_major = 3
jack_version_minor = libversion_minor
# libjack[server] version has 0 for major (for compatibility with other implementations),
# 3 for minor, and "1000*major + 100*minor + micro" as micro version (the minor libpipewire soversion number)
libjackversion = '@0@.@1@.@2@'.format(soversion, jack_version_major, jack_version_minor)

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

cc = meson.get_compiler('c')

if cc.has_header('features.h') and cc.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
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
spa_datadir = pipewire_datadir / spa_name

alsadatadir = pipewire_datadir / 'alsa-card-profile' / 'mixer'

pipewire_headers_dir = pipewire_name / 'pipewire'

pkgconfig = import('pkgconfig')

common_flags = [
  '-fvisibility=hidden',
  '-fno-strict-aliasing',
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
  '-Wmaybe-uninitialized',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-pedantic',
  '-Wdeprecated-declarations',
  '-Wunused-result',
  '-Werror=return-type',
]

cc_flags = common_flags + [
  '-D_GNU_SOURCE',
  '-DFASTPATH',
# '-DSPA_DEBUG_MEMCPY',
  '-Werror=implicit-function-declaration',
  '-Werror=int-conversion',
  '-Werror=old-style-declaration',
  '-Werror=old-style-definition',
  '-Werror=missing-parameter-type',
  '-Werror=strict-prototypes',
]
add_project_arguments(cc.get_supported_arguments(cc_flags), language: 'c')

have_cpp = add_languages('cpp', native: false, required : false)

if have_cpp
  cxx = meson.get_compiler('cpp')
  cxx_flags = common_flags + [ '-Wno-c99-designator' ]
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
cdata.set_quoted('PREFIX', prefix)
cdata.set_quoted('PIPEWIRE_CONFDATADIR', pipewire_confdatadir)
cdata.set_quoted('LOCALEDIR', pipewire_localedir)
cdata.set_quoted('LIBDIR', pipewire_libdir)
cdata.set_quoted('GETTEXT_PACKAGE', meson.project_name())
cdata.set_quoted('PACKAGE', 'pipewire')
cdata.set_quoted('PACKAGE_NAME', 'PipeWire')
cdata.set_quoted('PACKAGE_STRING', 'PipeWire @0@'.format(pipewire_version))
cdata.set_quoted('PACKAGE_TARNAME', 'pipewire')
cdata.set_quoted('PACKAGE_URL', 'https://pipewire.org')
cdata.set_quoted('PACKAGE_VERSION', pipewire_version)
cdata.set_quoted('MODULEDIR', modules_install_dir)
cdata.set_quoted('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
cdata.set_quoted('PLUGINDIR', spa_plugindir)
cdata.set_quoted('SPADATADIR', spa_datadir)
cdata.set_quoted('PA_ALSA_DATA_DIR', alsadatadir)
cdata.set('RTPRIO_SERVER', get_option('rtprio-server'))
cdata.set('RTPRIO_CLIENT', get_option('rtprio-client'))

if host_machine.endian() == 'big'
  cdata.set('WORDS_BIGENDIAN', 1)
endif

check_headers = [
  ['sys/mount.h', 'HAVE_SYS_MOUNT_H'],
  ['sys/param.h', 'HAVE_SYS_PARAM_H'],
  ['sys/random.h', 'HAVE_SYS_RANDOM_H'],
  ['sys/vfs.h', 'HAVE_SYS_VFS_H'],
  ['pwd.h', 'HAVE_PWD_H'],
  ['grp.h', 'HAVE_GRP_H'],
]

foreach h : check_headers
  cdata.set(h.get(1), cc.has_header(h.get(0)))
endforeach

cdata.set('HAVE_PIDFD_OPEN',
          cc.get_define('SYS_pidfd_open', prefix: '#include <sys/syscall.h>') != '')

systemd = dependency('systemd', required: get_option('systemd'))
systemd_dep = dependency('libsystemd',required: get_option('systemd'))
summary({'systemd conf data': systemd.found()}, bool_yn: true)
summary({'libsystemd': systemd_dep.found()}, bool_yn: true)
cdata.set('HAVE_SYSTEMD', systemd.found() and systemd_dep.found())

selinux_dep = dependency('libselinux', required: get_option('selinux'))
summary({'libselinux': selinux_dep.found()}, bool_yn: true)
cdata.set('HAVE_SELINUX', selinux_dep.found())

configinc = include_directories('.')
includes_inc = include_directories('include')
pipewire_inc = include_directories('src')

makedata = configuration_data()
makedata.set('BUILD_ROOT', meson.project_build_root())
makedata.set('SOURCE_ROOT', meson.project_source_root())
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
dbus_dep = dependency('dbus-1', required : get_option('dbus'))
summary({'dbus (Bluetooth, rt, portal, pw-reserve)': dbus_dep.found()}, bool_yn: true, section: 'Misc dependencies')
cdata.set('HAVE_DBUS', dbus_dep.found())
sdl_dep = dependency('sdl2', required : get_option('sdl2'))
summary({'SDL2 (video examples)': sdl_dep.found()}, bool_yn: true, section: 'Misc dependencies')
drm_dep = dependency('libdrm', required : false)
readline_dep = dependency('readline', required : get_option('readline'))

if not readline_dep.found()
  readline_dep = cc.find_library('readline', required : get_option('readline'))
endif

# Both the FFmpeg SPA plugin and the pw-cat FFmpeg integration use libavcodec.
# But only the latter also needs libavformat and libavutil.
# Search for these libraries here, globally, so both of these subprojects can reuse the results.
pw_cat_ffmpeg = get_option('pw-cat-ffmpeg')
ffmpeg = get_option('ffmpeg')
if pw_cat_ffmpeg.allowed() or ffmpeg.allowed()
  avcodec_dep = dependency('libavcodec', required: pw_cat_ffmpeg.enabled() or ffmpeg.enabled())
  avformat_dep = dependency('libavformat', required: pw_cat_ffmpeg.enabled())
  avutil_dep = dependency('libavutil', required: pw_cat_ffmpeg.enabled())
else
  avcodec_dep = dependency('', required: false)
endif
cdata.set('HAVE_PW_CAT_FFMPEG_INTEGRATION', pw_cat_ffmpeg.allowed())

opus_dep = dependency('opus', required : get_option('opus'))
summary({'opus (Bluetooth, RTP)': opus_dep.found()}, bool_yn: true, section: 'Misc dependencies')
cdata.set('HAVE_OPUS', opus_dep.found())

summary({'readline (for pw-cli)': readline_dep.found()}, bool_yn: true, section: 'Misc dependencies')
cdata.set('HAVE_READLINE', readline_dep.found())
ncurses_dep = dependency('ncursesw', required : false)
sndfile_dep = dependency('sndfile', version : '>= 1.0.20', required : get_option('sndfile'))
summary({'sndfile': sndfile_dep.found()}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump/filter-chain')
cdata.set('HAVE_SNDFILE', sndfile_dep.found())
libmysofa_dep = dependency('libmysofa', required : get_option('libmysofa'))
summary({'libmysofa': libmysofa_dep.found()}, bool_yn: true, section: 'filter-chain')
pulseaudio_dep = dependency('libpulse', required : get_option('libpulse'))
summary({'libpulse': pulseaudio_dep.found()}, bool_yn: true, section: 'Streaming between daemons')
avahi_dep = dependency('avahi-client', required : get_option('avahi'))
summary({'Avahi DNS-SD (Zeroconf)': avahi_dep.found()}, bool_yn: true,
  section: 'Streaming between daemons')

x11_dep = dependency('x11-xcb', required : get_option('x11'))
summary({'X11 (x11-bell)': x11_dep.found()}, bool_yn: true,
  section: 'Misc dependencies')

xfixes_dep = dependency('xfixes', required : get_option('x11-xfixes'), version: '>= 6')
cdata.set('HAVE_XFIXES_6', xfixes_dep.found())

canberra_dep = dependency('libcanberra', required : get_option('libcanberra'))
summary({'libcanberra (x11-bell)': canberra_dep.found()}, bool_yn: true,
  section: 'Misc dependencies')

libusb_dep = dependency('libusb-1.0', required : get_option('libusb'))
summary({'libusb (Bluetooth quirks)': libusb_dep.found()}, bool_yn: true, section: 'Backend')
cdata.set('HAVE_LIBUSB', libusb_dep.found())

cap_lib = dependency('libcap', required : false)
cdata.set('HAVE_LIBCAP', cap_lib.found())

glib2_dep = dependency('glib-2.0', required : get_option('flatpak'))
summary({'GLib-2.0 (Flatpak support)': glib2_dep.found()}, bool_yn: true, section: 'Misc dependencies')
flatpak_support = glib2_dep.found()
cdata.set('HAVE_GLIB2', flatpak_support)

gio_dep = dependency('gio-2.0', version : '>= 2.26.0', required : get_option('gsettings'))
summary({'GIO (GSettings)': gio_dep.found()}, bool_yn: true, section: 'Misc dependencies')

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

cdata.set('HAVE_GSTREAMER_DEVICE_PROVIDER', get_option('gstreamer-device-provider').allowed())

webrtc_dep = dependency('webrtc-audio-processing-1',
  version : ['>= 1.2' ],
  required : false)
cdata.set('HAVE_WEBRTC1', webrtc_dep.found())
if webrtc_dep.found()
  summary({'WebRTC Echo Canceling >= 1.2': webrtc_dep.found()}, bool_yn: true, section: 'Misc dependencies')
else
  webrtc_dep = dependency('webrtc-audio-processing',
    version : ['>= 0.2', '< 1.0'],
    required : get_option('echo-cancel-webrtc'))
  cdata.set('HAVE_WEBRTC', webrtc_dep.found())
  summary({'WebRTC Echo Canceling < 1.0': webrtc_dep.found()}, bool_yn: true, section: 'Misc dependencies')
endif

# On FreeBSD and MidnightBSD, epoll-shim library is required for eventfd() and timerfd()
epoll_shim_dep = (host_machine.system() == 'freebsd' or host_machine.system() == 'midnightbsd'
    ? dependency('epoll-shim', required: true)
    : dependency('', required: false))

libinotify_dep = (host_machine.system() == 'freebsd' or host_machine.system() == 'midnightbsd'
    ? dependency('libinotify', required: true)
    : dependency('', required: false))

# On FreeBSD and MidnightBSD, libintl library is required for gettext
libintl_dep = cc.find_library('intl', required: false)
if not libintl_dep.found()
    libintl_dep = dependency('intl', required: false)
endif
summary({'intl support': libintl_dep.found()}, bool_yn: true)

need_alsa = get_option('pipewire-alsa').enabled() or 'media-session' in get_option('session-managers')
alsa_dep = dependency('alsa', version : '>=1.1.7', required: need_alsa)
summary({'pipewire-alsa': alsa_dep.found()}, bool_yn: true)

if host_machine.system() == 'freebsd' or host_machine.system() == 'midnightbsd'
# On FreeBSD and MidnightBSD the OpenSSL library may come from base or a package.
# Check for a package first and fallback to the base library if we can't find it via pkgconfig
    openssl_lib = dependency('openssl', required: false)
    if not openssl_lib.found()
        openssl_lib = declare_dependency(link_args : [ '-lssl', '-lcrypto'])
    endif
else
    openssl_lib = dependency('openssl', required: get_option('raop'))
endif
summary({'OpenSSL (for raop-sink)': openssl_lib.found()}, bool_yn: true)

lilv_lib = dependency('lilv-0', required: get_option('lv2'))
summary({'lilv (for lv2 plugins)': lilv_lib.found()}, bool_yn: true)

libffado_dep = dependency('libffado', required: get_option('libffado'))
summary({'ffado': libffado_dep.found()}, bool_yn: true)

check_functions = [
  ['gettid', '#include <unistd.h>', ['-D_GNU_SOURCE'], []],
  ['memfd_create', '#include <sys/mman.h>', ['-D_GNU_SOURCE'], []],
  ['getrandom', '#include <stddef.h>\n#include <sys/random.h>', ['-D_GNU_SOURCE'], []],
  ['random_r', '#include <stdlib.h>', ['-D_GNU_SOURCE'], []],
  ['reallocarray', '#include <stdlib.h>', ['-D_GNU_SOURCE'], []],
  ['sigabbrev_np', '#include <string.h>', ['-D_GNU_SOURCE'], []],
  ['XSetIOErrorExitHandler', '#include <X11/Xlib.h>', [], [x11_dep]],
  ['malloc_trim', '#include <malloc.h>', [], []],
  ['malloc_info', '#include <malloc.h>', [], []],
]

foreach f : check_functions
  cdata.set('HAVE_' + f.get(0).to_upper(),
            cc.has_function(f.get(0),
                            prefix: f.get(1),
                            args: f.get(2),
                            dependencies: f.get(3)))
endforeach

installed_tests_metadir = pipewire_datadir / 'installed-tests' / pipewire_name
installed_tests_execdir = pipewire_libexecdir / 'installed-tests' / pipewire_name
installed_tests_enabled = get_option('installed_tests').allowed()
installed_tests_template = files('template.test.in')

if get_option('tests').allowed()
  gstack = find_program('gstack', required : false)
  cdata.set('HAVE_GSTACK', gstack.found())
endif

subdir('po')
subdir('spa')
subdir('src')

if get_option('tests').allowed()
  subdir('test')
endif

configure_file(output : 'config.h',
               configuration : cdata)

if get_option('pipewire-jack').allowed()
  subdir('pipewire-jack')
endif
if get_option('pipewire-v4l2').allowed()
  subdir('pipewire-v4l2')
endif

if alsa_dep.found()
  subdir('pipewire-alsa/alsa-plugins')
  subdir('pipewire-alsa/conf')
  subdir('pipewire-alsa/tests')
endif

generate_docs = get_option('man').enabled() or get_option('docs').enabled()
if get_option('man').allowed() or get_option('docs').allowed()
  doxygen = find_program('doxygen', required : generate_docs)
  pymod = import('python')
  python = pymod.find_installation('python3', required: generate_docs)
  generate_docs = doxygen.found() and python.found()
endif

install_docs = get_option('docs').require(generate_docs).allowed()
install_man = get_option('man').require(generate_docs).allowed()

summary({'Documentation ': install_docs}, bool_yn: true)
summary({'Man pages ': install_man}, bool_yn: true)

if generate_docs
  subdir('doc')
endif

setenv = find_program('pw-uninstalled.sh')
run_target('pw-uninstalled',
  command : [setenv,
             '-b@0@'.format(meson.project_build_root()),
             '-v@0@'.format(pipewire_version)]
)

devenv = environment()

builddir = meson.project_build_root()
srcdir = meson.project_source_root()

devenv.set('PIPEWIRE_CONFIG_DIR', pipewire_dep.get_variable('confdatadir'))
devenv.set('PIPEWIRE_MODULE_DIR', pipewire_dep.get_variable('moduledir'))

devenv.set('SPA_PLUGIN_DIR', spa_dep.get_variable('plugindir'))
devenv.set('SPA_DATA_DIR', spa_dep.get_variable('datadir'))

devenv.set('GST_PLUGIN_PATH', builddir / 'src'/ 'gst')

devenv.set('ALSA_PLUGIN_DIR', builddir / 'pipewire-alsa' / 'alsa-plugins')
devenv.set('ACP_PATHS_DIR', srcdir / 'spa' / 'plugins' / 'alsa' / 'mixer' / 'paths')
devenv.set('ACP_PROFILES_DIR', srcdir / 'spa' / 'plugins' / 'alsa' / 'mixer' / 'profile-sets')

devenv.set('LD_LIBRARY_PATH', builddir / 'pipewire-jack' / 'src')

devenv.set('PW_UNINSTALLED', '1')

meson.add_devenv(devenv)
