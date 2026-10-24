import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ELF = (os.environ.get("GOWA_ELF") or os.path.join(ROOT, "bin", "01.12", "GOWA.patched.elf"))

JOBS = {
    "meshprep": dict(ea=0x74b100, size=0x4440, short="mp", entry=0x4030, code_end=0x7960),
    "primbuilder": dict(ea=0x6f8800, size=0xce30, short="pb", entry=0x104d8, code_end=0x10a00),
    "groupvis": dict(ea=0x793500, size=0x7080, short="gv", entry=0xa978, code_end=0xad40),
    "sortprim": dict(ea=0x6f4a00, size=0x2790, short="sp", entry=0x6508, code_end=0x6750),
}


def image(job):
    j = JOBS[job]
    d = open(ELF, "rb").read()
    return d[j["ea"] - 0x10000:j["ea"] - 0x10000 + j["size"]]
