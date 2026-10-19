#!/usr/bin/env bash
# End to end (ctest: e2e_affinity_fresh): a FRESH client (empty
# client cache, EMPTY system-include memo, new coordinator) rebuilds a tree that two loopback workers have just
# compiled. Every TU must be probed with its full content key: unready = 0 (no request lost its affinity because the
# fingerprint was not ready); a cold coordinator must not leave the fingerprint to a background-priority thread.
# Prints the sysinc readiness counters and
# the affinity outcomes (misses is reported, not asserted: placement of Bloom hits is a separate matter).
# OPTICC_TEST_BURN=<n> adds n CPU burners for the fresh build (a loaded host).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-afr.XXXXXX)
BURN=""
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { for p in ${W1:-} ${W2:-} $BURN; do kill "$p" 2>/dev/null; done; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp-1" --object-cache "$T/wcache-1" > "$T/worker-1.out" 2> "$T/worker-1.err" & W1=$!
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp-2" --object-cache "$T/wcache-2" > "$T/worker-2.out" 2> "$T/worker-2.err" & W2=$!
HOSTS=""
for n in 1 2; do
  for _ in $(seq 300); do grep -q listening "$T/worker-$n.out" 2>/dev/null && break; sleep 0.1; done
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker-$n.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker-$n.out")
  [ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker $n did not start: $(cat "$T/worker-$n.err")"
  HOSTS="$HOSTS${HOSTS:+,}127.0.0.1:$PORT#$PUB"
done
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="$HOSTS" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json OPTICC_LOCAL_SLOTS=0
# the shared direct manifests serve a fresh client before any coordinator job exists (every TU an rdirect hit, none
# probed for affinity), so this test of the AFFINITY path switches the fast path off, as e2e_affinity_fullkey does
# (shared manifests are covered by e2e_rdirect)
export OPTICC_RDIRECT=off

start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 1200); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
q() { python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }

S=$T/w; mkdir -p "$S/inc"; N=${OPTICC_TEST_TUS:-160}
printf '#pragma once\n#define K 7\n' > "$S/inc/k.h"
for i in $(seq 1 "$N"); do
  { printf '#include "k.h"\n'; for f in 0 1 2 3 4 5; do printf 'int f%d_%d(int x) { int s = 0; for (int i = 0; i < x; ++i) s += (i * K + %d) ^ (s >> 3); return s; }\n' "$i" "$f" "$f"; done; } > "$S/t$i.c"
done
printf 'OBJS := $(patsubst %%.c,%%.o,$(wildcard *.c))\nall: $(OBJS)\n%%.o: %%.c\n\t$(CC) -O2 -MD -MT $@ -MF $@.d -Iinc -c $< -o $@\nclean:\n\trm -f *.o *.d\n' > "$S/Makefile"
sleep 2.4
build() { ( cd "$S" && make -s clean && make -s -j16 CC="$OPTICC $CC_REAL" ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; }

# clean build: fills both workers' caches
export OPTICC_LOCAL_CACHE_DIR=$T/lc1 OPTICC_SYSINC_MEMO_DIR=$T/sysmemo1; start_coord; build "clean"; stop_coord
echo "clean: affinity $(q "s['affinity']")"
for _ in $(seq "${OPTICC_TEST_BURN:-0}"); do ( while :; do :; done ) & BURN="$BURN $!"; done
# fresh client: empty client cache, empty memo, new coordinator; the build starts as soon as the socket exists (the
# worst case: a coordinator started ahead of the build normally has the fingerprint ready before the build)
export OPTICC_LOCAL_CACHE_DIR=$T/lc2 OPTICC_SYSINC_MEMO_DIR=$T/sysmemo2; start_coord; build "fresh"; stop_coord
for p in $BURN; do kill "$p" 2>/dev/null; done; BURN=""
echo "fresh: affinity $(q "s['affinity']")"
echo "fresh: sysinc ready_at_s $(q "s['sysinc']['ready_at_s']") scan_s $(q "s['sysinc']['scan_s']") requests_before_ready $(q "s['sysinc']['requests_before_ready']") wait_s $(q "s['sysinc']['wait_s']") wait_timeouts $(q "s['sysinc']['wait_timeouts']")"
[ "$(q "s['affinity']['unready']")" -eq 0 ] || fail "fresh: $(q "s['affinity']['unready']") requests got no affinity because the system-include fingerprint was not ready"
[ "$(q "s['affinity']['probed']")" -ge $((N * 9 / 10)) ] || fail "fresh: only $(q "s['affinity']['probed']") of $N TUs were probed with the full key"
[ "$(q "s['affinity']['bloom_jobs'] + s['affinity']['index_jobs']")" -ge $((N * 9 / 10)) ] || fail "fresh: only $(q "s['affinity']['bloom_jobs']") of $N TUs got Bloom affinity"
[ "$(q "s['sysinc']['wait_timeouts']")" -eq 0 ] || fail "fresh: $(q "s['sysinc']['wait_timeouts']") requests timed out waiting for the fingerprint"
[ "$(python3 -c "import json;s=json.load(open('$T/summary.json'));print(1 if s['sysinc']['ready_at_s'] >= 0 else 0)")" -eq 1 ] || fail "fresh: the fingerprint never became ready"
echo "PASS e2e_affinity_fresh"
