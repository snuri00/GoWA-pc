import os
import struct
import sys
from Crypto.Cipher import AES
from Crypto.Util import Counter
from keystore import key

PS3_KEY = key("PS3_KEY")
class Pkg:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0xC0)
        if h[:4] != b"\x7fPKG":
            raise ValueError("not a pkg")
        self.revision, self.type = struct.unpack(">HH", h[4:8])
        self.item_count = struct.unpack(">I", h[0x14:0x18])[0]
        self.data_offset, self.data_size = struct.unpack(">QQ", h[0x20:0x30])
        self.content_id = h[0x30:0x60].rstrip(b"\0").decode()
        self.riv = int.from_bytes(h[0x70:0x80], "big")
        if self.revision != 0x8000 or self.type != 1:
            raise ValueError("only retail ps3 pkgs supported")

    def read(self, off, size):
        start = off & ~15
        self.f.seek(self.data_offset + start)
        raw = self.f.read(size + (off - start))
        ctr = Counter.new(128, initial_value=(self.riv + start // 16) & ((1 << 128) - 1))
        return AES.new(PS3_KEY, AES.MODE_CTR, counter=ctr).decrypt(raw)[off - start:]

    def items(self):
        table = self.read(0, self.item_count * 32)
        for i in range(self.item_count):
            no, ns, do, ds, flags = struct.unpack(">IIQQI", table[i * 32:i * 32 + 28])
            name = self.read(no, ns).decode("utf-8", "replace")
            yield name, do, ds, flags & 0xFF

    def extract(self, dst, log=None):
        for name, do, ds, kind in self.items():
            path = os.path.join(dst, name)
            if kind == 4:
                os.makedirs(path, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as out:
                pos = 0
                while pos < ds:
                    n = min(ds - pos, 1 << 24)
                    out.write(self.read(do + pos, n))
                    pos += n
            if log:
                log(name, ds)


if __name__ == "__main__":
    p = Pkg(sys.argv[1])
    if len(sys.argv) == 2:
        print(p.content_id, p.item_count)
        for name, do, ds, kind in p.items():
            print(f"{ds:>12} {kind:>2} {name}")
    else:
        p.extract(sys.argv[2], lambda n, s: print(f"{s:>12} {n}"))
