#!/usr/bin/env bash
# Header prefetch end-to-end test on loopback (ctest: e2e_prefetch).
# Scale with OPTICC_PF_TUS / OPTICC_PF_HEADERS (defaults 120 / 400) for measurements.
# A kbuild-like project (120 TUs, 400 shared headers over 3 include dirs, ~60-header closures) is built in header-push
# mode on 2 workers with empty stores, three times, each with fresh workers and coordinator:
#   A  prefetch off            : every header reaches each worker through NEED (critical path)
#   B  prefetch on, cold       : NEED bytes ~0 (piggyback), background pushes popular headers
#   C  prefetch on, warm start : B's saved ranking pushes headers before the first job; piggyback bytes drop too
# Each build: objects byte-identical to a local build; -j16 against 2 workers exercises concurrent PREFETCH/COMPILE_HP
# on one connection. Prints the measured numbers.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-pf.XXXXXX)
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
fail() { echo "FAIL: $*"; cleanup; exit 1; }
trap cleanup EXIT

python3 - "$T/src" "${OPTICC_PF_TUS:-120}" "${OPTICC_PF_HEADERS:-400}" <<'PY'
import os, random, sys
root = sys.argv[1]; NT = int(sys.argv[2]); random.seed(7)
dirs = ["inc/a", "inc/b", "inc/c"]
for d in dirs: os.makedirs(os.path.join(root, d), exist_ok=True)
H = int(sys.argv[3])
for h in range(H):
    d = dirs[h % 3]
    deps = [x for x in random.sample(range(H), 3) if x > h]
    with open(os.path.join(root, d, f"h{h}.h"), "w") as f:
        f.write(f"#pragma once\n")
        for x in deps: f.write(f'#include "h{x}.h"\n')
        f.write(f"static inline int fn{h}(int v) {{ return v * {h + 1} + {h % 7}; }}\n")
        f.write("/* " + ("pad " * 200) + "*/\n")
pop = list(range(40))                                   # popular headers: every TU uses some of them
for t in range(NT):
    with open(os.path.join(root, f"t{t}.c"), "w") as f:
        inc = sorted(set(random.sample(pop, 12) + random.sample(range(40, H), 20)))
        for x in inc: f.write(f'#include "h{x}.h"\n')
        f.write(f"int t{t}(int v) {{ return " + " + ".join(f"fn{x}(v)" for x in inc) + "; }\n")
with open(os.path.join(root, "Makefile"), "w") as f:
    f.write("SRCS := $(wildcard *.c)\nOBJS := $(SRCS:.c=.o)\nCFLAGS ?= -O1 -Iinc/a -Iinc/b -Iinc/c\nall: $(OBJS)\n"
            "%.o: %.c\n\t$(CC) $(CFLAGS) -c $< -o $@\nclean:\n\trm -f *.o\n")
PY
cp -r "$T/src" "$T/ref"; ( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys

run() {   # run <label> <prefetch 0|1> <warmfile>
  local L=$1 PF=$2 WARM=$3 W=$T/$1; mkdir -p "$W/tmp1" "$W/tmp2"; cp -r "$T/src" "$W/src"
  local hosts=""
  for i in 1 2; do
    TMPDIR=$W/tmp$i "$OPTICCD" --listen 127.0.0.1:0 --jobs 3 --token e2e --key-dir "$T/wkeys" > "$W/worker$i.out" 2> "$W/worker$i.err" & PIDS+=($!)
    for _ in $(seq 300); do grep -q listening "$W/worker$i.out" 2>/dev/null && break; sleep 0.1; done
    local port pub; port=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$W/worker$i.out"); pub=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$W/worker$i.out")
    [ -n "$port" ] || fail "$L: worker $i did not start"
    hosts="$hosts 127.0.0.1:$port/3#$pub"
  done
  env OPTICC_SOCK=$W/c.sock OPTICC_HOSTS="$hosts" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push OPTICC_BINDING=early \
      OPTICC_SUMMARY=$W/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_PREFETCH=$PF OPTICC_PREFETCH_WARM=$WARM \
      "$OPTICC" --coordinator > "$W/coord.out" 2> "$W/coord.err" & local CP=$!; PIDS+=($CP)
  for _ in $(seq 600); do [ -S "$W/c.sock" ] && break; sleep 0.1; done
  [ -S "$W/c.sock" ] || fail "$L: coordinator did not start"
  sleep 1   # the coordinator starts before the build; warm-start pushes (OPTICC_PREFETCH_WARM_MB per node, ungated) run in this gap
  local t0; t0=$(python3 -c 'import time; print(time.time())')
  ( cd "$W/src" && OPTICC_SOCK=$W/c.sock make -s -j16 CC="$OPTICC $CC_REAL" ) || fail "$L: build failed"
  local t1; t1=$(python3 -c 'import time; print(time.time())')
  kill -TERM $CP; for _ in $(seq 100); do [ -s "$W/summary.json" ] && break; sleep 0.1; done
  for o in "$T"/ref/*.o; do cmp -s "$o" "$W/src/$(basename "$o")" || fail "$L: object differs: $(basename "$o")"; done
  python3 - "$W/summary.json" "$L" "$t0" "$t1" <<'PY'
import json, sys
s = json.load(open(sys.argv[1])); L = sys.argv[2]; wall = float(sys.argv[4]) - float(sys.argv[3])
tot = s.get("prefetch_totals", {}); jobs = [j for j in s["jobs"] if j["outcome"] == "remote"]
rounds = sum(j["rounds"] for j in jobs)
need = sum(j["miss_bytes"] for j in jobs); piggy = sum(j.get("piggy_bytes", 0) for j in jobs)
bg = tot.get("background_bytes", 0)
lat = sorted(j["t_decided"] - j["t_request"] for j in jobs)
print(f"RESULT {L}: wall={wall:.2f}s remote={len(jobs)}/{len(s['jobs'])} need_bytes={need} piggy_bytes={piggy} bg_bytes={bg} "
      f"held_hit_rate={tot.get('held_hit_rate', 0):.3f} rounds={rounds} dispatch_p50_ms={1e3*lat[len(lat)//2]:.1f}")
open(sys.argv[1] + ".line", "w").write(json.dumps({"label": L, "need": need, "piggy": piggy, "bg": bg, "jobs": len(jobs),
                                                    "hit": tot.get("held_hit_rate", 0), "wall": wall}))
PY
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; PIDS=()
}

run A 0 "$T/none.txt"
run B 1 "$T/warm.txt"
[ -s "$T/warm.txt" ] || fail "B did not save a popularity ranking"
run C 1 "$T/warm.txt"
python3 - "$T" <<'PY'
import json, sys
T = sys.argv[1]; r = {L: json.load(open(f"{T}/{L}/summary.json.line")) for L in "ABC"}
assert r["A"]["need"] > 0, "A: expected NEED traffic without prefetch"
assert r["B"]["need"] <= 0.02 * r["A"]["need"], f"B: NEED bytes not ~0: {r['B']['need']} vs {r['A']['need']}"
assert r["C"]["need"] <= 0.02 * r["A"]["need"], "C: NEED bytes not ~0"
assert r["C"]["piggy"] < r["B"]["piggy"], f"C: warm start did not reduce piggyback bytes: {r['C']['piggy']} vs {r['B']['piggy']}"
assert r["C"]["hit"] > r["B"]["hit"], "C: warm start did not raise the held hit rate"
# total bytes never exceed (closure union per node) plus the background budget
print("PASS prefetch e2e")
PY
[ $? -eq 0 ] || fail "assertions"
