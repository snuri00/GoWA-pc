import lzma
import os
import struct
import sys
import zlib


class Psarc:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0x20)
        if h[:4] != b"PSAR":
            raise ValueError("not a psarc")
        self.version = struct.unpack(">HH", h[4:8])
        self.compression = h[8:12].decode()
        self.toc_length, self.entry_size, self.entry_count, self.block_size, self.flags = struct.unpack(">5I", h[12:32])
        toc = self.f.read(self.toc_length - 0x20)
        self.entries = []
        for i in range(self.entry_count):
            e = toc[i * self.entry_size:(i + 1) * self.entry_size]
            self.entries.append((e[:16], struct.unpack(">I", e[16:20])[0], int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
        width = 2 if self.block_size <= 0x10000 else 3 if self.block_size <= 0x1000000 else 4
        table = toc[self.entry_count * self.entry_size:]
        self.blocks = [int.from_bytes(table[i:i + width], "big") for i in range(0, len(table) - width + 1, width)]
        names = self.read_entry(0).decode("utf-8", "replace").split("\n") if self.entry_count else []
        self.names = [""] + [n for n in names if n] if self.entry_count else []
        if len(self.names) != self.entry_count:
            self.names = [""] + names[:self.entry_count - 1]

    def read_entry(self, idx, limit=None):
        _, block, size, offset = self.entries[idx]
        want = size if limit is None else min(size, limit)
        out = bytearray()
        self.f.seek(offset)
        while len(out) < want:
            csize = self.blocks[block]
            if csize == 0:
                out += self.f.read(self.block_size)
            else:
                raw = self.f.read(csize)
                remaining = size - len(out)
                if csize == min(self.block_size, remaining):
                    out += raw
                elif self.compression == "zlib" and raw[:1] == b"\x78":
                    out += zlib.decompress(raw)
                elif self.compression == "lzma":
                    out += lzma.decompress(raw)
                else:
                    out += raw
            block += 1
        return bytes(out[:want])

    def files(self):
        for i in range(1, self.entry_count):
            yield i, self.names[i] if i < len(self.names) else f"__entry_{i}", self.entries[i][2]

    def extract(self, dst, select=None):
        for i, name, _ in self.files():
            if select and not select(name):
                continue
            path = os.path.join(dst, name.lstrip("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as out:
                out.write(self.read_entry(i))


if __name__ == "__main__":
    p = Psarc(sys.argv[1])
    if len(sys.argv) == 2:
        print(f"{p.compression} v{p.version} entries {p.entry_count} block {p.block_size:#x} flags {p.flags:#x}")
        for i, name, size in p.files():
            print(f"{size:>12} {name}")
    else:
        p.extract(sys.argv[2])
