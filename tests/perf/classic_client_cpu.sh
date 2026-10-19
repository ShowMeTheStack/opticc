#!/usr/bin/env bash
# Per-job CLIENT CPU of opticc classic mode on a kernel-like microbench (60 TUs, each preprocessing to ~0.7 MB of
# SDK headers), loopback worker. Client CPU = make/wrapper tree (getrusage of the build, /usr/bin/time) + the
# coordinator process (ps cputime before/after, since a pre-spawned coordinator's CPU is not in the build's own
# rusage). Baseline: the irreducible `cc -E` of every TU. Usage: classic_client_cpu.sh
set -u
# Test hygiene: every opticcd/coordinator started here dies with the script (lib.sh: OPTICC_EXIT_WITH_PARENT,
# process-group kill), and an EXIT trap also kills the two daemons by PID and removes the temp tree
. "$(dirname "$0")/../e2e/lib.sh"
cleanup_ccpu() { [ -n "${CP:-}" ] && kill "$CP" 2>/dev/null; [ -n "${WPID:-}" ] && kill "$WPID" 2>/dev/null; wait 2>/dev/null; [ -n "${T:-}" ] && rm -rf "$T"; e2e_kill_group; }
trap 'rc=$?; cleanup_ccpu; exit $rc' EXIT
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}; CC_REAL=${CC_REAL:-cc}
T=$(mktemp -d /tmp/opticc-ccpu.XXXXXX); mkdir -p "$T/src"
HDRS='#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n#include <pthread.h>\n#include <sys/socket.h>\n#include <sys/stat.h>\n#include <netinet/in.h>\n#include <signal.h>\n#include <time.h>\n#include <wchar.h>\n#include <locale.h>\n#include <fcntl.h>\n#include <unistd.h>\n#include <dirent.h>\n#include <sys/mman.h>\n#include <netdb.h>\n#include <CoreFoundation/CoreFoundation.h>\n'
for i in $(seq 1 60); do printf "$HDRS"'int f%d(int x){ return x * %d + (int)strlen("a"); }\n' $i $i > "$T/src/t$i.c"; done
printf 'SRCS := $(wildcard *.c)\nOBJS := $(SRCS:.c=.o)\nall: $(OBJS)\n%%.o: %%.c\n\t$(CC) -O2 -c $< -o $@\nclean:\n\trm -f *.o\n' > "$T/src/Makefile"
cpu_of() { ps -o cputime= -p "$1" 2>/dev/null | awk -F'[:.]' '{ if (NF==3) print $1*60+$2+$3/100; else print $1*3600+$2*60+$3+$4/100 }'; }
rus() { /usr/bin/time -p "$@" 2>&1 >/dev/null | awk '/^user/{u=$2} /^sys/{s=$2} END{print u+s}'; }
# baseline: cc -E of every TU
BASE=$( cd "$T/src" && rus bash -c 'for f in t*.c; do '"$CC_REAL"' -E $f -o /dev/null; done' )
# distcc-like client path (approximation: distcc preprocesses to a temp file and LZO-compresses it; lz4 -1 stands in
# for LZO1X-1, same speed class; distcc's own wrapper exec is not included, so this UNDER-estimates distcc)
DLIKE=$( cd "$T/src" && rus bash -c 'for f in t*.c; do '"$CC_REAL"' -E $f -o $f.i && lz4 -1 -q -f $f.i $f.i.lz4 && rm -f $f.i $f.i.lz4; done' )
python3 -c "print(f'cc -E alone {1e3*$BASE/60:.1f} ms/TU; distcc-like (cc -E to file + lz4 -1) {1e3*$DLIKE/60:.1f} ms/TU (overhead {1e3*($DLIKE-$BASE)/60:.1f} ms/TU, lower bound for distcc)')"
mkdir -p "$T/wkeys" "$T/ckeys"; "$B/src/opticc/opticc" keygen --dir "$T/ckeys" > /dev/null; cp "$T/ckeys/client.pub" "$T/wkeys/authorized_clients"; export OPTICC_KEY_DIR=$T/ckeys
"$B/src/opticcd/opticcd" --listen 127.0.0.1:0 --jobs 8 --token t --key-dir "$T/wkeys" --tmp "$T/wtmp" > "$T/w.out" 2> "$T/w.err" & WPID=$!
for _ in $(seq 50); do grep -q listening "$T/w.out" 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' "$T/w.out"); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' "$T/w.out")
run() {   # $1 label, extra env in $2
  rm -f "$T/c.sock"
  env $2 OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=t OPTICC_MODE=classic OPTICC_LOCAL_SLOTS=0 OPTICC_LOCAL_CACHE=off \
      OPTICC_PROBE_CACHE=$T/pc.json OPTICC_IDLE_TIMEOUT=60 "$B/src/opticc/opticc" --coordinator > /dev/null 2> "$T/c.err" & CP=$!
  for _ in $(seq 100); do [ -S "$T/c.sock" ] && break; sleep 0.05; done
  c0=$(cpu_of $CP); ( cd "$T/src" && make -s clean )
  MK=$( cd "$T/src" && export OPTICC_SOCK=$T/c.sock && rus make -s -j8 CC="$B/src/opticc/opticc $CC_REAL" )
  c1=$(cpu_of $CP); kill $CP; wait $CP 2>/dev/null
  python3 -c "b=$BASE; m=$MK; c=$c1-$c0; n=60; print(f'$1: client CPU {m+c:.2f} s = build tree {m:.2f} + coordinator {c:.2f}; per TU {1e3*(m+c)/n:.1f} ms (cc -E alone {1e3*b/n:.1f} ms -> opticc overhead {1e3*(m+c-b)/n:.1f} ms/TU, coordinator {1e3*c/n:.1f} ms/TU)')"
}
run "zstd 3 forced" "OPTICC_ZSTD=3"
run "adaptive (loopback: raw)" ""
kill $WPID; wait 2>/dev/null; rm -rf "$T"; WPID=; CP=; T=
