import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import spu_blocks

GOWA_SHA256 = {
    "1cb4f2533721acde768b7ad0c65ea4b3424f32c774c51daa35f3dac6847a0c18": "GOWA 01.12 patched",
}

GENERATORS = (
    ("edge", "edge", ["tests/edge/spu_cgen.py", "73e0,d618", "{gen}/edge/edge_cull_gen.cpp"]),
    ("edge_softbody", "edge", ["tests/edge/sb_cgen.py", "{gen}/edge/edge_softbody_gen.inc"]),
    ("edge_ovl", "edge", ["tests/edge/ovl_cgen.py", "{gen}/edge/edge_ovl_gen.cpp", "{gen}/edge/edge_ovl_gen.h"]),
    ("renjob", "renjob", ["tests/renjob/rj_cgen.py", "@tests/renjob/roots.txt", "{gen}/renjob/renjob_gen.cpp", "{gen}/renjob/renjob_gen.h"]),
    ("scream", "scream", ["tests/scream/sc_cgen.py", "all"]),
    ("scream_job", "scream", ["tests/scream/sc_job_cgen.py"]),
    ("cloth", "cloth", ["tests/cloth_shadows/cs_cgen.py", "cloth", "gen", "@tests/cloth_shadows/roots_cloth.txt", "{gen}/cloth/cloth_gen.cpp", "{gen}/cloth/cloth_gen.h"]),
    ("shadows", "shadows", ["tests/cloth_shadows/cs_cgen.py", "shadows", "gen", "@tests/cloth_shadows/roots_shadows.txt", "{gen}/shadows/shadows_gen.cpp", "{gen}/shadows/shadows_gen.h"]),
    ("meshprep", "meshprep", ["tests/meshprep/mp_cgen.py", "runtime"]),
    ("pm", "pm", ["tests/pm/pm_cgen.py", "{gen}/pm/pm_gen.cpp", "{gen}/pm/pm_gen.h"]),
    ("edgejob", "edgejob", ["tests/edgejob/ej_cgen.py", "{gen}/edgejob/ej_gen.cpp", "{gen}/edgejob/ej_gen.h"]),
    ("meshhier", "meshhier", ["tests/meshhier/mh_cgen.py", "{gen}/meshhier/mh_gen.cpp", "{gen}/meshhier/mh_gen.h"]),
    ("anim", "anim", ["tests/anim/an_cgen.py", "{gen}/anim/an_gen.cpp", "{gen}/anim/an_gen.h"]),
    ("tasks", "tasks", ["tests/tasks/task_cgen.py"]),
    ("mpjob", "mpjob", ["tests/mpjob/mpj_cgen.py", "{gen}/mpjob"]),
)

STAMP = "generated.txt"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def log(msg):
    print("[setup] " + msg, flush=True)


def run(argv, env=None, quiet=True):
    r = subprocess.run([sys.executable] + argv, cwd=ROOT, env=env, stdout=subprocess.PIPE if quiet else None, stderr=subprocess.PIPE if quiet else None, text=True)
    if r.returncode:
        if quiet:
            sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
        raise SystemExit("command failed: %s" % " ".join(argv))
    return r


def step_patch(args):
    if os.path.exists(args.elf):
        return
    src = os.path.join(os.path.dirname(args.elf), "GOWA.elf")
    if not os.path.exists(src):
        raise SystemExit("missing %s and %s: decrypt your GOWA.SELF first (tools/decrypt_all.py)" % (args.elf, src))
    log("patching %s" % src)
    run(["tools/apply_patches.py", "recomp/patches.json"], quiet=False)


def step_check_elf(args):
    digest = sha256(args.elf)
    name = GOWA_SHA256.get(digest)
    if name:
        log("ELF %s: %s" % (args.elf, name))
    elif args.allow_unknown:
        log("ELF %s: unknown sha256 %s, continuing" % (args.elf, digest))
    else:
        raise SystemExit("ELF %s has unknown sha256 %s (expected the patched 01.12 GOWA.elf); use --allow-unknown to force" % (args.elf, digest))


def step_reloc(args):
    reloc = os.path.join(args.fw, "reloc")
    if os.path.exists(os.path.join(reloc, "manifest.json")):
        return
    os.makedirs(reloc, exist_ok=True)
    log("relocating firmware modules into %s" % reloc)
    run(["tools/prx_relocate.py", "recomp/lle_layout.json", args.fw, reloc], quiet=False)


def step_ppu(args):
    out = os.path.join(args.out_root, "ppu")
    if not os.path.exists(args.nid_db):
        raise SystemExit("missing NID database %s (tools/nid_db.py)" % args.nid_db)
    log("PPU recompilation into %s" % out)
    run(["recomp/ppu/codegen.py", "recomp/images.json", args.nid_db, out], quiet=False)


def step_blocks(args):
    path = os.path.join(args.gen_root, "blocks", "run_manifest.bin")
    n = spu_blocks.build(spu_blocks.MANIFEST, args.elf, args.fw, path)
    log("%d SPU blocks extracted into %s" % (n, path))
    return os.path.dirname(path)


def step_spu_aot(args, blocks_dir):
    out = os.path.join(args.out_root, "spu")
    log("SPU AOT codegen into %s" % out)
    run(["recomp/spu/codegen.py", out, os.path.join(blocks_dir, "run*.bin")], quiet=False)


def step_natives(args, blocks_dir):
    gen = os.path.join(args.gen_root, "spu")
    env = dict(os.environ, GOWA_ELF=os.path.abspath(args.elf), GOWA_SPU_BLOCKS=os.path.abspath(blocks_dir), GOWA_GEN_DIR=os.path.abspath(gen))
    stamp = os.path.join(gen, STAMP)
    if os.path.exists(stamp):
        os.remove(stamp)
    want = set(args.only.split(",")) if args.only else None
    for name, sub, argv in GENERATORS:
        if want and name not in want:
            continue
        os.makedirs(os.path.join(gen, sub), exist_ok=True)
        t0 = time.time()
        run([a.replace("{gen}", gen) for a in argv], env=env)
        log("generated %-10s (%.1f s)" % (name, time.time() - t0))
    files = []
    for d, _, names in os.walk(gen):
        for n in names:
            if n.endswith((".cpp", ".h", ".inc")):
                p = os.path.join(d, n)
                files.append((os.path.relpath(p, gen).replace("\\", "/"), sha256(p)))
    files.sort()
    open(stamp, "w", newline="\n").write("".join("%s %s\n" % (h, p) for p, h in files))
    log("%d native SPU sources in %s" % (len(files), gen))


def main():
    ap = argparse.ArgumentParser(description="Generate all game-derived sources from your own game files")
    ap.add_argument("--elf", default=os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"), help="patched decrypted GOWA 01.12 ELF")
    ap.add_argument("--fw", default=os.path.join(ROOT, "fw", "4.91"), help="directory with decrypted firmware .prx modules")
    ap.add_argument("--nid-db", default=os.path.join(ROOT, "ref", "nid_db.json"))
    ap.add_argument("--out-root", default=os.path.join(ROOT, "out"), help="root for out/ppu and out/spu")
    ap.add_argument("--gen-root", default=os.path.join(ROOT, "out", "gen"), help="root for extracted blocks and generated SPU natives")
    ap.add_argument("--steps", default="spu", help="comma list of: ppu, aot, natives, spu (=aot,natives), all")
    ap.add_argument("--only", default="", help="comma list of native generator names")
    ap.add_argument("--allow-unknown", action="store_true")
    args = ap.parse_args()
    steps = set(args.steps.split(","))
    if "spu" in steps:
        steps |= {"aot", "natives"}
    if "all" in steps:
        steps |= {"ppu", "aot", "natives"}
    step_patch(args)
    step_check_elf(args)
    if "ppu" in steps:
        step_reloc(args)
        step_ppu(args)
    if steps & {"aot", "natives"}:
        blocks_dir = step_blocks(args)
        if "aot" in steps:
            step_spu_aot(args, blocks_dir)
        if "natives" in steps:
            step_natives(args, blocks_dir)
    log("done")


if __name__ == "__main__":
    main()
