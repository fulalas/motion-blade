import io
import os
import sys
import py7zr

src, dst = sys.argv[1], sys.argv[2]
want = ["plugins/libsvpflow1.so", "plugins/libsvpflow2.so"]
data = memoryview(open(src, "rb").read())
sig = b"7z\xbc\xaf\x27\x1c"


def extract_at(start):
    try:
        z = py7zr.SevenZipFile(io.BytesIO(data[start:]))
        names = set(z.getnames())
    except Exception:
        return False
    with z:
        if not set(want) <= names:
            return False
        z.extract(path=dst, targets=want)
    return True


i = data.obj.find(sig)
while i >= 0:
    if extract_at(i):
        for n in want:
            os.replace(f"{dst}/{n}", f"{dst}/{os.path.basename(n)}")
        os.rmdir(f"{dst}/plugins")
        sys.exit(0)
    i = data.obj.find(sig, i + 6)
sys.exit("SVP libraries not found in " + src)
