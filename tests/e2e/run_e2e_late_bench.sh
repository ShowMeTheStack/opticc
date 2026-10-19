#!/usr/bin/env bash
# End to end (ctest: e2e_late_bench): a worker answers before its compile benchmark is done (it listens
# first), so the coordinator rates it with the microbenchmark. The first compile result carrying the benchmark must
# replace that prior exactly once (summary "prior_refreshes"), with no restart, no new connection and identical objects.
#   A  worker benchmark delayed: hello ack has compile_bench_s = 0 -> after the benchmark lands, a build refreshes the
#      prior to local_compile_bench / worker_compile_bench (clamped), and only once
#   B  control: the coordinator starts after the worker's benchmark is known -> the probe already uses it, no refresh
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-lb.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {   # $1: benchmark delay (s)
  rm -f "$T/worker.out" "$T/worker.err"   # old logs must not satisfy the readiness loop (the child truncates them only after fork)
  OPTICC_TEST_COMPILE_BENCH_DELAY_S=$1 "$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
  [ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
}
stop_worker() { kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=; }
worker_bench() { python3 -c "
import json,sys
for l in open(sys.argv[1]):
    try: j=json.loads(l)
    except Exception: continue
    if j.get('event')=='compile-bench': print(j['s']); break
" "$T/worker.err"; }
start_coord() {
  export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
  export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=off OPTICC_LOCAL_SLOTS=0 OPTICC_PROBE_CACHE=$T/probe.json OPTICC_VERBOSE=1
  rm -f "$T/summary.json"
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
summary() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || fail "no coordinator"
  kill -USR1 "$p"; for _ in $(seq 100); do [ -s "$T/summary.json" ] && break; sleep 0.1; done; sleep 0.3; }
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; # wait for the PROCESS to exit (it may unlink the socket before its <=30 s drain of released jobs and uploads)
  for _ in $(seq 900); do { [ -n "$p" ] && kill -0 "$p" 2>/dev/null; } || { [ -S "$OPTICC_SOCK" ] && [ -z "$p" ]; } || break; sleep 0.1; done; sleep 0.3; }

S=$T/w; mkdir -p "$S/src"
for i in $(seq 0 11); do printf 'int f%d(int x) { int s = 0; for (int i = 0; i < x; ++i) s += i * %d; return s; }\n' "$i" "$i" > "$S/src/f$i.c"; done
cat > "$S/src/Makefile" <<'M'
OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
all: $(OBJS)
%.o: %.c
	$(CC) -O2 -c $< -o $@
M
cp -r "$S/src" "$S/ref"; cp -r "$S/src" "$S/d1"; cp -r "$S/src" "$S/d2"; cp -r "$S/src" "$S/d3"
( cd "$S/ref" && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"
same_objects() { for o in "$S/ref"/*.o; do cmp -s "$o" "$1/$(basename "$o")" || fail "object differs: $1/$(basename "$o")"; done; }

# --- A: benchmark arrives late --------------------------------------------------------------------------------------
start_worker 3
start_coord
grep -q '"compile_bench_s":0.0' "$T/coord.err" || fail "A: the probe should have seen no worker compile benchmark: $(grep probe "$T/coord.err")"
( cd "$S/d1" && make -s -j4 CC="$OPTICC $CC_REAL" ) > /dev/null 2>&1 || fail "A: build 1"
same_objects "$S/d1"
for _ in $(seq 300); do [ -n "$(worker_bench)" ] && break; sleep 0.1; done
WB=$(worker_bench); [ -n "$WB" ] || fail "A: the worker never finished its benchmark"
( cd "$S/d2" && make -s -j4 CC="$OPTICC $CC_REAL" ) > /dev/null 2>&1 || fail "A: build 2"
same_objects "$S/d2"
( cd "$S/d3" && make -s -j4 CC="$OPTICC $CC_REAL" ) > /dev/null 2>&1 || fail "A: build 3"
same_objects "$S/d3"
summary
python3 - "$T/summary.json" "$WB" <<'PY' || fail "A: prior refresh"
import json, sys
s = json.load(open(sys.argv[1])); wb = float(sys.argv[2])
pr = s.get("prior_refreshes", [])
print(f"A: worker bench {wb:.4f} s; refreshes {pr}; remote {s['jobs_remote']}/{s['jobs_total']}")
assert s["jobs_remote"] == s["jobs_total"] == 36, (s["jobs_remote"], s["jobs_total"])
assert len(pr) == 1, pr                                  # exactly once, although every later result carries it
e = pr[0]
assert abs(e["compile_bench_s"] - wb) < 1e-9, (e, wb)
want = min(8.0, max(0.125, e["local_compile_bench_s"] / wb))
assert abs(e["prior"] - want) < 1e-9, (e, want)
# one worker, no local samples (OPTICC_LOCAL_SLOTS=0): nothing can be fitted, so the estimate follows the new prior
assert abs(e["speed_after"] - e["prior"]) < 1e-9, e
node = [n for n in s["nodes"] if n["name"] == e["node"]][0]
assert abs(node["speed"] - e["prior"]) < 1e-9, (node, e)
PY
grep -q "compile benchmark arrived" "$T/coord.err" || fail "A: no refresh line in the coordinator log"
stop_coord; stop_worker

# --- B: control, benchmark known before the coordinator starts -------------------------------------------------------
: > "$T/worker.err"
start_worker 0
for _ in $(seq 300); do [ -n "$(worker_bench)" ] && break; sleep 0.1; done
[ -n "$(worker_bench)" ] || fail "B: the worker never finished its benchmark"
rm -rf "$S/d1"; cp -r "$S/src" "$S/d1"
start_coord
grep -q '"compile_bench_s":0.0' "$T/coord.err" && fail "B: the probe should have used the worker's compile benchmark"
( cd "$S/d1" && make -s -j4 CC="$OPTICC $CC_REAL" ) > /dev/null 2>&1 || fail "B: build"
same_objects "$S/d1"
summary
python3 -c "import json,sys;s=json.load(open(sys.argv[1]));pr=s.get('prior_refreshes',[]);print('B: refreshes',pr);assert pr==[],pr" "$T/summary.json" || fail "B: unexpected refresh"
stop_coord; stop_worker
echo "ok: late worker benchmark refreshes the prior once; a known benchmark needs no refresh"
