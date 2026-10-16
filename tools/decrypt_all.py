import glob
import hashlib
import os
import sys
from find_klic import npd_header, dev_hash_ok
from unself import Self


def find_klic(npd, hay):
    seen = set()
    for i in range(len(hay) - 15):
        k = hay[i:i + 16]
        if k in seen or k.count(0) > 4:
            continue
        seen.add(k)
        if dev_hash_ok(npd, k):
            return k
    return None


def main(bin_dir):
    out = []
    for ver_dir in sorted(glob.glob(os.path.join(bin_dir, "*", ""))):
        ver = os.path.basename(os.path.dirname(ver_dir))
        eboot = os.path.join(ver_dir, "EBOOT.BIN")
        eboot_elf = Self(eboot).elf()
        open(os.path.join(ver_dir, "EBOOT.elf"), "wb").write(eboot_elf)
        out.append((ver, "EBOOT.elf", eboot_elf, None))
        for name in ("GOWA.SELF", "TLOUCOUNTDOWN.SELF"):
            src = os.path.join(ver_dir, name)
            if not os.path.exists(src):
                continue
            npd = npd_header(src)
            klic = find_klic(npd, eboot_elf)
            if klic is None:
                for other in out:
                    klic = find_klic(npd, other[2])
                    if klic:
                        break
            elf = Self(src, klic).elf()
            dst = name.replace(".SELF", ".elf")
            open(os.path.join(ver_dir, dst), "wb").write(elf)
            out.append((ver, dst, elf, klic))
    with open(os.path.join(bin_dir, "ELF_SHA256SUMS"), "w") as m:
        for ver, name, data, klic in out:
            m.write(f"{hashlib.sha256(data).hexdigest()}  {ver}/{name}\n")
            print(f"{ver} {name:22} {len(data):>9}  klic={klic.hex().upper() if klic else '-'}")


if __name__ == "__main__":
    main(sys.argv[1])
