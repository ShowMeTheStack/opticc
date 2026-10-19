#!/usr/bin/env bash
# End to end (ctest: e2e_cache_nomd): without a dependency file (no -MD) a local compile could only be keyed on
# the SCANNED closure, which never contains the target of a computed `#include HDR`, so editing only that header would
# hit the old object. Such a compile must not be stored (record_direct refuses it too).
#   1 worker unreachable (every TU compiles on the client), gen.h = G 1, no -MD: compiled, NOT stored
#   2 gen.h -> G 2, make clean, same client cache: object == plain build with G 2 (no stale client-cache hit)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachen.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
# a worker address that is then shut down: the coordinator has no reachable worker, every TU compiles on the client
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
field() { python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }

S=$T/w; mkdir -p "$S/dist/g" "$S/ref/g"
for d in dist ref; do
  printf '#include HDR\nint f(void) { return G; }\n' > "$S/$d/f.c"
  printf '#define G 1\n' > "$S/$d/g/gen.h"
  cat > "$S/$d/Makefile" <<'M'
f.o: f.c
	$(CC) -O2 -DHDR='"gen.h"' -Ig -c f.c -o f.o
clean:
	rm -f f.o
M
done
sleep 2.4
plain() { ( cd "$S/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
build() { start_coord; ( cd "$S/dist" && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord; }

plain; build "1"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "1: object differs from the plain build"
SK=$(field "json.dumps(s.get('v4',{}).get('skips',{}))")
echo "1 G=1 local, no -MD: jobs $(field "[(j.get('outcome'), j.get('cache')) for j in s['jobs']]"), skips $SK"
echo "$SK" | grep -q "computed include without a dependency file" || fail "1: the store was not refused (skips: $SK)"
sleep 1.1; printf '#define G 2\n' > "$S/dist/g/gen.h"; printf '#define G 2\n' > "$S/ref/g/gen.h"; sleep 2.4
( cd "$S/dist" && make -s clean ); plain; build "2"
cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "2: STALE OBJECT — gen.h changed but the client cache served the G=1 object: $(field "[(j.get('outcome'), j.get('cache')) for j in s['jobs']]")"
echo "2 G=2: object correct, jobs $(field "[(j.get('outcome'), j.get('cache')) for j in s['jobs']]")"
echo "E2E-CACHE-NOMD PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
