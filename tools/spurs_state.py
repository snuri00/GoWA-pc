import sys

from memdump import MemDump


def main():
    if len(sys.argv) < 2:
        print("usage: spurs_state.py <memory.bin> [spurs_addr]")
        return 1
    d = MemDump(sys.argv[1])
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3001AD00
    b = d.read(base, 0x100)
    if b is None:
        print("spurs block unmapped")
        return 1
    print("spuIdling %02x flags1 %02x nSpus %d sysSrvMessage %02x wklSignal1 %04x wklSignal2 %04x wklFlagReceiver %02x" %
          (b[0x73], b[0x74], b[0x76], b[0x72], int.from_bytes(b[0x70:0x72], "big"), int.from_bytes(b[0x78:0x7a], "big"), b[0x77]))
    print("wklEnabled %08x wklMskB %08x" % (int.from_bytes(b[0xB0:0xB4], "big"), int.from_bytes(b[0xB4:0xB8], "big")))
    print("wid ready1 ready2 cur pend min max state1 status1 event1 state2 status2 event2")
    for w in range(16):
        print("%2d  %5d %6d %3x %4x %3d %3x %6d %7d %6d %6d %7d %6d" % (w, b[w], b[0x10 + w], b[0x20 + w], b[0x30 + w], b[0x40 + w], b[0x50 + w], b[0x80 + w], b[0x90 + w], b[0xA0 + w],
                                                                      b[0xD0 + w], b[0xE0 + w], b[0xF0 + w]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
