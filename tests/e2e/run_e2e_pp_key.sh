#!/usr/bin/env bash
# End to end (ctest: e2e_pp_key): a TU whose include graph has __has_include(MACRO) (LLVM's
# TargetPassRegistry.inc) is cached under the hash of its PREPROCESSED text (coordinator.cpp pp_key/store_pp).
#   1 build, then rebuild               -> preprocessed-key hit, object identical to a plain build
#   2 the tested file is removed         -> no hit (the __has_include result changed), object = plain build
#   3 it reappears with other content    -> no stale hit, object = plain build
#   4 an input edited inside the compile window -> store refused (racy), rebuild correct. The edit is made by the
#     compiler wrapper itself, (a) after the client was told to compile (t_go) but before the compiler reads, (b)
#     after the compiler read but before the store decision, so it cannot race with the coordinator's one-time
#     system-include walk. The client is released BEFORE the store decision (store_local_result), so the test waits
#     for the pp-store-decision trace line of the compile, then checks it says "refused".
#   5 remote decision (worker, no local slots): the compile falls back to local and is stored; the rebuild hits
#   6 with -g: no preprocessed key (DWARF carries every header's MD5, which the text does not):
#     nothing stored, the rebuild recompiles
#   7 a preprocessing diagnostic (#warning in the closure): no preprocessed key; the warning is printed each time
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-pp.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
[ "$(uname)" = Darwin ] && export OPTICC_RACY_S=0.02

cat > "$T/slowcc" <<EOF
#!/bin/sh
# test hook (case 4): EDIT_FILE=<file> EDIT_WHEN=before|after rewrites the file with EDIT_TEXT before the compiler
# reads it or after it has read it (both inside the compile window, before the store decision)
case " \$* " in *" -c "*) [ "\${EDIT_WHEN:-}" = before ] && printf '%s\\n' "\$EDIT_TEXT" > "\$EDIT_FILE";; esac
"$(command -v "$CC_REAL")" "\$@"; rc=\$?
case " \$* " in *" -c "*) [ "\${EDIT_WHEN:-}" = after ] && printf '%s\\n' "\$EDIT_TEXT" > "\$EDIT_FILE"; sleep 2;; esac
exit \$rc
EOF
chmod +x "$T/slowcc"

fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$OPTICC_SUMMARY" "$p"; python3 -c "import json;s=json.load(open('$OPTICC_SUMMARY'));print($1)"; }
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }

setup_tree() {   # $1 dir
  mkdir -p "$1/inc"
  printf '#define V 5\n' > "$1/inc/reg.def"
  printf '#pragma once\n#define REG "reg.def"\n#if __has_include(REG)\n#include REG\n#else\n#define V 1\n#endif\n' > "$1/inc/sel.h"
  printf '#include "sel.h"\nint t(void) { return V; }\n' > "$1/t.c"
  printf 'all: t.o\nt.o: t.c\n\t$(CC) -O2 -MD -MF t.o.d -Iinc -c t.c -o t.o\nclean:\n\trm -f t.o t.o.d\n' > "$1/Makefile"
}
build() {   # $1 label $2 dir $3 compiler
  ( cd "$2" && make -s clean && make -s CC="$OPTICC $3" ) > "$T/out.txt" 2>&1 || fail "$1: build failed: $(cat "$T/out.txt")"
}
check_ref() {   # $1 label $2 dir
  rm -rf "$T/ref"; cp -r "$2" "$T/ref"; ( cd "$T/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "$1: reference build"
  cmp -s "$T/ref/t.o" "$2/t.o" || fail "$1: object differs from a plain build (stale)"
  cmp -s "$T/ref/t.o.d" "$2/t.o.d" || fail "$1: dependency file differs from a plain build"
}
hits() { summ "s['v4']['pp']['hits']"; }

export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on

# local (no workers)
export OPTICC_HOSTS="" OPTICC_SUMMARY=$T/summary-l.json OPTICC_LOCAL_CACHE_DIR=$T/lc-l OPTICC_DIRECT_TRACE=$T/trace-l.tsv
setup_tree "$T/l"; sleep 0.5
start_coord
build "1a" "$T/l" "$CC_REAL"; h0=$(hits)
[ "$(summ "s['v4']['pp']['stores']")" -ge 1 ] || fail "1: not stored under a preprocessed key: $(summ "s['v4']")"
build "1b" "$T/l" "$CC_REAL"; h1=$(hits)
[ "$h1" -gt "$h0" ] || fail "1: rebuild did not hit the preprocessed key ($h0 -> $h1)"
check_ref "1" "$T/l"; echo "1: stored and hit under the preprocessed key"

rm "$T/l/inc/reg.def"; sleep 0.1
build "2" "$T/l" "$CC_REAL"; h2=$(hits)
[ "$h2" = "$h1" ] || fail "2: hit although the __has_include result changed"
check_ref "2" "$T/l"; echo "2: tested file removed -> no hit, object correct"

printf '#define V 9\n' > "$T/l/inc/reg.def"; sleep 0.1
build "3" "$T/l" "$CC_REAL"; h3=$(hits)
[ "$h3" = "$h2" ] || fail "3: stale hit after the tested file reappeared with other content"
check_ref "3" "$T/l"; echo "3: file reappears with other content -> no stale hit, object correct"

for when in before after; do
  v=$([ $when = before ] && echo 11 || echo 13)
  s0=$(summ "s['v4']['racy_store_skips']"); n0=$(grep -c pp-store-decision "$T/trace-l.tsv" 2>/dev/null)
  ( cd "$T/l" && make -s clean && EDIT_FILE="$T/l/inc/reg.def" EDIT_WHEN=$when EDIT_TEXT="#define V $v" make -s CC="$OPTICC $T/slowcc" ) > "$T/out.txt" 2>&1 \
    || fail "4$when: build failed: $(cat "$T/out.txt")"
  # the client is released before the store decision is made (store_local_result: "stored", then store_pp): wait for
  # the decision itself instead of reading the counter as soon as make returns
  for _ in $(seq 600); do [ "$(grep -c pp-store-decision "$T/trace-l.tsv" 2>/dev/null)" -gt "${n0:-0}" ] && break; sleep 0.1; done
  grep pp-store-decision "$T/trace-l.tsv" | tail -1 | cut -f3 | grep -q '^refused ' || fail "4$when: store decision not refused: $(grep pp-store-decision "$T/trace-l.tsv" | tail -1 | cut -f3)"
  [ "$(summ "s['v4']['racy_store_skips']")" -gt "$s0" ] || fail "4$when: store not refused although reg.def changed inside the compile window ($when the compiler read it); pp hits $(hits); reg.def now: $(cat "$T/l/inc/reg.def"); last decisions: $(grep pp-store-decision "$T/trace-l.tsv" | tail -2 | cut -f3)"
  build "4$when-rebuild" "$T/l" "$CC_REAL"; check_ref "4$when" "$T/l"
  echo "4$when: reg.def rewritten $when the compiler read it (inside the compile window) -> store refused, rebuild correct"
done
stop_coord

# 6 / 7: debug information, and a preprocessing diagnostic, refuse the preprocessed key
export OPTICC_HOSTS="" OPTICC_SUMMARY=$T/summary-g.json OPTICC_LOCAL_CACHE_DIR=$T/lc-g
setup_tree "$T/g"; sed -i.bak 's/-O2 -MD/-O2 -g -MD/' "$T/g/Makefile"
setup_tree "$T/w"; printf '#warning opticc-pp-diag\n' >> "$T/w/inc/reg.def"
sleep 0.5
start_coord
build "6a" "$T/g" "$CC_REAL"; build "6b" "$T/g" "$CC_REAL"
[ "$(summ "s['v4']['pp']['stores']")" -eq 0 ] || fail "6: a -g compile was stored under a preprocessed key"
[ "$(hits)" -eq 0 ] || fail "6: a -g compile hit a preprocessed key"
cp "$T/g/t.o" "$T/g-opticc.o"   # -g embeds the build directory: compare with a plain build IN THE SAME directory
( cd "$T/g" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "6: reference build"
cmp -s "$T/g/t.o" "$T/g-opticc.o" || fail "6: object differs from a plain -g build in the same directory"
echo "6: -g -> no preprocessed key, recompiled, object identical to a plain build"
build "7a" "$T/w" "$CC_REAL"; build "7b" "$T/w" "$CC_REAL"
grep -q "opticc-pp-diag" "$T/out.txt" || fail "7: the #warning was not printed on the rebuild (served from a cache?)"
[ "$(summ "s['v4']['pp']['stores']")" -eq 0 ] || fail "7: a TU with a preprocessing diagnostic was stored under a preprocessed key"
echo "7: preprocessing diagnostic -> no preprocessed key, warning printed on every build"
stop_coord

# remote decision: one worker, no local slots -> computed __has_include falls back to local, is stored, then hits
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_LOCAL_SLOTS=0 OPTICC_SUMMARY=$T/summary-r.json OPTICC_LOCAL_CACHE_DIR=$T/lc-r OPTICC_PROBE_CACHE=$T/probe.json
setup_tree "$T/r"; sleep 0.5
start_coord
build "5a" "$T/r" "$CC_REAL"
[ "$(summ "s['fallbacks']")" -ge 1 ] || fail "5: the computed __has_include TU did not take the fallback path: $(summ "{k: s[k] for k in ('jobs_remote','jobs_local','fallbacks')}")"
[ "$(summ "s['v4']['pp']['stores']")" -ge 1 ] || fail "5: fallback compile not stored"
r0=$(hits); build "5b" "$T/r" "$CC_REAL"
[ "$(hits)" -gt "$r0" ] || fail "5: rebuild after a fallback did not hit"
check_ref "5" "$T/r"; echo "5: remote decision -> local fallback stored -> rebuild hits"
stop_coord
echo "E2E-PP-KEY PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
