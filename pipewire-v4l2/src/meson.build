pipewire_v4l2_sources = [
  'pipewire-v4l2.c',
  'v4l2-func.c',
]

pipewire_v4l2_c_args = [
  # Meson enables large file support unconditionally, which redirect file
  # operations to 64-bit versions. This results in some symbols being
  # renamed, for instance open() being renamed to open64(). As the V4L2
  # adaptation wrapper needs to provide both 32-bit and 64-bit versions of
  # file operations, disable transparent large file support.
  '-U_FILE_OFFSET_BITS',
  '-D_FILE_OFFSET_BITS=32',
  '-D_LARGEFILE64_SOURCE',
  '-fvisibility=hidden',
]

libv4l2_path = get_option('libv4l2-path')
if libv4l2_path == ''
  libv4l2_path = modules_install_dir / 'v4l2'
  libv4l2_path_dlopen = modules_install_dir_dlopen / 'v4l2'
else
  libv4l2_path_dlopen = libv4l2_path
endif

tools_config = configuration_data()
tools_config.set('LIBV4L2_PATH', libv4l2_path_dlopen)

configure_file(input : 'pw-v4l2.in',
               output : 'pw-v4l2',
               configuration : tools_config,
               install_dir : pipewire_bindir)

pipewire_v4l2 = shared_library('pw-v4l2',
    pipewire_v4l2_sources,
    c_args : pipewire_v4l2_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, mathlib, dl_lib],
    install : true,
    install_dir : libv4l2_path,
)
