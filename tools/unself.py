import json
import os
import struct
import sys
import zlib
from Crypto.Cipher import AES
from Crypto.Util import Counter

NP_KLIC_KEY = bytes.fromhex("F2FBCA7A75B04EDC1390638CCDFDD1EE")
NP_KLIC_FREE = bytes.fromhex("72F990788F9CFF745725F08E4C128387")
SELF_TABLES = {4: "APP", 8: "NPDRM", 1: "LV0", 2: "LV1", 3: "LV2", 5: "ISO", 6: "LDR", 7: "UNK7"}
KEYS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "keys", "self_keys.json")


def ctr(key, iv, data):
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, "big"))).decrypt(data)


def find_key(self_type, revision, version):
    table = SELF_TABLES[self_type]
    cands = [k for k in json.load(open(KEYS_PATH)) if k["table"] == table and k["revision"] == revision]
    for k in cands:
        if int(k["ver_start"], 16) <= version <= int(k["ver_end"], 16):
            return k
    if len(cands) == 1:
        return cands[0]
    raise KeyError(f"no key for {table} rev {revision:#x} ver {version:#x}")


class Self:
    def __init__(self, path, klic=None):
        self.d = open(path, "rb").read()
        d = self.d
        if d[:4] != b"SCE\0":
            raise ValueError("not a SELF")
        self.key_revision, self.header_type, self.meta_off = struct.unpack(">HHI", d[8:16])
        self.header_len, self.data_len = struct.unpack(">QQ", d[16:32])
        ext = struct.unpack(">10Q", d[0x20:0x70])
        self.appinfo_off, self.elf_off, self.phdr_off, self.shdr_off = ext[1:5]
        self.auth_id, self.vendor_id, self.self_type, self.app_version = struct.unpack(">QIIQ", d[self.appinfo_off:self.appinfo_off + 24])
        self.klic = klic
        self.npd = None
        i = d.find(b"NPD\0", 0, self.header_len)
        if self.self_type == 8 and i > 0:
            self.npd = d[i:i + 0x80]
            if self.klic is None:
                self.klic = NP_KLIC_FREE

    def metadata(self):
        d = self.d
        start = self.meta_off + 0x20
        info = d[start:start + 0x40]
        if self.self_type == 8:
            npdrm_key = AES.new(NP_KLIC_KEY, AES.MODE_ECB).decrypt(self.klic)
            info = AES.new(npdrm_key, AES.MODE_CBC, b"\0" * 16).decrypt(info)
        k = find_key(self.self_type, self.key_revision, self.app_version)
        info = AES.new(bytes.fromhex(k["erk"]), AES.MODE_CBC, bytes.fromhex(k["riv"])).decrypt(info)
        if any(info[16:32]) or any(info[48:64]):
            raise ValueError("metadata info decryption failed (wrong klic or key)")
        hdrs = ctr(info[0:16], info[32:48], d[start + 0x40:self.header_len])
        _, _, section_count, key_count, _ = struct.unpack(">QIIII", hdrs[:0x18])
        sections = [struct.unpack(">QQIIIIIIII", hdrs[0x20 + i * 0x30:0x50 + i * 0x30]) for i in range(section_count)]
        base = 0x20 + section_count * 0x30
        keys = [hdrs[base + i * 16:base + i * 16 + 16] for i in range(key_count)]
        return sections, keys

    def elf(self):
        d = self.d
        sections, keys = self.metadata()
        ehdr = d[self.elf_off:self.elf_off + 0x40]
        if ehdr[4] != 2:
            raise ValueError("only ELF64 supported")
        e_phoff, e_shoff = struct.unpack(">QQ", ehdr[0x20:0x30])
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack(">HHHH", ehdr[0x36:0x3E])
        phdrs = d[self.phdr_off:self.phdr_off + e_phnum * e_phentsize]
        out = bytearray()

        def put(off, data):
            if len(out) < off + len(data):
                out.extend(b"\0" * (off + len(data) - len(out)))
            out[off:off + len(data)] = data

        put(0, ehdr)
        put(e_phoff, phdrs)
        for off, size, typ, idx, _, _, enc, kidx, ividx, comp in sections:
            if typ != 2:
                continue
            data = d[off:off + size]
            if enc == 3:
                data = ctr(keys[kidx], keys[ividx], data)
            if comp == 2:
                data = zlib.decompress(data)
            p_offset = struct.unpack(">Q", phdrs[idx * e_phentsize + 8:idx * e_phentsize + 16])[0]
            put(p_offset, data)
        if self.shdr_off and e_shnum:
            put(e_shoff, d[self.shdr_off:self.shdr_off + e_shnum * e_shentsize])
        return bytes(out)


if __name__ == "__main__":
    src = sys.argv[1]
    klic = bytes.fromhex(sys.argv[3]) if len(sys.argv) > 3 else None
    s = Self(src, klic)
    data = s.elf()
    dst = sys.argv[2]
    open(dst, "wb").write(data)
    print(f"{src} -> {dst} type {s.self_type} rev {s.key_revision:#x} size {len(data)}")
