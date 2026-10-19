#!/usr/bin/env bash
# ctest guard: runs after every test of the fixture "opticc_daemons" (FIXTURES_CLEANUP). Fails if any opticcd started
# from THIS build tree is still running (an orphaned listener left by a test) and terminates it by PID so the next run
# starts clean. Leftover coordinators fail the guard too: a coordinator listens only on a unix socket (no network port),
# but one left running means the test did not clean up, keeps CPU, memory and a socket alive past the test, and could be
# reused by a later wrapper with the same socket path. Every test must stop the coordinators it started or caused.
set -u
B=${OPTICC_BUILD:?}
D="$B/src/opticcd/opticcd"; C="$B/src/opticc/opticc --coordinator"
od=$(pgrep -f -- "$D" || true); co=$(pgrep -f -- "$C" || true)
for p in $od $co; do kill -TERM "$p" 2>/dev/null; done
sleep 0.5
for p in $od $co; do kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null; done
n_od=$(echo $od | wc -w | tr -d ' '); n_co=$(echo $co | wc -w | tr -d ' ')
echo "orphaned opticcd: $n_od (pids: ${od:-none}); leftover coordinators: $n_co (pids: ${co:-none})"
[ "$n_od" = 0 ] || { echo "FAIL: a test left opticcd daemons running (listeners)"; exit 1; }
[ "$n_co" = 0 ] || { echo "FAIL: a test left coordinators running (unix-socket only, but the test did not clean up)"; exit 1; }
echo "ok: no orphaned opticcd or coordinators"
