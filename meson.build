project('motionblade', 'c',
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: ['c_std=c11', 'warning_level=2', 'buildtype=release', 'default_library=static'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wshadow', '-Wstrict-prototypes', '-Wno-missing-field-initializers']),
  language: 'c')
if host_machine.system() != 'windows'
  add_project_arguments('-D_GNU_SOURCE', language: 'c')
endif

libm = cc.find_library('m', required: false)
public_inc = include_directories('core/include', 'vulkan/include')

subdir('core')
subdir('vulkan')

motionblade_dep = declare_dependency(
  link_with: [mbcore] + mbvulkan,
  include_directories: public_inc,
  dependencies: [libm, vulkan_dep])

if not meson.is_subproject()
  subdir('tools')
  subdir('tests')
endif
