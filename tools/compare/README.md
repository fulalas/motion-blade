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

Needs the Motion Blade build dependencies, plus git, cmake, a C++ compiler, curl, unzip, bc and python3
with venv.

afmf-linux runs through its test program, on its headless surface (`VK_EXT_headless_surface`). Only Mesa
has that surface, so with other drivers (NVIDIA) it lands on llvmpipe, the CPU. Under Wayland it then
presents to an invisible Wayland surface instead, on the GPU; that needs the wayland-client headers when
afmf is built. `AFMF_WSI=headless|wayland` picks the surface. If it still lands on a CPU device it is
skipped (`AFMF_ALLOW_CPU=1` to run it anyway).

Some tools can't be shipped, so the user provides them (a missing one only skips that tool):

- Lossless Scaling: `lsfg-vk.dll` from Lossless Scaling on Steam (beta branch "lsfg-vk"). Found in the
  Steam folder, next to `run.sh`, or with `LSFG_DLL=/path`.
- SVP: the official installer `svp*-linux.run`, next to `run.sh` or with `SVP_INSTALLER=/path`.
- NVIDIA Optical Flow: an FFmpeg with the `nvinterpolate` filter, with `FFMPEG=/path/to/ffmpeg`.

Quality: every other frame of 16 clips is dropped, each tool rebuilds it from the kept frames and the
result is compared with the real frame. Motion Blade, SVP and NVIDIA Optical Flow work in YUV like in a
video player. The others get 8-bit RGB (BT.709) and are compared with the real frame after the same
RGB round trip, so the conversion itself costs them nothing.

Performance: total time per generated 1080p frame, timed from outside the same way for every tool.
Each tool runs on 151 and on 451 frames from RAM (`/dev/shm`, about 10 GB free needed), and the
difference is divided by the 300 extra frames, so start-up is left out. One warm-up run, then the
median of 3 (`PERF_RUNS=n` to change). afmf-linux and NVIDIA Optical Flow keep the generated frame on the
GPU; the others copy it back to system memory. Close other GPU work and set the GPU to maximum performance
first.
