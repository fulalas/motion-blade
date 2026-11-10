BUILD = build/local
DEPS = $(abspath ../deps/bin)
export PATH := $(if $(wildcard $(DEPS)),$(DEPS):)$(PATH)

all: setup
	ninja -C $(BUILD)

test: all
	meson test -C $(BUILD) --print-errorlogs

setup:
	@tools/meson-setup.sh $(BUILD) $(CURDIR)

clean:
	rm -rf $(BUILD)

.PHONY: all test setup clean
