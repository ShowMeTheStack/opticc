#!/usr/bin/env bash
# End to end (ctest: e2e_cc_probes): kbuild-style compiler probes are answered by the wrapper's probe cache
# (opticc/probe.hpp) with exactly the result the compiler gives (exit status, stdout, stderr and output bytes) and
# miss whenever anything that can change that result changes. No coordinator or worker is needed: the cache directory
# is announced through <OPTICC_SOCK>.cache as a coordinator does.
#   (1) supported flag (-c -x c /dev/null -o F): store, then hit; identical rc / stdout / stderr / object
#   (2) unsupported flag: the failure is stored and replayed exactly (rc 1, same diagnostic, no output file)
#   (3) a forced include (-include h.h) is never cached; editing h.h changes the result accordingly
#   (4) a different compiler binary (copy, then touch) -> miss
#   (5) stdin probes (as-instr shape) with different content -> different keys; same content -> hit
#   (6) the output path differs between calls (kbuild's .tmp_$$/tmp): the object is written to the new path
#   (7) -dM -E -x c /dev/null (stdout) and --version: hit and byte-identical
#   (8) stdin content that names a file (#include) is never cached, and the result is still correct
#   (9) an environmental failure (missing output directory) is not stored
set -u
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
OPTICC=$B/src/opticc/opticc
CC_REAL=${CC_REAL:-$(command -v clang || command -v cc)}   # clang preferred (Ubuntu cc = gcc invokes an external as, which the probe cache refuses)
T=$(mktemp -d /tmp/opticc-e2e-probe.XXXXXX)
fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$T/lc/state" "$T/w/.tmp_1" "$T/w/.tmp_2"
printf '%s' "$T/lc" > "$T/c.sock.cache"
export OPTICC_SOCK=$T/c.sock OPTICC_DIRECT_TRACE=$T/trace.tsv
cd "$T/w" || fail "cd"
cnt() { grep -c "	$1	" "$T/trace.tsv" 2>/dev/null || true; }
run() {   # run <tag> <args...>: opticc and the plain compiler; compare rc, stdout, stderr
  local tag=$1; shift
  "$OPTICC" "$CC_REAL" "$@" > "$T/$tag.o.out" 2> "$T/$tag.o.err"; echo $? > "$T/$tag.o.rc"
}
ref() { local tag=$1; shift; "$CC_REAL" "$@" > "$T/$tag.r.out" 2> "$T/$tag.r.err"; echo $? > "$T/$tag.r.rc"; }
same() { cmp -s "$T/$1.o.rc" "$T/$2.r.rc" && cmp -s "$T/$1.o.out" "$T/$2.r.out" && cmp -s "$T/$1.o.err" "$T/$2.r.err"; }

# (1) supported flag
ref s1 -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/ref.o
run s1 -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/tmp
[ "$(cnt probe-store)" = 1 ] || fail "(1) first probe not stored: $(cat "$T/trace.tsv")"
cmp -s .tmp_1/tmp .tmp_1/ref.o || fail "(1) miss object differs from the compiler's"
same s1 s1 || fail "(1) miss streams/rc differ"
rm -f .tmp_1/tmp
run s1h -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/tmp
[ "$(cnt probe-hit)" = 1 ] || fail "(1) second probe not a hit"
cmp -s .tmp_1/tmp .tmp_1/ref.o || fail "(1) hit object differs"
same s1h s1 || fail "(1) hit streams/rc differ"
echo "ok (1) supported flag: stored, then hit, identical rc/stdout/stderr/object"

# (2) unsupported flag (failure cached)
ref f -Werror -fno-such-flag-opticc-xyz -c -x c /dev/null -o .tmp_1/f.o
[ "$(cat "$T/f.r.rc")" != 0 ] || fail "(2) the compiler accepted the bogus flag"
run f -Werror -fno-such-flag-opticc-xyz -c -x c /dev/null -o .tmp_1/f.o
run f2 -Werror -fno-such-flag-opticc-xyz -c -x c /dev/null -o .tmp_1/f.o
same f f || fail "(2) miss differs"; same f2 f || fail "(2) hit differs: $(cat "$T/f2.o.err") vs $(cat "$T/f.r.err")"
[ ! -e .tmp_1/f.o ] || fail "(2) an output file exists after a failure"
[ "$(cnt probe-hit)" = 2 ] || fail "(2) failure not replayed from the cache"
echo "ok (2) unsupported flag: failure stored and replayed exactly (rc $(cat "$T/f.r.rc"))"

# (3) forced include: never cached, follows the header
printf '#define X 1\n' > h.h
run i1 -Werror -include h.h -c -x c /dev/null -o .tmp_1/i.o
run i1b -Werror -include h.h -c -x c /dev/null -o .tmp_1/i.o
printf '#error edited\n' > h.h
ref i2 -Werror -include h.h -c -x c /dev/null -o .tmp_1/i.o
run i2 -Werror -include h.h -c -x c /dev/null -o .tmp_1/i.o
same i2 i2 || fail "(3) after editing the header the result differs from the compiler's (stale cached result)"
[ "$(cat "$T/i2.o.rc")" != 0 ] || fail "(3) the edited header did not take effect"
grep -q "probe-refused	file-touching argument -include" "$T/trace.tsv" || fail "(3) -include probe was not refused"
echo "ok (3) -include probes are not cached; an edited header changes the result"

# (4) a different compiler binary -> miss
printf '#!/bin/sh\nexec "%s" "$@"\n' "$CC_REAL" > "$T/cc-copy"; chmod +x "$T/cc-copy"   # another compiler executable (its own identity)
H0=$(cnt probe-hit)
"$OPTICC" "$T/cc-copy" -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/c.o >/dev/null 2>&1
[ "$(cnt probe-hit)" = "$H0" ] || fail "(4) a different compiler path hit the cache"
"$OPTICC" "$T/cc-copy" -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/c.o >/dev/null 2>&1
[ "$(cnt probe-hit)" = $((H0 + 1)) ] || fail "(4) same copied compiler did not hit"
sleep 0.05; touch "$T/cc-copy"
"$OPTICC" "$T/cc-copy" -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_1/c.o >/dev/null 2>&1
[ "$(cnt probe-hit)" = $((H0 + 1)) ] || fail "(4) a touched compiler binary hit the cache"
echo "ok (4) another or a touched compiler binary misses"

# (5) stdin probes: content is part of the key
A1='.byte 1'; A2='.byte 2'
printf '%s\n' "$A1" | "$CC_REAL" -c -x assembler-with-cpp -o .tmp_1/ra1.o -; printf '%s\n' "$A2" | "$CC_REAL" -c -x assembler-with-cpp -o .tmp_1/ra2.o -
H0=$(cnt probe-hit)
printf '%s\n' "$A1" | "$OPTICC" "$CC_REAL" -c -x assembler-with-cpp -o .tmp_1/a1.o - || fail "(5) stdin probe 1 failed"
printf '%s\n' "$A2" | "$OPTICC" "$CC_REAL" -c -x assembler-with-cpp -o .tmp_1/a2.o - || fail "(5) stdin probe 2 failed"
[ "$(cnt probe-hit)" = "$H0" ] || fail "(5) different stdin content hit"
printf '%s\n' "$A1" | "$OPTICC" "$CC_REAL" -c -x assembler-with-cpp -o .tmp_1/a1h.o - || fail "(5) stdin probe 1 again failed"
[ "$(cnt probe-hit)" = $((H0 + 1)) ] || fail "(5) same stdin content did not hit"
cmp -s .tmp_1/a1.o .tmp_1/ra1.o && cmp -s .tmp_1/a2.o .tmp_1/ra2.o && cmp -s .tmp_1/a1h.o .tmp_1/ra1.o || fail "(5) stdin objects differ from the compiler's"
echo "ok (5) stdin probes keyed by content; objects identical"

# (6) output path differs between calls
H0=$(cnt probe-hit)
run o6 -Werror -O2 -fno-strict-aliasing -c -x c /dev/null -o .tmp_2/tmp
[ "$(cnt probe-hit)" = $((H0 + 1)) ] || fail "(6) another output path did not hit"
cmp -s .tmp_2/tmp .tmp_1/ref.o || fail "(6) object at the new path differs"
echo "ok (6) the object is written to the new output path"

# (7) -dM -E (stdout) and --version
ref m -dM -E -x c /dev/null; run m -dM -E -x c /dev/null; run m2 -dM -E -x c /dev/null
same m m && same m2 m || fail "(7) -dM -E output differs"
ref v --version; run v --version; run v2 --version
same v v && same v2 v || fail "(7) --version output differs"
grep -q "probe-hit	0	-dM" "$T/trace.tsv" && grep -q "probe-hit	0	--version" "$T/trace.tsv" || fail "(7) -dM -E / --version did not hit"
echo "ok (7) -dM -E and --version hit with identical output"

# (8) stdin naming a file: not cached, still correct
printf '#define Y 2\n' > inc.h
printf '#include "inc.h"\nint y = Y;\n' | "$OPTICC" "$CC_REAL" -c -x c -o .tmp_1/s.o - || fail "(8) stdin include probe failed"
grep -q "probe-refused	stdin names files" "$T/trace.tsv" || fail "(8) stdin with #include was cached"
printf '#include "inc.h"\nint y = Y;\n' | "$CC_REAL" -c -x c -o .tmp_1/sr.o - && cmp -s .tmp_1/s.o .tmp_1/sr.o || fail "(8) object differs"
echo "ok (8) stdin naming a file is compiled, not cached"

# (9) environmental failure is not stored
S0=$(cnt probe-store)
"$OPTICC" "$CC_REAL" -Werror -O1 -c -x c /dev/null -o nodir/tmp > /dev/null 2>&1 && fail "(9) missing directory succeeded?"
[ "$(cnt probe-store)" = "$S0" ] || fail "(9) an environmental failure was stored"
mkdir -p nodir; "$OPTICC" "$CC_REAL" -Werror -O1 -c -x c /dev/null -o nodir/tmp || fail "(9) after mkdir the probe still fails (stale failure replayed)"
echo "ok (9) environmental failures are not cached"

echo "trace: $(cnt probe-hit) hits, $(cnt probe-store) stores, $(grep -c probe-refused "$T/trace.tsv") refused"
rm -rf "$T"
echo PASS
