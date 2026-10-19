#!/usr/bin/env bash
# End to end (ctest: e2e_hardening_scope): a worker may only obtain, through MISSING rounds, files inside the
# job's scope (opticc/servescope.hpp). The worker is started with OPTICC_TEST_MISSING_NAME so that, before every
# header-push compile, it asks the client for that name like a compromised worker would; it logs the paths it got back.
#   1 the client's Noise PRIVATE key (absolute path in OPTICC_KEY_DIR)  -> must NOT be shipped
#   2 a file outside the project (an absolute path under a sibling dir) -> must NOT be shipped
#   3 a header inside the project's include dir (absolute path)         -> IS shipped (the probe is not vacuous)
#   4 a relative name with a claimed includer outside the job's files -> NOT resolved there (includer trust)
# Every build must still succeed with a byte-identical object. Mutation check: with the scope check removed from the
# coordinator's MISSING handler, 1 and 2 fail.
set -u
. "$(dirname "$0")/lib.sh"
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-hardenings.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap 'cleanup; e2e_kill_group' EXIT

mkdir -p "$T/wkeys" "$T/ckeys" "$T/home" "$T/outside"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
[ -f "$T/ckeys/client.key" ] || fail "no client.key after keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys HOME=$T/home
printf 'not for workers\n' > "$T/outside/secret.txt"

S=$T/proj; mkdir -p "$S/src" "$S/inc"
printf '#include "h.h"\nint f(void) { return H; }\n' > "$S/src/f.c"
printf '#define H 3\n' > "$S/inc/h.h"
printf '#define EXTRA 1\n' > "$S/inc/extra.h"   # in scope, not part of f.c's closure
sleep 2.4   # inputs older than any racy margin

start_worker() {   # $1 = name the "compromised" worker asks for, $2 = the includer it claims (optional)
  [ -n "${WPID:-}" ] && { kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; }
  OPTICC_TEST_MISSING_NAME="$1" OPTICC_TEST_MISSING_INCLUDER="${2:-}" "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" \
    > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
}
run_probe() {   # $1 = label, $2 = name, $3 = claimed includer (optional); prints the shipped paths
  start_worker "$2" "${3:-}"
  export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
  export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=off OPTICC_PROBE_CACHE=$T/probe.json
  export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0
  rm -f "$S/src/f.o" "$T/ref.o"
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "$1: coordinator did not start: $(cat "$T/coord.err")"
  ( cd "$S/src" && "$OPTICC" "$CC_REAL" -O2 -I../inc -c f.c -o f.o ) > "$T/out.txt" 2>&1 || fail "$1: build failed: $(cat "$T/out.txt")"
  local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3
  ( cd "$S/src" && "$CC_REAL" -O2 -I../inc -c f.c -o "$T/ref.o" ) || fail "$1: reference build"
  cmp -s "$T/ref.o" "$S/src/f.o" || fail "$1: object differs from the plain build"
  python3 - "$T/worker.err" <<'P'
import json, sys
paths, probes = [], 0
for line in open(sys.argv[1]):
    try: j = json.loads(line)
    except Exception: continue
    if j.get("event") == "test-missing-probe": probes += 1; paths += j.get("paths", [])
print("probes=%d" % probes); print("\n".join(paths))
P
}

out=$(run_probe "1 key" "$T/ckeys/client.key") || exit 1
echo "$out" | grep -q '^probes=[1-9]' || fail "1: the worker never sent its probe (hook inactive?)"
echo "$out" | grep -qx "$T/ckeys/client.key" && fail "1: the client's PRIVATE KEY was shipped to a worker"
R=$(python3 -c "import json;print(json.load(open('$T/summary.json')).get('missing_refused',0))")
[ "${R:-0}" -ge 1 ] || fail "1: summary missing_refused=$R (expected >= 1)"
echo "1 private key: refused"

out=$(run_probe "2 outside" "$T/outside/secret.txt") || exit 1
echo "$out" | grep -qx "$T/outside/secret.txt" && fail "2: a file outside the job's scope was shipped"
echo "2 outside the project: refused"

out=$(run_probe "3 in scope" "$S/inc/extra.h") || exit 1
echo "$out" | grep -qx "$S/inc/extra.h" || fail "3: an in-scope header was not shipped (the probe would be vacuous): $out"
echo "3 in-scope header: shipped"

# 4: a worker-named includer that is not one of the job's files does not widen the search: "only_here.h"
# exists only next to that includer (inside the project scope); shipping it would mean the claimed includer was trusted
mkdir -p "$S/other"; printf '#define ONLY 1\n' > "$S/other/only_here.h"
out=$(run_probe "4 includer" "only_here.h" "$S/other/fake.c") || exit 1
echo "$out" | grep -q '^probes=[1-9]' || fail "4: the worker never sent its probe"
echo "$out" | grep -qx "$S/other/only_here.h" && fail "4: a worker-named includer outside the job's files widened the search"
echo "4 untrusted includer: not used"
echo "E2E-HARDENING-SCOPE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
