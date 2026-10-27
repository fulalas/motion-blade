# Motion Blade

Real-time motion interpolation for `mpv` on any GPU with Vulkan support. Increase a video's frame rate
2x to 8x or to your display's refresh rate.

## Why this exists

The current options available for PC have at least one drawback:

- SVP: paid and its engine has lower quality (below).
- RIFE: a heavy neural network, and slightly lower quality.
- NVIDIA Optical Flow: good quality, but closed source and needs at least an NVIDIA Turing (GTX 1630 or MX450).
- AMD Fluid Motion Frames: closed source and officially only Windows is supported.
- AMD FSR 3 frame generation: made for games; with video frames alone its quality is much lower.
- Lossless Scaling: paid, closed source, officially only Windows is supported, and lower quality.

Motion Blade is free and open source, and runs in real time on any vendor, even integrated GPUs like AMD 780M.

## Quality

Every other frame of 16 test clips dropped, rebuilt and compared with the real one (mean, higher is better):

| | PSNR | SSIM |
|---|---|---|
| Motion Blade | 38.73 | 0.938 |
| RIFE 4.16 lite (SVP 4.7) | 38.14 | 0.935 |
| Lossless Scaling 3.2.1 (LSFG 3.1) | 37.01 | 0.919 |
| SVP 4.7 own engine | 36.73 | 0.910 |
| afmf-linux 1.3.1 | 34.70 | 0.885 |
| AMD FSR 3.1.6 frame generation | 33.37 | 0.828 |

## How it works

Block motion search in both directions, vectors projected to the in-between time, then each
pixel blends the best nearby vectors. It handles hidden and revealed areas, scene cuts (falls
back to the nearest frame), light changes (flashes, sirens) and LED walls switching colours.
Above 1080p the motion analysis runs at half resolution; the final image is always full resolution.

## Build

Needs the mpv build dependencies (meson, ninja, ffmpeg, libplacebo, etc), Vulkan 1.1 headers
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
mpv --vf=motionblade --hwdec=auto video.mp4
```

Options: `--vf=motionblade=factor=auto:max-fps=0:max-gap=0.25`

- `factor`: `auto` (default) matches the display refresh rate; a number (2-8) multiplies the
  frame rate instead.
- `max-fps`: skip sources faster than this (0 = never skip).
- `max-gap`: skip timestamp gaps longer than this (seconds).

`Ctrl+m` cycles the factor: 2, 3, 4, off, auto.

Any `--hwdec` mode works. On Linux with VA-API, frames stay on the GPU from decoder to display
(`MOTIONBLADE_NO_ZEROCOPY=1` turns this off). Black bars are skipped. Formats: 8/10/16-bit 4:2:0,
4:2:2, 4:4:4, planar or NV12/P010.

## Tests

`make` builds `eval_vk`, which drops every other frame of a video, rebuilds it and scores it
against the real one. `tools/regress.sh` checks quality, scene cuts and speed against
`tools/baseline.txt`; `tools/fetch-data.sh` downloads the test clips.

## License

GPL 3.0, see `LICENSE`.
