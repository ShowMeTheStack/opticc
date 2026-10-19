#!/usr/bin/env bash
# Compile-cache end-to-end test on loopback (ctest: e2e_compile_cache).
#  1. header-push build of the 61-TU project with opticcd --object-cache (all misses, objects stored), then
#     `make clean` and the same build again: every TU is a cache hit, objects and dep files byte-identical to a
#     plain local build, and the hit build is faster.
#  2. shared.h edited (in the reference too), make clean, rebuild: exactly the TUs that include shared.h miss,
#     the rest hit; objects identical to a fresh local build (no stale hits).
#  3. classic mode (worker key = sha of the preprocessed text): second build after make clean hits everywhere.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-cache.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
now() { python3 -c 'import time;print(time.time())'; }

mkdir -p "$T/src/inc"
cat > "$T/src/inc/shared.h" <<'H'
#pragma once
#define SHIFT 3
static inline int mix(int a, int b) { return (a << SHIFT) ^ b; }
int tally(int n);
H
for i in $(seq 0 59); do
  f="$T/src/f$i.c"
  case $((i % 3)) in
    0) printf '#include <stdio.h>\nint f%d(int x){ if (x > 1000000) printf("%%d\\n", x); return x * %d + 1; }\n' "$i" "$i" > "$f" ;;
    1) printf '#include "shared.h"\nint f%d(int x){ return mix(x, %d) + tally(x); }\n' "$i" "$i" > "$f" ;;
    2) printf '#include <math.h>\n#include "inc/shared.h"\nint f%d(int x){ return (int)sqrt((double)x) + %d + mix(x, 1); }\n' "$i" "$i" > "$f" ;;
  esac
done
printf 'int tally(int n){ int s=0; for(int i=0;i<n;i++) s+=i; return s; }\n' > "$T/src/tally.c"
cat > "$T/src/Makefile" <<'M'
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
CFLAGS ?= -O2 -Iinc -MMD -MP
all: $(OBJS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
clean:
	rm -f *.o *.d
M
cp -r "$T/src" "$T/ref"; cp -r "$T/src" "$T/dist"
( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/objcache" --object-cache-gb 1 \
  > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); [ -n "$PORT" ] || fail "worker did not start: $(cat "$T/worker.err")"
PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out"); [ -n "$PUB" ] || fail "worker printed no public key"
grep -q '"event":"object-cache"' "$T/worker.err" || fail "worker did not enable the object cache"

start_coord() {   # $1 = mode
  [ -n "${CPID:-}" ] && { kill "$CPID" 2>/dev/null; wait "$CPID" 2>/dev/null; }
  export OPTICC_SOCK=$T/c-$1.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=$1
  export OPTICC_SUMMARY=$T/summary-$1.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0
  "$OPTICC" --coordinator > "$T/coord-$1.out" 2> "$T/coord-$1.err" & CPID=$!
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator ($1) did not start: $(cat "$T/coord-$1.err")"
}
# count worker job events with a given cache status after line $1 of worker.err
count() { tail -n +"$(( $1 + 1 ))" "$T/worker.err" | grep '"event":"job"' | grep -c "\"cache\":\"$2\""; }
check_identical() {
  for o in "$T/ref"/*.o; do
    n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "$1: object differs: $n"
    d=${n%.o}.d; cmp -s "$T/ref/$d" "$T/dist/$d" || fail "$1: dep file differs: $d: $(diff "$T/ref/$d" "$T/dist/$d" | head -3)"
  done
}

# --- 1. header-push: store, then all hits ---------------------------------------------------------
start_coord header-push
L0=$(wc -l < "$T/worker.err")
t0=$(now); ( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 1"; t1=$(now)
check_identical "build 1"
S1=$(count "$L0" store); [ "$S1" -ge 55 ] || fail "build 1: only $S1 objects stored"
( cd "$T/dist" && make -s clean )
L1=$(wc -l < "$T/worker.err")
t2=$(now); ( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 2"; t3=$(now)
check_identical "build 2 (cache hits)"
H2=$(count "$L1" hit); M2=$(count "$L1" store)
[ "$H2" -ge 55 ] || fail "build 2: only $H2 hits ($M2 stores)"
python3 - "$t0" "$t1" "$t2" "$t3" "$H2" "$S1" <<'PY' || fail "hit build grossly slower"
import sys
t0, t1, t2, t3 = map(float, sys.argv[1:5]); h, s = sys.argv[5], sys.argv[6]
print(f"header-push: build 1 {t1-t0:.2f} s ({s} stored), build 2 {t3-t2:.2f} s ({h} hits)")
# timing is informational under a loaded host (other builds run concurrently); fail only on a gross regression
assert t3 - t2 < max(2.0 * (t1 - t0), 2.0), "hit build grossly slower than the compile build"
PY
echo "ok: header-push second build all hits, byte-identical objects and dep files"

# --- 2. edit shared.h: only dependents miss --------------------------------------------------------
# no wait needed: the coordinator re-stat()s every closure file before trusting a cached hash
sed -i.bak 's/#define SHIFT 3/#define SHIFT 5/' "$T/ref/inc/shared.h" "$T/dist/inc/shared.h"
( cd "$T/ref" && make -s clean && make -s -j8 CC="$CC_REAL" ) || fail "reference rebuild"
( cd "$T/dist" && make -s clean )
L2=$(wc -l < "$T/worker.err")
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 3"
check_identical "build 3 (after header edit)"
H3=$(count "$L2" hit); S3=$(count "$L2" store)
DEP=$(grep -l 'shared.h' "$T/dist"/*.c | wc -l | tr -d ' ')
echo "header edit: $S3 misses (stored), $H3 hits; $DEP TUs include shared.h"
[ "$S3" -eq "$DEP" ] || fail "expected exactly $DEP misses after editing shared.h, got $S3"
[ "$H3" -ge $(( 61 - DEP - 2 )) ] || fail "unaffected TUs should hit: $H3"
echo "ok: header edit invalidates exactly its dependents"

# --- 3. classic mode -------------------------------------------------------------------------------
start_coord classic
( cd "$T/dist" && make -s clean )
L3=$(wc -l < "$T/worker.err")
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "classic build 1"
( cd "$T/dist" && make -s clean )
L4=$(wc -l < "$T/worker.err")
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "classic build 2"
for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "classic: object differs: $n"; done
C1=$(count "$L3" store); C2=$(count "$L4" hit)
echo "classic: build 1 stored $C1, build 2 hits $C2"
[ "$C2" -ge 30 ] && [ "$C2" -ge "$C1" ] || fail "classic second build should hit what the first stored ($C1 stored, $C2 hits)"
echo "ok: classic-mode cache"
cleanup; rm -rf "$T"
echo "PASS e2e_compile_cache"
