#!/usr/bin/env bash
# Minimal manual smoke test: worker + coordinator + two remote compiles.
set -u
. "$(dirname "$0")/lib.sh"   # daemons exit with this script; process-group cleanup
B=${OPTICC_BUILD:-$(cd "$(dirname "$0")/../.." && pwd)/build}
T=$(mktemp -d /tmp/opticc-smoke.XXXXXX); cd "$T"
printf '#include <stdio.h>\nint helper(int x);\nint main(void){ printf("%%d\\n", helper(3)); return 0; }\n' > a.c
printf 'int helper(int x){ return x+1; }\n' > b.c
mkdir -p wkeys ckeys; "$B/src/opticc/opticc" keygen --dir ckeys > /dev/null; cp ckeys/client.pub wkeys/authorized_clients; export OPTICC_KEY_DIR=$T/ckeys
"$B/src/opticcd/opticcd" --listen 127.0.0.1:0 --jobs 2 --token t1 --key-dir wkeys > worker.out 2> worker.err & WPID=$!
sleep 0.3; PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\).*/\1/p' worker.out); PUB=$(sed -n 's/.*pubkey=\(.*\)$/\1/p' worker.out)
export OPTICC_SOCK=$T/c.sock OPTICC_HOSTS="127.0.0.1:$PORT#$PUB" OPTICC_TOKEN=t1 OPTICC_POLICY=ect-window OPTICC_SUMMARY=$T/summary.json OPTICC_VERBOSE=1 OPTICC_IDLE_TIMEOUT=3 OPTICC_LOCAL_SLOTS=0
"$B/src/opticc/opticc" --coordinator > coord.out 2> coord.err & CPID=$!
sleep 1.5
"$B/src/opticc/opticc" cc -O2 -c a.c -o a.o; echo "a rc=$?"
"$B/src/opticc/opticc" cc -O2 -c b.c -o b.o; echo "b rc=$?"
cc -O2 -c a.c -o a_local.o; cc -O2 -c b.c -o b_local.o
cmp a.o a_local.o && echo "a identical"; cmp b.o b_local.o && echo "b identical"
cc a.o b.o -o prog && ./prog
wait $CPID
echo "--- worker"; cat worker.err; echo "--- coord"; cat coord.err
python3 -c "import json;s=json.load(open('$T/summary.json'));print({k:s[k] for k in ['policy','jobs_total','jobs_local','jobs_remote','fallbacks','rho','wall_s']}); print(s['nodes'][1])"
kill $WPID; wait $WPID 2>/dev/null; echo "dir $T"
