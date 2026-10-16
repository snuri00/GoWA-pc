import sys
from Crypto.Cipher import AES
from Crypto.Hash import CMAC
from keystore import key

NP_OMAC_KEY_2 = key("NP_OMAC_KEY_2")
def npd_header(path):
    d = open(path, "rb").read(0x1000)
    i = d.find(b"NPD\0")
    return d[i:i + 0x80]


def dev_hash_ok(npd, klic):
    key = bytes(a ^ b for a, b in zip(klic, NP_OMAC_KEY_2))
    return CMAC.new(key, ciphermod=AES).update(npd[:0x60]).digest() == npd[0x60:0x70]


def main(self_path, haystack_path):
    npd = npd_header(self_path)
    hay = open(haystack_path, "rb").read()
    seen = set()
    for i in range(len(hay) - 15):
        k = hay[i:i + 16]
        if k in seen or k.count(0) > 4:
            continue
        seen.add(k)
        if dev_hash_ok(npd, k):
            print(f"{i:#x} {k.hex().upper()}")
            return
    print("not found")


if __name__ == "__main__":
    main(*sys.argv[1:3])
