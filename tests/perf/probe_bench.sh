#!/usr/bin/env bash
# Micro-benchmark: wall time per kbuild-style compiler probe: plain compiler, opticc probe-cache hit, ccache
# (when installed) on its second call. Usage: tests/perf/probe_bench.sh [build dir] [N]
set -u
B=${1:-$(cd "$(dirname "$0")/../.." && pwd)/build}; N=${2:-200}
OPTICC=$B/src/opticc/opticc; CC_REAL=${CC_REAL:-$(command -v cc)}
T=$(mktemp -d /tmp/opticc-probe-bench.XXXXXX); mkdir -p "$T/lc/state" "$T/w/.tmp_1"; printf '%s' "$T/lc" > "$T/c.sock.cache"
export OPTICC_SOCK=$T/c.sock CCACHE_DIR=$T/ccache
cd "$T/w" || exit 1
ARGS=(-Werror -O2 -fno-strict-aliasing -Wno-address-of-packed-member -c -x c /dev/null -o .tmp_1/tmp)
ms() { python3 -c 'import time;print(time.time())'; }
bench() { local name=$1; shift; "$@" >/dev/null 2>&1; local t0; t0=$(ms); for _ in $(seq "$N"); do "$@" >/dev/null 2>&1; done
  python3 -c "import sys;print(f'{sys.argv[1]:28s} {(float(sys.argv[3])-float(sys.argv[2]))/int(sys.argv[4])*1000:7.2f} ms/probe')" "$name" "$t0" "$(ms)" "$N"; }
bench "compiler (no cache)" "$CC_REAL" "${ARGS[@]}"
bench "opticc probe-cache hit" "$OPTICC" "$CC_REAL" "${ARGS[@]}"
command -v ccache >/dev/null && bench "ccache hit" ccache "$CC_REAL" "${ARGS[@]}"
bench "compiler --version" "$CC_REAL" --version
bench "opticc --version hit" "$OPTICC" "$CC_REAL" --version
rm -rf "$T"
