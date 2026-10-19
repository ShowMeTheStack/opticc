#!/usr/bin/env bash
# End to end (ctest: e2e_cache_missing): the worker compile cache must file a header-push result under the
# closure the compile ACTUALLY read. A computed include (`#include HDR`, -DHDR='"gen.h"') is invisible to the client's
# scanner, so the worker obtains gen.h in a MISSING round. A result filed under the SHIPPED closure (which lacks
# gen.h) would let a later clean client with a DIFFERENT gen.h hit the stale object.
#   1 fresh client, clean tree, gen.h = G 1: remote compile with a MISSING round; the worker stores the result
#   2 gen.h -> G 2, clean tree (no .d), fresh client: must NOT be a worker hit; object == plain build with G 2
#   3 objects removed but .d kept (the client's view now names gen.h), fresh client: a worker hit (G 2), object correct
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
grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0   # worker cache only: no shared direct manifests, no client compiles
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
cache_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print(','.join(str(j.get('cache')) for j in s['jobs']))"; }

S=$T/w; mkdir -p "$S/dist/g" "$S/ref/g"
for d in dist ref; do
  printf '#include HDR\nint f(void) { return G; }\n' > "$S/$d/f.c"
  printf '#define G 1\n' > "$S/$d/g/gen.h"
  cat > "$S/$d/Makefile" <<'M'
f.o: f.c
	$(CC) -O2 -MD -MF f.o.d -DHDR='"gen.h"' -Ig -c f.c -o f.o
clean:
	rm -f f.o f.o.d
M
done
sleep 2.4   # inputs older than any racy margin
plain() { ( cd "$S/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
build() { rm -rf "$T/lc"; start_coord; ( cd "$S/dist" && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord; }

# 1 G 1, clean: remote with a MISSING round; the worker stores
plain; build "1"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "1: object differs from the plain build"
grep -q '"rounds":[1-9]' "$T/worker.err" || grep -q 'missing' "$T/worker.err" || echo "note: no MISSING round visible in the worker log"
echo "1 G=1 clean: cache=$(cache_of)"
# 2 G 2, clean tree, fresh client: no stale worker hit
sleep 1.1; printf '#define G 2\n' > "$S/dist/g/gen.h"; printf '#define G 2\n' > "$S/ref/g/gen.h"; sleep 2.4
( cd "$S/dist" && make -s clean ); plain; build "2"
C2=$(cache_of)
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "2: STALE OBJECT — gen.h changed but the worker served the G=1 object (cache=$C2)"
[ "$C2" != "hit" ] || fail "2: worker hit after gen.h changed (cache=$C2)"
echo "2 G=2 clean, fresh client: cache=$C2, object correct"
# 3 objects removed, .d kept: the client's closure names gen.h; the worker entry stored in 2 matches
rm -f "$S/dist/f.o"; build "3"
C3=$(cache_of)
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "3: object differs from the plain build (cache=$C3)"
[ "$C3" = "hit" ] || fail "3: expected a worker hit when the client's view names gen.h, got cache=$C3"
echo "3 .d kept, fresh client: cache=$C3, object correct"
echo "E2E-CACHE-MISSING PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
