#!/usr/bin/env bash
# End to end (ctest: e2e_cache_usr1_nonblocking): a SIGUSR1 summary waits for released jobs' store decisions
# (e2e_store_race), but that wait must not stall the coordinator's watcher loop, which carries the hedging timers, the
# idle timeout and SIGTERM handling. A local compile whose store is held 3 s after the client was released
# (OPTICC_TEST_STORE_DELAY_MS), SIGUSR1 right after the build returns:
#   - the USR1 summary appears only after the held store (>= 2.5 s after the signal): the wait still happens
#   - the watcher's longest gap between passes (summary v4.watcher_lag_max_s) stays below 1 s (about 3 s if the wait
#     ran inside the loop)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cacheu1.XXXXXX)
CPID=
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "$CPID" ] && kill "$CPID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"; export OPTICC_KEY_DIR=$T/ckeys
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off
OPTICC_TEST_STORE_DELAY_MS=3000 "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" & CPID=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
mkdir -p "$T/w"; printf 'int f(int x) { return x + 2; }\n' > "$T/w/f.c"; sleep 2.4
( cd "$T/w" && "$OPTICC" "$CC_REAL" -O2 -MD -MF f.o.d -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "build: $(cat "$T/out.txt")"
rm -f "$OPTICC_SUMMARY"; t0=$(date +%s.%N); kill -USR1 "$CPID"
for _ in $(seq 200); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done
[ -s "$OPTICC_SUMMARY" ] || fail "no USR1 summary within 10 s"
dt=$(python3 -c "print(round($(date +%s.%N) - $t0, 2))")
NJ=$(python3 -c "import json;print(len(json.load(open('$OPTICC_SUMMARY'))['jobs']))")
echo "USR1 summary after ${dt} s with $NJ job(s)"
[ "$NJ" -ge 1 ] || fail "the USR1 summary predates the released job's record"
python3 -c "import sys;sys.exit(0 if $dt >= 2.5 else 1)" || fail "the USR1 summary was written after ${dt} s, before the held store (3 s) finished: it does not wait for released jobs"
kill -TERM "$CPID"; for _ in $(seq 600); do kill -0 "$CPID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$CPID" 2>/dev/null && fail "coordinator did not exit"; wait "$CPID" 2>/dev/null; CPID=
LAG=$(python3 -c "import json;print(json.load(open('$OPTICC_SUMMARY'))['v4']['watcher_lag_max_s'])")
echo "watcher_lag_max_s $LAG"
python3 -c "import sys;sys.exit(0 if $LAG < 1.0 else 1)" || fail "the coordinator's watcher loop stalled for $LAG s (the USR1 wait ran inside it)"
echo "E2E-CACHE-USR1-NONBLOCKING PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
