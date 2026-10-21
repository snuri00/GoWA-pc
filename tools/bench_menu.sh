#!/usr/bin/env bash
set -e
cd "$(dirname "$0")/../run"
rm -f bench_trace.bin bench_trace.bin.str
pad=$(for t in $(seq 10 4 26); do printf "cross@%d," $t; done)
GOWA_EVTRACE=import GOWA_EVTRACE_FILE=bench_trace.bin GOWA_EVTRACE_AT=55 GOWA_EVTRACE_END=90 GOWA_AUTOPAD="${pad%,}" GOWA_ROOT="$(cd .. && pwd)" timeout 95 ../build/gowa.exe > /dev/null 2>&1 < /dev/null || true
grep "rsx: flip" gowa.log | sed -E 's/^. +([0-9.]+) .*rsx: flip ([0-9]+).*/\1 \2/' | awk '$1 >= 55 && $1 <= 92' | awk 'NR == 1 {t0 = $1; f0 = $2} {t = $1; f = $2} END {if (t > t0) printf "menu: %.2f flips/s over %.1fs\n", (f - f0) / (t - t0), t - t0}'
python ../tools/trace.py --file run/bench_trace.bin top import --limit 40 | awk '/smvd2DecodePicture2|cellVdecGetPicture /{printf "%s: %d calls, %.1f ms avg\n", $1, $2, $4 / $2}'
