import mmap
import struct
import sys


class MemDump:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        self.chunks = []
        pos = 0
        while pos + 8 <= len(self.m):
            addr, size = struct.unpack_from("<II", self.m, pos)
            self.chunks.append((addr, size, pos + 8))
            pos += 8 + size

    def read(self, addr, n):
        for a, size, off in self.chunks:
            if a <= addr and addr + n <= a + size:
                return bytes(self.m[off + addr - a: off + addr - a + n])
        return None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack(">I", b)[0] if b else None

    def hexdump(self, addr, n):
        b = self.read(addr, n)
        if b is None:
            return "%08x: unmapped" % addr
        lines = []
        for i in range(0, n, 16):
            words = " ".join("%08x" % struct.unpack_from(">I", b, i + k)[0] for k in range(0, min(16, n - i), 4))
            lines.append("%08x: %s" % (addr + i, words))
        return "\n".join(lines)


def main():
    if len(sys.argv) < 3:
        print("usage: memdump.py <memory.bin> <addr> [len]")
        return 1
    d = MemDump(sys.argv[1])
    print(d.hexdump(int(sys.argv[2], 16), int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100))
    return 0


if __name__ == "__main__":
    sys.exit(main())
