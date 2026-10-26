# Motion Blade

Real-time motion interpolation for mpv, on any GPU (Vulkan). Plays 24/30 fps video at your
display's refresh rate (60, 120, 144 Hz...), like the motion smoothing on TVs.

## Why this exists

- SVP: heavy, and its quality is not good enough.
- RIFE: same problems.
- NVIDIA Optical Flow: good quality, but closed source and NVIDIA only.
- AMD Fluid Motion Frames: closed source, Windows only.

Motion Blade is free and open source, and runs in real time at 1080p on any vendor, even
integrated GPUs like the AMD 780M.

## How it works

Block motion search in both directions, vectors projected to the in-between time, then each
pixel blends the best nearby vectors. It handles hidden and revealed areas, scene cuts (falls
back to the nearest frame) and light changes (flashes, sirens).

## Build

Needs the mpv build dependencies (meson, ninja, ffmpeg, libplacebo...), Vulkan 1.1 headers
and driver, and `glslc` (from shaderc).

```sh
./build.sh
```

The player is `build/mpv/mpv`. mpv 0.41.0 is a git submodule, patched at build time
(`patches/mpv.patch`). Arguments to the first `./build.sh` go to `meson setup`.

Windows (static, cross-built from Linux): extract
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64) into `winbuild/`, then run
`./build-windows.sh`. It writes `winbuild/motionblade-windows-x86_64.zip`, which only needs the GPU
driver.

## Use

```sh
mpv --vf=motionblade --hwdec=auto-copy --video-sync=display-resample video.mp4
```

Options: `--vf=motionblade=factor=auto:max-fps=0:max-gap=0.25`

- `factor`: `auto` (default) matches the display refresh rate; a number (2-8) multiplies the
  frame rate instead.
- `max-fps`: skip sources faster than this (0 = never skip).
- `max-gap`: skip timestamp gaps longer than this (seconds).

`Ctrl+m` cycles the factor: 2, 3, 4, off, auto.

Hardware decoding must use a copy mode (`auto-copy`, `vaapi-copy`). Formats: 8/10/16-bit
4:2:0, 4:2:2, 4:4:4, planar or NV12/P010.

## Tests

`make` builds `eval_vk`, which drops every other frame of a video, rebuilds it and scores it
against the real one. `tools/regress.sh` checks quality, scene cuts and speed against
`tools/baseline.txt`; `tools/fetch-data.sh` downloads the test clips.

## License

GPL 3.0, see `LICENSE`.
