import gzip
import struct
import sys
from Crypto.Cipher import AES
from keystore import key

D1_KEY = key("D1_KEY")
D1_IV = key("D1_IV")
class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]


def parse(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    r = Reader(raw)
    if r.take(4) != b"3IRD":
        raise ValueError("not an IRD")
    ird = {"version": r.u8()}
    ird["product"] = r.take(9).decode()
    ird["title"] = r.take(r.u8()).decode("utf-8", "replace")
    ird["sys_ver"] = r.take(4).decode()
    ird["game_ver"] = r.take(5).decode()
    ird["app_ver"] = r.take(5).decode()
    if ird["version"] == 7:
        ird["id"] = r.u32()
    ird["header"] = gzip.decompress(r.take(r.u32()))
    ird["footer"] = gzip.decompress(r.take(r.u32()))
    ird["region_md5"] = [r.take(16) for _ in range(r.u8())]
    ird["files"] = [(r.u64(), r.take(16)) for _ in range(r.u32())]
    r.u16()
    r.u16()
    if ird["version"] >= 9:
        ird["pic"] = r.take(115)
    ird["data1"] = r.take(16)
    ird["data2"] = r.take(16)
    if ird["version"] < 9:
        ird["pic"] = r.take(115)
    if ird["version"] > 7:
        ird["uid"] = r.u32()
    return ird


def disc_key(data1):
    return AES.new(D1_KEY, AES.MODE_CBC, D1_IV).encrypt(data1)


if __name__ == "__main__":
    ird = parse(sys.argv[1])
    print("version ", ird["version"])
    print("product ", ird["product"], ird["title"])
    print("versions", ird["sys_ver"], ird["game_ver"], ird["app_ver"])
    print("files   ", len(ird["files"]), "regions", len(ird["region_md5"]))
    print("data1   ", ird["data1"].hex().upper())
    print("data2   ", ird["data2"].hex().upper())
    print("disc_key", disc_key(ird["data1"]).hex().upper())
