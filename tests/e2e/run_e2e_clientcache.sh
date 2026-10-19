#!/usr/bin/env bash
# Client-local object cache end to end (ctest: e2e_client_cache). The worker runs WITHOUT an object cache.
#  1. header-push build of the 61-TU project: all remote, every result stored in the client cache;
#  2. make clean + rebuild: every TU answered from the client cache (no dispatch), objects and .d byte-identical;
#  3. shared.h edited (in the reference too), make clean, rebuild: exactly the 40 TUs that include it miss (remote),
#     the 21 others hit; objects identical to a fresh local build (no stale hits).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-ccache.XXXXXX)
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
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); [ -n "$PORT" ] || fail "worker did not start: $(cat "$T/worker.err")"
PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" & CPID=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
# the coordinator first writes every queued direct entry, then the summary: wait for a NEW summary file
fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
check() {   # $1 label, $2 expected client hits, $3 expected remote (min)
  fresh_summary "$T/summary.json" $CPID
  python3 - "$T/summary.json" "$1" "$2" "$3" <<'PY' || fail "$1: summary"
import json, sys
s = json.load(open(sys.argv[1])); lab, hits, rem = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
import os
jobs = s["jobs"]; prev, pfast = map(int, open(sys.argv[1] + ".n").read().split()) if os.path.exists(sys.argv[1] + ".n") else (0, 0)
fast = s["client_fast_hits"] - pfast   # answered by the wrapper's direct path (never reach the coordinator)
cur = jobs[prev:]; open(sys.argv[1] + ".n", "w").write(f"{len(jobs)} {s['client_fast_hits']}")
ch = sum(1 for j in cur if j["outcome"] == "client-cache") + fast; rm = sum(1 for j in cur if j["outcome"] == "remote")
print(f"{lab}: jobs {len(cur)} client-cache hits {ch} (direct {fast}) remote {rm}")
assert ch == hits, (ch, hits); assert rm >= rem, (rm, rem)
PY
}
same() { for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "$1: object differs: $n"; d=${n%.o}.d; cmp -s "$T/ref/$d" "$T/dist/$d" || fail "$1: dep differs: $d"; done; }
t0=$(now); ( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 1"; t1=$(now)
same "build 1"; check "build 1 (cold)" 0 61
( cd "$T/dist" && make -s clean ); t2=$(now); ( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 2"; t3=$(now)
same "build 2"; check "build 2 (repeat)" 61 0
echo "ok: repeat build answered from the client cache (61/61), byte-identical; cold $(python3 -c "print('%.2f'%($t1-$t0))") s, repeat $(python3 -c "print('%.2f'%($t3-$t2))") s"
printf '#define EXTRA_V2 1\n' >> "$T/dist/inc/shared.h"; printf '#define EXTRA_V2 1\n' >> "$T/ref/inc/shared.h"
( cd "$T/ref" && make -s clean && make -s -j8 CC="$CC_REAL" ) || fail "reference rebuild"
( cd "$T/dist" && make -s clean && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 3"
same "build 3"; check "build 3 (shared.h edited)" 21 40
echo "ok: header edit invalidates exactly its 40 dependents; 21 hits; no stale object"
# a comment-only edit (a comment appended to the header) keeps every token's position -> all 61 hit,
# and the objects/dep files equal a fresh local build of the edited tree (no stale object can result)
printf '/* opticc-touch %s */\n' "$(date +%s%N)" >> "$T/dist/inc/shared.h"; cp "$T/dist/inc/shared.h" "$T/ref/inc/shared.h"
( cd "$T/ref" && make -s clean && make -s -j8 CC="$CC_REAL" ) || fail "reference rebuild 4"
( cd "$T/dist" && make -s clean && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 4"
same "build 4"; check "build 4 (comment appended to shared.h)" 61 0
echo "ok: comment-only header edit: 61/61 client-cache hits, byte-identical to a fresh build"
# and a comment edit that MOVES tokens (inserted at the top) must miss for the 40 dependents
python3 - "$T/dist/inc/shared.h" <<'PY'
import sys; p=sys.argv[1]; t=open(p).read(); open(p,"w").write("// moved\n" + t)
PY
cp "$T/dist/inc/shared.h" "$T/ref/inc/shared.h"
( cd "$T/ref" && make -s clean && make -s -j8 CC="$CC_REAL" ) || fail "reference rebuild 5"
( cd "$T/dist" && make -s clean && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "build 5"
same "build 5"; check "build 5 (comment line inserted: lines move)" 21 40
echo "ok: a token-moving comment edit invalidates its 40 dependents"
echo "E2E-CLIENT-CACHE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
