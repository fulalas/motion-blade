import sys
import numpy as np
import vapoursynth as vs

lib, model, w, h, src, dst = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6]
core = vs.core
core.std.LoadPlugin(lib)
frames = np.memmap(src, np.uint8, "r").reshape(-1, h, w, 4)

def fill(n, f):
    out = f.copy()
    for p in range(3):
        np.copyto(np.asarray(out[p]), frames[n, :, :, p].astype(np.float32) / 255.0)
    return out

base = core.std.BlankClip(width=w, height=h, format=vs.RGBS, length=len(frames))
clip = core.std.ModifyFrame(base, base, fill)
out = core.rife.RIFE(clip, model_path=model, factor_num=2, gpu_id=0)
with open(dst, "wb") as f:
    rgba = np.full((h, w, 4), 255, np.uint8)
    for i in range(1, 2 * len(frames) - 1, 2):
        fr = out.get_frame(i)
        for p in range(3):
            rgba[:, :, p] = np.clip(np.rint(np.asarray(fr[p]) * 255.0), 0, 255).astype(np.uint8)
        f.write(rgba.tobytes())
