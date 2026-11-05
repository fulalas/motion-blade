framekit_inc = include_directories('.')
framekit = static_library('framekit', 'pixel_formats.c', 'y4m.c', include_directories: [public_inc, framekit_inc],
  dependencies: libm)
framekit_dep = declare_dependency(link_with: framekit, include_directories: framekit_inc, dependencies: libm)
