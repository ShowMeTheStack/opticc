#!/usr/bin/env bash
# End to end (ctest: e2e_cache_torn_store): a cache entry left torn by a crash, a power loss or a full
# disk (object shorter than recorded, meta missing) must be a miss on EVERY reader: the compiler wrapper's direct path
# and the coordinator's client cache (a meta-less object must not be served as rc 0 with an empty .d, and an object's
# length is checked).
#   1 worker unreachable (local compile), -MD: compiled, stored, direct entry recorded
#   2 make clean: a direct hit (control: the path under test is live)
#   3 the cached object truncated to 100 bytes; make clean: no direct hit, no client-cache hit, object == plain build
#   4 make clean (entry rewritten by 3): a direct hit again; then the meta deleted; make clean: no hit, object correct
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc; OPTICCD=$B/src/opticcd/opticcd
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}
T=$(mktemp -d /tmp/opticc-e2e-cacheo.XXXXXX)
fail() { echo "FAIL: $*"; cleanup; exit 1; }
cleanup() { [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; for p in $(lsof -t "$T/c.sock" 2>/dev/null); do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$T/wkeys" "$T/ckeys"; "$OPTICC" keygen --dir "$T/ckeys" > /dev/null || fail "keygen"
cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$OPTICCD" --listen 127.0.0.1:0 --jobs 1 --token e2e --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/worker.out" 2> "$T/worker.err" & WPID=$!
for _ in $(seq 300); do grep -q listening "$T/worker.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/worker.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/worker.out")
[ -n "$PORT" ] && [ -n "$PUB" ] || fail "no worker address"
kill "$WPID"; wait "$WPID" 2>/dev/null; WPID=
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=e2e OPTICC_POLICY=ect-window OPTICC_MODE=header-push
export OPTICC_SUMMARY=$T/summary.json OPTICC_IDLE_TIMEOUT=600 OPTICC_LOCAL_CACHE=on OPTICC_LOCAL_CACHE_DIR=$T/lc OPTICC_PROBE_CACHE=$T/probe.json
export OPTICC_RDIRECT=off OPTICC_DIRECT_TRACE=$T/direct.trace
start_coord() {
  "$OPTICC" --coordinator > "$T/coord.out" 2> "$T/coord.err" &
  for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] && break; sleep 0.05; done
  [ -S "$OPTICC_SOCK" ] || fail "coordinator did not start: $(cat "$T/coord.err")"
}
stop_coord() { local p; p=$(lsof -t "$OPTICC_SOCK" 2>/dev/null | head -1); [ -n "$p" ] && kill -TERM "$p"; for _ in $(seq 600); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done; sleep 0.3; }
jobs_of() { python3 -c "import json;s=json.load(open('$T/summary.json'));print([(j.get('outcome'), j.get('cache')) for j in s['jobs'] if j.get('src','').endswith('f.c')])" 2>/dev/null; }
# the direct path needs the system-include signature the LIVE coordinator publishes next to its socket after its first
# request: a warm-up compile (another TU) before each build, then wait for the file
warm() { ( cd "$T" && printf 'int w;\n' > warm.c && "$OPTICC" "$CC_REAL" -c warm.c -o warm.o ) > /dev/null 2>&1; for _ in $(seq 100); do ls "$OPTICC_SOCK".sysinc.* > /dev/null 2>&1 && break; sleep 0.1; done; }
S=$T/w; mkdir -p "$S/dist" "$S/ref"
for d in dist ref; do
  printf 'static int t[256] = {1, 2, 3};\nint f(int i) { return t[i & 255] * 7; }\nint g(int i) { return f(i) + f(i + 1); }\n' > "$S/$d/f.c"
  printf 'f.o: f.c\n\t$(CC) -O2 -MD -MF f.o.d -c f.c -o f.o\nclean:\n\trm -f f.o f.o.d\n' > "$S/$d/Makefile"
done
sleep 2.4
( cd "$S/ref" && make -s CC="$CC_REAL" ) > /dev/null 2>&1 || fail "reference build"
build() { start_coord; warm; : > "$T/direct.trace"; ( cd "$S/dist" && make -s clean && make -s CC="$OPTICC $CC_REAL" ) > "$S/out.txt" 2>&1 || fail "$1: build: $(cat "$S/out.txt")"; stop_coord;
          cmp -s "$S/ref/f.o" "$S/dist/f.o" || fail "$1: object differs from the plain build (torn entry served?): $(jobs_of) $(cat "$T/direct.trace")"
          grep -q "f.o:" "$S/dist/f.o.d" && grep -q "f.c" "$S/dist/f.o.d" || fail "$1: dependency file lost its inputs: $(cat "$S/dist/f.o.d")"; }
direct_hit() { cut -f2 "$T/direct.trace" | grep -qx hit; }
build "1"; echo "1 first: $(jobs_of)"
build "2"; direct_hit || fail "2: control — no direct hit on an intact entry ($(cat "$T/direct.trace")): the test would be vacuous"
echo "2 intact: direct hit"
OBJ=$(basename "$(grep -l "f\.c" "$T"/lc/o/*.meta | head -1)" .meta); [ -n "$OBJ" ] && [ -f "$T/lc/o/$OBJ" ] || fail "no cached object of f.c found"
head -c 100 "$T/lc/o/$OBJ" > "$T/short" && cat "$T/short" > "$T/lc/o/$OBJ"
build "3"; direct_hit && fail "3: a TRUNCATED object was served by the direct path"
J=$(jobs_of); echo "$J" | grep -q "client-cache" && fail "3: a TRUNCATED object was served by the client cache ($J)"
echo "3 truncated: miss ($(cut -f3 "$T/direct.trace" | head -1)), $J, object correct"
build "4a"; direct_hit || fail "4a: no direct hit after the entry was rewritten ($(cat "$T/direct.trace"))"
rm -f "$T/lc/o/$OBJ.meta"
build "4"; direct_hit && fail "4: an object WITHOUT its meta was served by the direct path"
J=$(jobs_of); echo "$J" | grep -q "client-cache" && fail "4: an object without its meta was served by the client cache ($J)"
echo "4 meta deleted: miss, $J, object correct"
# 5: the entry rewritten by 4, then one byte of the object changed IN PLACE (same size): no hit anywhere
build "5a"; direct_hit || fail "5a: no direct hit after the entry was rewritten ($(cat "$T/direct.trace"))"
OBJ=$(basename "$(grep -l "f\.c" "$T"/lc/o/*.meta | head -1)" .meta)
python3 -c "import sys;p=sys.argv[1];b=bytearray(open(p,'rb').read());b[len(b)//2]^=0x5a;open(p,'r+b').write(b)" "$T/lc/o/$OBJ"
build "5"; direct_hit && fail "5: a same-size CORRUPTED object was served by the direct path"
J=$(jobs_of); echo "$J" | grep -q "client-cache" && fail "5: a same-size corrupted object was served by the client cache ($J)"
echo "5 one byte flipped (same size): miss ($(cut -f3 "$T/direct.trace" | head -1)), $J, object correct"
# 6: a meta field of the wrong type (depfile_body a number) is a miss, never a failed compile
build "6a"; direct_hit || fail "6a: no direct hit after the entry was rewritten ($(cat "$T/direct.trace"))"
OBJ=$(basename "$(grep -l "f\.c" "$T"/lc/o/*.meta | head -1)" .meta)
python3 -c "import json,sys;p=sys.argv[1];m=json.load(open(p));m['depfile_body']=5;json.dump(m,open(p,'w'))" "$T/lc/o/$OBJ.meta"
build "6"; direct_hit && fail "6: an entry with a malformed meta was served by the direct path"
echo "6 malformed meta: miss ($(cut -f3 "$T/direct.trace" | head -1)), object correct"
echo "E2E-CACHE-TORN-STORE PASS ($T)"
cleanup; trap - EXIT; rm -rf "$T"; exit 0
