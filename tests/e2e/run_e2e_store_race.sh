#!/usr/bin/env bash
# End to end (ctest: e2e_store_race): a header edited WHILE its compile runs must not produce a cache entry
# keyed on the edited content (the object was compiled from the old one). Local: the compiler has read its inputs and
# written the object, then lingers 2 s (the edit lands meanwhile). Remote: the worker sleeps 2.5 s before compiling the
# copy it was shipped (the edit lands meanwhile). In both cases the store must be refused (summary field v4.racy_store_skips),
# and the rebuild with the edited header must compile it again: objects identical to a plain build's, no stale hit.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-race.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

# the "compiler": the real one, then (for -c compiles) 2 s of lingering after the object is written
cat > "$T/slowcc" <<EOF
#!/bin/sh
"$(command -v "$CC_REAL")" "\$@"; rc=\$?
case " \$* " in *" -c "*) sleep 2;; esac
exit \$rc
EOF
chmod +x "$T/slowcc"

fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$OPTICC_SUMMARY" "$p"; python3 -c "import json;s=json.load(open('$OPTICC_SUMMARY'));print($1)"; }
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }

setup_tree() {   # $1 dir
  mkdir -p "$1/inc"
  printf '#pragma once\n#define V 100\n' > "$1/inc/h.h"
  printf '#include "h.h"\nint t(void) { return V; }\n' > "$1/t.c"
  printf 'all: t.o\nt.o: t.c inc/h.h\n\t$(CC) -O2 -MD -MT $@ -MF $@.d -Iinc -c t.c -o t.o\nclean:\n\trm -f t.o t.o.d\n' > "$1/Makefile"
}
check_against_reference() {   # $1 label, $2 dir: plain build of the same (edited) tree must equal opticc's object
  rm -rf "$T/ref"; cp -r "$2" "$T/ref"; ( cd "$T/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "$1: reference build"
  cmp -s "$T/ref/t.o" "$2/t.o" || fail "$1: STALE object (does not match a plain build of the edited header)"
}
race() {   # $1 label, $2 dir, $3 seconds to wait before the edit, $4 compiler
  ( cd "$2" && make -s clean && make -s CC="$OPTICC $4" ) > "$T/out.txt" 2>&1 & local mk=$!
  sleep "$3"
  printf '#pragma once\n#define V 200\n' > "$2/inc/h.h"   # same size, other content
  wait "$mk" || fail "$1: build failed: $(cat "$T/out.txt")"
  [ "$(summ "s['v4']['racy_store_skips']")" -ge 1 ] || fail "$1: the store was not refused: $(summ "s['v4']")"
  ( cd "$2" && make -s clean && make -s CC="$OPTICC $4" ) > "$T/out.txt" 2>&1 || fail "$1: rebuild failed"
  check_against_reference "$1" "$2"
  echo "$1: store refused ($(summ "s['v4']['racy_store_skips']") racy store skips), rebuild after the edit is correct"
}

export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on

# 1 local compile (no workers)
setup_tree "$T/l"; sleep 2.4
export OPTICC_HOSTS="" OPTICC_SUMMARY=$T/summary-l.json OPTICC_LOCAL_CACHE_DIR=$T/lc-l
start_coord
mkdir -p "$T/warm"; printf 'int w(void) { return 1; }\n' > "$T/warm/w.c"   # another TU: the compiler's system dirs probed once
( cd "$T/warm" && "$OPTICC" "$T/slowcc" -O2 -c w.c -o w.o ) > /dev/null 2>&1 || fail "warm-up build"
sleep 0.3
race "1 local" "$T/l" 1.0 "$T/slowcc"
stop_coord

# 2 remote compile on a slow worker (header-push, the edit lands while the worker sleeps before compiling)
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
OPTICC_TEST_SLOW_MS=2500 "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] || fail "worker did not start: $(cat "$T/worker.err")"
setup_tree "$T/r"; sleep 2.4
export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_LOCAL_SLOTS=0 OPTICC_SUMMARY=$T/summary-r.json OPTICC_LOCAL_CACHE_DIR=$T/lc-r OPTICC_PROBE_CACHE=$T/probe.json
start_coord
race "2 remote" "$T/r" 1.0 "$(command -v "$CC_REAL")"
[ "$(summ "s['jobs_remote']")" -ge 1 ] || fail "2 remote: the compile did not run on the worker"
stop_coord
echo "E2E-STORE-RACE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
