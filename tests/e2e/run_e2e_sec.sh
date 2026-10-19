#!/usr/bin/env bash
# End-to-end security tests on loopback (ctest: e2e_security).
#  b. plain is refused unless the worker opts in (--accept plain) AND the client asks (OPTICC_SECURITY=plain);
#     with both, a plain build works and is byte-identical; a PSK-protected worker refuses a client without the PSK.
#  c. an unknown worker key is refused unless OPTICC_TOFU=1, which records it in known_workers; the next build
#     needs neither TOFU nor the #pubkey in the host spec.
#  d. a client whose key is not in authorized_clients is refused (and the build falls back to local).
#  e. OPTICC_VERIFY=0.3 with one worker that corrupts objects (OPTICC_TEST_CORRUPT=1): mismatches are detected,
#     the lying worker is disabled, every object is still byte-identical to the reference build.
#  Informational: loopback probe throughput plain vs noise and per-job wall overhead.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-sec.XXXXXX)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
fail() { echo "FAIL: $*"; cleanup; exit 1; }
trap cleanup EXIT
now() { python3 -c 'import time;print(time.time())'; }

# --- sources (same generator as run_e2e.sh) -----------------------------------
mkdir -p "$T/src/inc"
printf '#pragma once\n#define SHIFT 3\nstatic inline int mix(int a, int b) { return (a << SHIFT) ^ b; }\nint tally(int n);\n' > "$T/src/inc/shared.h"
for i in $(seq 0 59); do
  f="$T/src/f$i.c"
  case $((i % 3)) in
    0) printf '#include <stdio.h>\nint f%d(int x){ if (x > 1000000) printf("%%d\\n", x); return x * %d + 1; }\n' "$i" "$i" > "$f" ;;
    1) printf '#include "shared.h"\nint f%d(int x){ return mix(x, %d) + tally(x); }\n' "$i" "$i" > "$f" ;;
    2) printf '#include <math.h>\nint f%d(int x){ return (int)sqrt((double)x) + %d; }\n' "$i" "$i" > "$f" ;;
  esac
done
printf 'int tally(int n){ int s=0; for(int i=0;i<n;i++) s+=i; return s; }\n' > "$T/src/tally.c"
printf 'SRCS := $(wildcard *.c)\nOBJS := $(SRCS:.c=.o)\nCFLAGS ?= -O2 -Iinc\nall: $(OBJS)\n%%.o: %%.c\n\t$(CC) $(CFLAGS) -c $< -o $@\n' > "$T/src/Makefile"
cp -r "$T/src" "$T/ref"; ( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"
check_objects() { for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$1/$n" || fail "$2: object differs: $n"; done; }

# --- keys -------------------------------------------------------------------------
mkdir -p "$T/ckeys" "$T/ckeys2" "$T/w1" "$T/w2" "$T/w3"
"$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail keygen
"$OPTICC" keygen --dir "$T/ckeys2" > /dev/null || fail keygen2
cp "$T/ckeys/client.pub" "$T/w1/authorized_clients"; cp "$T/ckeys/client.pub" "$T/w2/authorized_clients"; cp "$T/ckeys/client.pub" "$T/w3/authorized_clients"
echo "sixteen-byte-psk-secret" > "$T/psk"

start_worker() {  # name keydir extra-args... ; sets PORT_<name> PUB_<name>
  local name=$1 kd=$2; shift 2
  "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$kd" "$@" > "$T/$name.out" 2> "$T/$name.err" & PIDS+=($!)
  for _ in $(seq 300); do grep -q listening "$T/$name.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/$name.out" || fail "worker $name did not start: $(cat "$T/$name.err")"
  eval "PORT_$name=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/$name.out")"
  eval "PUB_$name=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/$name.out")"
}
# one coordinator per scenario: run a build with the given env, then stop it
run_build() {  # dir label env...
  local dir=$1 label=$2; shift 2
  rm -rf "$dir"; cp -r "$T/src" "$dir"
  local sock="$T/$label.sock" sum="$T/$label.json"
  # started directly in the background (no subshell), so $! is the coordinator itself and the kill below reaches it
  env "$@" OPTICC_SOCK="$sock" OPTICC_SUMMARY="$sum" OPTICC_TOKEN=e2e OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=1 OPTICC_POLICY=ect-window OPTICC_VERBOSE=1 \
      "$OPTICC" --coordinator > "$T/$label.coord.out" 2> "$T/$label.coord.err" & local cpid=$!
  for _ in $(seq 600); do [ -S "$sock" ] && break; sleep 0.1; done
  [ -S "$sock" ] || { echo "coordinator for $label did not start:"; cat "$T/$label.coord.err"; return 2; }
  local t0; t0=$(now)
  ( cd "$dir" && env "$@" OPTICC_SOCK="$sock" OPTICC_TOKEN=e2e make -s -j8 CC="$OPTICC $CC_REAL" ) > "$T/$label.make.out" 2>&1; local rc=$?
  BUILD_S=$(python3 -c "print(f'{$(now)-$t0:.2f}')")
  rm -f "$sum"; kill -USR1 $cpid; for _ in $(seq 600); do [ -s "$sum" ] && break; sleep 0.05; done; kill $cpid; wait $cpid 2>/dev/null
  return $rc
}
summary() { python3 -c "import json,sys; s=json.load(open('$T/$1.json')); print($2)"; }

# ================= b. plain opt-in ========================================================
start_worker w1 "$T/w1"                       # default: noise only
start_worker w2 "$T/w2" --accept both         # also plain, no PSK
start_worker w3 "$T/w3" --accept plain --psk "$T/psk"
# b1: client asks plain, worker w1 refuses -> no worker, build local
run_build "$T/b1" b1 OPTICC_KEY_DIR="$T/ckeys" OPTICC_SECURITY=plain OPTICC_HOSTS="127.0.0.1:$PORT_w1" || fail "b1 build rc"
grep -q "refused plain connection\|not used\|probe.*\"ok\":false" "$T/b1.coord.err" || fail "b1: worker should have refused plain: $(head -3 "$T/b1.coord.err")"
[ "$(summary b1 "s['jobs_remote']")" = 0 ] || fail "b1: jobs went remote over a refused plain channel"
grep -q '"why":"plain connection but --accept plain not given"' "$T/w1.err" || fail "b1: worker did not log the plain refusal"
echo "ok b1: worker without --accept plain refuses plaintext; build fell back to local"
# b2: worker accepts plain but the client did not ask (auto, key known) -> noise is used, not plain
run_build "$T/b2" b2 OPTICC_KEY_DIR="$T/ckeys" OPTICC_HOSTS="127.0.0.1:$PORT_w2#$PUB_w2" || fail "b2 build rc"
[ "$(summary b2 "s['nodes'][1]['channel']")" = noise ] || fail "b2: expected noise channel"
check_objects "$T/b2" b2
NOISE_UP=$(summary b2 "round(s['probes'][0]['bw_up_mbps'])"); NOISE_S=$BUILD_S
echo "ok b2: key known -> noise even though the worker would accept plain (probe up $NOISE_UP Mbit/s, build ${NOISE_S}s)"
# b3: both opt in -> plain works, identical objects
run_build "$T/b3" b3 OPTICC_KEY_DIR="$T/ckeys" OPTICC_SECURITY=plain OPTICC_HOSTS="127.0.0.1:$PORT_w2" || fail "b3 build rc"
[ "$(summary b3 "s['nodes'][1]['channel']")" = plain ] || fail "b3: expected plain channel"
[ "$(summary b3 "s['jobs_remote'] > 0")" = True ] || fail "b3: nothing went remote"
check_objects "$T/b3" b3
PLAIN_UP=$(summary b3 "round(s['probes'][0]['bw_up_mbps'])"); PLAIN_S=$BUILD_S
echo "ok b3: plain with opt-in on both sides, objects identical (probe up $PLAIN_UP Mbit/s, build ${PLAIN_S}s)"
# b3': the ",plain" host-spec suffix is an equivalent opt-in
run_build "$T/b3p" b3p OPTICC_KEY_DIR="$T/ckeys" OPTICC_HOSTS="127.0.0.1:$PORT_w2,plain" || fail "b3p build rc"
[ "$(summary b3p "s['nodes'][1]['channel']")" = plain ] || fail "b3p: expected plain via ,plain suffix"
echo "ok b3': ',plain' host suffix accepted"
# b4: PSK worker: without the PSK refused, with it accepted
run_build "$T/b4" b4 OPTICC_KEY_DIR="$T/ckeys" OPTICC_SECURITY=plain OPTICC_HOSTS="127.0.0.1:$PORT_w3" || fail "b4 build rc"
[ "$(summary b4 "s['jobs_remote']")" = 0 ] || fail "b4: remote jobs without PSK"
grep -q "requires a pre-shared key" "$T/b4.coord.err" || fail "b4: expected PSK error: $(grep probe "$T/b4.coord.err")"
run_build "$T/b5" b5 OPTICC_KEY_DIR="$T/ckeys" OPTICC_SECURITY=plain OPTICC_PSK_FILE="$T/psk" OPTICC_HOSTS="127.0.0.1:$PORT_w3" || fail "b5 build rc"
[ "$(summary b5 "s['jobs_remote'] > 0")" = True ] || fail "b5: PSK client got no remote jobs: $(grep probe "$T/b5.coord.err")"
check_objects "$T/b5" b5
echo "ok b4/b5: PSK-protected plain worker refuses without the key, accepts with it"

# ================= c. unknown key / TOFU ==================================================
run_build "$T/c1" c1 OPTICC_KEY_DIR="$T/ckeys" OPTICC_HOSTS="127.0.0.1:$PORT_w1" || fail "c1 build rc"
grep -q "no key known for this worker" "$T/c1.coord.err" || fail "c1: expected 'no key known' refusal: $(head -2 "$T/c1.coord.err")"
[ "$(summary c1 "s['jobs_remote']")" = 0 ] || fail "c1: remote jobs with unknown key"
[ ! -f "$T/ckeys/known_workers" ] || fail "c1: known_workers written without TOFU"
run_build "$T/c2" c2 OPTICC_KEY_DIR="$T/ckeys" OPTICC_TOFU=1 OPTICC_HOSTS="127.0.0.1:$PORT_w1" || fail "c2 build rc"
grep -q "TOFU: recorded" "$T/c2.coord.err" || fail "c2: TOFU did not record"
grep -q "127.0.0.1:$PORT_w1 $PUB_w1" "$T/ckeys/known_workers" || fail "c2: known_workers missing entry: $(cat "$T/ckeys/known_workers")"
[ "$(summary c2 "s['nodes'][1]['channel']")" = noise ] || fail "c2: expected noise after TOFU"
check_objects "$T/c2" c2
run_build "$T/c3" c3 OPTICC_KEY_DIR="$T/ckeys" OPTICC_HOSTS="127.0.0.1:$PORT_w1" || fail "c3 build rc"
[ "$(summary c3 "s['nodes'][1]['channel']")" = noise ] || fail "c3: known_workers not used on the next build"
echo "ok c: unknown key refused; TOFU records it; subsequent build uses known_workers"

# ================= d. unauthorized client key =============================================
run_build "$T/d1" d1 OPTICC_KEY_DIR="$T/ckeys2" OPTICC_HOSTS="127.0.0.1:$PORT_w1#$PUB_w1" || fail "d1 build rc"
[ "$(summary d1 "s['jobs_remote']")" = 0 ] || fail "d1: unauthorized client got remote jobs"
grep -q "client key not authorized" "$T/w1.err" || fail "d1: worker did not log the unauthorized key"
check_objects "$T/d1" d1
echo "ok d: client key not in authorized_clients refused; build completed locally"

# ================= e. verify mode with a corrupting worker =================================
mkdir -p "$T/w4"; cp "$T/ckeys/client.pub" "$T/w4/authorized_clients"
OPTICC_TEST_CORRUPT=1 start_worker w4 "$T/w4"
run_build "$T/e1" e1 OPTICC_KEY_DIR="$T/ckeys" OPTICC_VERIFY=0.3 OPTICC_VERIFY_SEED=7 OPTICC_LOCAL_SLOTS=0 \
  OPTICC_HOSTS="127.0.0.1:$PORT_w1#$PUB_w1 127.0.0.1:$PORT_w4#$PUB_w4" || fail "e1 build rc: $(tail -3 "$T/e1.make.out")"
python3 - "$T/e1.json" <<'PY' || fail "e1 summary"
import json, sys
s = json.load(open(sys.argv[1]))
print(f"verify: jobs={s['verify_jobs']} mismatches={s['verify_mismatches']} recompiled={s['verify_recompiled']} nodes={[ (n['name'], n.get('verify_failed'), n.get('disabled')) for n in s['nodes'][1:]]}")
# once the liar is disabled no second worker remains, so verification stops after the first mismatches
assert s["verify_jobs"] >= 1, s["verify_jobs"]
assert 1 <= s["verify_mismatches"] <= s["verify_jobs"]
bad = [n for n in s["nodes"][1:] if n.get("verify_failed", 0) > 0]
assert len(bad) == 1 and bad[0]["disabled"], bad
PY
grep -q "VERIFY_MISMATCH" "$T/e1.coord.err" || fail "e1: no VERIFY_MISMATCH logged"
check_objects "$T/e1" e1
echo "ok e: corrupting worker caught by verify mode and disabled; all objects correct"

echo "timing (informational): noise build ${NOISE_S}s vs plain build ${PLAIN_S}s; loopback probe up noise ${NOISE_UP} vs plain ${PLAIN_UP} Mbit/s"
echo "E2E-SEC PASS ($T)"
cleanup; trap - EXIT
rm -rf "$T"
exit 0
