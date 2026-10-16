import glob
import hashlib
import os
import re
import shutil
import sys
from pkg import Pkg


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main(pkg_dir, game_dir, bin_dir):
    pkgs = sorted(glob.glob(os.path.join(pkg_dir, "*.pkg")), key=lambda p: re.search(r"-A(\d{4})-", p).group(1))
    manifest = open(os.path.join(bin_dir, "SHA256SUMS"), "a")
    for path in pkgs:
        ver = re.search(r"-A(\d\d)(\d\d)-", path)
        ver = f"{ver.group(1)}.{ver.group(2)}"
        bins = []
        Pkg(path).extract(game_dir, lambda n, s: bins.append(n) if n.upper().endswith((".SELF", ".BIN", ".SPRX")) else None)
        snap = os.path.join(bin_dir, ver)
        os.makedirs(snap, exist_ok=True)
        for n in bins:
            dst = os.path.join(snap, os.path.basename(n))
            shutil.copy2(os.path.join(game_dir, n), dst)
            manifest.write(f"{sha256(dst)}  {ver}/{os.path.basename(n)}\n")
        print(ver, "installed", ", ".join(os.path.basename(n) for n in bins))
    manifest.close()


if __name__ == "__main__":
    main(*sys.argv[1:4])
