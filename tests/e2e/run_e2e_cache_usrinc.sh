#!/usr/bin/env bash
# End to end (ctest: e2e_cache_usrinc): a header under /usr/... reached through a user -I flag (as
# pkg-config / llvm-config output does: -I/usr/lib/x86_64-linux-gnu/glib-2.0/include, -I/usr/lib/llvm-17/include) is NOT
# in the compiler's search list, so the system-include fingerprint does not cover it. It must be an ordinary input
# (hashed, keyed); dropping every /usr/... path from keys would make an upgrade of such a header a stale hit.
#   1 worker unreachable (local compiles), -I/usr/lib/opticc-cache-$$/inc, v.h = V 1, -MD: compiled and cached
#   2 v.h -> V 2 (a "package upgrade"), make clean, same client cache: object == plain build with V 2
# Needs write access to /usr/lib (e.g. tests run as root); otherwise skipped (exit 77).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
U=/usr/lib/opticc-cache-$$
mkdir -p "$U/inc" 2>/dev/null || { echo "SKIP: /usr/lib not writable"; exit 77; }
T=$(mktemp -d /tmp/opticc-e2e-caches.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$U"; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 1 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])"; }
S=$T/w; mkdir -p "$S/dist" "$S/ref"
printf '#define V 1\n' > "$U/inc/v.h"
for d in dist ref; do
  printf '#include "v.h"\nint f(void) { return V; }\n' > "$S/$d/f.c"
  printf 'f.o: f.c\n\t$(CC) -O2 -MD -MF f.o.d -I%s/inc -c f.c -o f.o\nclean:\n\trm -f f.o f.o.d\n' "$U" > "$S/$d/Makefile"
done
sleep 2.4
plain() { ( cd "$S/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
build() { start_coord; ( cd "$S/dist" && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord; }
plain; build "1"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "1: object differs from the plain build"
echo "1 V=1: $(jobs_of)"
sleep 1.1; printf '#define V 2\n' > "$U/inc/v.h"; sleep 2.4
( cd "$S/dist" && make -s clean ); plain; build "2"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "2: STALE OBJECT — a header under /usr reached through -I changed but the cached V=1 object was served: $(jobs_of)"
echo "2 V=2 (header 'upgraded'): object correct, $(jobs_of)"
# 3 positive: unchanged header -> a client-cache hit (the header is keyed, not excluded)
( cd "$S/dist" && make -s clean ); build "3"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "3: object differs"
J3=$(jobs_of); echo "3 unchanged: $J3"
echo "$J3" | grep -q "client-cache" || fail "3: the unchanged rebuild was not a client-cache hit ($J3)"
echo "E2E-CACHE-USRINC PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
