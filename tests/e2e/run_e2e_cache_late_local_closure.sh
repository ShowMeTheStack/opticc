#!/usr/bin/env bash
# End to end (ctest: e2e_cache_late_local_closure): a header-push job that turns local after its closure was
# built (here: a computed __has_include, which a worker cannot evaluate) must not build the closure a second time after
# the compile (scan + stat walk + hashing): its store needs none, since it is stored under the preprocessed key.
#   1 LOCAL_SLOTS=0, -MD, computed __has_include: header-push decision, local compile, object == plain build,
#     exactly ONE closure build (summary v4.closure_builds)
#   2 rebuild: served from the preprocessed-key cache, object correct (the store still happened)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachel.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
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
field() { python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }
S=$T/w; mkdir -p "$S/dist" "$S/ref"
for d in dist ref; do
  printf '#define CFG "cfg.h"\n#if __has_include(CFG)\n#include CFG\n#endif\nint f(int x) { return x * 4; }\n' > "$S/$d/f.c"
  printf '/* config */\n' > "$S/$d/cfg.h"
done
sleep 2.4
( cd "$S/ref" && "$CC_REAL" -O2 -MD -MF f.o.d -c f.c -o f.o ) || fail "plain build"
build() { start_coord; ( cd "$S/dist" && rm -f f.o f.o.d && "$OPTICC" "$CC_REAL" -O2 -MD -MF f.o.d -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; stop_coord
          cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "$1: object differs from the plain build"; }
build 1; O=$(field "[(j.get('outcome'), j.get('cache')) for j in s['jobs']]"); NB=$(field "s['v4'].get('closure_builds', -1)")
echo "1 computed __has_include: $O, closure builds $NB"
echo "$O" | grep -q "fallback" || fail "1: the job did not take the header-push decision and turn local ($O) — the test would be vacuous"
[ "$NB" -eq 1 ] || fail "1: the closure was built $NB times (a late-local job must not re-scan after the compile)"
build 2; O=$(field "[(j.get('outcome'), j.get('cache')) for j in s['jobs']]"); echo "2 rebuild: $O"
echo "$O" | grep -q "client-cache" || fail "2: not served from the preprocessed-key cache ($O): the store did not happen"
echo "E2E-CACHE-LATE-LOCAL-CLOSURE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
