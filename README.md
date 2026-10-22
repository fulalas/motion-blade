# mpv with memc (motion interpolation)

mpv 0.41.0 source with the `--vf=memc` filter added.

## Build

Needs the usual mpv build dependencies (meson, ninja, ffmpeg, libplacebo, ...), Vulkan headers and
`glslc` (from shaderc).

```sh
meson setup build --buildtype=release
ninja -C build
```

The player is `build/mpv`.

## Files added or changed

- `video/filter/vf_memc.c`: the mpv filter
- `video/filter/memc/`: Vulkan interpolation engine and shaders
- `meson.build`, `filters/user_filters.c`, `filters/user_filters.h`: filter registration and shader build
- `DOCS/man/vf.rst`: `memc` entry
- `ci/build-mingw64-static.sh`: static Windows build
- `MEMC_VERSION`, `AGENTS.md`, `BUILD-MEMC.md`: version file and notes
- `etc/input.conf`: `Ctrl+m` cycles the interpolation factor (2, 3, 4, off, auto)

## Shaders

The build compiles `video/filter/memc/shaders/*.comp` to SPIR-V headers in the build folder with
`glslc` (rules in `video/filter/memc/shaders/meson.build`), so edits to `.comp`/`.glsl` files are
picked up by a normal rebuild.

## Windows (static)

Cross-build from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64):
extract its release archive inside a work folder, keeping the `llvm-mingw-*-ucrt-*` folder name,
then run from this folder with the work folder as argument (needs meson, ninja, cmake, nasm, make,
pkg-config, glslc, git, curl, tar, xz, zip and a native C compiler as `cc`):

```sh
ci/build-mingw64-static.sh ../winbuild
```

It downloads and builds FFmpeg, libplacebo, libass, LuaJIT and the other dependencies as static
libraries, and writes `../winbuild/mpv-memc-windows-x86_64.zip`. The only non-system DLL
`mpv.exe` needs is `vulkan-1.dll`, which comes with the GPU driver. The zip has a
`portable_config/mpv.conf` that turns on `memc`.
