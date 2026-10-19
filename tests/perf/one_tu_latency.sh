#!/usr/bin/env bash
# Small/incremental builds. Time a 1-TU compile (a) plain local cc, (b) opticc with NO coordinator running
# (the client spawns one: cold start incl. worker probes), (c) opticc with a warm coordinator (kept alive between builds).
# Prints medians of N runs. Usage: one_tu_latency.sh [N]
set -u
. "$(dirname "$0")/../e2e/lib.sh"   # daemons exit with this script; process-group cleanup
N=${1:-5}
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
T=$(mktemp -d /tmp/opticc-1tu.XXXXXX); cd "$T"
printf '#include <stdio.h>\nint helper(int x){ return printf("%%d\\n", x) + 1; }\n' > a.c
mkdir -p wkeys ckeys; "$B/src/opticc/opticc" keygen --dir ckeys > /dev/null; cp ckeys/client.pub wkeys/authorized_clients; export OPTICC_KEY_DIR=$T/ckeys
"$B/src/opticcd/opticcd" --listen 127.0.0.1:0 --jobs 4 --token t1 --key-dir wkeys > worker.out 2> worker.err & WPID=$!
for _ in $(seq 50); do grep -q listening worker.out 2>/dev/null && break; sleep 0.1; done
PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' worker.out); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' worker.out)
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=t1 OPTICC_POLICY=ect-window OPTICC_LOCAL_SLOTS=0
t() { python3 -c 'import time;print(time.time())'; }
med() { python3 -c 'import sys,statistics;v=[float(x) for x in sys.argv[1:]];print("%.3f"%statistics.median(v))' "$@"; }
L=(); C=(); W=()
for i in $(seq "$N"); do
  a=$(t); cc -O2 -c a.c -o a_local.o; b=$(t); L+=($(python3 -c "print($b-$a)"))
  # cold: no coordinator; the client spawns one (idle timeout 1 s so it exits before the next cold run)
  OPTICC_IDLE_TIMEOUT=1 bash -c 'a=$(python3 -c "import time;print(time.time())"); '"$B"'/src/opticc/opticc cc -O2 -c a.c -o a.o; b=$(python3 -c "import time;print(time.time())"); python3 -c "print($b-$a)"' > cold.txt; C+=($(cat cold.txt))
  for _ in $(seq 40); do [ -S "$OPTICC_SOCK" ] || break; sleep 0.1; done
done
# warm: one coordinator kept alive, N timed compiles after a first one
OPTICC_IDLE_TIMEOUT=60 "$B/src/opticc/opticc" cc -O2 -c a.c -o a.o
for i in $(seq "$N"); do a=$(t); "$B/src/opticc/opticc" cc -O2 -c a.c -o a.o; b=$(t); W+=($(python3 -c "print($b-$a)")); done
cmp -s a.o a_local.o && same=identical || same=DIFFERENT
echo "1-TU latency (median of $N): local cc $(med "${L[@]}") s | opticc cold (spawns coordinator) $(med "${C[@]}") s | opticc warm coordinator $(med "${W[@]}") s | object $same"
for p in $(lsof -t "$OPTICC_SOCK" 2>/dev/null); do kill $p; done; kill $WPID 2>/dev/null; wait 2>/dev/null; rm -rf "$T"
