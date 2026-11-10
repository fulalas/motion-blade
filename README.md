# Motion Blade

Real-time motion interpolation for `mpv` on any GPU with Vulkan support. Increase video frame
rate from 2x to 8x, or to your display's refresh rate.

## Why this exists

Motion Blade is free, open source and runs on GPUs from any vendor. Even an integrated GPU like the
AMD 780M interpolates 4K 24 fps video to 48 fps in real time.

| | License | OS | GPU | Performance cost (lower is better) | Fidelity (higher is better) | Detail (higher is better) |
|---|---|---|---|---|---|---|
| Motion Blade 4.5.0 | open source | Linux, Windows | any with Vulkan | 16.6 ms | 100% | 100% |
| SVP 4.7 | paid, closed | Windows, macOS, Linux | any | 40.6 ms | 80% | 68% |
| RIFE 4.16 lite | open source | Linux, Windows, macOS | any | 117.8 ms | 74% | 91% |
| Lossless Scaling 3.2.2 (lsfg-vk 2.0) | paid, closed | Windows (Linux: lsfg-vk) | any | 5.8 ms | 67% | 85% |
| AMD Fluid Motion Frames (afmf-linux 1.3.1) | open source | Linux | any with Vulkan | 111.8 ms | 51% | 47% |
| NVIDIA Optical Flow (SDK 5.0.7) | free, closed | Windows, Linux | NVIDIA Turing or newer | 21.1 ms | 46% | 58% |
| AMD FSR 3.1.4 frame generation | open source | Windows (Linux: community port) | any | 8.2 ms | 43% | 35% |

- Performance cost: time per generated 1080p frame.
- Fidelity: pixel error compared with Motion Blade's.
- Detail: loss of edges, textures and detail compared with Motion Blade's.
- Tested on an NVIDIA GTX 1650 SUPER.

## How it works

Block motion search in both directions, vectors projected to the in-between time, then each
pixel blends the best nearby vectors. It handles hidden and revealed areas, scene cuts (falls
back to the nearest frame), light changes (flashes, sirens). Above 2.5 megapixels (larger than
1920x1080) the motion analysis runs at half resolution; the final image is always full resolution.

## Build

It requires mpv build dependencies (meson, ninja, ffmpeg, libplacebo, etc), Vulkan headers
and `glslc` (from shaderc). Arguments to the first `./build.sh` go to `meson setup`.

```sh
./build.sh
```

Windows (static, cross-built from Linux): extract
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64) into `winbuild/`, then run
`./build-windows.sh`. It writes `build/motionblade-windows-x86_64.zip`.

## Usage

```sh
mpv --vf=motionblade video.mp4
```

Options: `--vf=motionblade=factor=2:max-fps=0:max-gap=0.25:pulldown=auto:zerocopy=auto`

- `factor`: a number from 2 to 8 to multiply the frame rate; `auto` matches the display
  refresh rate instead (default 2).
- `max-fps`: skip sources faster than this (default 0 = never skip).
- `max-gap`: skip timestamp gaps longer than this, in seconds (default 0.25, 0 = never skip).
- `pulldown`: film turned into 29.97 fps for TV or DVD repeats some fields and stutters; `auto`
  finds it and restores the original 23.976 fps frames first (default `auto`, `no` to turn off). Only
  for frames in system memory (software decoding or `--hwdec=auto-copy`); keep `--deinterlace` off.
- `zerocopy`: set to `no` if you see wrong colours or glitches (default `auto`).

It works with or without hardware decoding. With `--hwdec=vaapi` or `--gpu-api=vulkan --hwdec=vulkan`
the video stays on the GPU, so the interpolation uses about 25% less CPU.

The output never goes above your screen's refresh rate. On a 60 Hz screen, a 30 fps video at 4x plays
at 60 fps, and a 60 fps video plays as it is.

`Alt+m` switches between interpolation 2x, 4x, 8x, auto and off.

## Stutter and high GPU usage

If playback stutters, set the GPU power mode to maximum performance as power saving can keep its clocks
too low.

| | Windows | Linux (as root) |
|---|---|---|
| NVIDIA | Control Panel → Manage 3D settings → Power management mode → Prefer maximum performance | `nvidia-settings -a '[gpu:0]/GpuPowerMizerMode=1'` |
| AMD | Windows power mode → Best performance | `echo high > /sys/class/drm/cardN/device/power_dpm_force_performance_level` |
| Intel | Graphics Command Center → System → Power → Maximum Performance | `cat gt_max_freq_mhz > gt_min_freq_mhz` in `/sys/class/drm/cardN/` |

## Tests

`make` builds `eval` and the tests, `make test` runs them. `build/local/tools/eval/eval clip.mkv` drops
every other frame of a video, rebuilds it and scores it against the real one (`-backend cpu` uses the C
reference instead of the GPU). `tools/regress.sh` checks quality, scene cuts and speed against
`tools/baseline.txt`; `tools/fetch-data.sh` downloads the test clips. `MOTIONBLADE_PROFILE=1` prints the
GPU time of each step. `tools/compare/run.sh` produces the comparison table above (see
`tools/compare/README.md`).

## License

GPL 3.0, see `LICENSE`.
