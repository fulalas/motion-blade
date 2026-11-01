support = static_library('testsupport', 'support/check.c', 'support/frames.c',
  include_directories: [public_inc, include_directories('support')],
  dependencies: libm)
support_dep = declare_dependency(link_with: support, include_directories: include_directories('support'))

foreach name : ['test_engine', 'test_params', 'test_schedule']
  test(name, executable(name, 'unit' / name + '.c', dependencies: [core_dep, support_dep]),
    suite: 'unit', timeout: 120)
endforeach

foreach name : ['test_bars', 'test_texture']
  test(name, executable(name, 'unit' / name + '.c', dependencies: [core_private_dep, support_dep]),
    suite: 'unit')
endforeach

tee = static_library('tee', 'support/tee_backend.c', 'support/tee_passes.c',
  include_directories: [public_inc, include_directories('support')])
test('test_backends', executable('test_backends', 'conformance/test_backends.c',
    link_with: tee, dependencies: [motionblade_dep, support_dep]),
  suite: 'gpu', timeout: 600)

test('check_layers', find_program('check_layers.sh'), suite: 'unit')
