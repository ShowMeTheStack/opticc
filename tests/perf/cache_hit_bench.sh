#!/usr/bin/env bash
# Cache benchmark: opticc client cache (warm coordinator, loopback worker) vs ccache direct mode on a
# kbuild-like tree (gen_kbuild_tree.py: ~600-header closures, 9 -I dirs, kbuild's -Wp,-MMD,<dir>/.<obj>.d form).
# Scenarios per tool: cold (populate) -> repeat (objects + .d removed) -> touch-header (a mid-popularity header edited
# by appending a comment; dependents must rebuild). Prints wall seconds and per-TU hit cost; checks objects and .d
# files are byte-identical to an uncached build. Usage: cache_hit_bench.sh [TUS] [HEADERS] [JOBS]
set -u
TUS=${1:-400}; HDR=${2:-12000}; J=${3:-8}
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}; ROOTSRC=$(cd "$(dirname "$0")/../.." && pwd)
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
T=$(mktemp -d /tmp/opticc-cachebench.XXXXXX)
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p"; done; wait 2>/dev/null; }
trap cleanup EXIT
[ -n "${KEEP:-}" ] && trap 'cleanup; echo "kept $T"' EXIT
now() { python3 -c 'import time;print(time.time())'; }
python3 "$ROOTSRC/tests/perf/gen_kbuild_tree.py" "$T/g" --headers "$HDR" --tus "$TUS" > /dev/null
python3 "$ROOTSRC/tests/perf/make_compilable.py" "$T/g/tree" "$T/g/tus.json" > /dev/null   # stub missing includes
# Makefile from tus.json: relative paths (cwd = tree root) and kbuild's dependency-file form
python3 - "$T/g/tus.json" "$T/g/tree" > "$T/g/tree/Makefile" <<'PY'
import json, os, sys
tus = json.load(open(sys.argv[1])); root = sys.argv[2]
rel = lambda p: os.path.relpath(p, root) if os.path.isabs(p) and p.startswith(root) else p
objs = []
rules = []
for t in tus:
    a = [rel(x) for x in t["argv"][1:]]
    i = a.index("-c"); src = a[i + 1]; obj = a[a.index("-o") + 1]
    flags = [x for k, x in enumerate(a) if k not in (i, i + 1, a.index("-o"), a.index("-o") + 1)]
    d = os.path.join(os.path.dirname(obj), "." + os.path.basename(obj) + ".d")
    objs.append(obj)
    rules.append(f"{obj}: {src}\n\t$(CC) -O2 -Wp,-MMD,{d} -D__H_MACRO='<stub_macro.h>' {' '.join(flags)} -c {src} -o {obj}\n")
deps = " ".join(os.path.join(os.path.dirname(o), "." + os.path.basename(o) + ".d") for o in objs)
print("all: " + " ".join(objs) + "\n" + "".join(rules) + f"-include {deps}\n" + "clean:\n\tfind . -name '*.o' -delete; find . -name '.*.o.d' -delete\n")
PY
cp -a "$T/g/tree" "$T/ref"; cp -a "$T/g/tree" "$T/cc"; cp -a "$T/g/tree" "$T/op"
HTOUCH=$(python3 - "$T/g/tree" <<'PY'
import os, sys, re
# pick a header included by ~10-30% of TUs: count direct includes of the TU sources' first-level headers
root = sys.argv[1]; cnt = {}
for dp, dn, fn in os.walk(root):
    for f in fn:
        if f.endswith(".c"):
            for h in re.findall(r"#include <([^>]+)>", open(os.path.join(dp, f)).read()): cnt[h] = cnt.get(h, 0) + 1
tus = sum(1 for dp, dn, fn in os.walk(root) for f in fn if f.endswith(".c"))
best = min(cnt, key=lambda h: abs(cnt[h] - 0.03 * tus))
print(best)
PY
)
findh() { for d in arch/x86/include arch/x86/include/generated include build/include arch/x86/include/uapi arch/x86/include/generated/uapi include/uapi build/include/generated/uapi tools/include; do [ -f "$1/$d/$HTOUCH" ] && { echo "$1/$d/$HTOUCH"; return; }; done; }
run() { local dir=$1; shift; local a b; a=$(now); ( cd "$dir" && make -s -j"$J" "$@" ) || { echo "BUILD FAILED in $dir"; exit 1; }; b=$(now); python3 -c "print(f'{$b-$a:.2f}')"; }
# reference (uncached) build
REF=$(run "$T/ref" CC=clang)
# ccache direct mode
export CCACHE_DIR=$T/ccdir CCACHE_BASEDIR=$T/cc CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime CCACHE_MAXSIZE=5G
CC_COLD=$(run "$T/cc" CC="ccache clang"); CC_REP=999
for k in 1 2 3; do ( cd "$T/cc" && make -s clean ); x=$(run "$T/cc" CC="ccache clang"); CC_REP=$(python3 -c "print(min($CC_REP,$x))"); done
H=$(findh "$T/cc"); echo "/* touched */" >> "$H"; CC_TOUCH=$(run "$T/cc" CC="ccache clang")
CC_STATS=$(ccache -s 2>/dev/null | tr '\n' ' ' | tr -s ' ')
unset CCACHE_DIR CCACHE_BASEDIR CCACHE_SLOPPINESS CCACHE_MAXSIZE
# opticc: loopback worker + warm coordinator, client cache on
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null; cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token b --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 600); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=b OPTICC_POLICY=ect-window OPTICC_MODE=header-push \
       OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_DEBUG_CACHE=${OPTICC_DEBUG_CACHE:-}
startco() { "$OPTICC" --coordinator >> "$T/coord.out" 2>> "$T/coord.err" & for _ in $(seq 1200); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done; }
# The summary path is reused across RESTART=1 coordinators: remove it, then wait for THIS coordinator's summary
# (the SIGUSR1 summary waits for pending local stores); SIGTERM drains for up to 30 s, so wait for the processes
usr1_summary() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || return 0; rm -f "$T/summary.json"; kill -USR1 "$p" 2>/dev/null
  for _ in $(seq 600); do python3 -c "import json,sys;json.load(open(sys.argv[1]))" "$T/summary.json" 2>/dev/null && break; sleep 0.05; done; }
stopco() { local ps; usr1_summary; cp "$T/summary.json" "$T/summary.$(date +%s%N).json" 2>/dev/null; ps=$(lsof -t "$OPTICC_SOCK" 2>/dev/null)
  for p in $ps; do kill $p; done; for p in $ps; do for _ in $(seq 900); do kill -0 "$p" 2>/dev/null || break; sleep 0.05; done; done; rm -f "$OPTICC_SOCK"; }
# RESTART=1: a NEW coordinator before every timed build: cold in-memory hit state
maybe_restart() { [ -n "${RESTART:-}" ] && { stopco; startco; }; true; }
startco
OP_COLD=$(run "$T/op" CC="$OPTICC clang"); OP_REP=999
for k in 1 2 3; do ( cd "$T/op" && make -s clean ); maybe_restart; x=$(run "$T/op" CC="$OPTICC clang"); OP_REP=$(python3 -c "print(min($OP_REP,$x))"); done
H=$(findh "$T/op"); echo "/* touched */" >> "$H"; maybe_restart; OP_TOUCH=$(run "$T/op" CC="$OPTICC clang")
# correctness after the repeat+touch sequence: rebuild the reference with the same touch and compare objects and .d bodies
H=$(findh "$T/ref"); echo "/* touched */" >> "$H"; ( cd "$T/ref" && make -s -j"$J" CC=clang ) > /dev/null
DIFF=0; for o in $(cd "$T/ref" && find . -name '*.o'); do cmp -s "$T/ref/$o" "$T/op/$o" || DIFF=$((DIFF+1)); done
DDIFF=0; for d in $(cd "$T/ref" && find . -name '.*.o.d'); do cmp -s "$T/ref/$d" "$T/op/$d" || DDIFF=$((DDIFF+1)); done
usr1_summary
HITS=$(python3 -c "import json;s=json.load(open('$T/summary.json'));print(s.get('client_cache_hits', s.get('cache_client_hits','?')), s.get('jobs_total'))" 2>/dev/null)
echo "RESTART=${RESTART:-0} TUs=$TUS headers=$HDR -j$J | uncached $REF s | ccache cold $CC_COLD repeat $CC_REP touch-header $CC_TOUCH | opticc cold $OP_COLD repeat $OP_REP touch-header $OP_TOUCH | per-TU repeat: ccache $(python3 -c "print(f'{$CC_REP*$J/$TUS*1000:.1f}')") ms opticc $(python3 -c "print(f'{$OP_REP*$J/$TUS*1000:.1f}')") ms (wall×jobs/TUs) | opticc vs ref: objects differing $DIFF, .d differing $DDIFF | touched $HTOUCH | opticc summary hits/jobs $HITS"
echo "ccache: $CC_STATS" | cut -c1-400
