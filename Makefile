CC ?= gcc
CFLAGS ?= -O2 -march=native -Wall -Wextra -Wno-missing-field-initializers -g
LDLIBS = -lm -lpthread
GLSLC ?= $(if $(wildcard ../deps/bin/glslc),../deps/bin/glslc,glslc)
GFLAGS = -O --target-env=vulkan1.1 -mfmt=c

S = src/shaders
O = build/eval/shaders
SH = $(O)/conf.h $(O)/down.h $(O)/norm.h $(O)/me_full.h $(O)/me_iter.h $(O)/me_refine.h $(O)/me_cross.h $(O)/me_local.h $(O)/tproj.h $(O)/tsel.h $(O)/illum.h $(O)/ilmed.h $(O)/cutdec.h \
     $(O)/interp8.h $(O)/interp16.h $(O)/interp8nv.h $(O)/interp16nv.h

all: eval_vk

shaders: $(SH)

$(O)/%.h: $(S)/%.comp $(S)/common.glsl $(S)/sad.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -o $@ $<
$(O)/interp8.h: $(S)/interp.comp $(S)/common.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -o $@ $<
$(O)/interp16.h: $(S)/interp.comp $(S)/common.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -DDEPTH16 -o $@ $<
$(O)/interp8nv.h: $(S)/interp.comp $(S)/common.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -DNV -o $@ $<
$(O)/interp16nv.h: $(S)/interp.comp $(S)/common.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -DNV -DDEPTH16 -o $@ $<

eval_vk: tools/eval.c src/memc_vk.c src/memc.h $(SH)
	$(CC) $(CFLAGS) -Isrc -Ibuild/eval -o $@ tools/eval.c src/memc_vk.c $(LDLIBS) -lvulkan

clean:
	rm -rf eval_vk build/eval
