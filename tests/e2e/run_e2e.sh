#!/usr/bin/env bash
# End-to-end test on loopback (ctest: e2e_loopback).
#  1. 60 generated C files built with `make -j8 CC="opticc cc"` against a local
#     opticcd; every object must be byte-identical to a plain local build,
#     dep files must name the real object, a balanced share (30-90%) of jobs must have gone remote
#     (tiny TUs on loopback: the local-slot threshold keeps roughly half local with one local slot),
#     the summary JSON must parse.
#  2. A file with a compile error must fail through opticc with rc!=0 and the
#     compiler's diagnostic on stderr.
#  3. The worker is killed mid-build; the build must still succeed (fallback).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

# --- generate sources ------------------------------------------------------
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
    2) printf '#include <math.h>\nint f%d(int x){ return (int)sqrt((double)x) + %d; }\n' "$i" "$i" > "$f" ;;
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
clean:
	rm -f *.o *.d
M
cp -r "$T/src" "$T/ref"; cp -r "$T/src" "$T/dist"; cp -r "$T/src" "$T/kill"

# --- reference build -------------------------------------------------------
( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"

# --- worker + coordinator ----------------------------------------------------
# keys: client key authorized on the worker; the host spec carries the worker's public key (Noise IK)
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); [ -n "$PORT" ] || fail "worker did not start"
PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out"); [ -n "$PUB" ] || fail "worker printed no public key"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=1 OPTICC_VERBOSE=1
# fresh probe cache per run: client and loopback worker share this machine, so both benchmarks must be taken at the same
# moment (a 7-day cached client bench from an idle moment against a fresh worker bench under load skewed the remote share)
export OPTICC_PROBE_CACHE=$T/probe-cache.json
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" & CPID=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"

# --- 1. distributed build ---------------------------------------------------
t0=$(python3 -c 'import time;print(time.time())')
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "distributed build rc"
t1=$(python3 -c 'import time;print(time.time())')
for o in "$T/ref"/*.o; do
  n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "object differs: $n"
  d=${n%.o}.d; grep -q "^$n:" "$T/dist/$d" || fail "dep file target wrong: $d -> $(head -1 "$T/dist/$d")"
done
echo "ok: 61 objects byte-identical, dep files correct"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary check"
import json, sys
s = json.load(open(sys.argv[1]))
tot, rem = s["jobs_total"], s["jobs_remote"]
print(f"summary: total={tot} remote={rem} local={s['jobs_local']} fallbacks={s['fallbacks']} rho={s['rho']:.3f} wall={s['wall_s']:.2f}s window={s['nodes'][1]['window']}")
assert tot == 61, tot
assert 0.3 * tot <= rem <= 0.9 * tot, (rem, tot)
assert s["fallbacks"] == 0
PY

# --- 2. compile error propagates --------------------------------------------
printf 'int broken(void) { return 1 +; }\n' > "$T/dist/bad.c"
( cd "$T/dist" && "$OPTICC" "$CC_REAL" -O2 -c bad.c -o bad.o 2> bad.err ); rc=$?
[ $rc -ne 0 ] || fail "compile error not propagated (rc 0)"
grep -q "error:" "$T/dist/bad.err" || fail "diagnostic missing: $(cat "$T/dist/bad.err")"
[ ! -f "$T/dist/bad.o" ] || fail "object produced despite error"
echo "ok: compile error rc=$rc, diagnostic present"

# --- 3. worker killed mid-build -> fallback -----------------------------------
( cd "$T/kill" && make -s -j8 CC="$OPTICC $CC_REAL" ) & MPID=$!
sleep 0.25; kill -9 $WPID; wait $WPID 2>/dev/null; WPID=
wait $MPID || fail "build after worker kill rc"
for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/kill/$n" || fail "object differs after fallback: $n"; done
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary after kill"
import json, sys
s = json.load(open(sys.argv[1]))
print(f"after kill: total={s['jobs_total']} remote={s['jobs_remote']} fallbacks={s['fallbacks']} node_disabled={s['nodes'][1]['disabled']}")
assert s["jobs_total"] == 123
assert s["nodes"][1]["disabled"] or s["fallbacks"] >= 1
PY
echo "ok: build survived worker death"

# --- overhead measurement (informational) -----------------------------------
( cd "$T/ref" && make -s clean && t2=$(python3 -c 'import time;print(time.time())') && make -s -j8 CC="$CC_REAL" && t3=$(python3 -c 'import time;print(time.time())') && echo "timing: plain cc -j8 $(python3 -c "print(f'{$t3-$t2:.2f}')")s vs opticc -j8 (1 local slot + loopback worker x4) $(python3 -c "print(f'{$t1-$t0:.2f}')")s" )
echo "E2E PASS ($T)"
cleanup; trap - EXIT
rm -rf "$T"
exit 0
