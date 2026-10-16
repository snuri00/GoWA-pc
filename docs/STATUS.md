# GOWA-pc status

## Phase 0 — inventory and oracle (done, except gameplay-driven items)

- Target: BCES01741 disc + updates 01.04–01.12 (all nine Sony packages, SHA-1 verified).
- `GOWA.SELF` 1.12 → `PPU-3a0b43e4a5f4bfea64f53612ee7c5d990f88129c`, 1.04 → `PPU-3a63ecc6…` (match RPCS3 patch database).
- GOWA 1.12: 25 import modules / 350 functions, SDK `0x430001`, 7643 OPD entries, single TOC `0x83f2b0`.
- SPU code is fully static: 49 images / 1251 functions inside GOWA, 9 images / 57 functions in libsre; PSARC data contains no SPU code.
- Open: RPCS3 baseline fps/trace logs and SPU cache collection (need gameplay).

## Phase 1 — PPU lifter and build pipeline (done)

| Exit criterion | Result |
| --- | --- |
| All functions lifted and compiled, no unknown opcode | 19074 functions, 0 unimplemented mnemonics, 62/62 jump tables resolved, 0 unresolved `bctr` |
| PPU op conformance vs RPCS3 interpreter | 142 mnemonics, 2783 real GOWA encodings, 44528 states, 0 mismatches |
| Full and incremental build times | full 78 s, one chunk + link 11.5 s (i7-8700K, clang-cl 19.1.5, `-O1`) |

Conformance oracle: RPCS3 0.0.43-20284-2ba2f75b, PPU interpreter, accurate NJ / vector NaN / FPCC / SAT enabled.

Known, deliberate deviations from the oracle:

- Scalar FP NaN results follow PPC hardware (operand priority A, B, C; default NaN `0x7FF8000000000000`; no sign flip from internal negation). RPCS3 inherits x86 NaN rules here; 217 of 44528 states differ only in NaN sign/payload.
- FPSCR status bits (FPRF, exception bits) are not modelled; `mffs` is excluded from conformance. GOWA executes `mffs` at one site.
- VSCR.SAT is not tracked; GOWA never executes `mfvscr`. RPCS3 also disables SAT by default.
- RPCS3 reads and writes VSCR in vector word 0; the port follows the ISA (word 3).

Memory, branch, reservation and system instructions are not covered by the conformance harness yet; they are validated through the runtime.

## Generated, untracked inputs

- `ref/nid_db.json` — `tools/nid_db.py` over `ref/rpcs3` at `2ba2f75`.
- `out/include/ppu/estimate_tables.h` — `tools/extract_estimate_tables.py` over the same checkout (GPL-derived, never committed).
- `out/ppu/` — `recomp/ppu/codegen.py bin/01.12/GOWA.elf ref/nid_db.json out/ppu`.
