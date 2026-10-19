#!/usr/bin/env bash
# End to end (ctest: e2e_diag_key): under the comment-insensitive key a
# comment edit can move a diagnostic's column, so a compile with ANY output is never stored under it; and a comment
# whose content can change a diagnostic (here '/*' inside a block comment, -Wcomment) makes the file use the raw hash.
#   A  a TU that warns: build 1 not stored (skip reason recorded), build 2 compiles again (no hit) and prints the warning
#   B  a silent TU: stored and hit (control)
#   C  a silent TU whose block comment contains '/*': a comment edit is a MISS (raw hash), and a build with -Wcomment
#      prints the warning instead of a cached silent result
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc: external as, other diagnostics)
T=$(mktemp -d /tmp/opticc-e2e-diag.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
[ "$(uname)" = Darwin ] && export OPTICC_RACY_S=0.02

fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$OPTICC_SUMMARY" "$p"; python3 -c "import json;s=json.load(open('$OPTICC_SUMMARY'));print($1)"; }
hits() { summ "s['client_fast_hits'] + s['client_cache_hits']"; }

export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push OPTICC_IDLE_TIMEOUT=600
export OPTICC_LOCAL_CACHE=on OPTICC_HOSTS="" OPTICC_SUMMARY=$T/summary.json OPTICC_LOCAL_CACHE_DIR=$T/lc
"$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
[ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
mkdir -p "$T/warm"; printf 'int w(void) { return 1; }\n' > "$T/warm/w.c"
( cd "$T/warm" && "$OPTICC" "$CC_REAL" -O2 -c w.c -o w.o ) > /dev/null 2>&1 || fail "warm-up build"

mk() {   # $1 dir, $2 source text, $3 extra flags
  mkdir -p "$1"; printf '%s' "$2" > "$1/t.c"
  printf 'all: t.o\nt.o: t.c\n\t$(CC) -O2 %s -MD -MF t.o.d -c t.c -o t.o\nclean:\n\trm -f t.o t.o.d\n' "$3" > "$1/Makefile"
}
build() { ( cd "$1" && make -s clean && make -s CC="$OPTICC $CC_REAL" ) > "$T/out.txt" 2>&1 || fail "$1: build: $(cat "$T/out.txt")"; }

# A: a warning (unused variable) -> never stored under the comment-insensitive key
mk "$T/a" 'int f(void) { int unused_v; return 0; }
' "-Wall"
sleep 2.4
h0=$(hits); build "$T/a"; grep -q "unused" "$T/out.txt" || fail "A: build 1 printed no warning: $(cat "$T/out.txt")"
build "$T/a"; h1=$(hits)
grep -q "unused" "$T/out.txt" || fail "A: build 2 lost the warning"
[ "$h1" = "$h0" ] || fail "A: a warning TU was served from the comment-insensitive cache (hits $h0 -> $h1)"
summ "s['v4']['skips']" | grep -q "compiler output under the comment-insensitive key" || fail "A: skip reason not recorded: $(summ "s['v4']['skips']")"
echo "A: warning TU not stored, recompiled, warning printed both times"

# B: control: a silent TU is stored and hit
mk "$T/b" 'int g(void) { return 2; } /* c */
' "-Wall"
sleep 2.4
build "$T/b"; h0=$(hits); build "$T/b"; h1=$(hits)
[ "$h1" -gt "$h0" ] || fail "B: silent TU did not hit (hits $h0 -> $h1)"
echo "B: silent TU stored and hit"

# C: '/*' inside a block comment -> raw hash: the comment edit that triggers -Wcomment is a MISS and warns
mk "$T/c" 'int h(void) { return 3; } /* plain */
' "-Wcomment"
sleep 2.4
build "$T/c"; h0=$(hits)
printf 'int h(void) { return 3; } /* x /* y */\n' > "$T/c/t.c"; sleep 2.4
build "$T/c"; h1=$(hits)
grep -q "comment" "$T/out.txt" || fail "C: the -Wcomment warning was not printed (served from cache?): $(cat "$T/out.txt")"
[ "$h1" = "$h0" ] || fail "C: a comment edit introducing '/*' was served from the cache (hits $h0 -> $h1)"
echo "C: '/*' in a comment forces the raw hash; the warning is printed"
echo "PASS e2e_diag_key"
