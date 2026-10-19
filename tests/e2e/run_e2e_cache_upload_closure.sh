#!/usr/bin/env bash
# End to end (ctest: e2e_cache_upload_closure): the write-back must vouch only for closure hashes taken
# AFTER the compile. A test hook (OPTICC_TEST_UPLOAD_STALE_DEPS=1) replaces the closure hashes the upload carries by
# values that no longer describe the files (as a scan-time hash of a header edited before the compile would be). The
# upload must then be refused, and the worker must store nothing. Control session without the hook: the upload is
# stored, so the check is not vacuous. Without the re-hash the stale-keyed upload would be stored and the test fails.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cacheu.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {
  "$OPTICCD" --listen "$1" --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" >> "$T/worker.out" 2>> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
}
stop_worker() { kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=; : > "$T/worker.out"; }
start_worker 127.0.0.1:0
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
stop_worker
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; # wait for the PROCESS to exit (it may unlink the socket before its <=30 s drain of released jobs and uploads)
  for _ in $(seq 900); do { [ -n "$p" ] && kill -0 "$p" 2>/dev/null; } || { [ -S "$OPTICC_SOCK" ] && [ -z "$p" ]; } || break; sleep 0.1; done; sleep 0.3; }
stored() { grep '"event":"cache-put"' "$T/worker.err" 2>/dev/null | grep -c '"outcome":"stored"'; }

S=$T/w; mkdir -p "$S/inc"
printf '#pragma once\n#define K 5\n' > "$S/inc/h.h"
printf '#include "h.h"\nint f(int x) { return x + K; }\n' > "$S/f.c"
printf '#include "h.h"\nint g(int x) { return x * K; }\n' > "$S/g.c"
sleep 2.4
# grep -c prints 0 AND exits 1 on no match, so `|| echo 0` would print "0\n0": default only an empty result
puts() { local n; n=$(grep -c '"event":"cache-put"' "$T/worker.err" 2>/dev/null); echo "${n:-0}"; }
# a SIGUSR1 summary of the live coordinator records an upload decision (sent or refused as racy); exit 2 = not yet parseable
DECIDED_PY='import json,sys
try: u=json.load(open(sys.argv[1]))["v4"]["uploads"]
except Exception: sys.exit(2)
sys.exit(0 if u.get("sent",0)+u.get("skipped_racy",0)>=1 else 1)'
decided() { local p rc; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || return 1
  rm -f "$T/summary.json"; kill -USR1 "$p" 2>/dev/null || return 1
  for _ in $(seq 100); do python3 -c "$DECIDED_PY" "$T/summary.json" 2>/dev/null; rc=$?; [ $rc -ne 2 ] && return $rc; sleep 0.05; done; return 1; }
session() {   # $1: source, $2: hook value -> builds locally (worker down), then admits the worker so the upload runs
  # worker.err is appended across sessions (the cumulative stored count is deliberate), so the wait compares
  # against this session's starting count instead of matching the previous session's cache-put event at once
  local base; base=$(puts)
  OPTICC_TEST_UPLOAD_STALE_DEPS=$2 start_coord
  ( cd "$S" && "$OPTICC" "$CC_REAL" -O2 -MD -MF "$1.o.d" -Iinc -c "$1.c" -o "$1.o" ) > "$T/out.txt" 2>&1 || fail "build $1: $(cat "$T/out.txt")"
  start_worker "127.0.0.1:$PORT"
  # both sessions wait (<= 20 s) until the coordinator has admitted the worker and decided the upload; stopping earlier
  # can leave a session with no upload attempt at all (caught by the summary check below)
  for _ in $(seq 100); do decided && break; sleep 0.2; done
  if [ "$2" = 0 ]; then for _ in $(seq 200); do [ "$(puts)" -gt "$base" ] && break; sleep 0.1; done; fi
  stop_coord; stop_worker   # stop_coord waits for the coordinator to exit, i.e. for its upload drain
  # the upload decision (sent or refused) must be in the summary, so the assertions below are not vacuous
  python3 -c "import json,sys;u=json.load(open(sys.argv[1]))['v4']['uploads'];sys.exit(0 if (u.get('sent',0)+u.get('skipped_racy',0))>=1 else 1)" "$T/summary.json" \
    || fail "session $1: no upload decision recorded in the summary"
}
# control: no hook -> the upload is stored
session f 0
N0=$(stored); [ "$N0" -ge 1 ] || fail "control: the upload was not stored (worker log: $(tail -3 "$T/worker.err"))"
echo "control (fresh closure hashes): uploads stored $N0"
# stale closure hashes -> refused, nothing stored
session g 1
N1=$(stored); [ "$N1" -eq "$N0" ] || fail "stale closure hashes were uploaded and STORED ($N0 -> $N1): the worker filed an object under hashes that do not describe its inputs"
RACY=$(python3 -c "import json;print(json.load(open('$T/summary.json'))['v4']['uploads'].get('skipped_racy',0))")
[ "$RACY" -ge 1 ] || fail "the stale upload was not counted as refused (skipped_racy=$RACY)"
echo "stale closure hashes: refused (skipped_racy=$RACY), stored still $N1"
echo "E2E-CACHE-UPLOAD-CLOSURE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
