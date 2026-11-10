import mmap
import sys

src, w, h, even = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
full = sys.argv[5] if len(sys.argv) > 5 else None
size = w * h * 3 // 2
with open(src, "rb") as s:
    data = mmap.mmap(s.fileno(), 0, access=mmap.ACCESS_READ)
    n = len(data) // size
    order = list(range(n)) + list(range(n - 2, -1, -1)) + list(range(1, n))
    with open(even, "wb") as e, open(full or "/dev/null", "wb") as f:
        if full:
            f.write(f"YUV4MPEG2 W{w} H{h} F50:1 Ip A1:1 C420jpeg\n".encode())
        for k, i in enumerate(order):
            frame = data[i * size:(i + 1) * size]
            if full:
                f.write(b"FRAME\n")
                f.write(frame)
            if k % 2 == 0:
                e.write(frame)
