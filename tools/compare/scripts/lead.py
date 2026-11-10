import shutil
import sys

src, dst, w, h, lead = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
size = w * h * 4
with open(src, "rb") as s, open(dst, "wb") as f:
    count = s.seek(0, 2) // size
    for i in range(lead, 0, -1):
        s.seek(min(i, count - 1) * size)
        f.write(s.read(size))
    s.seek(0)
    shutil.copyfileobj(s, f)
