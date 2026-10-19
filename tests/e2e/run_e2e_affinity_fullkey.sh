#!/usr/bin/env bash
# End to end (ctest: e2e_affinity_fullkey): worker-cache affinity must name the
# exact closure content. Build 1 compiles every TU on the worker (its cache and Bloom filter fill). Then:
#   A  a semantic edit of the header every TU includes; a FRESH client (empty client cache) rebuilds: no TU may get
#      Bloom affinity (bloom_jobs = 0); a filter keyed on the manifest key alone would "hit" every TU
#   B  an unchanged rebuild by a fresh client: every TU gets affinity and every one becomes a real worker-cache hit
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-aff.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json OPTICC_LOCAL_SLOTS=0
# the machine-wide system-include memo (content fingerprints): a test-local directory, filled once below
export OPTICC_SYSINC_MEMO_DIR=$T/sysmemo
# this test exercises worker-cache AFFINITY routing; a fresh client would otherwise be answered from the shared
# direct manifests (opticc/rdirect.hpp) before any scheduling, so they are switched off here (e2e_rdirect tests them)
export OPTICC_RDIRECT=off

start_coord() {
  touch "$T/coord.t0"; sleep 0.05
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
  # as a long-running setup starts it ahead of the build: the start-up pre-warm (stat walk + memo) finishes before the build. Once the memo
  # exists, wait for the pre-warm itself (it rewrites every driver's memo when its content fingerprint is ready); a
  # fixed 5 s was too short on a loaded host (the tracker runs at background QoS), and an unready fingerprint gives
  # no affinity at all, which also made check A pass vacuously
  if ls "$T/sysmemo"/*.memo2 > /dev/null 2>&1; then
    for _ in $(seq 1800); do [ -z "$(find "$T/sysmemo" -name '*.memo2' ! -newer "$T/coord.t0")" ] && break; sleep 0.1; done
    [ -z "$(find "$T/sysmemo" -name '*.memo2' ! -newer "$T/coord.t0")" ] || fail "the start-up pre-warm did not finish in 180 s"
  else sleep 5; fi
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
aff() { python3 -c "import json;s=json.load(open('$T/summary.json'));print(s['affinity']['$1'])"; }

S=$T/w; mkdir -p "$S/inc"
printf '#pragma once\n#define K 7\n' > "$S/inc/k.h"
for i in $(seq 0 7); do printf '#include "k.h"\nint f%d(int x) { return x * K + %d; }\n' "$i" "$i" > "$S/f$i.c"; done
printf 'OBJS := $(patsubst %%.c,%%.o,$(wildcard *.c))\nall: $(OBJS)\n%%.o: %%.c\n\t$(CC) -O2 -MD -MT $@ -MF $@.d -Iinc -c $< -o $@\nclean:\n\trm -f *.o *.d\n' > "$S/Makefile"
N=8; sleep 2.4
build() { ( cd "$S" && make -s clean && make -s -j4 CC="$OPTICC $CC_REAL" ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; }

export OPTICC_LOCAL_CACHE_DIR=$T/lc1; start_coord; build "build 1"
# a machine computes its system-include content fingerprint once (memo); wait for it as a long-lived client would
for _ in $(seq 1200); do ls "$T/sysmemo"/*.memo2 > /dev/null 2>&1 && break; sleep 0.25; done
ls "$T/sysmemo"/*.memo2 > /dev/null 2>&1 || fail "the system-include content memo was never written"
stop_coord    # build 1 filled the worker cache
# the worker's HELLO Bloom filter is taken when a coordinator connects: the next coordinators see these entries
printf '#pragma once\n#define K 9\n' > "$S/inc/k.h"; sleep 2.4                      # A: semantic header edit
export OPTICC_LOCAL_CACHE_DIR=$T/lc2; start_coord; build "A"; stop_coord
[ "$(aff unready)" -eq 0 ] || fail "A: $(aff unready) TUs had no system-include fingerprint (check would be vacuous)"
[ "$(aff probed)" -eq "$N" ] || fail "A: expected $N full-key probes, got $(aff probed)"
[ "$(aff bloom_jobs)" -eq 0 ] || fail "A: $(aff bloom_jobs) TUs got Bloom affinity after the header changed (stale holder)"
echo "A: header edited, fresh client: 0 Bloom-affinity jobs"
export OPTICC_LOCAL_CACHE_DIR=$T/lc3; start_coord; build "B"; stop_coord            # B: unchanged since A, fresh client
[ "$(aff bloom_jobs)" -eq "$N" ] || fail "B: expected $N Bloom-affinity jobs, got $(aff bloom_jobs) (probed $(aff probed), unready $(aff unready))"
[ "$(aff real_hits)" -eq "$N" ] || fail "B: expected $N real hits, got $(aff real_hits) (misses $(aff misses))"
echo "B: unchanged rebuild, fresh client: $N affinity jobs, $N real hits"
echo "PASS e2e_affinity_fullkey"
