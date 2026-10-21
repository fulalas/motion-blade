# mpv with memc (motion interpolation)

mpv 0.41.0 source with the `--vf=memc` filter added.

## Build

Needs the usual mpv build dependencies (meson, ninja, ffmpeg, libplacebo, ...) and Vulkan headers.

```sh
meson setup build --buildtype=release
ninja -C build
```

The player is `build/mpv`.

## Files added or changed

- `video/filter/vf_memc.c`: the mpv filter
- `video/filter/memc/`: Vulkan interpolation engine and shaders
- `meson.build`, `filters/user_filters.c`, `filters/user_filters.h`: filter registration
- `etc/input.conf`: `Ctrl+m` cycles the interpolation factor (2, 3, 4, off, auto)

## Shaders

The compiled shaders (`video/filter/memc/shaders/*.h`) are included, so `glslc` is not needed
to build. After editing a `.comp`/`.glsl` file, run `video/filter/memc/shaders/gen.sh` (needs `glslc`).

## Windows (static)

Cross-build from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64):
extract it into a work folder, then run from this folder (needs meson, ninja, cmake, nasm, git,
curl, zip):

```sh
ci/build-mingw64-static.sh ../winbuild
```

It downloads and builds FFmpeg, libplacebo, libass, LuaJIT and the other dependencies as static
libraries, and writes `../winbuild/mpv-memc-windows-x86_64.zip`. The only non-system DLL
`mpv.exe` needs is `vulkan-1.dll`, which comes with the GPU driver. The zip has a
`portable_config/mpv.conf` that turns on `memc`.
