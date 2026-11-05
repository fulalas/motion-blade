vulkan_dep = dependency('vulkan')

glslc = find_program('glslc', native: true)
glslc_command = [glslc, '-O', '--target-env=vulkan1.1', '-mfmt=c', '-I', meson.project_source_root() / 'core' / 'include',
  '-MD', '-MF', '@DEPFILE@', '-o', '@OUTPUT@', '@INPUT@']

shader_names = ['bar_flags', 'chroma_offsets', 'cross_check', 'decide_cut', 'downsample', 'fallback', 'full_search',
  'global_motion', 'iterate_search', 'local_search', 'measure', 'median', 'normalize', 'offset_median', 'panel_spread',
  'panels', 'project', 'select', 'split']

shader_headers = []
foreach name : shader_names
  shader_headers += custom_target(name + '.spv.h', input: 'shaders' / name + '.comp', output: name + '.spv.h',
    depfile: name + '.spv.h.d', command: glslc_command)
endforeach
foreach name : ['interpolate', 'extrapolate']
  foreach variant : [['8', []], ['16', ['-DDEPTH16']], ['8nv', ['-DNV']], ['16nv', ['-DNV', '-DDEPTH16']]]
    shader_headers += custom_target(name + variant[0] + '.spv.h', input: 'shaders' / name + '.comp',
      output: name + variant[0] + '.spv.h', depfile: name + variant[0] + '.spv.h.d',
      command: glslc_command + variant[1])
  endforeach
endforeach

vulkan_sources = files(
  'src/backend.c',
  'src/device.c',
  'src/device_frames.c',
  'src/export.c',
  'src/jobs.c',
  'src/memory.c',
  'src/passes.c',
  'src/pipeline_cache.c',
  'src/pipelines.c',
  'src/transfers.c',
)

mbvulkan = [static_library('mbvulkan', vulkan_sources, shader_headers,
  include_directories: [public_inc, include_directories('src', 'shaders')],
  dependencies: vulkan_dep)]
