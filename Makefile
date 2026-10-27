CC ?= gcc
CFLAGS ?= -O2 -march=native -Wall -Wextra -Wno-missing-field-initializers -g
LDLIBS = -lm
GLSLC ?= $(if $(wildcard ../deps/bin/glslc),../deps/bin/glslc,glslc)
GFLAGS = -O --target-env=vulkan1.1 -mfmt=c

S = src/shaders
O = build/eval/shaders
PLAIN = $(filter-out interp,$(basename $(notdir $(wildcard $(S)/*.comp))))
INTERP = $(O)/interp8.h $(O)/interp16.h $(O)/interp8nv.h $(O)/interp16nv.h
SH = $(PLAIN:%=$(O)/%.h) $(INTERP)

all: eval_vk

shaders: $(SH)

$(O)/%.h: $(S)/%.comp $(S)/common.glsl $(S)/sad.glsl
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -o $@ $<
$(O)/interp8nv.h $(O)/interp16nv.h: GDEFS += -DNV
$(O)/interp16.h $(O)/interp16nv.h: GDEFS += -DDEPTH16
$(INTERP): $(S)/interp.comp $(S)/common.glsl
	@mkdir -p $(O)
	$(GLSLC) $(strip $(GFLAGS) $(GDEFS)) -o $@ $<

eval_vk: tools/eval.c src/motionblade_vk.c src/motionblade.h $(SH)
	$(CC) $(CFLAGS) -Isrc -Ibuild/eval -o $@ tools/eval.c src/motionblade_vk.c $(LDLIBS) -lvulkan

clean:
	rm -rf eval_vk build/eval
