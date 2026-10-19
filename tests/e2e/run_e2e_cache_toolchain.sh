#!/usr/bin/env bash
# End to end (ctest: e2e_cache_toolchain): a header-push result is produced by the WORKER's toolchain. The
# client may file it under its own compiler / system-include identity (client cache, direct entry, shared manifest)
# only when the worker reports the same identity. Two drivers `fakecc` with the same name but different binaries
# (wrapper scripts around the real compiler): the client compiles with A.
#   control: worker resolves fakecc to A -> the result is cached; a rebuild is a client-cache hit
#   test:    worker resolves fakecc to B -> nothing cached (toolchain_skips >= 1); a rebuild goes remote again
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
REALCC=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachet.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/cA" "$T/cB"
printf '#!/bin/sh\n# driver variant A\nexec %s "$@"\n' "$REALCC" > "$T/cA/fakecc"
printf '#!/bin/sh\n# driver variant B (a different binary)\nexec %s "$@"\n' "$REALCC" > "$T/cB/fakecc"
chmod +x "$T/cA/fakecc" "$T/cB/fakecc"
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {   # $1: PATH directory holding the worker's fakecc
  rm -f "$T/worker.out" "$T/worker.err"   # the redirection truncates only in the child: the loop below could read the PREVIOUS worker's port
  PATH="$1:$PATH" "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache.$(basename "$1")" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
  export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB"
}
stop_worker() { kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=; }
export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
field() { python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }
S=$T/w; mkdir -p "$S"
printf 'int f(int x) { return x * 3; }\n' > "$S/f.c"
sleep 2.4
build() { start_coord; ( cd "$S" && rm -f f.o f.o.d && "$OPTICC" "$T/cA/fakecc" -O2 -MD -MF f.o.d -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "build: $(cat "$T/out.txt")"; stop_coord; }
session() {   # $1: worker driver dir; prints "<first outcome> <toolchain_skips> <second outcome>"
  export OPTICC_LOCAL_CACHE_DIR=$T/lc.$(basename "$1"); rm -rf "$OPTICC_LOCAL_CACHE_DIR"
  start_worker "$1"; build; local o1 sk; o1=$(field "s['jobs'][0]['outcome']"); sk=$(field "s['v4'].get('toolchain_skips',0)")
  build; local o2; o2=$(field "s['jobs'][0]['outcome']"); stop_worker
  echo "$o1 $sk $o2"
}
read -r A1 ASK A2 <<< "$(session "$T/cA")"
echo "control (worker = client toolchain): first $A1, toolchain_skips $ASK, rebuild $A2"
[ "$A1" = "remote" ] || fail "control: the first build did not run remotely ($A1)"
[ "$ASK" -eq 0 ] || fail "control: a same-toolchain result was refused ($ASK)"
[ "$A2" = "client-cache" ] || fail "control: the rebuild was not a client-cache hit ($A2) — the check would be vacuous"
read -r B1 BSK B2 <<< "$(session "$T/cB")"
echo "test (worker toolchain differs): first $B1, toolchain_skips $BSK, rebuild $B2"
[ "$B1" = "remote" ] || fail "test: the first build did not run remotely ($B1)"
[ "$BSK" -ge 1 ] || fail "test: a result from a different worker toolchain was cached (toolchain_skips=$BSK)"
[ "$B2" != "client-cache" ] || fail "test: the rebuild was served from the client cache: a foreign-toolchain object was filed under the client's identity"
# cache off: nothing would be stored, so nothing may be counted as a toolchain skip
export OPTICC_LOCAL_CACHE=off
read -r C1 CSK C2 <<< "$(session "$T/cB")"
echo "cache off (worker toolchain differs): first $C1, toolchain_skips $CSK"
[ "$C1" = "remote" ] || fail "cache off: the build did not run remotely ($C1)"
[ "$CSK" -eq 0 ] || fail "cache off: $CSK toolchain skips counted although no store could happen"
echo "E2E-CACHE-TOOLCHAIN PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
