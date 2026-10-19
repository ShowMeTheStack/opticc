#!/usr/bin/env bash
# End to end (ctest: e2e_pp_key_hardening): the guards on the LOCAL preprocessed-text key (used for TUs with a
# computed __has_include):
#   H1 the file tested by __has_include appears after the compile read, before the post-compile preprocessing
#      -> store refused ("read different files"); the rebuild compiles again and is correct
#   H3 __TIME__ -> not stored
#   H5 __builtin_COLUMN -> no preprocessed key
#   H6 -P -> no preprocessed key
#   H8 inline asm .incbin in a header -> no preprocessed key; the .incbin'd file edited -> the rebuild has the
#      new content (equal to a plain build)
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-pph.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
[ "$(uname)" = Darwin ] && export OPTICC_RACY_S=0.02

cat > "$T/slowcc" <<EOF
#!/bin/sh
# test hook: EDIT_FILE / EDIT_WHEN=after / EDIT_TEXT around the COMPILE only (-c)
"$(command -v "$CC_REAL")" "\$@"; rc=\$?
case " \$* " in *" -c "*) if [ "\${EDIT_WHEN:-}" = after ]; then printf '%s\\n' "\$EDIT_TEXT" > "\$EDIT_FILE"; sleep 2; fi;; esac
exit \$rc
EOF
chmod +x "$T/slowcc"

export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on
fresh_summary() { local f=$1 pid=$2 m0; m0=$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")
  kill -USR1 "$pid"; for _ in $(seq 600); do sleep 0.1; [ "$(python3 -c "import os,sys;print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else 0)" "$f")" != "$m0" ] && break; done; sleep 0.1; }
summ() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] || { echo 0; return; }
  fresh_summary "$T/summary.json" "$p"; python3 -c "import json;s=json.load(open('$T/summary.json'));print($1)"; }
start_coord() { export OPTICC_LOCAL_CACHE_DIR=$1
  "$OPTICC" --coordinator > "$T/coord.out" 2>> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"; }
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done
  for _ in $(seq 300); do [ -n "$p" ] && kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; sleep 0.2; }
skips() { summ "sum(v['n'] for k, v in s['v4'].get('skips', {}).items() if k.startswith('$1'))"; }
stores() { summ "s['v4']['pp']['stores']"; }
hits() { summ "s['v4']['pp']['hits']"; }

mktree() {   # $1 dir, $2 body of t.c (after the include), $3 extra flags, $4 extra header text
  mkdir -p "$1/inc"
  printf '#define V 5\n' > "$1/inc/reg.def"
  printf '#pragma once\n#define REG "reg.def"\n#if __has_include(REG)\n#include REG\n#else\n#define V 1\n#endif\n%s\n' "$4" > "$1/inc/sel.h"
  printf '#include "sel.h"\n%s\n' "$2" > "$1/t.c"
  printf 'all: t.o\nt.o: t.c\n\t$(CC) -O2 %s -MD -MF t.o.d -Iinc -c t.c -o t.o\nclean:\n\trm -f t.o t.o.d\n' "$3" > "$1/Makefile"
}
build() { ( cd "$2" && make -s clean && make -s CC="$OPTICC $3" ) > "$T/out.txt" 2>&1 || fail "$1: build failed: $(cat "$T/out.txt")"; }
check_ref() { rm -rf "$T/ref"; cp -r "$2" "$T/ref"; ( cd "$T/ref" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "$1: reference build"
  cmp -s "$T/ref/t.o" "$2/t.o" || fail "$1: object differs from a plain build (stale)"; }
BODY='int t(void) { return V; }'

# H1 (publisher order, local store): reg.def absent at the compile, created after it
mktree "$T/b" "$BODY" "" ""; rm -f "$T/b/inc/reg.def"; sleep 2.4
start_coord "$T/lc1"
( cd "$T/b" && make -s clean && EDIT_FILE="$T/b/inc/reg.def" EDIT_WHEN=after EDIT_TEXT='#define V 9' make -s CC="$OPTICC $T/slowcc" ) > "$T/out.txt" 2>&1 || fail "H1: build: $(cat "$T/out.txt")"
for _ in $(seq 100); do [ "$(skips 'preprocessing and compile read different files')" -ge 1 ] && break; sleep 0.1; done
[ "$(skips 'preprocessing and compile read different files')" -ge 1 ] || fail "H1: file-set change not refused; skips $(summ "s['v4'].get('skips')")"
[ "$(stores)" -eq 0 ] || fail "H1: stored although the file set changed"
build "H1 rebuild" "$T/b" "$CC_REAL"; check_ref "H1" "$T/b"
stop_coord; echo "H1 tested file appeared after the compile: store refused, rebuild correct"

# H3 __TIME__
mktree "$T/c" 'const char *when = __TIME__; int t(void) { return V; }' "" ""; sleep 2.4
start_coord "$T/lc3"; build "H3" "$T/c" "$CC_REAL"
for _ in $(seq 50); do [ "$(skips 'time macro')" -ge 1 ] && break; sleep 0.1; done
[ "$(skips 'time macro')" -ge 1 ] && [ "$(stores)" -eq 0 ] || fail "H3: a __TIME__ TU was stored; skips $(summ "s['v4'].get('skips')")"
stop_coord; echo "H3 __TIME__: not stored"

# H5 __builtin_COLUMN
mktree "$T/e" 'int t(void) { return V + __builtin_COLUMN(); }' "" ""; sleep 2.4
start_coord "$T/lc5"; build "H5" "$T/e" "$CC_REAL"
for _ in $(seq 50); do [ "$(skips 'column-dependent text')" -ge 1 ] && break; sleep 0.1; done
[ "$(skips 'column-dependent text')" -ge 1 ] && [ "$(stores)" -eq 0 ] || fail "H5: __builtin_COLUMN not refused; skips $(summ "s['v4'].get('skips')")"
stop_coord; echo "H5 __builtin_COLUMN: no preprocessed key"

# H6 -P
mktree "$T/f" "$BODY" "-P" ""; sleep 2.4
start_coord "$T/lc6"; build "H6" "$T/f" "$CC_REAL"
for _ in $(seq 50); do [ "$(skips 'preprocessor output flag -P')" -ge 1 ] && break; sleep 0.1; done
[ "$(stores)" -eq 0 ] || fail "H6: stored with -P"
stop_coord; echo "H6 -P: no preprocessed key ($(skips 'preprocessor output flag -P') refusal(s) by flag)"

# H8 inline asm .incbin in a header of a computed-__has_include TU
mktree "$T/g" "$BODY" "" "__asm__(\".section .rodata\\n.incbin \\\"$T/g/blob.bin\\\"\\n.text\");"
printf 'AAAA' > "$T/g/blob.bin"; sleep 2.4
start_coord "$T/lc8"; build "H8a" "$T/g" "$CC_REAL"; check_ref "H8a" "$T/g"
for _ in $(seq 50); do [ "$(skips 'assembler file directive')" -ge 1 ] && break; sleep 0.1; done
[ "$(skips 'assembler file directive')" -ge 1 ] && [ "$(stores)" -eq 0 ] || fail "H8: .incbin not refused; skips $(summ "s['v4'].get('skips')") stores $(stores)"
printf 'BBBB' > "$T/g/blob.bin"; sleep 2.4
build "H8b" "$T/g" "$CC_REAL"; check_ref "H8b" "$T/g"
grep -q BBBB "$T/g/t.o" || fail "H8: the rebuilt object lacks the new .incbin content"
stop_coord; echo "H8 inline-asm .incbin in a header: no preprocessed key; the edited blob is in the rebuilt object"
echo "E2E-PP-KEY-HARDENING PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
