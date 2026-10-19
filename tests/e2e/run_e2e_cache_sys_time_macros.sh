#!/usr/bin/env bash
# End to end (ctest: e2e_cache_sys_time_macros): system headers are left out of every key
# (the fingerprint covers them), so a time macro used through a SYSTEM header was invisible to the per-TU scan. The
# fingerprint tracker marks such headers; a TU that may read one (its -MD dependency file lists it) is not stored under
# a content key, on the worker or the client, and every OTHER TU of that compiler still is (no global switch-off). The system directory is a private one: the "compiler" is a wrapper adding -idirafter $T/sys, so the
# compiler's own search list (and fingerprint) contains it; nothing outside $T is written (survives a SIGKILL).
#   1 f.c includes <stamp.h> (BUILT = __TIME__): remote, refused by the worker; refused by the client (summary count)
#   2 a second later: same client cache -> no client-cache hit; fresh client cache -> no worker-cache hit
#   3 control: g.c includes <plain.h> from the same system directory: stored by the worker, a client-cache hit on rebuild
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cachey.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/sys" "$T/bin" "$T/wkeys" "$T/ckeys" "$T/home"
printf '#define BUILT __TIME__\n' > "$T/sys/stamp.h"; printf '#define PLAIN "fixed"\n' > "$T/sys/plain.h"
printf '#!/bin/sh\nexec %s -idirafter %s "$@"\n' "$(command -v "$CC_REAL")" "$T/sys" > "$T/bin/syscc"; chmod +x "$T/bin/syscc"
"$T/bin/syscc" -E -x c -v /dev/null 2>&1 | grep -q "^ $T/sys\$" || fail "the private system directory is not in the wrapper's search list"
export PATH="$T/bin:$PATH"   # worker, coordinator and wrapper all resolve the compiler by name (as a build does)
"$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
export OPTICC_SYSINC_MEMO_DIR=$T/memo-c
PATH="$T/bin:$PATH" "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_LOCAL_SLOTS=0
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() {   # waits for the process to EXIT (it removes the socket before its bounded drain)
  local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || return 0
  kill -TERM "$p"; for _ in $(seq 600); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$p" 2>/dev/null && fail "coordinator $p did not exit"; return 0
}
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])" 2>/dev/null; }
tm() { python3 -c "import json;s=json.load(open('$T/summary.json'));print(s['v4']['time_macros']['refusals'])" 2>/dev/null; }
S=$T/w; mkdir -p "$S"
printf '#include <stamp.h>\nconst char *built(void) { return BUILT; }\n' > "$S/f.c"
printf '#include <plain.h>\nconst char *built(void) { return PLAIN; }\n' > "$S/g.c"
sleep 2.4
build() { export OPTICC_LOCAL_CACHE_DIR=$2; start_coord
  ( cd "$S" && rm -f "$3.o" && PATH="$T/bin:$PATH" "$OPTICC" syscc -O2 -MD -MF "$3.o.d" -c "$3.c" -o "$3.o" ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"
  stop_coord; }
build 1 "$T/lc1" f; J=$(jobs_of); echo "1 __TIME__ via a system header: $J, client refusals $(tm)"; echo "$J" | grep -q "'remote'" || fail "1: not remote ($J)"
echo "$J" | grep -q "'store'" && fail "1: the worker stored a TU using a time macro through a system header ($J)"
[ "$(tm)" -ge 1 ] || fail "1: the client did not count a time-macro refusal"
sleep 1.1
build 2a "$T/lc1" f; J=$(jobs_of); echo "2a same client cache: $J"; echo "$J" | grep -q "client-cache" && fail "2a: served from the client cache"
build 2b "$T/lc2" f; J=$(jobs_of); echo "2b fresh client cache: $J"; echo "$J" | grep -q "'hit'" && fail "2b: served from the worker cache"
build 3a "$T/lc3" g; J=$(jobs_of); echo "3a control (plain system header): $J"; echo "$J" | grep -q "'store'" || fail "3a: control — the worker did not store a TU without a time macro ($J)"
build 3b "$T/lc3" g; J=$(jobs_of); echo "3b rebuild: $J"; echo "$J" | grep -q "client-cache" || fail "3b: control — no client-cache hit for a TU without a time macro ($J)"
echo "E2E-CACHE-SYS-TIME-MACROS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
