glslc = find_program('glslc', native: true)
memc_glslc = [glslc, '-O', '--target-env=vulkan1.1', '-mfmt=c', '-MD', '-MF', '@DEPFILE@', '-o', '@OUTPUT@', '@INPUT@']
memc_shaders = []
foreach s : ['conf', 'down', 'norm', 'me_full', 'me_iter', 'me_refine', 'tproj', 'tsel', 'illum', 'ilmed', 'cutdec']
    memc_shaders += custom_target(s + '.h', input: s + '.comp', output: s + '.h', depfile: s + '.h.d',
                                  command: memc_glslc)
endforeach
foreach v : [['interp8', []], ['interp16', ['-DDEPTH16']], ['interp8nv', ['-DNV']], ['interp16nv', ['-DNV', '-DDEPTH16']]]
    memc_shaders += custom_target(v[0] + '.h', input: 'interp.comp', output: v[0] + '.h', depfile: v[0] + '.h.d',
                                  command: memc_glslc + v[1])
endforeach
