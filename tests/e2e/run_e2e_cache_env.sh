#!/usr/bin/env bash
# End to end (ctest: e2e_cache_env): an environment variable that changes what the compiler reads (here
# CPATH) must key every cache and keep the compile off the workers (they have neither the variable nor its directories).
# If only the direct key held it, CPATH=dirA then CPATH=dirB would serve the dirA object from the client cache.
#   1 CPATH=dirA (v.h: V 1), worker up, LOCAL_SLOTS=0: compiled on the client (never remote), object == plain build
#   2 CPATH=dirB (v.h: V 2), make clean, same caches: object == plain build with dirB (no stale hit)
#   3 CPATH=dirB again, make clean: served from a cache (the keyed entry still hits), object correct
#   4 no CPATH, -I dirA instead: runs remotely as before (the environment, not the TU, forced the local runs)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachee.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0 OPTICC_DIRECT_TRACE=$T/direct.trace
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])" 2>/dev/null; }
S=$T/w; mkdir -p "$S/dist" "$S/ref" "$T/dirA" "$T/dirB"
printf '#define V 1\n' > "$T/dirA/v.h"; printf '#define V 2\n' > "$T/dirB/v.h"
for d in dist ref; do
  printf '#include <v.h>\nint f(int x) { return x * V; }\n' > "$S/$d/f.c"
  printf 'f.o: f.c\n\t$(CC) -O2 $(EXTRA) -MD -MF f.o.d -c f.c -o f.o\nclean:\n\trm -f f.o f.o.d\n' > "$S/$d/Makefile"
done
sleep 2.4
plain() { ( cd "$S/ref" && make -s clean && env "$@" make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
build() { local n=$1; shift; : > "$T/direct.trace"; start_coord
          ( cd "$S/dist" && make -s clean && env "$@" make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$n: build: $(cat "$S/out.txt")"; stop_coord
          cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "$n: STALE OBJECT — differs from the plain build with the same environment: $(jobs_of) $(cat "$T/direct.trace")"; }
plain CPATH="$T/dirA"; build 1 CPATH="$T/dirA"; J=$(jobs_of); echo "1 CPATH=dirA: $J"
wjobs() { grep -c '"event":"job"' "$T/worker.err"; }   # the worker's own log: the summary may miss a local-only job still storing at exit
[ "$(wjobs)" -eq 0 ] || fail "1: a compile under CPATH ran on a worker ($(wjobs) worker jobs; $J)"
plain CPATH="$T/dirB"; build 2 CPATH="$T/dirB"; J=$(jobs_of); echo "2 CPATH=dirB: object correct, $J"
build 3 CPATH="$T/dirB"; J=$(jobs_of)
{ echo "$J" | grep -q "client-cache" || cut -f2 "$T/direct.trace" | grep -qx hit; } || fail "3: the unchanged rebuild under CPATH=dirB was not served from a cache ($J; $(cat "$T/direct.trace"))"
echo "3 CPATH=dirB again: cached, $J"
plain EXTRA="-I$T/dirA"; build 4 EXTRA="-I$T/dirA"; J=$(jobs_of); echo "4 no CPATH: $J"
[ "$(wjobs)" -ge 1 ] || fail "4: without the environment the compile no longer runs remotely ($J)"
echo "E2E-CACHE-ENV PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
