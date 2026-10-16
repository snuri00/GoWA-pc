import json
import struct
import sys

R_ADDR32, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA, R_REL24, R_REL14, R_ADDR64, R_REL64, R_ADDR16_LO_DS = 1, 4, 5, 6, 10, 11, 38, 44, 57


def relocate(src, base):
    d = bytearray(open(src, "rb").read())
    if struct.unpack(">H", d[0x10:0x12])[0] != 0xFFA4:
        raise ValueError("not a PRX")
    phoff = struct.unpack(">Q", d[0x20:0x28])[0]
    phes, phn = struct.unpack(">HH", d[0x36:0x3A])
    phdrs = []
    for i in range(phn):
        off = phoff + i * phes
        phdrs.append(list(struct.unpack(">IIQQQQQQ", d[off:off + 56])) + [off])
    loads = [p for p in phdrs if p[0] == 1]
    seg_addr = [base + p[3] for p in loads]
    seg_file = [p[2] for p in loads]
    seg_vaddr = [p[3] for p in loads]

    def file_off(addr):
        for i, p in enumerate(loads):
            if seg_addr[i] <= addr < seg_addr[i] + p[5]:
                return seg_file[i] + addr - seg_addr[i]
        return None

    counts = {}
    for p in phdrs:
        if p[0] != 0x700000A4:
            continue
        rel = d[p[2]:p[2] + p[5]]
        for i in range(0, len(rel), 24):
            offset, _, index_value, index_addr, rtype, addend = struct.unpack(">QHBBIQ", rel[i:i + 24])
            raddr = seg_addr[index_addr] + offset
            rdata = (0 if index_value == 0xFF else seg_addr[index_value]) + addend
            fo = file_off(raddr)
            counts[rtype] = counts.get(rtype, 0) + 1
            if fo is None:
                continue
            if rtype == R_ADDR32:
                struct.pack_into(">I", d, fo, rdata & 0xFFFFFFFF)
            elif rtype == R_ADDR16_LO:
                struct.pack_into(">H", d, fo, rdata & 0xFFFF)
            elif rtype == R_ADDR16_HI:
                struct.pack_into(">H", d, fo, (rdata >> 16) & 0xFFFF)
            elif rtype == R_ADDR16_HA:
                struct.pack_into(">H", d, fo, ((rdata >> 16) + (1 if rdata & 0x8000 else 0)) & 0xFFFF)
            elif rtype == R_REL24:
                w = struct.unpack_from(">I", d, fo)[0]
                v = ((rdata - raddr) & 0x03FFFFFC)
                struct.pack_into(">I", d, fo, (w & 0xFC000003) | v)
            elif rtype == R_REL14:
                w = struct.unpack_from(">I", d, fo)[0]
                v = ((rdata - raddr) & 0xFFFC)
                struct.pack_into(">I", d, fo, (w & 0xFFFF0003) | v)
            elif rtype == R_ADDR64:
                struct.pack_into(">Q", d, fo, rdata & 0xFFFFFFFFFFFFFFFF)
            elif rtype == R_REL64:
                struct.pack_into(">Q", d, fo, (rdata - raddr) & 0xFFFFFFFFFFFFFFFF)
            elif rtype == R_ADDR16_LO_DS:
                h = struct.unpack_from(">H", d, fo)[0]
                struct.pack_into(">H", d, fo, (h & 3) | (rdata & 0xFFFC))
            else:
                raise ValueError(f"unknown relocation type {rtype}")
    for p in loads:
        struct.pack_into(">Q", d, p[8] + 0x10, base + p[3])
    mi = loads[0][4]
    attr, version = struct.unpack(">HH", d[mi:mi + 4])
    name = d[mi + 4:mi + 32].split(b"\0")[0].decode()
    toc, ent, ent_end, stub, stub_end = struct.unpack(">5I", d[mi + 32:mi + 52])
    start = stop = None
    ea = ent
    while ea < ent_end:
        fo = file_off(ea)
        size = d[fo]
        attr_e = struct.unpack(">H", d[fo + 4:fo + 6])[0]
        nf, nv, nt = struct.unpack(">HHH", d[fo + 6:fo + 12])
        name_p, nid_p, addr_p = struct.unpack(">3I", d[fo + 0x10:fo + 0x1C])
        if attr_e & 0x8000:
            for k in range(nf + nv + nt):
                nid = struct.unpack(">I", d[file_off(nid_p + 4 * k):file_off(nid_p + 4 * k) + 4])[0]
                opd = struct.unpack(">I", d[file_off(addr_p + 4 * k):file_off(addr_p + 4 * k) + 4])[0]
                if nid == 0xBC9A0086:
                    start = opd
                elif nid == 0xAB779874:
                    stop = opd
        ea += size
    entry = start or 0
    struct.pack_into(">Q", d, 0x18, entry)
    return bytes(d), {"name": name, "base": base, "toc": toc, "module_start": start, "module_stop": stop,
                      "segments": [[a, p[5], p[6], p[1]] for a, p in zip(seg_addr, loads)], "relocations": counts}


def main(layout_json, src_dir, out_dir):
    layout = json.load(open(layout_json))
    manifest = []
    for mod in layout:
        data, info = relocate(f"{src_dir}/{mod['file']}", int(mod["base"], 16))
        info["file"] = mod["file"].replace(".prx", ".elf")
        open(f"{out_dir}/{info['file']}", "wb").write(data)
        manifest.append(info)
        print(f"{info['name']:24} base {info['base']:#x} toc {info['toc']:#x} start {info['module_start'] and hex(info['module_start'])} relocs {info['relocations']}")
    json.dump(manifest, open(f"{out_dir}/manifest.json", "w"), indent=1)


if __name__ == "__main__":
    main(*sys.argv[1:4])
