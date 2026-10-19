#!/usr/bin/env bash
# opticc DIRECT cache end to end (ctest: e2e_direct_cache): the compiler wrapper answers repeat compiles itself.
# Every step is `make clean && make` with opticc and, in a reference copy that saw the same edits, with the plain
# compiler; every object and dependency file must be byte-identical (no stale object can result). Run once with
# CMake/ninja's dependency form (-MD -MT -MF) and once with kbuild's (-Wp,-MMD,<dir>/.<obj>.d).
#   1 cold                     all remote; direct entries recorded (inputs are older than 2 s)
#   2 repeat                   every TU answered by the wrapper (client_fast_hits == all)
#   2b comment-only edits of a header and of sources: still answered by the wrapper (comment-insensitive positives)
#   3 header edit, same size, mtime restored (touch -r)       dependents miss the direct path; correct objects
#   4 shadowing header created earlier in the -I path       includers miss (negative probes); correct objects
#   5 -D change                different key; correct objects
#   6 -I order change          different key; the other header wins; correct objects
#   7 racy edit (build right after the edit, then again)    correct objects both times
#   8 header replaced by another file (new inode) with the same size and mtime    correct objects
#   9 computed include (#include HDR, -DHDR=...)             correct objects; edit of the computed target is seen
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-direct.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 4 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); [ -n "$PORT" ] || fail "worker did not start: $(cat "$T/worker.err")"
PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_SLOTS=0 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc
export OPTICC_DIRECT_TRACE=$T/trace.tsv   # per-decision trace (wrappers + coordinator record skips)

# the coordinator first writes every queued direct entry, then the summary: wait for a NEW summary file
fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
fast() {   # client_fast_hits of the running coordinator (cumulative since it started)
  local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$T/summary.json" "$p"; python3 -c "import json;print(json.load(open('$T/summary.json')).get('client_fast_hits',0))"
}

run_flavour() {   # $1 name, $2 dependency flags (make syntax), $3 TUs the direct path must not record (see below)
  local F=$1 DEP=$2 S="$T/$1" NREC=$3

  mkdir -p "$S/src/inc1" "$S/src/inc2" "$S/src/sub"
  cat > "$S/src/inc2/common.h" <<'H'
#pragma once
#define SCALE 3
static inline int scale(int x) { return x * SCALE; }
H
  printf '#pragma once\n#define LIMIT 100\n' > "$S/src/inc2/limits_.h"
  printf '#pragma once\n#define CT_VALUE 7\n' > "$S/src/inc2/ct.h"
  for i in $(seq 0 23); do
    case $((i % 3)) in
      0) printf '#include <stdio.h>\n#include "common.h"\nint f%d(int x){ if (x > 1000000) printf("%%d\\n", x); return scale(x) + %d; }\n' "$i" "$i" ;;
      1) printf '#include <common.h>\n#include <limits_.h>\n#ifndef EXTRA\n#define EXTRA 0\n#endif\nint f%d(int x){ return scale(x) + LIMIT + EXTRA + %d; }\n' "$i" "$i" ;;
      2) printf '#include <limits_.h>\nint f%d(int x){ return x + LIMIT * %d; }\n' "$i" "$i" ;;
    esac > "$S/src/f$i.c"
  done
  printf '#include HDR\nint fct(void){ return CT_VALUE; }\n' > "$S/src/ct.c"
  cat > "$S/src/Makefile" <<M
SRCS := \$(wildcard *.c)
OBJS := \$(SRCS:.c=.o)
IFLAGS ?= -Iinc1 -Iinc2
DFLAGS ?=
all: \$(OBJS)
ct.o: ct.c
	\$(CC) -O2 $DEP \$(IFLAGS) \$(DFLAGS) -DHDR='<ct.h>' -c \$< -o \$@
%.o: %.c
	\$(CC) -O2 $DEP \$(IFLAGS) \$(DFLAGS) -c \$< -o \$@
clean:
	rm -f *.o *.d .*.d
M
  cp -r "$S/src" "$S/ref"; cp -r "$S/src" "$S/dist"; rm -rf "$S/src"
  local N; N=$(ls "$S/dist"/*.c | wc -l | tr -d ' ')
  sleep 2.2   # inputs must be older than the racy window for entries to be recorded
  step() {   # $1 label, $2 expected fast hits (or "-" = any), rest: extra make args
    local lab=$1 want=$2; shift 2
    ( cd "$S/ref" && make -s clean && make -s -j8 CC="$CC_REAL" "$@" ) > /dev/null 2>&1 || fail "$F $lab: reference build"
    local f0 f1; f0=$(fast)
    ( cd "$S/dist" && make -s clean && make -s -j8 CC="$OPTICC $CC_REAL" "$@" ) > "$S/out.txt" 2>&1 || fail "$F $lab: opticc build: $(cat "$S/out.txt")"
    f1=$(fast)
    for o in "$S/ref"/*.o; do n=$(basename "$o"); cmp -s "$o" "$S/dist/$n" || fail "$F $lab: object differs: $n"; done
    for d in $(cd "$S/ref" && ls -a | grep '\.d$'); do cmp -s "$S/ref/$d" "$S/dist/$d" || fail "$F $lab: dep file differs: $d"; done
    [ "$(ls "$S/dist"/*.o | wc -l)" -eq "$N" ] || fail "$F $lab: missing objects"
    echo "$F $lab: direct hits $((f1 - f0))/$N, objects + dep files identical"
    [ "$want" = "-" ] || [ "$((f1 - f0))" -eq "$want" ] || fail "$F $lab: expected $want direct hits, got $((f1 - f0))"
  }
  both() { for x in ref dist; do ( cd "$S/$x" && eval "$1" ); done; }
  step "1 cold" 0
  step "2 repeat" "$((N - NREC))"
  # 2b: comment-only edits (a comment appended to a header and to sources) still hit in the wrapper: positives
  # are compared by the comment-insensitive key hash (no debug info, no -C), objects must stay byte-identical
  both 'printf "/* touched */\n" >> inc2/common.h; printf "/* touched */\n" >> f1.c; printf "/* touched */\n" >> f3.c'
  step "2b comment appended to a header and two sources" "$((N - NREC))"
  # 3: same-size content change with the old mtime restored: only ctime betrays it
  both 'cp -p inc2/common.h .bak; sed "s/SCALE 3/SCALE 4/" .bak > inc2/common.h; touch -r .bak inc2/common.h; rm .bak'
  step "3 same-size edit, mtime restored" "-"
  # 4: a header created earlier in the search path shadows inc2/common.h for <> includes and for "" includes
  both 'printf "#pragma once\n#define SCALE 5\nstatic inline int scale(int x) { return x * SCALE + 1; }\n" > inc1/common.h'
  step "4 shadowing header" "-"
  step "4b repeat" "-"
  step "5 -D change" "-" DFLAGS=-DEXTRA=2
  step "6 -I order change" "-" IFLAGS="-Iinc2 -Iinc1"
  # 7: racy edit: build immediately after editing (inputs younger than 2 s), then again right after another edit
  both 'printf "#pragma once\n#define LIMIT 101\n" > inc2/limits_.h'
  step "7 racy edit" "-"
  both 'printf "#pragma once\n#define LIMIT 102\n" > inc2/limits_.h'
  step "7b racy edit again" "-"
  # 8: header replaced by a different file (new inode) with the same size and the same mtime
  both 'printf "#pragma once\n#define LIMIT 103\n" > n.h; touch -r inc2/limits_.h n.h; mv n.h inc2/limits_.h'
  step "8 replaced, same size + mtime" "-"
  # 9: computed include target edited
  both 'printf "#pragma once\n#define CT_VALUE 8\n" > inc2/ct.h'
  step "9 computed include target edited" "-"
  sleep 2.2; step "10 settled" "-"; step "11 repeat" "$((N - NREC))"
}

"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
# kbuild form: ct.c's computed include under -MMD without -nostdinc is never recorded (the dependency file omits
# system headers, so what a computed include might have reached there is unknown): it goes to the coordinator.
run_flavour cmake '-MD -MT $@ -MF $@.d' 0
run_flavour kbuild '-Wp,-MMD,$(@D)/.$(@F).d' 1
# the trace attributes every decision: hits, misses with the changed input, shadowing, and the never-recorded TU
grep -q "$(printf '\thit\t')" "$T/trace.tsv" || fail "trace: no hit lines"
grep -q "positive content: .*common.h" "$T/trace.tsv" || fail "trace: no content-mismatch miss for common.h"
grep -q "shadowing candidate appeared" "$T/trace.tsv" || fail "trace: no shadowing miss"
grep -q "record-skipped.*computed include" "$T/trace.tsv" || fail "trace: no record-skipped line for the computed-include TU"
echo "ok: trace $(grep -c "$(printf '\thit\t')" "$T/trace.tsv") hits, $(grep -c "$(printf '\tmiss\t')" "$T/trace.tsv") misses, $(grep -c record-skipped "$T/trace.tsv") record-skipped"
echo "E2E-DIRECT PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
