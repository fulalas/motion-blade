CC ?= gcc
CFLAGS ?= -O2 -march=native -Wall -Wextra -Wno-missing-field-initializers -g
LDLIBS = -lm
GLSLC ?= $(if $(wildcard ../deps/bin/glslc),../deps/bin/glslc,glslc)
GFLAGS = -O --target-env=vulkan1.1 -mfmt=c

S = src/shaders
O = build/eval/shaders
PLAIN = $(shell cat $(S)/shaders.txt)
INTERP = $(O)/interp8.h $(O)/interp16.h $(O)/interp8nv.h $(O)/interp16nv.h
SH = $(PLAIN:%=$(O)/%.h) $(INTERP)

all: eval_vk

shaders: $(SH)

$(O)/%.h: $(S)/%.comp
	@mkdir -p $(O)
	$(GLSLC) $(GFLAGS) -MD -MF $@.d -o $@ $<
$(O)/interp8nv.h $(O)/interp16nv.h: GDEFS += -DNV
$(O)/interp16.h $(O)/interp16nv.h: GDEFS += -DDEPTH16
$(INTERP): $(S)/interp.comp
	@mkdir -p $(O)
	$(GLSLC) $(strip $(GFLAGS) $(GDEFS)) -MD -MF $@.d -o $@ $<
-include $(wildcard $(O)/*.d)

eval_vk: tools/eval.c src/motionblade_vk.c src/motionblade.h $(SH)
	$(CC) $(CFLAGS) -Isrc -Ibuild/eval -o $@ tools/eval.c src/motionblade_vk.c $(LDLIBS) -lvulkan

build/flick: tools/flick.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf eval_vk build/eval build/flick
