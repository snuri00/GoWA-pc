import argparse
import io
import os
import struct

from PIL import Image, ImageDraw

FORMATS = {0x86: ("DXT1", 8), 0x87: ("DXT3", 16), 0x88: ("DXT5", 16), 0x85: ("ARGB", 4)}


class MemDump:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.regions = []
        pos = 0
        size = os.path.getsize(path)
        while pos < size:
            self.f.seek(pos)
            a, n = struct.unpack("<II", self.f.read(8))
            self.regions.append((a, n, pos + 8))
            pos += 8 + n

    def read(self, ea, n):
        for a, length, off in self.regions:
            if a <= ea and ea + n <= a + length:
                self.f.seek(off + ea - a)
                return self.f.read(n)
        return None


def mip_sizes(code, w, h, levels):
    out = []
    for _ in range(levels):
        if code == 0x85:
            out.append((w, h, w * h * 4))
        else:
            out.append((w, h, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * FORMATS[code][1]))
        w, h = max(1, w // 2), max(1, h // 2)
    return out


def decode(code, w, h, data):
    if code == 0x85:
        return Image.frombytes("RGBA", (w, h), data, "raw", "ARGB").convert("RGB")
    hdr = bytearray(128)
    struct.pack_into("<4sIIIIII", hdr, 0, b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(data), 0)
    struct.pack_into("<II4s", hdr, 76, 32, 0x4, FORMATS[code][0].encode())
    struct.pack_into("<I", hdr, 108, 0x1000)
    return Image.open(io.BytesIO(bytes(hdr) + data)).convert("RGB")


def main():
    ap = argparse.ArgumentParser(description="view a texture mip chain from a gowa memory dump")
    ap.add_argument("dump")
    ap.add_argument("addr", type=lambda s: int(s, 0))
    ap.add_argument("fmt", type=lambda s: int(s, 0), help="format byte, e.g. 0x86 0x88 0x85")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("--levels", type=int, default=0)
    ap.add_argument("--out", default="texview.png")
    ap.add_argument("--tile", type=int, default=256)
    args = ap.parse_args()
    code = args.fmt & ~0x60
    md = MemDump(args.dump)
    levels = args.levels or (max(args.width, args.height).bit_length())
    mips = mip_sizes(code, args.width, args.height, levels)
    t = args.tile
    sheet = Image.new("RGB", (t * len(mips), t + 14), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    ea = args.addr
    for i, (w, h, n) in enumerate(mips):
        data = md.read(ea, n)
        label = "%d %dx%d @%08x" % (i, w, h, ea)
        print(label, "unmapped" if data is None else "")
        if data is not None and w >= 4 or code == 0x85:
            try:
                sheet.paste(decode(code, max(w, 4), max(h, 4), data if w >= 4 else data + bytes(FORMATS[code][1] * 1)).resize((t, t), Image.NEAREST), (i * t, 14))
            except Exception as e:
                print("  decode failed", e)
        d.text((i * t + 2, 1), label, fill=(255, 255, 0))
        ea += n
    sheet.save(args.out)
    print("saved", args.out)


if __name__ == "__main__":
    main()
