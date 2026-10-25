# mpv-memc

Motion-compensated frame interpolation for mpv, running on the GPU (Vulkan compute).
Adds `--vf=memc`, which doubles (or more) the frame rate of any video.

How it works: block motion search in both directions (coarse-to-fine, candidate based),
motion vectors projected to the in-between time, then each pixel picks the best of the
nearby vectors, with occlusion checks and a fallback to the nearest frame on scene cuts.
Light changes between frames (flashes, sirens) are measured per block and compensated.

## Files

- `src/`: the engine (`memc_vk.c`, `memc.h`), the mpv filter (`vf_memc.c`) and the shaders.
- `mpv/`: mpv 0.41.0 as a git submodule, never edited.
- `patches/mpv.patch`: the few lines mpv needs to know about the filter (`meson.build`,
  `filters/user_filters.{c,h}`, a `Ctrl+m` binding in `etc/input.conf`, the man page entry).
- `tools/`: test tool `eval.c` and test scripts.
- `build/`, `winbuild/`, `eval_vk`: build output, not in git.

## Build

Needs the usual mpv build dependencies (meson, ninja, ffmpeg, libplacebo, ...), a Vulkan 1.1
GPU and driver, Vulkan headers and `glslc` (from shaderc; `../deps/bin/glslc` is used if present).

```sh
./build.sh
```

The player is `build/mpv/mpv`. `tools/mpv-src.sh` fetches the submodule, copies it to
`build/mpv-src`, applies the patch and links the files from `src/` into it, so edits in `src/`
are picked up by the next `./build.sh`. Arguments to the first `./build.sh` go to `meson setup`.

## Windows (static)

Cross-build from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64):
extract its release archive inside `winbuild/`, keeping the `llvm-mingw-*-ucrt-*` folder name,
then run (needs meson, ninja, cmake, nasm, make, pkg-config, glslc, git, curl, tar, xz, zip and a
native C compiler as `cc`):

```sh
./build-windows.sh
```

It downloads and builds FFmpeg, libplacebo, libass, LuaJIT and the other dependencies as static
libraries, and writes `winbuild/mpv-memc-windows-x86_64.zip`. The only non-system DLL
`mpv.exe` needs is `vulkan-1.dll`, which comes with the GPU driver. The zip has a
`portable_config/mpv.conf` that turns on `memc`.

## Use

```sh
mpv --vf=memc --hwdec=auto-copy --video-sync=display-resample video.mkv
```

Options: `--vf=memc=factor=auto:max-fps=0:max-gap=0.25`

- `factor`: `auto` (default) outputs one frame per display refresh, for any
  source/display rate pair (24 fps on 60, 120, 144 Hz...). A number (2-8) gives a
  fixed multiple instead. Without a display (encoding), `auto` doubles.
- `max-fps`: skip interpolation if the source is faster than this (0 = never skip)
- `max-gap`: skip interpolation across timestamp gaps longer than this (seconds)

`Ctrl+m` cycles the factor while playing: 2, 3, 4, off, auto. It replaces the whole `--vf` chain.

Hardware decoding must use a copy mode (`auto-copy`, `vaapi-copy`), since the filter
reads frames from system memory. Formats: 8/10/16-bit 4:2:0, 4:2:2, 4:4:4, planar or NV12/P010.

## Test tool

`make` builds `eval_vk`: takes a high frame rate video (`.y4m`, or anything else through ffmpeg),
keeps every other frame, rebuilds the dropped ones and scores them (PSNR/SSIM) against the real frames.

```sh
./eval_vk clip_50fps.y4m -n 41            # quality per frame
./eval_vk clip.y4m -n 41 -nometric -repeat 8   # speed under sustained load
MEMC_PROFILE=1 ./eval_vk ...              # GPU time per pass
```

## Test data and scripts

Test clips live in `../samples` (override with `$MEMC_DATA`):

- `suite/`: 16 50/60 fps clips (Xiph, Big Buck Bunny) for scoring, lossless FFV1 `.mkv`, only the
  frames the tests use (41 or 81).
- `light/`: 5 of them with added light changes, made by `tools/flicker.sh` (FFV1 `.mkv`).
- `film/`: real encoded clips to play in mpv:
  - `crown_vic_siren_flicker_784p24.mp4`: red siren flashing every frame.
  - `passengers_breakfast_11_cuts_1080p24.mkv`: dark film with 11 hard cuts.
  - `bbb_cuts_1080p24.mp4`: 24 fps cartoon with many cuts.
  - `concert_cut_backlit_422_1080p30.mp4`: 4:2:2 concert clip, cut near the start, flat backlit areas.
  - `motion_graphics_fast_text_720x1280p30.f4v`: portrait motion graphics, fast sliding text, zooms and wipes.
  - `bbb_1080p60.mp4`, `bbb_squirrel_branch_1080p30.mp4`: 60 and 30 fps sources.
  - `bmx_thin_fast_tire_1440p50.mp4`: thin fast rear tire crossing the background (~5.93 s), first 8 s only.
  - `sdr_shoe_hidden_behind_leg_800p24.mp4`: shoe hidden behind the other leg for one frame (~0:14),
    toe tip at 12.7 s.

Scripts:

- `tools/fetch-data.sh`: downloads and makes `suite/` and the `bbb_*` film clips (downloads ~1.2 GB,
  keeps ~0.9 GB).
  The siren, Passengers, bmx and sdr clips are not downloadable.
- `tools/regress.sh [--update]`: regression check (about 1 minute). Compares quality (suite, light
  changes, other pixel formats), scene cuts, speed per frame and per GPU pass, and the `interp` register
  count against `tools/baseline.txt`, and prints FAIL for anything worse than the tolerances. `--update`
  saves the current results as the new baseline. Speed is only comparable at the same GPU power level
  (shown at the top); close mpv first.
- `tools/suite.sh [eval args]`: scores all clips (local / PSNR / SSIM / chroma PSNR), e.g. `tools/suite.sh -p fg_pix=30`.
- `tools/flicker.sh [eval args]`: same, on clips with added light changes. `LIGHT=flicker` (default, red light on alternate frames), `LIGHT=fade` (fade to dark), `LIGHT=flash` (single-frame white flashes).
- `tools/cmp.sh suite/clip frame w:h:x:y out.png other.y4m...`: side-by-side crop of truth and outputs.
- Cut test: `ffmpeg -i ../samples/film/passengers_breakfast_11_cuts_1080p24.mkv -f yuv4mpegpipe - | ./eval_vk /dev/stdin -n 2000 | grep -c CUT` (expect 11).
