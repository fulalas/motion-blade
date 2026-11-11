# Comparison with other tools

Scores Motion Blade, RIFE, AMD Fluid Motion Frames (afmf-linux), AMD FSR 3, Lossless Scaling,
NVIDIA Optical Flow and SVP on the same clips and GPU, for quality and performance.

```sh
tools/compare/run.sh                    # all tools, quality and performance
tools/compare/run.sh perf               # performance only
tools/compare/run.sh quality rife fsr   # some tools only
```

The first run downloads and builds what it needs into `tools/compare/local/` and downloads the test
clips (about 1.5 GB, into the folder set by `MOTIONBLADE_DATA`, `../samples` by default). Later runs
reuse them. Results are printed and appended to `tools/compare/results.txt`.

## Requirements

For every tool:

- Linux x86_64 and a GPU with a Vulkan driver, plus the Vulkan loader and headers.
- The Motion Blade build dependencies (see the main README), including meson and ninja.
- git, CMake, a C++20 compiler (GCC or Clang), pkg-config, curl, unzip and bc.
- Python 3.12 or newer with venv and pip. VapourSynth and numpy are installed into `local/venv`.
- FFmpeg and ffprobe with libx264 (used to make the performance clip).
- Internet on the first run: GitHub, git.lsfg-vk.dev, PyPI and media.xiph.org.
- About 10 GB free in `/dev/shm` for the performance test.

Each tool also needs the items below. A tool that is missing something is skipped.

| Tool | Also needs |
|---|---|
| RIFE | OpenMP (comes with GCC; libomp with Clang) |
| afmf-linux | CMake 3.28 or newer; glslang (built if `glslangValidator` is missing); Mesa, or a Wayland session and the wayland-client headers (see below) |
| FSR 3 | Vulkan 1.3; glslang (as above) |
| Lossless Scaling | Vulkan 1.2; `lsfg-vk.dll` from Lossless Scaling on Steam (beta branch "lsfg-vk"), found in the Steam folder, next to `run.sh`, or with `LSFG_DLL=/path` |
| SVP | The official installer `svp*-linux.run`, next to `run.sh` or with `SVP_INSTALLER=/path`; an OpenCL driver with the `libOpenCL.so.1` loader (Mesa rusticl, ROCm or NVIDIA) |
| NVIDIA Optical Flow | An NVIDIA GPU (Turing or newer) with the proprietary driver (CUDA and NVENC); an FFmpeg build with the `nvinterpolate` filter and `hevc_nvenc`, set with `FFMPEG=/path/to/ffmpeg` |

afmf-linux runs through its test program, on its headless surface (`VK_EXT_headless_surface`). Only Mesa
has that surface, so with other drivers (NVIDIA) it lands on llvmpipe, the CPU. Under Wayland it then
presents to an invisible Wayland surface instead, on the GPU; that needs the wayland-client headers when
afmf is built. `AFMF_WSI=headless|wayland` picks the surface. If it still lands on a CPU device it is
skipped. FSR 3 and Lossless Scaling stop the same way on a CPU device, and SVP when it finds no OpenCL GPU
(`ALLOW_CPU=1` runs them anyway).

## How it measures

Quality: every other frame of 16 clips is dropped, each tool rebuilds it from the kept frames and the
result is compared with the real frame. Motion Blade, SVP and NVIDIA Optical Flow work in YUV like in a
video player. The others get 8-bit RGB (BT.709) and are compared with the real frame after the same
RGB round trip, so the conversion itself costs them nothing.

Performance: total time per generated 1080p frame, timed from outside the same way for every tool.
Each tool runs on 151 and on 451 frames from RAM (`/dev/shm`, about 10 GB free needed), and the
difference is divided by the 300 extra frames, so start-up is left out. RIFE and SVP are asked for a few
frames ahead, as a video player does, so the GPU is never left waiting while a frame is saved. One warm-up run, then the
median of 3 (`PERF_RUNS=n` to change). afmf-linux and NVIDIA Optical Flow keep the generated frame on the
GPU; the others copy it back to system memory. Close other GPU work and set the GPU to maximum performance
first.
