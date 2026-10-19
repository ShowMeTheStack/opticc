#!/usr/bin/env bash
# End to end (ctest: e2e_writeback): client-local compile results are written back into the worker's compile
# cache (CACHE_PUT), so a FRESH client (empty client cache) finds every TU at the worker.
#   1 the worker is down: the cold build compiles every TU on the client (late admission keeps retrying it)
#   2 the worker comes up on the same address; the coordinator admits it and uploads every local result
#   3 the coordinator exits (draining uploads); a NEW coordinator with an EMPTY client cache rebuilds with no local
#     slots: every TU must be a worker-cache hit, and every object identical to a plain build's
#   4 a worker started with --no-cache-uploads stores nothing (refused uploads are counted, nothing is served)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-wb.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {   # $1: listen address, rest: extra flags
  rm -f "$T/worker.out" "$T/worker.err"   # old logs must not satisfy the readiness loop (the child truncates them only after fork)
  local addr=$1; shift
  "$OPTICCD" --listen "$addr" --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" "$@" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
}
start_worker 127.0.0.1:0
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
# this test exercises the WRITE-BACK (CACHE_PUT -> COMPILE_HP hits); a fresh client would otherwise be answered
# (partly or wholly) from the shared direct manifests published by the same cold build, so they are off here (e2e_rdirect)
export OPTICC_RDIRECT=off

fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$T/summary.json" "$p"; python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; # wait for the PROCESS to exit (it may unlink the socket before its <=30 s drain of released jobs and uploads)
  for _ in $(seq 900); do { [ -n "$p" ] && kill -0 "$p" 2>/dev/null; } || { [ -S "$OPTICC_SOCK" ] && [ -z "$p" ]; } || break; sleep 0.1; done; sleep 0.3; }

S=$T/w; mkdir -p "$S/src/inc"
printf '#pragma once\n#define K 7\nstatic inline int k(int x) { return x * K; }\n' > "$S/src/inc/k.h"
for i in $(seq 0 9); do printf '#include "k.h"\nint f%d(int x) { return k(x) + %d; }\n' "$i" "$i" > "$S/src/f$i.c"; done
cat > "$S/src/Makefile" <<'M'
OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
all: $(OBJS)
%.o: %.c
	$(CC) -O2 -MD -MT $@ -MF $@.d -Iinc -c $< -o $@
clean:
	rm -f *.o *.d
M
cp -r "$S/src" "$S/ref"; cp -r "$S/src" "$S/dist"; rm -rf "$S/src"; N=10
sleep 2.4   # inputs older than any racy margin
( cd "$S/ref" && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"
same_objects() { for o in "$S/ref"/*.o; do cmp -s "$o" "$S/dist/$(basename "$o")" || fail "$1: object differs: $(basename "$o")"; done; }

# 1 worker down: everything local
start_coord
( cd "$S/dist" && make -s -j4 CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "cold build: $(cat "$S/out.txt")"
same_objects "1 cold"
[ "$(summ "s['jobs_local']")" -eq "$N" ] || fail "1 cold: expected $N local jobs, got $(summ "s['jobs_local']")"
echo "1 cold, worker down: $N/$N local, objects identical; uploads queued $(summ "s['v4']['uploads']['queued']")"
# 2 worker up on the same address: admitted, uploads sent
start_worker "127.0.0.1:$PORT"
for _ in $(seq 120); do [ "$(summ "s['v4']['uploads']['sent']")" -ge "$N" ] && break; sleep 0.5; done
[ "$(summ "s['v4']['uploads']['sent']")" -ge "$N" ] || fail "2: uploads not sent: $(summ "s['v4']['uploads']"); $(tail -5 "$T/coord.err")"
echo "2 worker admitted: uploads $(summ "s['v4']['uploads']")"
stop_coord
# the worker's first upload waits for its system-include content fingerprint (one-time; seconds on a macOS SDK)
for _ in $(seq 600); do [ "$(grep -c '"outcome":"stored"' "$T/worker.err")" -ge "$N" ] && break; sleep 0.1; done
[ "$(grep -c '"outcome":"stored"' "$T/worker.err")" -eq "$N" ] || fail "2: worker stored $(grep -c '"outcome":"stored"' "$T/worker.err") of $N uploads: $(grep cache-put "$T/worker.err" | tail -3)"
# 3 fresh client: empty client cache, no local slots: every TU a worker-cache hit
rm -rf "$T/lc"; ( cd "$S/dist" && make -s clean )
export OPTICC_LOCAL_SLOTS=0
start_coord
( cd "$S/dist" && make -s -j4 CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "fresh build: $(cat "$S/out.txt")"
same_objects "3 fresh"
HITS=$(summ "sum(1 for j in s['jobs'] if j.get('cache') == 'hit')")
[ "$HITS" -eq "$N" ] || fail "3 fresh: expected $N worker-cache hits, got $HITS: $(summ "[(j['src'][-6:], j['outcome'], j.get('cache')) for j in s['jobs']]")"
echo "3 fresh client: $HITS/$N worker-cache hits, objects identical"
stop_coord
# 4 a worker that refuses uploads stores nothing (same sequence as 1-2: worker down for the build, then up)
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=; rm -rf "$T/wcache" "$T/lc"; ( cd "$S/dist" && make -s clean )
unset OPTICC_LOCAL_SLOTS
start_coord
( cd "$S/dist" && make -s -j4 CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "4 build: $(cat "$S/out.txt")"
start_worker "127.0.0.1:$PORT" --no-cache-uploads
for _ in $(seq 120); do [ "$(summ "s['v4']['uploads']['sent']")" -ge "$N" ] && break; sleep 0.5; done
stop_coord
refused_cp() { grep '"event":"cache-put"' "$T/worker.err" | grep -c '"outcome":"refused"'; }   # DIRECT_PUTs are refused too (own event)
for _ in $(seq 50); do [ "$(refused_cp)" -ge "$N" ] && break; sleep 0.1; done
grep -q '"outcome":"stored"' "$T/worker.err" && fail "4: a worker started with --no-cache-uploads stored an upload"
[ "$(refused_cp)" -eq "$N" ] || fail "4: expected $N refused uploads, got $(refused_cp)"
echo "4 --no-cache-uploads: $N uploads refused, none stored"
echo "E2E-WRITEBACK PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
