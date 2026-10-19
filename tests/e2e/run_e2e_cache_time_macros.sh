#!/usr/bin/env bash
# End to end (ctest: e2e_cache_time_macros): a TU expanding __TIME__ (here through a header) is not a
# function of its inputs' bytes; it must never be served from a content-keyed cache (ccache refuses it unless
# sloppiness=time_macros).
#   1 remote compile (LOCAL_SLOTS=0): not stored by the worker, not stored by the client
#   2 a second later, same client cache: no client-cache hit; a fresh client cache: no worker-cache hit
#   3 control: the same TU without the macro is a client-cache hit on rebuild (the path under test is live)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachem.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])" 2>/dev/null; }
S=$T/w; mkdir -p "$S"
printf '#define BUILT __TIME__\n' > "$S/stamp.h"; printf '#define BUILT "fixed"\n' > "$S/nostamp.h"
printf '#include "stamp.h"\nconst char *built(void) { return BUILT; }\n' > "$S/f.c"
printf '#include "nostamp.h"\nconst char *built(void) { return BUILT; }\n' > "$S/g.c"
sleep 2.4
build() {   # $1 step, $2 client cache dir, $3 source (f: __TIME__ through stamp.h, g: none)
  export OPTICC_LOCAL_CACHE_DIR=$2; start_coord
  ( cd "$S" && rm -f "$3.o" && "$OPTICC" "$CC_REAL" -O2 -MD -MF "$3.o.d" -c "$3.c" -o "$3.o" ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"
  stop_coord; }
build 1 "$T/lc1" f; J=$(jobs_of); echo "1 __TIME__: $J"; echo "$J" | grep -q "'remote'" || fail "1: not remote ($J)"
echo "$J" | grep -q "'store'" && fail "1: the worker stored a __TIME__ TU ($J)"
sleep 1.1
build 2a "$T/lc1" f; J=$(jobs_of); echo "2a same client cache: $J"; echo "$J" | grep -q "client-cache" && fail "2a: a __TIME__ TU was served from the client cache"
build 2b "$T/lc2" f; J=$(jobs_of); echo "2b fresh client cache: $J"; echo "$J" | grep -q "'hit'" && fail "2b: a __TIME__ TU was served from the worker cache"
build 3a "$T/lc3" g; J=$(jobs_of); echo "3a control without the macro: $J"; echo "$J" | grep -q "'store'" || fail "3a: control — the worker did not store ($J)"
build 3b "$T/lc3" g; J=$(jobs_of); echo "3b rebuild: $J"
echo "$J" | grep -q "client-cache" || fail "3b: control — the TU without the macro was not cached ($J): the test would be vacuous"
# 4: the time macro arrives through a -D value only (h.c has no macro in any file)
printf 'const char *built(void) { return STAMP; }\n' > "$S/h.c"; sleep 2.4
buildd() { export OPTICC_LOCAL_CACHE_DIR=$2; start_coord
  ( cd "$S" && rm -f h.o && "$OPTICC" "$CC_REAL" -O2 -DSTAMP=__TIME__ -MD -MF h.o.d -c h.c -o h.o ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"
  stop_coord; }
buildd 4a "$T/lc4"; J=$(jobs_of); echo "4a -DSTAMP=__TIME__: $J"; echo "$J" | grep -q "'store'" && fail "4a: the worker stored a TU whose -D value uses a time macro ($J)"
sleep 1.1
buildd 4b "$T/lc4"; J=$(jobs_of); echo "4b same client cache: $J"; echo "$J" | grep -q "client-cache" && fail "4b: served from the client cache"
buildd 4c "$T/lc5"; J=$(jobs_of); echo "4c fresh client cache: $J"; echo "$J" | grep -q "'hit'" && fail "4c: served from the worker cache"
echo "E2E-CACHE-TIME-MACROS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
