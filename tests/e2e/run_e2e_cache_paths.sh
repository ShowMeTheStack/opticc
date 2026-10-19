#!/usr/bin/env bash
# End to end (ctest: e2e_cache_paths): an object depends on how paths are SPELLED (__FILE__) and, with debug info,
# on the compile directory (DW_AT_comp_dir). Keys that relativise every path under cwd, replace the source by "$SRC"
# and leave cwd out would share such objects where they must not be. Remote compiles, a worker
# cache, and a FRESH client cache per build (so a wrong object could only come from a shared key):
#   1 checkout a, -g: compiled remotely, stored by the worker
#   2 checkout b (same layout, other directory), -g: object == plain build in b (comp_dir b, not a)
#   3 checkout a, __FILE__ with the source spelled absolute, then relative: each object == its plain build
#   4 positive: checkout a, -g, unchanged: a worker-cache hit (same directory and spelling still share)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachep.XXXXXX)
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
N=0
# $1 step, $2 directory, $3 source spelling, $4 flags: an opticc compile with a fresh client cache, compared with the
# plain compile of the same command line in the same directory
step() {
  N=$((N + 1)); export OPTICC_LOCAL_CACHE_DIR=$T/lc.$N
  ( cd "$2" && rm -f f.o && $CC_REAL $4 -c "$3" -o f.o && mv f.o ref.o ) || fail "$1: plain compile"
  start_coord; ( cd "$2" && "$OPTICC" "$CC_REAL" $4 -c "$3" -o f.o ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; stop_coord
  cmp -s "$2/ref.o" "$2/f.o" || fail "$1: WRONG OBJECT — another directory's or spelling's object was served: $(jobs_of)"
}
for c in a b; do mkdir -p "$T/$c"; printf 'const char *where(void) { return __FILE__; }\nint f(int x) { return x * 5; }\n' > "$T/$c/f.c"; done
sleep 2.4
step "1" "$T/a" f.c "-O2 -g"; J=$(jobs_of); echo "1 a -g: $J"; echo "$J" | grep -q "'remote'" || fail "1: not remote ($J)"
step "2" "$T/b" f.c "-O2 -g"; echo "2 b -g: object correct, $(jobs_of)"
step "3a" "$T/a" "$T/a/f.c" "-O2"; echo "3a a absolute source: $(jobs_of)"
step "3b" "$T/a" f.c "-O2"; echo "3b a relative source: object correct, $(jobs_of)"
step "4" "$T/a" f.c "-O2 -g"; J=$(jobs_of); echo "4 a -g again: $J"
echo "$J" | grep -q "'hit'" || fail "4: the unchanged compile in the same directory was not a worker-cache hit ($J)"
echo "E2E-CACHE-PATHS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
