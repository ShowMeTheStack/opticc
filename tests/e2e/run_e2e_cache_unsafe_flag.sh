#!/usr/bin/env bash
# End to end (ctest: e2e_cache_unsafe_flag): a flag that makes the compiler read a file no key names must
# never be cached on any path (direct path, client cache, shared manifests, write-back, worker cache; see
# direct::safe_args), or editing the file would serve the old object.
# The flag here is -fauto-profile=<abs>/p.prof (clang's alias of -fprofile-sample-use, which -fprofile* does not catch,
# so it IS distributed): clang does NOT list the profile in the .d (unlike sanitizer
# ignore-lists and -fprofile-list, which it does list, so the depfile already keys them), and with
# -ffunction-sections the profile decides whether f is placed in .text.hot.f or .text.f.
#   A (local, worker unreachable): profile marks f hot -> .text.hot.f; profile without f, make clean -> .text.f
#   B (remote, LOCAL_SLOTS=0, a FRESH client cache per build, so only the worker cache could serve it): same check.
#     The profile is named by an absolute path: the loopback worker shares the file system and reads it.
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
READELF=$(command -v llvm-readelf || command -v readelf) || { echo "SKIP: no readelf"; exit 77; }
case "$("$CC_REAL" --version 2>&1)" in *clang*) ;; *) echo "SKIP: needs clang (sample-profile text format)"; exit 77;; esac
T=$(mktemp -d /tmp/opticc-e2e-cachef.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
start_worker() {
  rm -f "$T/worker.out" "$T/worker.err"   # the redirection truncates only in the child: the loop below could read the PREVIOUS worker's port
  "$OPTICCD" --listen 127.0.0.1:0 --jobs 2 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" --object-cache "$T/wcache" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
  for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
  grep -q listening "$T/worker.out" || fail "worker did not start: $(cat "$T/worker.err")"
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
  export OPTICC_HOSTS="127.0.0.1:$PORT#$PUB"
}
stop_worker() { kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=; }
export OPTICC_SOCK=$T/c.sock OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs']])"; }
S=$T/w; mkdir -p "$S"
printf 'int g(int);\nint f(int x){\n if (x > 5)\n  return g(x)*7;\n return x*3;\n}\n' > "$S/f.c"
printf 'f.o: f.c\n\t$(CC) -O2 -gline-tables-only -ffunction-sections -fauto-profile=%s/p.prof -MD -MF f.o.d -c f.c -o f.o\nclean:\n\trm -f f.o f.o.d\n' "$S" > "$S/Makefile"
HOT='f:10000:10000\n 0: 10000\n 1: 10000\n 2: 10000\n 3: 1\n'; COLD='zz:10000:100\n 1: 10000\n'
prof() { sleep 1.1; printf "$1" > "$S/p.prof"; sleep 2.4; }
section() { "$READELF" -SW "$S/f.o" | grep -o ' \.text\.[a-z.]*f ' | head -1 | tr -d ' '; }
build() { start_coord; ( cd "$S" && make -s clean && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord; }
# the profile must matter, or the test is vacuous
prof "$HOT"; ( cd "$S" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "plain build"; P1=$(section)
prof "$COLD"; ( cd "$S" && make -s clean && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "plain build"; P2=$(section)
[ "$P1" = ".text.hot.f" ] && [ "$P2" = ".text.f" ] || fail "the profile does not move f ($P1 / $P2): the test would be vacuous"
# A: local compiles + client cache
start_worker; stop_worker   # the address now refuses connections
export OPTICC_LOCAL_CACHE_DIR=$T/lcA
prof "$HOT"; build "A1"; [ "$(section)" = ".text.hot.f" ] || fail "A1: f not hot ($(section))"
echo "A1 hot: $(jobs_of)"
prof "$COLD"; build "A2"
[ "$(section)" = ".text.f" ] || fail "A2: STALE OBJECT — the profile changed but the cached object was served ($(section)): $(jobs_of)"
echo "A2 cold: object correct, $(jobs_of)"
# B: remote compiles, fresh client cache each build: only the worker cache could serve a stale object
start_worker; export OPTICC_LOCAL_SLOTS=0
export OPTICC_LOCAL_CACHE_DIR=$T/lcB1; prof "$HOT"; build "B1"; [ "$(section)" = ".text.hot.f" ] || fail "B1: f not hot ($(section))"
J=$(jobs_of); echo "B1 hot: $J"; echo "$J" | grep -q "'remote'" || fail "B1: did not run remotely ($J) — the worker half would be vacuous"
export OPTICC_LOCAL_CACHE_DIR=$T/lcB2; prof "$COLD"; build "B2"
[ "$(section)" = ".text.f" ] || fail "B2: STALE OBJECT from the worker cache ($(section)): $(jobs_of)"
echo "B2 cold: object correct, $(jobs_of)"
stop_worker
echo "E2E-CACHE-UNSAFE-FLAG PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
