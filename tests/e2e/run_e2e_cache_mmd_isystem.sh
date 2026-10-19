#!/usr/bin/env bash
# End to end (ctest: e2e_cache_mmd_isystem): a -MMD dependency file omits every header found through a
# SYSTEM directory, including a project's own -isystem directory, which no fingerprint covers. Such a header must still
# be keyed: editing it must never serve the old object.
#   A (local, worker unreachable, client cache): -MMD -isystem sysinc, p.h V 1 -> stored; p.h V 2 -> object == plain
#   B (remote, LOCAL_SLOTS=0, a FRESH client cache per build: only the worker cache could serve it): the same
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachei.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {
  rm -f "$T/worker.out" "$T/worker.err"
  "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
  export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB"
}
stop_worker() { kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=; }
export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() {
  local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || return 0
  kill -TERM "$p"; for _ in $(seq 600); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$p" 2>/dev/null && fail "coordinator $p did not exit"; return 0
}
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])" 2>/dev/null; }
S=$T/w; mkdir -p "$S/dist/sysinc" "$S/ref/sysinc"
for d in dist ref; do
  printf '#include <p.h>\nint f(int x) { return x * V; }\n' > "$S/$d/f.c"
  printf 'f.o: f.c\n\t$(CC) -O2 -MMD -MF f.o.d -isystem sysinc -c f.c -o f.o\nclean:\n\trm -f f.o f.o.d\n' > "$S/$d/Makefile"
done
setv() { sleep 1.1; for d in dist ref; do printf '#define V %s\n' "$1" > "$S/$d/sysinc/p.h"; done; sleep 2.4; }
plain() { ( cd "$S/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
build() { start_coord; ( cd "$S/dist" && make -s clean && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord
          cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "$1: STALE OBJECT — an -isystem header edit under -MMD served the old object: $(jobs_of)"; }
# A: local
start_worker; stop_worker
export OPTICC_LOCAL_CACHE_DIR=$T/lcA
setv 1; plain; build A1; echo "A1 V=1: $(jobs_of)"
build A1b; J=$(jobs_of); echo "A1b unchanged: $J"; echo "$J" | grep -q "client-cache" || fail "A1b: control — the unchanged rebuild was not a client-cache hit ($J)"
setv 2; plain; build A2; echo "A2 V=2: object correct, $(jobs_of)"
# B: remote, fresh client cache per build
start_worker; export OPTICC_LOCAL_SLOTS=0
export OPTICC_LOCAL_CACHE_DIR=$T/lcB1; setv 3; plain; build B1; J=$(jobs_of); echo "B1 V=3: $J"; echo "$J" | grep -q "'remote'" || fail "B1: not remote ($J)"
export OPTICC_LOCAL_CACHE_DIR=$T/lcB1b; build B1b; J=$(jobs_of); echo "B1b unchanged, fresh client: $J"; echo "$J" | grep -q "'hit'" || fail "B1b: control — no worker-cache hit ($J)"
export OPTICC_LOCAL_CACHE_DIR=$T/lcB2; setv 4; plain; build B2; echo "B2 V=4: object correct, $(jobs_of)"
stop_worker
echo "E2E-CACHE-MMD-ISYSTEM PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
