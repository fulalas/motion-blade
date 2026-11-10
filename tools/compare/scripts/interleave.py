import sys
orig, gen, dst, w, h, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
size = w * h * 3 // 2
o = open(orig, "rb").read()
g = open(gen, "rb").read()
with open(dst, "wb") as f:
    f.write(f"YUV4MPEG2 W{w} H{h} F25:1 Ip A1:1 C420jpeg\n".encode())
    for i in range(n):
        f.write(b"FRAME\n")
        if i % 2 == 0:
            f.write(o[i * size:(i + 1) * size])
        else:
            k = i // 2
            f.write(g[k * size:(k + 1) * size])
