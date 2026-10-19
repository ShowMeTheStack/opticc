#!/usr/bin/env bash
# End to end (ctest: e2e_rdirect): SHARED DIRECT MANIFESTS (opticc/rdirect.hpp). Client A builds and publishes
# its direct entries to the worker; FRESH clients (empty client cache) are answered from them, or must not be:
#   1 fresh client, same tree: every TU a shared-manifest hit, objects and dependency files identical to a plain build
#   2 a header edited (real edit) on the fresh client: no hit; compiled; objects equal to a plain build of the edited tree
#   3 another compiler identity (a different compiler path): no hit
#   4 a shadowing header appears earlier in the include search (the source's directory): no hit; compiled with it
#   5 an input rewritten (same bytes) just before the build: served, but not recorded locally (younger than the margin)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-rd.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json

fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$T/summary.json" "$p"; python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }
start_coord() {   # $1: client cache directory (a NEW one = a fresh client)
  export OPTICC_LOCAL_CACHE_DIR=$1
  "$OPTICC" --coordinator > "$T/coord.out" 2>> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done
  for _ in $(seq 300); do [ -n "$p" ] && kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; sleep 0.2; }

S=$T/w; mkdir -p "$S/src/inc"
printf '#pragma once\n#define K 7\nstatic inline int k(int x) { return x * K; }\n' > "$S/src/inc/k.h"
for i in $(seq 0 9); do printf '#include "k.h"\nint f%d(int x) { return k(x) + %d; }\n' "$i" "$i" > "$S/src/f$i.c"; done
cat > "$S/src/Makefile" <<'M'
OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
all: $(OBJS)
%.o: %.c
	$(CC) -O2 -MD -MT $@ -MF $@.d -Iinc -c $< -o $@
clean:
	rm -f *.o *.d
M
cp -r "$S/src" "$S/ref"; cp -r "$S/src" "$S/dist"; rm -rf "$S/src"; N=10
sleep 2.4   # inputs older than any racy margin
plain() { ( cd "$S/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"; }
same_outputs() {
  for o in "$S/ref"/*.o; do cmp -s "$o" "$S/dist/$(basename "$o")" || fail "$1: object differs: $(basename "$o")"; done
  for d in "$S/ref"/*.o.d; do cmp -s "$d" "$S/dist/$(basename "$d")" || fail "$1: dependency file differs: $(basename "$d")"; done
}
build() { ( cd "$S/dist" && make -s clean && make -s -j4 CC="$1" ) > "$S/out.txt" 2>&1 || fail "$2 build: $(cat "$S/out.txt")"; }
rd() { summ "s['v4']['rdirect']$1"; }

# 0 client A builds and publishes
plain
start_coord "$T/lcA"; build "$OPTICC $CC_REAL" "0 publisher"; same_outputs "0 publisher"
stop_coord
NM=$(find "$T/wcache/rd" -type f 2>/dev/null | wc -l | tr -d ' ')
[ "$NM" -eq "$N" ] || fail "0: worker holds $NM published manifests, expected $N; $(grep direct-put "$T/worker.err" | tail -3)"
echo "0 client A: built, $NM manifests published"

# 1 fresh client, same tree: every TU from the shared manifests (no local slots: a miss would be visible as a remote job)
export OPTICC_LOCAL_SLOTS=0
start_coord "$T/lc1"; build "$OPTICC $CC_REAL" "1 fresh"; same_outputs "1 fresh"
[ "$(rd "['hits']")" -eq "$N" ] || fail "1: expected $N shared-manifest hits, got $(rd "")"
[ "$(rd "['recorded_locally']")" -eq "$N" ] || fail "1: expected $N local records, got $(rd "")"
stop_coord
echo "1 fresh client: $N/$N shared-manifest hits, objects and dependency files identical, recorded locally"

# 2 a real header edit on the fresh client: no hit, compiled correctly
printf '#pragma once\n#define K 8\nstatic inline int k(int x) { return x * K; }\n' > "$S/dist/inc/k.h"; cp "$S/dist/inc/k.h" "$S/ref/inc/k.h"; plain
start_coord "$T/lc2"; build "$OPTICC $CC_REAL" "2 edited"; same_outputs "2 edited"
[ "$(rd "['hits']")" -eq 0 ] || fail "2: a header edit must miss, got $(rd "")"
stop_coord
echo "2 header edited on the fresh client: 0 hits, objects equal to a plain build of the edited tree"
printf '#pragma once\n#define K 7\nstatic inline int k(int x) { return x * K; }\n' > "$S/dist/inc/k.h"; cp "$S/dist/inc/k.h" "$S/ref/inc/k.h"; plain
sleep 2.4

# 3 another compiler identity (the same compiler behind another path): no hit
mkdir -p "$T/bin"; printf '#!/bin/sh\nexec %s "$@"\n' "$(command -v "$CC_REAL")" > "$T/bin/cc-other"; chmod +x "$T/bin/cc-other"
start_coord "$T/lc3"; build "$OPTICC $T/bin/cc-other" "3 other compiler"
[ "$(rd "['hits']")" -eq 0 ] || fail "3: another compiler identity must miss, got $(rd "")"
stop_coord
echo "3 other compiler identity: 0 hits"

# 4 a shadowing header in the source's directory (searched before -Iinc for #include \"k.h\"): no hit
printf '#pragma once\n#define K 9\nstatic inline int k(int x) { return x * K; }\n' > "$S/dist/k.h"; cp "$S/dist/k.h" "$S/ref/k.h"; plain
start_coord "$T/lc4"; build "$OPTICC $CC_REAL" "4 shadowed"; same_outputs "4 shadowed"
[ "$(rd "['hits']")" -eq 0 ] || fail "4: a shadowing header must miss, got $(rd "")"
stop_coord
rm -f "$S/dist/k.h" "$S/ref/k.h"; plain
echo "4 shadowing header appeared: 0 hits, objects compiled with it"

# 5 an input rewritten with the same bytes just before the build: served (content), not recorded locally (racy)
export OPTICC_RACY_S=5
cp "$S/dist/inc/k.h" "$T/k.h.tmp"; mv "$T/k.h.tmp" "$S/dist/inc/k.h"
start_coord "$T/lc5"; build "$OPTICC $CC_REAL" "5 racy"; same_outputs "5 racy"
[ "$(rd "['hits']")" -eq "$N" ] || fail "5: same bytes must hit, got $(rd "")"
[ "$(rd "['recorded_locally']")" -eq 0 ] || fail "5: an input younger than the margin must not be recorded locally, got $(rd "")"
stop_coord
echo "5 input rewritten just now (same bytes): $N hits, 0 recorded locally"
echo "E2E-RDIRECT PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
