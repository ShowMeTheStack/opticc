#!/usr/bin/env bash
# Probe-cache validation end to end on loopback. Start 1 measures and caches the worker's
# rtt/bandwidth; the cache is then rewritten as if measured on a different network (rtt 80 ms, 0.1 Mbit/s); start 2
# must detect the RTT mismatch, re-probe before the first dispatch and schedule with the FRESH values; start 3 (cache
# fresh and consistent) reuses it.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}; OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
T=$(mktemp -d /tmp/opticc-e2e-pc.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null; cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/w.out" 2> "$T/w.err" & WPID=$!
for _ in $(seq 50); do grep -q listening "$T/w.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/w.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/w.out"); [ -n "$PORT" ] || fail "worker"
export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_PROBE_CACHE=$T/pc.json OPTICC_LOCAL_CACHE=off OPTICC_IDLE_TIMEOUT=1
start() {   # $1 = label: start a coordinator, wait for its summary, print the probe record
  rm -f "$T/c.sock"; OPTICC_SOCK=$T/c.sock OPTICC_SUMMARY=$T/s-$1.json "$OPTICC" --coordinator > /dev/null 2> "$T/c-$1.err"
  python3 - "$T/s-$1.json" "$1" <<'PY'
import json, sys
s = json.load(open(sys.argv[1])); p = s["probes"][0]
print(f"{sys.argv[2]}: probe_cached={p.get('probe_cached')} rtt_ms={p['rtt_ms']:.3f} bw_up_mbps={p['bw_up_mbps']:.0f}")
open(sys.argv[1] + ".v", "w").write(json.dumps(p))
PY
}
start first
python3 - "$T/pc.json" <<'PY' || fail "tamper"
import json, sys
c = json.load(open(sys.argv[1])); assert c["workers"], c
for k, v in c["workers"].items(): v.update({"rtt_s": 0.080, "up": 1e5, "down": 1e5})   # "measured on another network"
json.dump(c, open(sys.argv[1], "w"))
PY
start changed
start reuse
python3 - "$T" <<'PY' || fail "assertions"
import json, sys
f = lambda n: json.load(open(f"{sys.argv[1]}/s-{n}.json.v"))
a, b, c = f("first"), f("changed"), f("reuse")
assert a["probe_cached"] is False                                   # first contact: measured
assert b["probe_cached"] is False and b["bw_up_mbps"] > 10 and b["rtt_ms"] < 20, b   # stale entry rejected, fresh values
assert c["probe_cached"] is True and c["bw_up_mbps"] > 10, c          # consistent fresh entry reused
PY
echo "E2E-PROBECACHE PASS"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
