#!/usr/bin/env bash
# tests/perf/direct_bench.py starts its worker with --exit-with-parent and an atexit kill. Kill the benchmark with
# SIGKILL (no atexit, no finally: what a ctest timeout or an OOM kill does) while its worker runs, and require the worker
# to be gone within 5 s.
# Clean-up: SIGKILL of the benchmark leaves its `make` running; make's next `opticc` wrapper finds no coordinator and
# starts a NEW one (ppid 1, own session, started ~3 s after the kill). So, after the worker check, the test kills the
# benchmark's whole process group (it runs under setsid: python, make, wrappers) and then every coordinator whose
# environment points into the benchmark's directory (OPTICC_SOCK=$T/db/<tool>.sock), by PID, TERM then KILL, and finally
# requires that none is left.
set -u
R="$(cd "$(dirname "$0")/../.." && pwd)"
B=${OPTICC_BUILD:-$R/build}
command -v clang >/dev/null || { echo "skip: no clang"; exit 77; }   # ctest SKIP_RETURN_CODE 77
T=$(mktemp -d /tmp/opticc-e2e-dbk.XXXXXX)
PY=
ours() {   # PIDs of coordinators of THIS build whose environment points into the benchmark's directory
  local p; for p in $(pgrep -f -- "$B/src/opticc/opticc --coordinator"); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -q "^OPTICC_SOCK=$T/db/" && echo "$p"; done; }
stop_ours() { local p ps; ps=$(ours); for p in $ps; do kill -TERM "$p" 2>/dev/null; done
  for p in $ps; do for _ in $(seq 450); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; kill -KILL "$p" 2>/dev/null; done; }
cleanup() { [ -n "$PY" ] && kill -KILL -- "-$PY" 2>/dev/null; sleep 1; stop_ours; rm -rf "$T"; }
trap cleanup EXIT
# ctest exports OPTICC_EXIT_WITH_PARENT=1 to every bench test; unset here so the benchmark's OWN --exit-with-parent is what
# is tested. setsid: the benchmark is the leader of its own process group (its PID is the group ID).
setsid env -u OPTICC_EXIT_WITH_PARENT OPTICC_BUILD=$B python3 "$R/tests/perf/direct_bench.py" --tus 12 --headers 60 --tools direct -j 2 --dir "$T/db" > "$T/bench.out" 2>&1 & PY=$!
for _ in $(seq 1200); do grep -q listening "$T/db/worker.out" 2>/dev/null && break; kill -0 "$PY" 2>/dev/null || break; sleep 0.1; done
grep -q listening "$T/db/worker.out" 2>/dev/null || { echo "FAIL: the benchmark's worker never started"; tail -20 "$T/bench.out"; exit 1; }
WP=$(pgrep -P "$PY" -x opticcd | head -1)
[ -n "$WP" ] || { echo "FAIL: no opticcd child of the benchmark (pid $PY)"; exit 1; }
for _ in $(seq 100); do [ -n "$(pgrep -P "$PY" -x opticc)" ] && break; sleep 0.1; done   # prefer killing mid-build
kill -KILL "$PY"   # the benchmark ALONE first: the worker must exit by itself (--exit-with-parent)
for _ in $(seq 50); do kill -0 "$WP" 2>/dev/null || break; sleep 0.1; done
fail=0
if kill -0 "$WP" 2>/dev/null; then echo "FAIL: worker $WP survived its killed benchmark (orphaned listener)"; kill -KILL "$WP"; fail=1
else echo "ok:   worker $WP exited with its SIGKILLed benchmark (--exit-with-parent)"; fi
# then the rest of the benchmark's group (make and its wrappers), then its coordinators (incl. any a wrapper re-spawned)
kill -KILL -- "-$PY" 2>/dev/null; wait "$PY" 2>/dev/null; sleep 1
echo "info: coordinators of this benchmark after the group kill: $(ours | tr '\n' ' ')(stopped here by PID)"
stop_ours; PY=
sleep 3
left=$(ours | tr '\n' ' ')
if [ -n "$left" ]; then echo "FAIL: coordinators of the killed benchmark still running: $left"
  ps -o pid=,ppid=,pgid=,etimes=,args= -p "${left// /,}" | sed 's/^/diag: /'; fail=1
else echo "ok:   no coordinator of the killed benchmark left"; fi
exit $fail
