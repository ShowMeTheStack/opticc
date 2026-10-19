#!/usr/bin/env bash
# End to end (ctest: e2e_hardening_bounds): the WORKER applies the frame and probe bounds on a live connection; the unit
# tests (hard_frame_limit_pre_auth, ...) set the limits themselves and cannot show that opticcd does.
#   P  before HELLO: a frame header declaring 100 MiB is refused at once ("bad frame length ... > limit 65536"), the
#      connection is closed within 2 s; without the pre-auth limit the worker waits to read 100 MiB
#   W  after HELLO: a BW probe asking for 512 MiB of download is answered with at most 64 MiB
set -u
. "$(dirname "$0")/lib.sh"
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICCD=$B/src/opticcd/opticcd
T=$(mktemp -d /tmp/opticc-e2e-hardeningb.XXXXXX)
fail() { echo "FAIL: $*"; [ -f "$T/worker.err" ] && tail -5 "$T/worker.err"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && { kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; }; WPID=; }
trap 'cleanup; e2e_kill_group' EXIT
"$OPTICCD" --listen 127.0.0.1:0 --accept plain --jobs 1 --token e2e --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out")
python3 - "$PORT" <<'P' || fail "P: pre-auth frame limit"
import socket, struct, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
s.sendall(b"OPTICC-PLAIN\n"); assert s.recv(1) == b"\x00"
s.sendall(struct.pack(">I", 100 << 20) + bytes([1]))      # a "HELLO" declaring 100 MiB; no payload follows
s.settimeout(2.0); t0 = time.time()
try:
    closed = s.recv(1) == b""
except socket.timeout:
    closed = False
except ConnectionResetError:   # the worker closed with our unread bytes queued: a reset is a close
    closed = True
dt = time.time() - t0
assert closed, "connection still open %.1f s after a 100 MiB pre-auth frame header (the worker is waiting for the payload)" % dt
print("P: 100 MiB pre-auth frame refused, connection closed after %.2f s" % dt)
P
for _ in $(seq 30); do grep -q "limit 65536" "$T/worker.err" && break; sleep 0.1; done
grep -q "limit 65536" "$T/worker.err" || fail "P: the worker did not log the pre-auth frame limit"
python3 - "$PORT" <<'P' || fail "W: BW probe cap"
import json, socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30)
def rd(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError("worker closed the connection")
        b += c
    return b
def frame(t, payload): return struct.pack(">I", len(payload) + 1) + bytes([t]) + payload
def pack(h, blob=b""): hb = json.dumps(h).encode(); return struct.pack(">I", len(hb)) + hb + blob
s.sendall(b"OPTICC-PLAIN\n"); assert rd(1) == b"\x00"
s.sendall(frame(1, json.dumps({"token": "e2e", "client": "e2e-hardening-bounds"}).encode()))
n = struct.unpack(">I", rd(4))[0]; t = rd(1)[0]; rd(n - 1); assert t == 2, t        # HELLO_ACK
s.sendall(frame(5, pack({"down_bytes": 512 << 20})))                                # BW asking for 512 MiB
n = struct.unpack(">I", rd(4))[0]
assert n <= (64 << 20) + 4096, "BW reply of %d bytes for a 512 MiB request (cap 64 MiB)" % n
print("W: BW probe for 512 MiB answered with %d bytes" % n)
P
echo "E2E-HARDENING-BOUNDS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
