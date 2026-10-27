glslc = find_program('glslc', native: true)
mb_glslc = [glslc, '-O', '--target-env=vulkan1.1', '-mfmt=c', '-MD', '-MF', '@DEPFILE@', '-o', '@OUTPUT@', '@INPUT@']
mb_shaders = []
foreach s : import('fs').read('shaders.txt').strip().split(' ')
    mb_shaders += custom_target(s + '.h', input: s + '.comp', output: s + '.h', depfile: s + '.h.d',
                                  command: mb_glslc)
endforeach
foreach v : [['interp8', []], ['interp16', ['-DDEPTH16']], ['interp8nv', ['-DNV']], ['interp16nv', ['-DNV', '-DDEPTH16']]]
    mb_shaders += custom_target(v[0] + '.h', input: 'interp.comp', output: v[0] + '.h', depfile: v[0] + '.h.d',
                                  command: mb_glslc + v[1])
endforeach
