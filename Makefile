BUILD = build/local
DEPS = $(abspath ../deps/bin)
export PATH := $(if $(wildcard $(DEPS)),$(DEPS):)$(PATH)

all: $(BUILD)/build.ninja
	ninja -C $(BUILD)

test: all
	meson test -C $(BUILD) --print-errorlogs

$(BUILD)/build.ninja:
	meson setup $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
