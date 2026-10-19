#!/usr/bin/env bash
# End to end (ctest: e2e_cache_classic_g): the classic (preprocessed-text) worker cache key has no compile
# directory, but with debug info the object records one (DW_AT_comp_dir). Two checkouts of one layout must not share
# such objects: a classic compile with debug info is not cached by the worker. Without debug info they still share.
#   1 checkout a, -g: remote (classic)        2 checkout b, -g: NOT a worker-cache hit
#   3 checkout a, no -g: remote, stored       4 checkout b, no -g: a worker-cache hit (control: the cache is live)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cacheg.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=classic
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() {   # waits for the process to EXIT (it removes the socket before its bounded drain)
  local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || return 0
  kill -TERM "$p"; for _ in $(seq 600); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$p" 2>/dev/null && fail "coordinator $p did not exit"; return 0
}
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])" 2>/dev/null; }
N=0
step() {   # $1 step, $2 checkout, $3 flags; a fresh client cache each time (only the worker cache can serve)
  N=$((N + 1)); export OPTICC_LOCAL_CACHE_DIR=$T/lc.$N
  start_coord; ( cd "$2" && rm -f f.o && "$OPTICC" "$CC_REAL" $3 -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; stop_coord
}
for c in a b; do mkdir -p "$T/$c"; printf 'int f(int x) { return x * 9; }\n' > "$T/$c/f.c"; done
step 1 "$T/a" "-O2 -g"; J=$(jobs_of); echo "1 a -g: $J"; echo "$J" | grep -q "'remote'" || fail "1: not remote ($J)"
step 2 "$T/b" "-O2 -g"; J=$(jobs_of); echo "2 b -g: $J"; echo "$J" | grep -q "'hit'" && fail "2: checkout b got checkout a's debug-info object from the classic worker cache ($J)"
step 3 "$T/a" "-O2"; J=$(jobs_of); echo "3 a: $J"
step 4 "$T/b" "-O2"; J=$(jobs_of); echo "4 b: $J"; echo "$J" | grep -q "'hit'" || fail "4: control — no worker-cache hit without debug info ($J): the test would be vacuous"
echo "E2E-CACHE-CLASSIC-G PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
