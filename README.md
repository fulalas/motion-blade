# Motion Blade

Real-time motion interpolation for mpv, on any GPU (Vulkan). Plays 24/30 fps video at your
display's refresh rate (60, 120, 144 Hz...), like the motion smoothing on TVs.

## Why this exists

- SVP: paid, and its own engine has lower quality (below).
- RIFE: a heavy neural network, and slightly lower quality.
- NVIDIA Optical Flow: good quality, but closed source and needs at least an NVIDIA Turing (GTX 1630 or MX450).
- AMD Fluid Motion Frames: closed source, lower quality and officially only Windows is supported.

Motion Blade is free and open source, and runs in real time at 1080p on any vendor, even
integrated GPUs like the AMD 780M.

## Quality

Every other frame of 16 test clips rebuilt and compared with the real one (mean, higher is better):

| | PSNR | SSIM |
|---|---|---|
| Motion Blade | 38.74 | 0.938 |
| RIFE 4.16 lite (SVP 4.7) | 38.14 | 0.935 |
| RIFE 4.4 (SVP 4.7) | 37.86 | 0.937 |
| SVP 4.7 own engine | 36.73 | 0.910 |
| AMD Fluid Motion Frames (afmf-linux 1.3.1) | 34.70 | 0.885 |

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
mpv --vf=motionblade --hwdec=auto --video-sync=display-resample video.mp4
```

Options: `--vf=motionblade=factor=auto:max-fps=0:max-gap=0.25`

- `factor`: `auto` (default) matches the display refresh rate; a number (2-8) multiplies the
  frame rate instead.
- `max-fps`: skip sources faster than this (0 = never skip).
- `max-gap`: skip timestamp gaps longer than this (seconds).

`Ctrl+m` cycles the factor: 2, 3, 4, off, auto.

Any `--hwdec` mode works. On Linux with VA-API, frames stay on the GPU from decoder to display
(`MOTIONBLADE_NO_ZEROCOPY=1` turns this off). Black bars are skipped. Formats: 8/10/16-bit 4:2:0, 4:2:2,
4:4:4, planar or NV12/P010.

## Tests

`make` builds `eval_vk`, which drops every other frame of a video, rebuilds it and scores it
against the real one. `tools/regress.sh` checks quality, scene cuts and speed against
`tools/baseline.txt`; `tools/fetch-data.sh` downloads the test clips.

## License

GPL 3.0, see `LICENSE`.
