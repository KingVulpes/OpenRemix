xz_include = include_directories('linux/include/linux', 'userspace')
xz_embedded = static_library('xz_embedded',
  ['linux/lib/xz/xz_crc32.c', 'linux/lib/xz/xz_dec_lzma2.c', 'linux/lib/xz/xz_dec_stream.c'],
  include_directories : xz_include,
  gnu_symbol_visibility : 'hidden', pic : true, override_options : ['warning_level=0', 'werror=false'])
xz_dep = declare_dependency(link_with : xz_embedded, include_directories : xz_include)
install_data('COPYING', rename : 'XZEmbedded.txt', install_dir : get_option('datadir') / 'remix/licenses')
