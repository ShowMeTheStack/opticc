#!/usr/bin/env bash
# End to end (ctest: e2e_upload_race): the write-back must HASH the source
# before the stat check that vouches for it. A test hook pauses the upload thread between the two steps; the test edits
# the source (same size, other content) during the pause. Required: the upload is refused (racy) and the worker stores
# nothing. With the old order (stat, then hash) the new content's hash would be uploaded with the object compiled from
# the OLD content (the shared worker cache poisoned); reversing the order makes this test fail.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-ur.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {
  rm -f "$T/worker.out" "$T/worker.err"   # old logs must not satisfy the readiness loop (the child truncates them only after fork)
  "$OPTICCD" --listen "$1" --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
}
start_worker 127.0.0.1:0
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_TEST_UPLOAD_GAP_MS=3000 OPTICC_TEST_UPLOAD_GAP_FILE=$T/gap
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"

S=$T/w; mkdir -p "$S"
printf 'int f(int x) { return x + 1; }\n' > "$S/f.c"
sleep 2.4   # older than any racy margin
( cd "$S" && "$OPTICC" "$CC_REAL" -O2 -MD -MF f.o.d -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "build: $(cat "$T/out.txt")"
start_worker "127.0.0.1:$PORT"   # admitted later: the upload thread then hashes f.c, pauses, and stats it
for _ in $(seq 600); do [ -f "$T/gap" ] && break; sleep 0.05; done
[ -f "$T/gap" ] || fail "the upload never reached the hash/stat gap: $(tail -3 "$T/coord.err")"
printf 'int f(int x) { return x + 2; }\n' > "$S/f.c"   # same size, other content, written inside the gap
for _ in $(seq 200); do grep -q '"event":"cache-put"' "$T/worker.err" && break; sleep 0.1; done
sleep 4
grep -q '"outcome":"stored"' "$T/worker.err" && fail "the worker stored an upload whose source changed between hash and stat (poisoned key)"
echo "PASS e2e_upload_race: the edit inside the hash/stat gap refused the upload; nothing stored"
