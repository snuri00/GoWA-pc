#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RE="${GOWA_RE_TOOLS:-/d/re-tools}"
GHIDRA="$RE/ghidra_12.1.4_PUBLIC"
export JAVA_HOME="$(cygpath -w "$RE"/jdk-21*)"
export PATH="$RE/$(basename "$RE"/jdk-21*)/bin:$PATH"
PROJ="$RE/projects"
mkdir -p "$PROJ"
cmd="$1"
shift || true
HEADLESS="$GHIDRA/support/analyzeHeadless.bat"
case "$cmd" in
import)
    cmd //c "$(cygpath -w "$HEADLESS")" "$(cygpath -w "$PROJ")" gowa -import "$(cygpath -w "$ROOT/bin/01.12/GOWA.patched.elf")" -processor "PowerPC:BE:64:A2ALT-32addr" -overwrite -max-cpu 8
    ;;
decomp)
    out="${GOWA_DECOMP_OUT:-$ROOT/out/decomp}"
    mkdir -p "$out"
    cmd //c "$(cygpath -w "$HEADLESS")" "$(cygpath -w "$PROJ")" gowa -process GOWA.patched.elf -noanalysis -scriptPath "$(cygpath -w "$ROOT/tools/ghidra")" -postScript ExportDecomp.java "$(cygpath -w "$out")" --names "$(cygpath -w "$ROOT/docs/re/symbols.txt")" "$@"
    ;;
*)
    echo "usage: tools/ghidra.sh import | decomp <addr>... | decomp @listfile"
    exit 1
    ;;
esac
