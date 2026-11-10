import sys

a, src, dst, size = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
data = open(a, "rb").read()
count = open(src, "rb").seek(0, 2) // size
total = len(data) // size
if total < 2 * count:
    sys.exit(f"nvinterpolate gave {total} frames for {count} inputs")
with open(dst, "wb") as f:
    for i in range(3, 2 * count, 2):
        f.write(data[i * size:(i + 1) * size])
