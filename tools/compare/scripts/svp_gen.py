import sys
import ctypes
import numpy as np
import vapoursynth as vs

def opencl_gpus():
    try:
        cl = ctypes.CDLL("libOpenCL.so.1")
    except OSError:
        return 0
    n = ctypes.c_uint(0)
    if cl.clGetPlatformIDs(0, None, ctypes.byref(n)) != 0 or n.value == 0:
        return 0
    platforms = (ctypes.c_void_p * n.value)()
    cl.clGetPlatformIDs(n.value, platforms, None)
    total = 0
    for p in platforms:
        d = ctypes.c_uint(0)
        if cl.clGetDeviceIDs(ctypes.c_void_p(p), ctypes.c_uint64(1 << 2), 0, None, ctypes.byref(d)) == 0:
            total += d.value
    return total

lib, w, h, src, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
gpu = 1 if opencl_gpus() > 0 else 0
if not gpu:
    print("svp: no OpenCL GPU found, running on the CPU", file=sys.stderr)
core = vs.core
core.std.LoadPlugin(f"{lib}/libsvpflow1.so")
core.std.LoadPlugin(f"{lib}/libsvpflow2.so")
size = w * h * 3 // 2
data = np.memmap(src, np.uint8, "r")
count = len(data) // size
frames = data[:count * size].reshape(count, size)

def fill(n, f):
    out = f.copy()
    y = frames[n]
    np.copyto(np.asarray(out[0]), y[:w * h].reshape(h, w))
    np.copyto(np.asarray(out[1]), y[w * h:w * h + w * h // 4].reshape(h // 2, w // 2))
    np.copyto(np.asarray(out[2]), y[w * h + w * h // 4:].reshape(h // 2, w // 2))
    return out

base = core.std.BlankClip(width=w, height=h, format=vs.YUV420P8, length=count, fpsnum=25, fpsden=1)
clip = core.std.ModifyFrame(base, base, fill)
sup = core.svp1.Super(clip, "{gpu:%d}" % gpu)
vec = core.svp1.Analyse(sup["clip"], sup["data"], clip, "{}")
out = core.svp2.SmoothFps(clip, sup["clip"], sup["data"], vec["clip"], vec["data"],
                          "{rate:{num:2,den:1,abs:false},scene:{mode:0}}")
in_between = out[1:2 * count - 1:2].frames(prefetch=4) if count > 1 else ()
with open(dst, "wb") as f:
    for fr in in_between:
        for p in range(3):
            f.write(np.ascontiguousarray(np.asarray(fr[p])).tobytes())
