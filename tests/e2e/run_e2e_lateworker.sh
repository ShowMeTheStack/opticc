#!/usr/bin/env bash
# A configured worker that is DOWN when the coordinator starts (connection refused) must be retried
# and admitted during the build: worker 2 starts 5 s after the build begins and must still receive jobs; objects are
# byte-identical to a local build; the summary records the late admission.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}; OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd; CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-late.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { for p in ${W1:-} ${W2:-} ${CP:-}; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/src" "$T/ref" "$T/w1" "$T/w2" "$T/ckeys"
for i in $(seq 1 120); do printf '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\nint f%d(int x){ return x * %d + (int)strlen("q"); }\n' $i $i > "$T/src/t$i.c"; done
printf 'SRCS := $(wildcard *.c)\nOBJS := $(SRCS:.c=.o)\nall: $(OBJS)\n%%.o: %%.c\n\t$(CC) -O2 -c $< -o $@\n' > "$T/src/Makefile"
cp "$T/src/"*.c "$T/src/Makefile" "$T/ref/"; ( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"
"$OPTICC" keygen --dir "$T/ckeys" > /dev/null; cp "$T/ckeys/client.pub" "$T/w1/authorized_clients"; cp "$T/ckeys/client.pub" "$T/w2/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
P2=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
# create worker 2's host key once (to know its public key), then stop it: it is DOWN when the coordinator starts
"$OPTICCD" --listen 127.0.0.1:$P2 --jobs 2 --token e2e --key-dir "$T/w2" --tmp "$T/w2/tmp" > "$T/w2a.out" 2> "$T/w2a.err" & W2=$!
for _ in $(seq 50); do grep -q listening "$T/w2a.out" 2>/dev/null && break; sleep 0.1; done
PUB2=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/w2a.out"); kill $W2; wait $W2 2>/dev/null; W2=; [ -n "$PUB2" ] || fail "worker 2 key"
OPTICC_TEST_SLOW_MS=150 "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/w1" --tmp "$T/w1/tmp" > "$T/w1.out" 2> "$T/w1.err" & W1=$!
for _ in $(seq 50); do grep -q listening "$T/w1.out" 2>/dev/null && break; sleep 0.1; done
P1=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/w1.out"); PUB1=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/w1.out")
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$P1#$PUB1 127.0.0.1:$P2#$PUB2" OPTICC_TOKEN=e2e OPTICC_LOCAL_SLOTS=0 OPTICC_LOCAL_CACHE=off \
       OPTICC_PROBE_CACHE=$T/pc.json OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=60 OPTICC_VERBOSE=1
"$OPTICC" --coordinator > "$T/c.out" 2> "$T/c.err" & CP=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done; [ -S "$OPTICC_SOCK" ] || fail "coordinator (30 s; start-up benchmarks are slow on a loaded host): $(tail -3 "$T/c.err")"
( cd "$T/src" && make -s -j8 CC="$OPTICC $CC_REAL" ) & MK=$!
sleep 5
OPTICC_TEST_SLOW_MS=150 "$OPTICCD" --listen 127.0.0.1:$P2 --jobs 2 --token e2e --key-dir "$T/w2" --tmp "$T/w2/tmp" > "$T/w2.out" 2> "$T/w2.err" & W2=$!
wait $MK || fail "build rc"
for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/src/$n" || fail "object differs: $n"; done
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CP; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary"
import json, sys
s = json.load(open(sys.argv[1]))
nodes = {n["name"]: n for n in s["nodes"]}
late = s.get("late_admissions", [])
print("late admissions:", [(x["node"], round(x["t"], 2)) for x in late], "jobs per node:", {k: v.get("jobs") for k, v in nodes.items()})
assert late and any(x["node"].endswith(":" + str(s["nodes"][2]["name"]).split(":")[1]) for x in late), late
assert s["nodes"][2]["jobs"] > 0, s["nodes"][2]
PY
grep -q "admitted late" "$T/c.err" || fail "no admission log line"
echo "E2E-LATEWORKER PASS"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
