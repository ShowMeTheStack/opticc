#!/usr/bin/env python3
"""Per-hit cost of the wrapper's direct path vs ccache's direct mode, without any worker (no TCP listener):
the coordinator runs with no hosts, so the cold compiles are local and recorded; then each TU's compile command is
repeated K times through each tool and the children's CPU (user+sys) is divided by the number of hits.
--regen-mb M: every TU also force-includes a generated header of M MB that is REGENERATED with identical bytes (new
inode, new ctime) between the recording and the timed hits, as LLVM's tablegen outputs are after `ninja -t clean`.
Usage: hit_cost.py [--tree /tmp/opticc-benchtrees/12000-300-7-14/g] [--tus 40] [-k 10] [--regen-mb 4]"""
import argparse, json, os, resource, shutil, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
B = os.environ.get("OPTICC_BUILD", os.path.join(ROOT, "build")); OPTICC = os.path.join(B, "src/opticc/opticc")


def cpu():
    r = resource.getrusage(resource.RUSAGE_CHILDREN); return r.ru_utime + r.ru_stime


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--tree", default="/tmp/opticc-benchtrees/12000-300-7-14/g")
    ap.add_argument("--tus", type=int, default=40); ap.add_argument("-k", type=int, default=10)
    ap.add_argument("--regen-mb", type=float, default=0)
    a = ap.parse_args()
    T = tempfile.mkdtemp(prefix="oc-hit-", dir="/tmp")   # short: unix socket paths are limited
    shutil.copytree(a.tree, f"{T}/g", symlinks=True)
    tus = json.loads(open(f"{T}/g/tus.json").read().replace(a.tree + "/", f"{T}/g/"))[: a.tus]
    root = f"{T}/g/tree"; os.makedirs(f"{T}/o", exist_ok=True)
    cmds = []
    big = f"{T}/gen/big.h"
    body = "".join(f"#define OPTICC_GEN_{i} {i}\n" for i in range(int(a.regen_mb * 1e6 / 30))) if a.regen_mb else ""
    def regen():
        os.makedirs(f"{T}/gen", exist_ok=True); open(big + ".tmp", "w").write(body); os.rename(big + ".tmp", big)
    if a.regen_mb: regen()
    for i, t in enumerate(tus):
        av = list(t["argv"]); o = av.index("-o"); av[o + 1] = f"{T}/o/t{i}.o"
        av += [f"-Wp,-MMD,{T}/o/.t{i}.o.d", "-O2", "-D__H_MACRO=<stub_macro.h>"] + (["-include", big] if a.regen_mb else [])
        cmds.append(av)
    time.sleep(2.5)   # inputs older than any racy margin
    env = dict(os.environ, OPTICC_SOCK=f"{T}/c.sock", OPTICC_HOSTS="", OPTICC_LOCAL_CACHE="on", OPTICC_LOCAL_CACHE_DIR=f"{T}/lc",
               OPTICC_IDLE_TIMEOUT="600", OPTICC_SUMMARY=f"{T}/summary.json", OPTICC_MODE="header-push", OPTICC_POLICY="ect-window")
    co = subprocess.Popen([OPTICC, "--coordinator"], env=env, stdout=subprocess.DEVNULL, stderr=open(f"{T}/coord.err", "w"))
    for _ in range(200):
        if os.path.exists(f"{T}/c.sock"): break
        time.sleep(0.05)
    for av in cmds: subprocess.run([OPTICC] + av, cwd=root, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    co.send_signal(signal.SIGUSR1); time.sleep(3)   # SIGUSR1: drain the record queue, write the summary
    cenv = dict(os.environ, CCACHE_DIR=f"{T}/ccache", CCACHE_SLOPPINESS="time_macros,include_file_mtime,include_file_ctime", CCACHE_BASEDIR=root)
    for av in cmds: subprocess.run(["ccache"] + av, cwd=root, env=cenv, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if a.regen_mb: regen(); time.sleep(0.5)   # regenerated, identical bytes; settled
    res = {}
    for name, pre, e in [("opticc-direct", [OPTICC], env), ("opticc-direct-nomemo", [OPTICC], dict(env, OPTICC_HASH_MEMO="off")),
                         ("ccache", ["ccache"], cenv), ("bare-exec(true)", None, env)]:
        c0 = cpu(); w0 = time.time()
        for _ in range(a.k):
            for av in cmds:
                argv = ["/usr/bin/true"] if pre is None else pre + av
                subprocess.run(argv, cwd=root, env=e, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        n = a.k * len(cmds); res[name] = ((cpu() - c0) / n * 1e3, (time.time() - w0) / n * 1e3)
    tenv = dict(env, OPTICC_DEBUG_DIRECT_TIMING="1")
    for av in cmds[:5]:
        r = subprocess.run([OPTICC] + av, cwd=root, env=tenv, capture_output=True, text=True)
        print(" | ".join(l for l in r.stderr.splitlines() if "direct-timing" in l or "PROF" in l)[-260:])
    s = json.load(open(f"{T}/summary.json")) if os.path.exists(f"{T}/summary.json") else {}
    sizes = [os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(f"{T}/lc/d") for f in fs]
    csz = [os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(f"{T}/ccache") for f in fs if f.endswith("M")]
    print(json.dumps({"per_hit_ms_cpu_wall": res, "recorded": s.get("v4", {}).get("recorded"), "fast_hits_file": None,
                      "manifest_bytes_mean": sum(sizes) / max(1, len(sizes)), "ccache_manifest_bytes_mean": sum(csz) / max(1, len(csz))}, indent=1))
    print("memo files:", sum(len(fs) for _, _, fs in os.walk(f"{T}/lc/h")), "cache dir:", open(f"{T}/c.sock.cache").read() if os.path.exists(f"{T}/c.sock.cache") else None)
    co.terminate(); co.wait()
    shutil.rmtree(T, ignore_errors=True)


if __name__ == "__main__":
    main()
