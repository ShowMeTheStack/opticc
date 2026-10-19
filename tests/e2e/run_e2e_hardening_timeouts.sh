#!/usr/bin/env bash
# End to end (ctest: e2e_hardening_timeouts): no peer can make a build hang, and a malformed request is answered.
#   A  a COMPILE / COMPILE_HP frame that does not decode gets a RESULT (worker-error) for its id, and the connection
#      keeps serving (PING -> PONG)                                          [opticcd reject_request]
#   B  a worker that accepts a job and never answers (OPTICC_TEST_SLOW_MS = 10 min, before the compiler starts):
#      the coordinator abandons the attempt at OPTICC_ATTEMPT_TIMEOUT_S=3 and the job falls back to a local compile,
#      header-push and classic                                               [coordinator attempt deadline]
#   C  a compiler that hangs on the worker (a wrapper script that execs `sleep 600` there): opticcd kills it at
#      OPTICC_COMPILE_TIMEOUT_S=2 and reports worker-error -> local fallback  [opticcd compile watchdog]
#   D  the same silent worker with the coordinator's deadline left at its default: the WRAPPER gives up at
#      OPTICC_WRAPPER_TIMEOUT_S=3 and compiles locally                       [wrapper <-> coordinator timeout]
# Every build must finish within 60 s with an object byte-identical to the plain build. Mutation check: without the
# respective timeout, B/C/D hang until the 60 s bound and fail; without reject_request, A gets no RESULT and fails.
set -u
. "$(dirname "$0")/lib.sh"
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-hardeningt.XXXXXX)
fail() { echo "FAIL: $*"; [ -f "$T/coord.err" ] && tail -5 "$T/coord.err"; cleanup; exit 1; }
stop_coord() { local p; p=$(lsof -t "$T/c.sock" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p" 2>/dev/null
               for _ in $(seq 100); do [ -S "$T/c.sock" ] || break; sleep 0.1; done; rm -f "$T/c.sock"; }
stop_worker() { [ -n "${WPID:-}" ] && { kill -9 "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; }; WPID=; }
cleanup() { stop_coord; stop_worker; wait 2>/dev/null; }
trap 'cleanup; e2e_kill_group' EXIT

mkdir -p "$T/src" "$T/fakebin"
printf '#include "h.h"\nint f(void) { return H; }\n' > "$T/src/f.c"; printf '#define H 5\n' > "$T/src/h.h"
# stall-cc: the real compiler, except that on a worker (OPTICC_E2E_STALL set) a compile (-c) never finishes
cat > "$T/fakebin/stall-cc" <<EOF
#!/bin/sh
if [ -n "\${OPTICC_E2E_STALL:-}" ]; then case " \$* " in *" -c "*) exec sleep 600;; esac; fi
exec "$CC_REAL" "\$@"
EOF
chmod +x "$T/fakebin/stall-cc"
( cd "$T/src" && "$CC_REAL" -O2 -c f.c -o "$T/ref.o" ) || fail "reference build"
sleep 2.4   # inputs older than any racy margin

start_worker() {   # env assignments for the worker as arguments
  stop_worker
  env "$@" "$OPTICCD" --listen 127.0.0.1:0 --accept plain --jobs 2 --token e2e --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out")
}
start_coord() {   # env assignments for the coordinator as arguments
  stop_coord
  env OPTICC_SOCK="$T/c.sock" OPTICC_HOSTS="127.0.0.1:$PORT/2,plain" OPTICC_TOKEN=e2e OPTICC_SECURITY=plain OPTICC_POLICY=ect-window \
      OPTICC_SUMMARY="$T/summary.json" OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=off OPTICC_PROBE_CACHE="$T/probe.json" \
      OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0 "$@" "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$T/c.sock" ] && break; sleep 0.05; done
  [ -S "$T/c.sock" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
build() {   # $1 label, $2 compiler, rest: env for the wrapper; must finish within 60 s with the reference object
  local label=$1 cc=$2; shift 2
  rm -f "$T/src/f.o"
  local t0; t0=$(date +%s)
  ( cd "$T/src" && env OPTICC_SOCK="$T/c.sock" "$@" timeout 60 "$OPTICC" "$cc" -O2 -c f.c -o f.o ) > "$T/out.txt" 2>&1 \
    || fail "$label: build failed or hung (rc $?, $(( $(date +%s) - t0 )) s): $(tail -3 "$T/out.txt")"
  cmp -s "$T/ref.o" "$T/src/f.o" || fail "$label: object differs from the plain build"
  echo "$label: built in $(( $(date +%s) - t0 )) s, object identical"
}
summary_field() { rm -f "$T/summary.json"; kill -USR1 "$(lsof -t "$T/c.sock" 2>/dev/null | head -1)" 2>/dev/null; for _ in $(seq 600); do [ -s "$T/summary.json" ] && break; sleep 0.05; done
                  python3 -c "import json,sys;print(json.load(open('$T/summary.json')).get(sys.argv[1],0))" "$1"; }

# --- A: malformed requests are answered -------------------------------------------------------------------------------
start_worker
python3 - "$PORT" <<'P' || fail "A: malformed request handling"
import json, socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20)
def rd(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError("worker closed the connection")
        b += c
    return b
def frame(t, payload): return struct.pack(">I", len(payload) + 1) + bytes([t]) + payload
def pack(h, blob=b""): hb = json.dumps(h).encode(); return struct.pack(">I", len(hb)) + hb + blob
def read_frame(): n = struct.unpack(">I", rd(4))[0]; t = rd(1)[0]; return t, rd(n - 1)
s.sendall(b"OPTICC-PLAIN\n"); assert rd(1) == b"\x00"
s.sendall(frame(1, json.dumps({"token": "e2e", "client": "e2e-hardening"}).encode()))
t, _ = read_frame(); assert t == 2, t                                   # HELLO_ACK
for typ, rid, payload in ((10, 77, {"id": 77, "argv": "not-a-list"}), (14, 78, {"id": 78})):
    s.sendall(frame(typ, pack(payload)))
    t, p = read_frame()                                                 # times out (20 s) when no RESULT comes
    hl = struct.unpack(">I", p[:4])[0]; h = json.loads(p[4:4 + hl])
    assert t == 11 and h["id"] == rid and h["status"] == "worker-error", (t, h)
    print("A: type %d id %d -> RESULT %s" % (typ, rid, h["status"]))
s.sendall(frame(10, b"\x00\x01garbage"))                                # no readable id: logged, no RESULT
s.sendall(frame(3, b"")); t, _ = read_frame(); assert t == 4, t         # the connection still serves: PONG
s.sendall(frame(13, b""))
print("A: connection still serving after malformed frames")
P
grep -q '"bad-compile-frame"' "$T/worker.err" || fail "A: worker did not log the malformed frame"

# --- B: silent worker, coordinator deadline ---------------------------------------------------------------------------
start_worker OPTICC_TEST_SLOW_MS=600000
for mode in header-push classic; do
  start_coord OPTICC_MODE=$mode OPTICC_ATTEMPT_TIMEOUT_S=3
  build "B $mode" "$CC_REAL"
  n=$(summary_field attempt_timeouts); [ "${n:-0}" -ge 1 ] || fail "B $mode: summary attempt_timeouts=$n (expected >= 1)"
  stop_coord
done

# --- C: hanging compiler, worker watchdog -----------------------------------------------------------------------------
start_worker OPTICC_E2E_STALL=1 OPTICC_COMPILE_TIMEOUT_S=2 PATH="$T/fakebin:$PATH"
start_coord OPTICC_MODE=header-push
build "C" "$T/fakebin/stall-cc"
python3 - "$T/worker.err" <<'P' || fail "C: the worker did not report a killed compile (worker-error, rc 137)"
import json, sys
jobs = []
for line in open(sys.argv[1]):
    try: j = json.loads(line)
    except Exception: continue
    if j.get("event") == "job": jobs.append(j)
print("C: worker jobs:", [(j.get("status"), j.get("rc")) for j in jobs])
assert any(j.get("status") == "worker-error" and j.get("rc") == 137 for j in jobs)
P
stop_coord

# --- D: silent worker, default coordinator deadline, wrapper timeout --------------------------------------------------
start_worker OPTICC_TEST_SLOW_MS=600000
start_coord OPTICC_MODE=header-push
build "D" "$CC_REAL" OPTICC_WRAPPER_TIMEOUT_S=3
grep -q 'compiling locally' "$T/out.txt" || fail "D: the wrapper did not report its local fallback"
stop_coord

echo "E2E-HARDENING-TIMEOUTS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
