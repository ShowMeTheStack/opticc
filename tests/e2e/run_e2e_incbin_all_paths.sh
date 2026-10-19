#!/usr/bin/env bash
# End to end (ctest: e2e_incbin_all_paths): an inline-asm .incbin in a HEADER
# reads a file that no key and no dependency file lists. Editing ONLY that file must give a rebuild with the new content
# on every cache path:
#   A the client cache (and with it the direct entry, write-back and shared manifest): local compiles
#   B the worker's header-push cache: every compile on the worker, a fresh client cache per build
#   C the worker's classic (.i-keyed) cache: as B in classic mode
# (The local preprocessed-text key is e2e_pp_key_hardening H8.) The .incbin path is absolute, so the worker on this
# machine reads the same file.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-inc.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
[ "$(uname)" = Darwin ] && export OPTICC_RACY_S=0.02

mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "worker did not start: $(cat "$T/worker.err")"
export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
start_coord() { export OPTICC_LOCAL_CACHE_DIR=$1
  "$OPTICC" --coordinator > "$T/coord.out" 2>> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"; }
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done
  for _ in $(seq 300); do [ -n "$p" ] && kill -0 "$p" 2>/dev/null || break; sleep 0.1; done; sleep 0.2; }

S=$T/s; mkdir -p "$S"
printf '#pragma once\n__asm__(".section .rodata\\n.incbin \\"%s\\"\\n.text");\n' "$S/blob.bin" > "$S/blob.h"
printf '#include "blob.h"\nint t(void) { return 1; }\n' > "$S/t.c"
printf 'all: t.o\nt.o: t.c\n\t$(CC) -O2 -MD -MF t.o.d -c t.c -o t.o\nclean:\n\trm -f t.o t.o.d\n' > "$S/Makefile"
build() { ( cd "$S" && make -s clean && make -s CC="$OPTICC $CC_REAL" ) > "$T/out.txt" 2>&1 || fail "$1: build failed: $(cat "$T/out.txt")"; }
check() {   # $1 label, $2 expected bytes in the object
  grep -q "$2" "$S/t.o" || fail "$1: the object lacks the current .incbin content ($2): a stale cache hit"
  cp "$S/t.o" "$T/opticc.o"; ( cd "$S" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "$1: reference build"
  cmp -s "$S/t.o" "$T/opticc.o" || fail "$1: object differs from a plain build"; }
run_case() {   # $1 label, $2 "same" (one coordinator for both builds) or "fresh" (a new client cache per build)
  printf 'AAAA' > "$S/blob.bin"; sleep 2.4
  start_coord "$T/lc-$1-1"; build "$1 first" ; check "$1 first" AAAA
  [ "$2" = fresh ] && { stop_coord; start_coord "$T/lc-$1-2"; }
  build "$1 warm" ; check "$1 warm" AAAA            # a cache may answer here (it must not, but the bytes agree anyway)
  printf 'BBBB' > "$S/blob.bin"; sleep 2.4         # ONLY the .incbin'd file changes
  [ "$2" = fresh ] && { stop_coord; start_coord "$T/lc-$1-3"; }
  build "$1 edited"; check "$1 edited" BBBB
  stop_coord
}

# A: the client cache (no workers: every compile is local)
export OPTICC_HOSTS="" OPTICC_MODE=header-push; unset OPTICC_LOCAL_SLOTS
run_case A same; echo "A client cache: the edited .incbin file is in the rebuilt object"
# B: the worker's header-push cache (no local slots; a fresh client cache per build, so only the worker can answer)
export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_MODE=header-push OPTICC_LOCAL_SLOTS=0
run_case B fresh; echo "B worker header-push cache: the edited .incbin file is in the rebuilt object"
# C: the worker's classic cache (keyed by the preprocessed text)
export OPTICC_MODE=classic
run_case C fresh; echo "C worker classic cache: the edited .incbin file is in the rebuilt object"
echo "E2E-INCBIN-ALL-PATHS PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
