#!/usr/bin/env bash
# GCC through opticc. Every other e2e test uses clang. Loopback:
#  1. 31 generated C files (stdio / shared header / kbuild-style -Wp,-MMD dependency flag) built with gcc via opticc in
#     header-push and in classic mode, no local slots: objects and dependency files byte-identical to plain gcc.
#  2. With the client cache on, a repeat build (after make clean) is byte-identical too; the cache hits it gets are
#     PRINTED (gcc runs an external `as`, which the probe cache refuses; the count is informational, not a pass
#     criterion).
# The remote/local split per mode is printed; remote must be > 0 in both modes (gcc is actually distributed).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
GCC=$(command -v gcc) || { echo "skip: no gcc"; exit 77; }   # ctest SKIP_RETURN_CODE 77 (a skip is not a pass)
T=$(mktemp -d /tmp/opticc-e2e-gcc.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/src/inc"
printf '#pragma once\n#define SHIFT 3\nstatic inline int mix(int a, int b) { return (a << SHIFT) ^ b; }\n' > "$T/src/inc/shared.h"
for i in $(seq 0 29); do
  case $((i % 3)) in
    0) printf '#include <stdio.h>\nint f%d(int x){ if (x > 1000000) printf("%%d\\n", x); return x * %d + 1; }\n' "$i" "$i" ;;
    1) printf '#include "shared.h"\nint f%d(int x){ return mix(x, %d); }\n' "$i" "$i" ;;
    2) printf '#include <string.h>\n#include "inc/shared.h"\nint f%d(const char *s){ return (int)strlen(s) + mix(%d, 1); }\n' "$i" "$i" ;;
  esac > "$T/src/f$i.c"
done
printf 'int tally(int n){ int s=0; for(int i=0;i<n;i++) s+=i; return s; }\n' > "$T/src/tally.c"
cat > "$T/src/Makefile" <<'M'
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
all: $(OBJS)
%.o: %.c
	$(CC) -O2 -Iinc -Wp,-MMD,.$@.d -c $< -o $@
clean:
	rm -f *.o .*.d
M
cp -r "$T/src" "$T/ref"
( cd "$T/ref" && make -s -j8 CC="$GCC" ) || fail "reference gcc build"
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window
export OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_PROBE_CACHE=$T/probe.json
start_coord() { "$OPTICC" --coordinator > "$T/coord.$1.out" 2> "$T/coord.$1.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done; [ -S "$OPTICC_SOCK" ] || fail "coordinator ($1): $(cat "$T/coord.$1.err")"; }
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"
  for _ in $(seq 900); do { [ -n "$p" ] && kill -0 "$p" 2>/dev/null; } || break; sleep 0.1; done; rm -f "$OPTICC_SOCK"; }
same() { local d=$1 n; for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$d/$n" || fail "$2: object differs: $n"
    cmp -s "$T/ref/.$n.d" "$d/.$n.d" || fail "$2: dependency file differs: .$n.d: $(diff "$T/ref/.$n.d" "$d/.$n.d" | head -3)"; done; }
split() { python3 -c "import json,sys;s=json.load(open(sys.argv[1]));o={}
for j in s['jobs']: o[j['outcome']]=o.get(j['outcome'],0)+1
print(json.dumps(o));sys.exit(0 if o.get('remote',0)>0 or sys.argv[2]=='any' else 1)" "$1" "$2"; }
for mode in header-push classic; do
  rm -rf "$T/$mode"; cp -r "$T/src" "$T/$mode"
  OPTICC_MODE=$mode OPTICC_SUMMARY=$T/sum.$mode.json start_coord "$mode"
  ( cd "$T/$mode" && make -s -j8 CC="$OPTICC $GCC" ) > "$T/make.$mode.out" 2>&1 || fail "$mode build: $(tail -5 "$T/make.$mode.out")"
  stop_coord; same "$T/$mode" "$mode"
  echo "ok: gcc $mode: 31 objects + dependency files byte-identical; outcomes $(split "$T/sum.$mode.json" remote || fail "$mode: no remote compile")"
done
rm -rf "$T/cache"; cp -r "$T/src" "$T/cache"
for pass in cold repeat; do
  OPTICC_MODE=header-push OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_SUMMARY=$T/sum.$pass.json start_coord "$pass"
  ( cd "$T/cache" && make -s clean && make -s -j8 CC="$OPTICC $GCC" ) > "$T/make.$pass.out" 2>&1 || fail "cache $pass build"
  stop_coord; same "$T/cache" "cache $pass"
  echo "ok: gcc cache $pass: byte-identical; outcomes $(split "$T/sum.$pass.json" any)"
done
echo "E2E-GCC PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
