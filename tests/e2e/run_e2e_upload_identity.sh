#!/usr/bin/env bash
# End to end (ctest: e2e_upload_identity): the worker files an uploaded object under ITS OWN compiler's key,
# so it must refuse an upload produced by a DIFFERENT compiler. Two compiler drivers
# with the same name `fakecc` but different binaries (two wrapper scripts around cc): the client compiles with A, the
# worker resolves `fakecc` to B. Every upload must be refused as "compiler-mismatch" and nothing stored; a positive
# control with the worker on A stores every upload.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
T=$(mktemp -d /tmp/opticc-e2e-uid.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

REALCC=$(command -v cc) || fail "no cc"
mkdir -p "$T/cA" "$T/cB"
printf '#!/bin/sh\n# driver variant A\nexec %s "$@"\n' "$REALCC" > "$T/cA/fakecc"
printf '#!/bin/sh\n# driver variant B (a different binary)\nexec %s "$@"\n' "$REALCC" > "$T/cB/fakecc"
chmod +x "$T/cA/fakecc" "$T/cB/fakecc"

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {   # $1: listen address, $2: PATH directory holding the worker's fakecc
  rm -f "$T/worker.out" "$T/worker.err"   # old logs must not satisfy the readiness loop (the child truncates them only after fork)
  local addr=$1 cdir=$2
  PATH="$cdir:$PATH" "$OPTICCD" --listen "$addr" --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache.$(basename "$cdir")" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
}
start_worker 127.0.0.1:0 "$T/cB"
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json

start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; # wait for the PROCESS to exit (it may unlink the socket before its <=30 s drain of released jobs and uploads)
  for _ in $(seq 900); do { [ -n "$p" ] && kill -0 "$p" 2>/dev/null; } || { [ -S "$OPTICC_SOCK" ] && [ -z "$p" ]; } || break; sleep 0.1; done; sleep 0.3; }

S=$T/w; mkdir -p "$S/src"
for i in $(seq 0 5); do printf 'int f%d(int x) { return x + %d; }\n' "$i" "$i" > "$S/src/f$i.c"; done
cat > "$S/src/Makefile" <<'M'
OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
all: $(OBJS)
%.o: %.c
	$(CC) -O2 -MD -MT $@ -MF $@.d -c $< -o $@
M
N=6; sleep 2.4   # inputs older than any racy margin

run_case() {   # $1: worker driver dir, $2: expected outcome, $3: label
  local wdir=$1 want=$2 label=$3
  rm -rf "$S/dist" "$T/lc"; cp -r "$S/src" "$S/dist"; export OPTICC_LOCAL_CACHE_DIR=$T/lc
  # worker down: every TU compiles on the client (with driver A), then the worker comes up and receives the uploads
  start_coord
  ( cd "$S/dist" && make -s -j4 CC="$OPTICC $T/cA/fakecc" ) > "$S/out.txt" 2>&1 || fail "$label: build: $(cat "$S/out.txt")"
  start_worker "127.0.0.1:$PORT" "$wdir"
  for _ in $(seq 600); do [ "$(grep -c '"event":"cache-put"' "$T/worker.err")" -ge "$N" ] && break; sleep 0.1; done
  stop_coord
  local got; got=$(grep -c "\"outcome\":\"$want\"" "$T/worker.err")
  [ "$got" -eq "$N" ] || fail "$label: expected $N uploads with outcome $want, got $got: $(grep cache-put "$T/worker.err" | tail -2)"
  if [ "$want" != stored ]; then grep -q '"outcome":"stored"' "$T/worker.err" && fail "$label: a mismatched upload was stored"; fi
  echo "$label: $N/$N uploads $want"
  kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
}
run_case "$T/cB" compiler-mismatch "different driver binary"
run_case "$T/cA" stored "same driver (positive control)"
echo "PASS e2e_upload_identity"
