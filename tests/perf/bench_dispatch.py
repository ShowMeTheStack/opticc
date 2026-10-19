#!/usr/bin/env python3
"""Coordinator dispatch-throughput benchmark: N concurrent clients on the coordinator's unix
socket, each looping request → decision → done (zero-cost "compiles"), so the measured rate is the coordinator's
request/decide/complete path including the scheduler and its locks.
Usage: bench_dispatch.py OPTICC_BINARY [--clients 120] [--seconds 10] [--hosts ""]   (hosts empty → all-local decisions)
"""
import argparse, json, os, socket, subprocess, sys, tempfile, threading, time

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("opticc"); ap.add_argument("--clients", type=int, default=120)
    ap.add_argument("--seconds", type=float, default=10); ap.add_argument("--hosts", default="")
    a = ap.parse_args()
    d = tempfile.mkdtemp(prefix="ocb-", dir="/tmp"); sock = os.path.join(d, "c.sock")
    env = dict(os.environ, OPTICC_SOCK=sock, OPTICC_HOSTS=a.hosts, OPTICC_POLICY="ect-window", OPTICC_SECURITY="plain",
               OPTICC_SUMMARY=os.path.join(d, "summary.json"), OPTICC_IDLE_TIMEOUT="600")
    p = subprocess.Popen([a.opticc, "--coordinator"], env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, "coord.log"), "w"))
    for _ in range(200):
        if os.path.exists(sock): break
        time.sleep(0.05)
    src = os.path.join(d, "x.c"); open(src, "w").write("int x;\n")
    req = {"op": "request", "src": src, "out": src + ".o", "lang": "c", "cwd": d, "argv": ["cc", "-c", src, "-o", src + ".o"], "cc_local": "cc", "src_bytes": 7}
    count = [0]; lat = []; lock = threading.Lock(); stop = time.time() + a.seconds; errs = [0]
    def client():
        while time.time() < stop:
            try:
                s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
                t = time.time(); f.write(json.dumps(req) + "\n"); f.flush(); dec = json.loads(f.readline())
                f.write(json.dumps({"op": "done", "rc": 0, "obj_bytes": 100, "cpu_s": 0.0}) + "\n"); f.flush(); s.close()
                with lock: count[0] += 1; lat.append(time.time() - t)
            except Exception:
                with lock: errs[0] += 1
    t0 = time.time(); ths = [threading.Thread(target=client) for _ in range(a.clients)]
    [t.start() for t in ths]; [t.join() for t in ths]; el = time.time() - t0
    p.terminate(); p.wait(5)
    lat.sort(); q = lambda x: lat[min(len(lat) - 1, int(x * len(lat)))] * 1000 if lat else None
    print(json.dumps({"clients": a.clients, "dispatches": count[0], "seconds": round(el, 2), "per_s": round(count[0] / el, 1),
                      "p50_ms": q(0.5), "p99_ms": q(0.99), "errors": errs[0]}))

if __name__ == "__main__":
    main()
