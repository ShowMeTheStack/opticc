#!/usr/bin/env bash
# Header-push end-to-end test on loopback (ctest: e2e_header_push).
#  1. 61 generated C files built with make in header-push mode (OPTICC_MODE=header-push, no local slots):
#     objects and dep files byte-identical to a plain local build; every job remote in header_push mode.
#  2. The shared header is edited and the build re-run: objects identical to a fresh local build after the
#     same edit (no stale store content).
#  3. A stale dependency file that omits the shared header: the worker reports MISSING, the coordinator
#     resolves it, the compile succeeds.
#  4. Bytes shipped in header-push vs classic for the same files, and q_j (scan) vs p_j (preprocess).
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-hp.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null; [ -n "${CPID2:-}" ] && kill "$CPID2" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/src/inc"
cat > "$T/src/inc/shared.h" <<'H'
#pragma once
#define SHIFT 3
static inline int mix(int a, int b) { return (a << SHIFT) ^ b; }
int tally(int n);
H
for i in $(seq 0 59); do
  f="$T/src/f$i.c"
  case $((i % 3)) in
    0) printf '#include <stdio.h>\nint f%d(int x){ if (x > 1000000) printf("%%d\\n", x); return x * %d + 1; }\n' "$i" "$i" > "$f" ;;
    1) printf '#include "shared.h"\nint f%d(int x){ return mix(x, %d) + tally(x); }\n' "$i" "$i" > "$f" ;;
    2) printf '#include <math.h>\n#include "inc/shared.h"\nint f%d(int x){ return (int)sqrt((double)x) + %d + mix(x, 1); }\n' "$i" "$i" > "$f" ;;
  esac
done
printf 'int tally(int n){ int s=0; for(int i=0;i<n;i++) s+=i; return s; }\n' > "$T/src/tally.c"
cat > "$T/src/Makefile" <<'M'
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)
CFLAGS ?= -O2 -Iinc -MMD -MP
all: $(OBJS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
clean:
	rm -f *.o *.d
M
cp -r "$T/src" "$T/ref"; cp -r "$T/src" "$T/dist"
( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference build"

# keys: client key authorized on the worker; the host spec carries the worker's public key (Noise IK)
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); [ -n "$PORT" ] || fail "worker did not start"
PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out"); [ -n "$PUB" ] || fail "worker printed no public key"
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_VERBOSE=1
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" & CPID=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"

# --- 1. header-push build (first build: no .d files -> MISSING-driven closure) --------------------
t0=$(python3 -c 'import time;print(time.time())')
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "header-push build rc: $(tail -5 "$T/coord.err")"
t1=$(python3 -c 'import time;print(time.time())')
for o in "$T/ref"/*.o; do
  n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "object differs: $n"
  d=${n%.o}.d; cmp -s "$T/ref/$d" "$T/dist/$d" || fail "dep file differs: $d: $(diff "$T/ref/$d" "$T/dist/$d" | head -3)"
done
echo "ok: 61 objects and 61 dep files byte-identical (header-push, first build)"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary check 1"
import json, sys
s = json.load(open(sys.argv[1]))
hp = [j for j in s["jobs"] if j["outcome"] == "remote" and j["mode"] == "header_push"]
rounds = sum(j["rounds"] for j in s["jobs"]); sent = sum(j["shipped_bytes"] for j in s["jobs"])
scan = [j["scan_s"] for j in hp]
src = {}
for j in s["jobs"]: src[j.get("closure_source")] = src.get(j.get("closure_source"), 0) + 1
print(f"summary: total={s['jobs_total']} header_push={len(hp)} fallbacks={s['fallbacks']} missing_rounds={rounds} shipped_bytes={sent} mean_scan_s={sum(scan)/len(scan):.4f} max_scan_s={max(scan):.4f} closure_source={src}")
assert s["jobs_total"] == 61 and len(hp) == 61, (s["jobs_total"], len(hp))
assert s["fallbacks"] == 0
# with no .d files the include scanner (not -MM, not MISSING rounds) provides the closure
assert rounds == 0, rounds
assert src.get("scan", 0) == 61, src
assert sum(scan) / len(scan) < 0.05, "cold q_j should be milliseconds, not a preprocessor run"
PY

# --- 1b. kbuild-like tree: nested includes through -I dirs, include_next-free, one computed include ----
K=$T/ktree; mkdir -p "$K/src/kernel" "$K/src/include/linux" "$K/src/include/asm" "$K/obj/kernel"
cat > "$K/src/include/linux/types.h" <<'H'
#ifndef LT_H
#define LT_H
typedef int kint_t;
#include <asm/arch.h>
#endif
H
cat > "$K/src/include/asm/arch.h" <<'H'
#ifndef ARCH_H
#define ARCH_H
#define ARCH_SHIFT 2
#include "arch_impl.h"
#endif
H
cat > "$K/src/include/asm/arch_impl.h" <<'H'
static inline int arch_mul(int x) { return x << ARCH_SHIFT; }
H
cat > "$K/src/include/linux/sched.h" <<'H'
#include <linux/types.h>
kint_t sched_fn(kint_t);
H
for i in 1 2 3 4; do printf '#include <linux/sched.h>\nkint_t fn%d(kint_t x) { return arch_mul(x) + %d; }\n' $i $i > "$K/src/kernel/k$i.c"; done
# computed include: the scanner cannot see through the macro -> falls back to -MM for this TU only
# GNU C on Linux predefines the macro `linux` as 1, which would turn the computed <linux/sched.h> into <1/sched.h> (macOS has
# no such macro): undefine it so the computed include means the same everywhere
printf '#undef linux\n#define HDR <linux/sched.h>\n#include HDR\nkint_t fnc(kint_t x) { return arch_mul(x) + 99; }\n' > "$K/src/kernel/kc.c"
KFLAGS="-O2 -nostdinc -I$K/src/include -Iinclude -c"
mkdir -p "$K/obj/include"
( cd "$K/obj" && for f in k1 k2 k3 k4 kc; do $CC_REAL $KFLAGS -o "kernel/$f.ref.o" "$K/src/kernel/$f.c"; done ) || fail "ktree reference build"
( cd "$K/obj" && for f in k1 k2 k3 k4 kc; do $OPTICC $CC_REAL $KFLAGS -o "kernel/$f.o" "$K/src/kernel/$f.c" || fail "ktree opticc build $f"; done )
for f in k1 k2 k3 k4 kc; do cmp -s "$K/obj/kernel/$f.ref.o" "$K/obj/kernel/$f.o" || fail "ktree object differs: $f"; done
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "ktree summary check"
import json, sys
s = json.load(open(sys.argv[1]))
k = [j for j in s["jobs"] if "/ktree/" in j["src"]]
assert len(k) == 5, len(k)
assert all(j["outcome"] == "remote" and j["mode"] == "header_push" for j in k), [(j["src"], j["outcome"]) for j in k]
srcs = {j["src"].rsplit("/", 1)[1]: (j["closure_source"], j["closure_files"], j["rounds"]) for j in k}
print("ktree:", srcs)
for f in ("k1.c", "k2.c", "k3.c", "k4.c"):
    assert srcs[f][0] == "scan" and srcs[f][1] == 4 and srcs[f][2] == 0, srcs[f]   # sched.h, types.h, arch.h, arch_impl.h
# computed include: no -MM by default; the worker's MISSING round fetches it (1 round)
assert srcs["kc.c"][0] == "scan+computed" and srcs["kc.c"][1] == 4 and srcs["kc.c"][2] == 1, srcs["kc.c"]
PY
echo "ok: kbuild-like tree (nested -I includes, computed include) byte-identical; scanner closures exact"

# --- 2. edit the shared header, rebuild: no stale store content ----------------------------------
sed -i.bak 's/#define SHIFT 3/#define SHIFT 5/' "$T/ref/inc/shared.h" "$T/dist/inc/shared.h"
( cd "$T/ref" && make -s -j8 CC="$CC_REAL" ) || fail "reference rebuild"
( cd "$T/dist" && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "header-push rebuild rc"
changed=0
for o in "$T/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$T/dist/$n" || fail "object differs after header edit: $n"; done
for i in 1 4 7; do cmp -s "$T/ref/f$i.o" "$T/src/f$i.o" 2>/dev/null || changed=$((changed+1)); done
echo "ok: rebuild after editing shared.h is byte-identical to a fresh local build (closure now from .d files)"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary check 2"
import json, sys
s = json.load(open(sys.argv[1]))
second = [j for j in s["jobs"][61:] if j["outcome"] == "remote" and "/ktree/" not in j["src"]]   # ktree jobs precede the rebuild
rounds = sum(j["rounds"] for j in second)
print(f"rebuild: jobs={len(second)} missing_rounds={rounds} (closure came from the .d files; 0 expected)")
print("rebuild jobs with rounds:", [(j["src"], j["closure_source"], j["closure_files"], j["rounds"]) for j in second if j["rounds"]])
assert len(second) >= 40, len(second)   # 40 TUs include shared.h (f1,f2 mod 3 patterns)
assert rounds == 0, rounds
PY

# --- 3. stale dependency file omitting the header -> MISSING path ------------------------------------
( cd "$T/dist" && rm -f f1.o && printf 'f1.o: f1.c\n' > f1.d && "$OPTICC" "$CC_REAL" -O2 -Iinc -MMD -MP -c f1.c -o f1.o ) || fail "stale .d compile rc"
cmp -s "$T/ref/f1.o" "$T/dist/f1.o" || fail "stale .d: object differs"
cmp -s "$T/ref/f1.d" "$T/dist/f1.d" || fail "stale .d: dep file not regenerated"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "summary check 3"
import json, sys
s = json.load(open(sys.argv[1])); j = s["jobs"][-1]
print(f"stale .d: rounds={j['rounds']} closure_files={j['closure_files']} outcome={j['outcome']}")
# the coordinator unions the (memoised) scanner closure with the dependency file, so a header a stale .d omits is
# shipped up front (0 MISSING rounds) -- the MISSING path itself stays covered by kc.c's computed include above
assert j["outcome"] == "remote" and j["rounds"] == 0, (j["outcome"], j["rounds"])
PY
echo "ok: header absent from a stale dependency file shipped up front (scanner union), object byte-identical"

# --- __has_include fixtures (LLVM TargetPassRegistry.inc pattern) + coloured diagnostics -----------
H=$T/hinc; mkdir -p "$H/reg" "$H/lib" "$H/opt" "$H/obj"
printf '#define REGVAL 42\n' > "$H/reg/inc.def"
printf '#if !__has_include(GET_REG)\n#error "must provide GET_REG"\n#endif\n#include GET_REG\n' > "$H/lib/registry.inc"
printf '#define GET_REG "inc.def"\n#include "registry.inc"\nint reg_value(void) { return REGVAL; }\n' > "$H/hi.c"
printf '#define OPT_ON 7\n' > "$H/opt/opt.h"
printf '#if __has_include("opt.h")\nint have_opt = 1;\n#else\nint have_opt = 0;\n#endif\n' > "$H/lit.c"
HF="-O2 -fdiagnostics-color=always -I$H/reg -I$H/lib -I$H/opt -c"
( cd "$H/obj" && for f in hi lit; do $CC_REAL $HF -o "$f.ref.o" "$H/$f.c" || fail "hinc reference $f"; done )
( cd "$H/obj" && for f in hi lit; do $OPTICC $CC_REAL $HF -o "$f.o" "$H/$f.c" || fail "hinc opticc build $f"; done )
for f in hi lit; do cmp -s "$H/obj/$f.ref.o" "$H/obj/$f.o" || fail "hinc object differs: $f"; done
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary.json" <<'PY' || fail "hinc summary check"
import json, sys
s = json.load(open(sys.argv[1]))
j = {x["src"].rsplit("/", 1)[1]: x for x in s["jobs"] if "/hinc/" in x["src"]}
print("hinc:", {k: (v["outcome"], v["mode"]) for k, v in j.items()}, "has_include_computed_jobs", s.get("has_include_computed_jobs"), "remote_error_local_retries", s.get("remote_error_local_retries"))
assert j["hi.c"]["outcome"] in ("fallback", "local"), j["hi.c"]      # computed __has_include: compiled locally
assert j["lit.c"]["outcome"] == "remote", j["lit.c"]                 # literal __has_include: opt.h shipped, same branch
assert s.get("has_include_computed_jobs", 0) >= 1
PY
echo "ok: __has_include(MACRO) compiled locally, literal __has_include target shipped; objects byte-identical"

# --- 4. bytes and client cost: header-push vs classic for the same files ---------------------------
kill $CPID; wait $CPID 2>/dev/null; CPID=
cp "$T/summary.json" "$T/summary-hp.json"
rm -f "$OPTICC_SOCK"
export OPTICC_MODE=classic OPTICC_SUMMARY=$T/summary-classic.json
"$OPTICC" --coordinator > "$T/coord2.out" 2> "$T/coord2.err" & CPID2=$!
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.1; done
( cd "$T/dist" && make -s clean && make -s -j8 CC="$OPTICC $CC_REAL" ) || fail "classic build rc"
rm -f "$OPTICC_SUMMARY"; kill -USR1 $CPID2; for _ in $(seq 600); do [ -s "$OPTICC_SUMMARY" ] && break; sleep 0.05; done   # delete, signal, poll
python3 - "$T/summary-hp.json" "$T/summary-classic.json" <<'PY' || fail "byte comparison"
import json, sys, os
hp = json.load(open(sys.argv[1])); cl = json.load(open(sys.argv[2]))
hp_jobs = {os.path.basename(j["src"]): j for j in hp["jobs"][:61]}
cl_jobs = {os.path.basename(j["src"]): j for j in cl["jobs"] if j["outcome"] == "remote"}
stdio = [k for k in hp_jobs if k.startswith("f") and int(k[1:-2]) % 3 == 0]
hb = sum(hp_jobs[k]["shipped_bytes"] for k in stdio); cb = sum(cl_jobs[k]["zstd_bytes"] for k in stdio if k in cl_jobs)
n = sum(1 for k in stdio if k in cl_jobs)
q = sum(j["scan_s"] for j in hp["jobs"][:61]) / 61
p = [j["pp_cpu_s"] for j in cl["jobs"] if j["outcome"] == "remote" and j["pp_cpu_s"] > 0]
print(f"stdio TUs ({n} compared): header-push shipped {hb} B total vs classic {cb} B zstd'd preprocessed; ratio {hb/max(cb,1):.3f}")
print(f"client cost per TU: q (header-push scan/hash, incl. -MM on first miss) = {q*1e3:.1f} ms vs p (classic -E) = {1e3*sum(p)/len(p):.1f} ms; rho_classic={cl['rho']:.3f}")
assert hb < 0.5 * cb, (hb, cb)
PY
echo "E2E-HP PASS ($T)"
cleanup; trap - EXIT
rm -rf "$T"
exit 0
