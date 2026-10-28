# Motion Blade

Real-time motion interpolation for `mpv` on any GPU with Vulkan support. Increase a video's frame rate
2x to 8x or to your display's refresh rate.

## Why this exists

The current options available for PC have at least one drawback:

- RIFE: a heavy neural network, and slightly lower quality.
- Lossless Scaling: paid, closed source, officially only Windows is supported, and lower quality.
- SVP: paid and its engine has lower quality (below).
- AMD Fluid Motion Frames: closed source and officially only Windows is supported.
- AMD FSR 3 frame generation: made for games; with video frames alone its quality is much lower.
- NVIDIA Optical Flow: good quality, but closed source and needs at least an NVIDIA Turing (GTX 1630 or MX450).

Motion Blade is free and open source, and runs in real time on any vendor, even integrated GPUs like AMD 780M.

## Quality

Generated frames compared with the real ones (higher is better):

- PSNR (dB): how close the pixels are.
- SSIM (0 to 1): how alike edges, textures and detail look. Blur and double images lower it.

| | PSNR | SSIM |
|---|---|---|
| Motion Blade | 38.73 | 0.938 |
| RIFE 4.16 lite (SVP 4.7) | 38.14 | 0.935 |
| Lossless Scaling 3.2.1 (LSFG 3.1) | 37.01 | 0.919 |
| SVP 4.7 own engine | 36.73 | 0.910 |
| AMD Fluid Motion Frames for linux 1.3.1 | 34.70 | 0.885 |
| AMD FSR 3.1.6 frame generation | 33.37 | 0.828 |

## How it works

Block motion search in both directions, vectors projected to the in-between time, then each
pixel blends the best nearby vectors. It handles hidden and revealed areas, scene cuts (falls
back to the nearest frame), light changes (flashes, sirens). Above 2.5 megapixels (larger than
1920x1080) the motion analysis runs at half resolution; the final image is always full resolution.

## Build

Motion Blade builds on top of mpv, which is a git submodule, patched at build time
(`patches/mpv.patch`). Arguments to the first `./build.sh` go to `meson setup`.

It requires mpv build dependencies (meson, ninja, ffmpeg, libplacebo, etc), Vulkan headers
and `glslc` (from shaderc).

```sh
./build.sh
```

Windows (static, cross-built from Linux): extract
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64) into `winbuild/`, then run
`./build-windows.sh`. It writes `winbuild/motionblade-windows-x86_64.zip`.

## Use

```sh
mpv --vf=motionblade video.mp4
```

Options: `--vf=motionblade=factor=2:max-fps=0:max-gap=0.25:zerocopy=auto`

- `factor`: a number from 2 to 8 to multiply the frame rate; `auto` matches the display
  refresh rate instead (default 2).
- `max-fps`: skip sources faster than this (default 0 = never skip).
- `max-gap`: skip timestamp gaps longer than this, in seconds (default 0.25, 0 = never skip).
- `zerocopy`: set to `no` if you see wrong colours or glitches (default `auto`).

It works with or without hardware decoding. With `--hwdec=vaapi` or `--gpu-api=vulkan --hwdec=vulkan`
the video stays on the GPU, so the interpolation uses about 25% less CPU.

The output never goes above your screen's refresh rate. On a 60 Hz screen, a 30 fps video at 4x plays
at 60 fps, and a 60 fps video plays as it is.

`Ctrl+M` switches between interpolation 2x, 4x, 8x, auto and off.

## Tests

`make` builds `eval_vk`, which drops every other frame of a video, rebuilds it and scores it
against the real one. `tools/regress.sh` checks quality, scene cuts and speed against
`tools/baseline.txt`; `tools/fetch-data.sh` downloads the test clips. `MOTIONBLADE_PROFILE=1` prints
the GPU time of each step.

## License

GPL 3.0, see `LICENSE`.
