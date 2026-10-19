#!/usr/bin/env bash
# Hedging in the real coordinator and the OPTICC_WINDOW path.
# Two loopback workers over the plain channel: worker A sleeps 3 s before every compile (OPTICC_TEST_SLOW_MS),
# worker B is normal. With policy ect-window-hedge (kappa rule) the coordinator's timer thread must duplicate
# jobs stuck on A at kappa x E (E ~ tens of ms), so the build finishes far sooner than the slow path allows,
# objects stay byte-identical, and the summary reports hedges. Then OPTICC_WINDOW=slots must set every
# worker's window to its core count.
set -euo pipefail
B=${OPTICC_BUILD:-build}; OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd; CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-hedge.XXXXXX)
fail() { echo "FAIL: $*"; [ -f "$T/coord.err" ] && tail -20 "$T/coord.err"; exit 1; }
cleanup() { for p in ${WA:-} ${WB:-} ${CPID:-} ${CPID2:-}; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$T/src/inc"
echo '#define K 7' > "$T/src/inc/k.h"
for i in $(seq 1 24); do printf '#include "k.h"\nint f%d(int x) { return x * K + %d; }\n' $i $i > "$T/src/f$i.c"; done
cat > "$T/src/Makefile" <<'M'
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
CFLAGS ?= -O2 -Iinc
all: $(OBJS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
M
cp -r "$T/src" "$T/ref"; cp -r "$T/src" "$T/dist"; cp -r "$T/src" "$T/win"
( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"
# workers: A slow, B normal; plain channel on loopback with explicit opt-in on both sides
OPTICC_TEST_SLOW_MS=3000 "$OPTICCD" --listen 127.0.0.1:0 --accept plain --jobs 4 --token e2e > "$T/wa.out" 2> "$T/wa.err" & WA=$!
"$OPTICCD" --listen 127.0.0.1:0 --accept plain --jobs 4 --token e2e > "$T/wb.out" 2> "$T/wb.err" & WB=$!
for f in wa wb; do for _ in $(seq 300); do grep -q listening "$T/$f.out" 2>/dev/null && break; sleep 0.1; done; done
PA=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/wa.out"); PB=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/wb.out")
[ -n "$PA" ] && [ -n "$PB" ] || fail "workers did not start"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PA/4,plain 127.0.0.1:$PB/4,plain" OPTICC_TOKEN=e2e OPTICC_SECURITY=plain
export OPTICC_POLICY=ect-window-hedge OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_VERBOSE=1 OPTICC_PREDICTOR=affine
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" & CPID=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
t0=$(python3 -c 'import time;print(time.time())')
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "hedged build rc"
t1=$(python3 -c 'import time;print(time.time())')
for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "object differs: $n"; done
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" "$t0" "$t1" <<'PY' || fail "hedge summary check"
import json, sys
s = json.load(open(sys.argv[1])); wall = float(sys.argv[3]) - float(sys.argv[2])
hedged = [j for j in s["jobs"] if j["hedges"] > 0]
slow = [j for j in s["jobs"] if j["outcome"] == "remote" and j["node"] == 1]
print(f"hedge: wall={wall:.2f}s jobs={s['jobs_total']} hedges={s['hedges']} hedged_jobs={len(hedged)} remote_on_slow={len(slow)} fallbacks={s['fallbacks']} policy={s['policy']}")
assert s["fallbacks"] == 0
assert s["hedges"] >= 2, s["hedges"]                 # jobs stuck on the slow worker were duplicated (hedge_max = #workers = 2 at a time)
# 24 jobs; with 4 slots on the slow worker and 3 s per compile, no hedging would take >= 3 s per wave on A;
# hedged duplicates on B finish in tens of ms, so the whole build must be well under one slow wave per 4 jobs
assert wall < 9.0, wall
assert all(j["rc"] == 0 for j in s["jobs"])
PY
echo "ok: coordinator hedging fires on timers (slow worker duplicated, objects identical)"
kill $CPID; wait $CPID 2>/dev/null || true; rm -f "$OPTICC_SOCK"

# --- OPTICC_WINDOW=slots: windows equal the core counts -------------------------------------------
export OPTICC_POLICY=ect-window OPTICC_WINDOW=slots OPTICC_SUMMARY=$T/summary2.json
"$OPTICC" --coordinator > "$T/coord2.out" 2> "$T/coord2.err" & CPID2=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
( cd "$T/win" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "window build rc"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID2; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary2.json" <<'PY' || fail "window summary check"
import json, sys
s = json.load(open(sys.argv[1]))
ws = [n["window"] for n in s["nodes"][1:]]
print("windows:", ws, "window_scale:", s["window_scale"])
assert s["window_scale"] == 0.0
assert ws == [4, 4], ws
PY
echo "ok: OPTICC_WINDOW=slots sets N_k = cores"
