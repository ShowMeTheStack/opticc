#!/usr/bin/env python3
"""Cache-hit benchmark: ccache direct mode vs opticc without the direct path (tool "nodirect", OPTICC_DIRECT=off: every hit
goes wrapper -> coordinator -> scan -> key-hash check) vs opticc with the wrapper's direct path (tool "direct"), on a
generated tree.

Flavours:
  kbuild  C, -nostdinc, 9 -I dirs, kbuild's -Wp,-MMD,<dir>/.<obj>.d   (gen_kbuild_tree.py, ~600-header closures)
  cmake   as kbuild but CMake/ninja's dependency form -MD -MT <obj> -MF <obj>.d, system headers included (<stddef.h>,
          <stdint.h>), larger closures (LLVM: 1-3k headers)
Sequence per tool (a NEW coordinator before every timed build when --restart):
  cold -> repeat x3 (clean, rebuild; min) -> touch-source (comment appended to one TU) -> touch-header (comment appended
  to a header included by ~3% of TUs) -> fresh-client (opticc only: EMPTY client cache, worker object cache warm; vs
  ccache with an empty cache = a full compile).
Reports wall seconds and CPU seconds (make's children + the coordinator), and checks every object and dependency file
against an uncached reference build that saw the same edits (byte-identical).
Usage: direct_bench.py [--flavour kbuild|cmake] [--tus 300] [--headers 12000] [--fanout 7] [--tu-includes 14] [-j 8]
                       [--restart] [--keep] [--tools ccache,nodirect,direct]
"""
import argparse, json, os, re, resource, shutil, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
B = os.environ.get("OPTICC_BUILD", os.path.join(ROOT, "build"))
OPTICC = os.path.join(B, "src/opticc/opticc"); OPTICCD = os.path.join(B, "src/opticcd/opticcd")


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), check=True, **kw)


def gen(T, a):
    # generated trees are cached (make_compilable.py takes minutes): /tmp/opticc-benchtrees/<headers>-<tus>-<fanout>-<inc>
    cache = f"/tmp/opticc-benchtrees/{a.headers}-{a.tus}-{a.fanout}-{a.tu_includes}"
    if not os.path.exists(f"{cache}/done"):
        shutil.rmtree(cache, ignore_errors=True); os.makedirs(cache)
        sh([sys.executable, os.path.join(HERE, "gen_kbuild_tree.py"), f"{cache}/g", "--headers", str(a.headers), "--tus", str(a.tus),
            "--fanout", str(a.fanout), "--tu-includes", str(a.tu_includes)], stdout=subprocess.DEVNULL)
        sh([sys.executable, os.path.join(HERE, "make_compilable.py"), f"{cache}/g/tree", f"{cache}/g/tus.json"], stdout=subprocess.DEVNULL)
        open(f"{cache}/done", "w").write("1")
    shutil.copytree(f"{cache}/g", f"{T}/g", symlinks=True)
    with open(f"{T}/g/tus.json") as f: txt = f.read()
    with open(f"{T}/g/tus.json", "w") as f: f.write(txt.replace(f"{cache}/g/", f"{T}/g/"))
    tus = json.load(open(f"{T}/g/tus.json")); root = f"{T}/g/tree"
    rel = lambda p: os.path.relpath(p, root) if os.path.isabs(p) and p.startswith(root) else p
    objs, rules = [], []
    for t in tus:
        av = [rel(x) for x in t["argv"][1:]]
        i = av.index("-c"); src = av[i + 1]; o = av.index("-o"); obj = av[o + 1]
        flags = [x for k, x in enumerate(av) if k not in (i, i + 1, o, o + 1)]
        if a.flavour == "cmake":
            flags = [x for x in flags if x != "-nostdinc"]
            with open(os.path.join(root, src)) as f: body = f.read()
            with open(os.path.join(root, src), "w") as f: f.write("#include <stddef.h>\n#include <stdint.h>\n" + body)
            depf = obj + ".d"; dep = f"-MD -MT {obj} -MF {depf}"
        else:
            depf = os.path.join(os.path.dirname(obj), "." + os.path.basename(obj) + ".d"); dep = f"-Wp,-MMD,{depf}"
        post = ""
        if a.flavour == "kbuild":   # like kbuild's fixdep: the .d is consumed and DELETED right after the compile
            post = f"\t@cp {depf} {depf}.cmd && rm -f {depf}\n"; depf = depf + ".cmd"
        objs.append((obj, depf))
        rules.append(f"{obj}: {src}\n\t$(CC) -O2 {dep} -D__H_MACRO='<stub_macro.h>' {' '.join(flags)} -c {src} -o {obj}\n" + post)
    with open(os.path.join(root, "Makefile"), "w") as f:
        f.write("all: " + " ".join(o for o, _ in objs) + "\n" + "".join(rules) + "-include " + " ".join(d for _, d in objs) + "\n"
                "clean:\n\tfind . -name '*.o' -delete; find . -name '*.o.d' -delete; find . -name '*.o.d.cmd' -delete\n")
    # header to touch: included directly by ~3% of TUs; source to touch: the first TU
    cnt = {}
    for dp, _, fn in os.walk(root):
        for x in fn:
            if x.endswith(".c"):
                for h in re.findall(r"#include <([^>]+)>", open(os.path.join(dp, x)).read()): cnt[h] = cnt.get(h, 0) + 1
    cnt.pop("stddef.h", None); cnt.pop("stdint.h", None)
    hdr = min(cnt, key=lambda h: abs(cnt[h] - 0.03 * len(tus)))
    dirs = ["arch/x86/include", "arch/x86/include/generated", "include", "build/include", "arch/x86/include/uapi",
            "arch/x86/include/generated/uapi", "include/uapi", "build/include/generated/uapi", "tools/include"]
    hrel = next(os.path.join(d, hdr) for d in dirs if os.path.exists(os.path.join(root, d, hdr)))
    srel = rel(tus[0]["src"])
    return root, hrel, srel, len(tus), [o for o, _ in objs], [d for _, d in objs]


def timed(cwd, cc, j, env):
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN); t0 = time.time()
    subprocess.run(["make", "-s", f"-j{j}", f"CC={cc}"], cwd=cwd, env=env, check=True)
    t1 = time.time(); r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    return t1 - t0, (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)


def clean(cwd):
    subprocess.run(["make", "-s", "clean"], cwd=cwd, check=True)


def append_comment(path, tag):
    with open(path, "a") as f: f.write(f"/* {tag} {time.time_ns()} */\n")


class Opticc:
    def __init__(self, T, env, restart, binary=OPTICC):
        self.T, self.env, self.restart, self.binary = T, env, restart, binary
        self.cpu_total = 0.0

    def coord_pid(self):
        p = getattr(self, "proc", None)
        if p is not None and p.poll() is None: return p.pid
        try: out = subprocess.run(["lsof", "-t", self.env["OPTICC_SOCK"]], capture_output=True, text=True).stdout.split()
        except FileNotFoundError: out = []
        return int(out[0]) if out else None

    def coord_cpu(self, pid):
        try:   # Linux: utime + stime from /proc (no procps needed)
            f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
            return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
        except OSError: pass
        out = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
        if not out: return 0.0
        parts = [float(x) for x in out.replace("-", ":").split(":")]
        s = 0.0
        for p in parts: s = s * 60 + p
        return s

    def stop(self):
        pid = self.coord_pid()
        if pid is None: return {}
        cpu = self.coord_cpu(pid)
        # The summary path is reused by every restarted coordinator, so remove the previous build's file first and
        # wait for THIS coordinator's summary to appear complete (a fixed 0.4 s could return the previous build's numbers)
        try: os.unlink(self.env["OPTICC_SUMMARY"])
        except FileNotFoundError: pass
        os.kill(pid, signal.SIGUSR1)
        summ = {}
        for _ in range(600):   # <= 30 s: the SIGUSR1 summary waits for pending local stores
            try: summ = json.load(open(self.env["OPTICC_SUMMARY"])); break
            except Exception: time.sleep(0.05)
        os.kill(pid, signal.SIGTERM)
        # wait for the process itself: SIGTERM drains released-but-unstored jobs and uploads for up to 30 s
        if getattr(self, "proc", None) is not None and self.proc.pid == pid:
            try: self.proc.wait(timeout=45)
            except Exception: pass
        else:
            for _ in range(900):
                try: os.kill(pid, 0)
                except OSError: break
                time.sleep(0.05)
        for _ in range(100):
            if not os.path.exists(self.env["OPTICC_SOCK"]): break
            time.sleep(0.05)
        try: os.unlink(self.env["OPTICC_SOCK"])
        except FileNotFoundError: pass
        summ["_coord_cpu"] = cpu
        return summ

    def fasthits(self):
        try: return os.path.getsize(os.path.join(self.env["OPTICC_LOCAL_CACHE_DIR"], "state", "fasthits"))
        except OSError: return 0

    def start(self):   # pre-spawned outside the timed build
        self.proc = subprocess.Popen([self.binary, "--coordinator"], env=self.env, stdout=subprocess.DEVNULL,
                         stderr=open(os.path.join(self.T, "coord.err"), "a"), start_new_session=True)
        for _ in range(1200):
            if os.path.exists(self.env["OPTICC_SOCK"]): break
            time.sleep(0.05)

    def build(self, cwd, j, extra_env=None):
        f0 = self.fasthits()
        saved = self.env
        if extra_env: self.env = dict(self.env, **extra_env)
        self.start()
        wall, cpu = timed(cwd, f"{self.binary} clang", j, self.env)
        self.env = saved
        summ = self.stop()   # the coordinator's CPU is part of the cost; a fresh one is spawned by the next build
        ccpu = summ.get("_coord_cpu", 0.0)
        jobs = summ.get("jobs", [])
        return dict(wall=wall, cpu=cpu + ccpu, coord_cpu=ccpu, fast=self.fasthits() - f0,
                    hits=sum(1 for x in jobs if x.get("outcome") == "client-cache"),
                    remote=sum(1 for x in jobs if x.get("outcome") == "remote"),
                    local=sum(1 for x in jobs if x.get("outcome") in ("local", "fallback")), v4=summ.get("v4", {}),
                    bloom=summ.get("bloom_affinity_jobs", 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flavour", default="kbuild", choices=["kbuild", "cmake"]); ap.add_argument("--tus", type=int, default=300)
    ap.add_argument("--headers", type=int, default=12000); ap.add_argument("--fanout", type=int, default=7)
    ap.add_argument("--tu-includes", type=int, default=14); ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--restart", action="store_true", default=True); ap.add_argument("--keep", action="store_true")
    ap.add_argument("--tools", default="ccache,nodirect,direct"); ap.add_argument("--dir", default="")
    ap.add_argument("--remote-cold", action="store_true", help="cold builds with OPTICC_LOCAL_SLOTS=0 (as on the testbed, where ~90%% go remote): warms the worker cache for fresh-client")
    ap.add_argument("--nodirect-opticc", default="", help="opticc binary for the nodirect tool (default: this build with OPTICC_DIRECT=off)")
    a = ap.parse_args()
    T = a.dir or tempfile.mkdtemp(prefix="opticc-dbench-", dir="/tmp")
    tools = a.tools.split(",")
    t0 = time.time(); root, hrel, srel, ntu, objs, deps = gen(T, a)
    print(f"# tree {root}: {ntu} TUs, flavour {a.flavour}, touch-header {hrel}, touch-source {srel} (gen {time.time() - t0:.0f} s)", flush=True)
    clos = subprocess.run([OPTICC, "--scan-closure"] + json.load(open(f"{T}/g/tus.json"))[0]["argv"], capture_output=True, text=True, cwd=root).stdout
    print(f"# closure of TU 0: {sum(1 for l in clos.splitlines() if not l.startswith('#'))} headers", flush=True)
    for t in ["ref"] + tools: shutil.copytree(root, f"{T}/{t}", symlinks=True)
    base_env = dict(os.environ)
    res = {}
    # reference: uncached, and its edits mirror every tool's sequence
    w, c = timed(f"{T}/ref", "clang", a.j, base_env); res["uncached"] = dict(wall=w, cpu=c)
    print(f"uncached build: {w:.2f} s wall, {c:.1f} CPU-s", flush=True)

    worker = None
    if any(t != "ccache" for t in tools):
        os.makedirs(f"{T}/wkeys"); sh([OPTICC, "keygen", "--dir", f"{T}/ckeys"], stdout=subprocess.DEVNULL)
        shutil.copy(f"{T}/ckeys/client.pub", f"{T}/wkeys/authorized_clients")
        wout = open(f"{T}/worker.out", "w")
        # --exit-with-parent (the worker dies with this script even if it is killed) and the kill is registered
        # with atexit at once, so a failure before the try/finally below can no longer orphan a listener
        worker = subprocess.Popen([OPTICCD, "--listen", "127.0.0.1:0", "--jobs", "4", "--token", "b", "--key-dir", f"{T}/wkeys",
                                   "--tmp", f"{T}/wtmp", "--object-cache", f"{T}/wcache", "--exit-with-parent"],
                                  stdout=wout, stderr=open(f"{T}/worker.err", "w"))
        import atexit; atexit.register(lambda w=worker: (w.poll() is None) and w.kill())
        for _ in range(600):
            txt = open(f"{T}/worker.out").read()
            if "listening" in txt: break
            time.sleep(0.1)
        port = re.search(r"127\.0\.0\.1:(\d+)", txt).group(1); pub = re.search(r"pubkey=(\S+)", txt).group(1)
    try:
        for tool in tools:
            d = f"{T}/{tool}"; r = res[tool] = {}
            if tool == "ccache":
                env = dict(base_env, CCACHE_DIR=f"{T}/ccdir", CCACHE_BASEDIR=d, CCACHE_SLOPPINESS="time_macros,include_file_mtime,include_file_ctime", CCACHE_MAXSIZE="5G")
                run = lambda: dict(zip(("wall", "cpu"), timed(d, "ccache clang", a.j, env)))
            else:
                env = dict(base_env, OPTICC_KEY_DIR=f"{T}/ckeys", OPTICC_SOCK=f"{T}/{tool}.sock", OPTICC_HOSTS=f"127.0.0.1:{port}#{pub}", OPTICC_TOKEN="b",
                           OPTICC_POLICY="ect-window", OPTICC_MODE="header-push", OPTICC_SUMMARY=f"{T}/{tool}.summary.json", OPTICC_IDLE_TIMEOUT="600",
                           OPTICC_LOCAL_CACHE="on", OPTICC_LOCAL_CACHE_DIR=f"{T}/{tool}.lc", OPTICC_DIRECT="on" if tool == "direct" else "off")
                op = Opticc(T, env, a.restart, a.nodirect_opticc if (tool == "nodirect" and a.nodirect_opticc) else OPTICC)
                run = lambda: op.build(d, a.j)
            r["cold"] = op.build(d, a.j, {"OPTICC_LOCAL_SLOTS": "0"}) if (a.remote_cold and tool != "ccache") else run()
            reps = []
            for _ in range(3): clean(d); reps.append(run())
            r["repeat"] = min(reps, key=lambda x: x["wall"]); r["repeat_all"] = reps
            time.sleep(1.1); append_comment(os.path.join(d, srel), "touch-source"); r["touch-source"] = run()   # make: 1 s mtimes
            time.sleep(1.1); append_comment(os.path.join(d, hrel), "touch-header"); r["touch-header"] = run()
            for k in ["cold", "repeat", "touch-source", "touch-header"]:
                x = r[k]; extra = "" if tool == "ccache" else f" | fast {x['fast']} coord-hits {x['hits']} remote {x['remote']} local {x['local']} coord-cpu {x['coord_cpu']:.1f} {x.get('v4') or ''}"
                print(f"{tool:6s} {k:12s} {x['wall']:7.2f} s wall {x['cpu']:7.1f} CPU-s{extra}", flush=True)
        # correctness: the reference gets the same two edits (touch-source, touch-header), then everything must match
        append_comment(f"{T}/ref/{srel}", "touch-source"); append_comment(f"{T}/ref/{hrel}", "touch-header")
        for tool in tools:   # copy the edited files so that comment bytes are identical
            shutil.copy(f"{T}/{tool}/{srel}", f"{T}/ref/{srel}"); shutil.copy(f"{T}/{tool}/{hrel}", f"{T}/ref/{hrel}")
            subprocess.run(["make", "-s", f"-j{a.j}", "CC=clang"], cwd=f"{T}/ref", check=True)
            bad_o = sum(1 for o in objs if open(f"{T}/ref/{o}", "rb").read() != open(f"{T}/{tool}/{o}", "rb").read())
            bad_d = sum(1 for x in deps if open(f"{T}/ref/{x}", "rb").read() != open(f"{T}/{tool}/{x}", "rb").read())
            res[tool]["mismatch"] = [bad_o, bad_d]
            print(f"{tool:6s} correctness: objects differing {bad_o}/{len(objs)}, dep files differing {bad_d}/{len(deps)}", flush=True)
        # fresh client: empty client cache, worker cache warm (opticc) vs an empty ccache (a full compile)
        for tool in tools:
            d = f"{T}/{tool}"; clean(d)
            if tool == "ccache":
                env = dict(base_env, CCACHE_DIR=f"{T}/ccdir-fresh", CCACHE_BASEDIR=d, CCACHE_SLOPPINESS="time_macros,include_file_mtime,include_file_ctime")
                w, c = timed(d, "ccache clang", a.j, env); res[tool]["fresh-client"] = dict(wall=w, cpu=c)
                print(f"{tool:6s} fresh-client {w:7.2f} s wall {c:7.1f} CPU-s (empty ccache)", flush=True)
            else:
                env = dict(base_env, OPTICC_KEY_DIR=f"{T}/ckeys", OPTICC_SOCK=f"{T}/{tool}.sock", OPTICC_HOSTS=f"127.0.0.1:{port}#{pub}", OPTICC_TOKEN="b",
                           OPTICC_POLICY="ect-window", OPTICC_MODE="header-push", OPTICC_SUMMARY=f"{T}/{tool}.summary.json", OPTICC_IDLE_TIMEOUT="600",
                           OPTICC_LOCAL_CACHE="on", OPTICC_LOCAL_CACHE_DIR=f"{T}/{tool}.lc-fresh", OPTICC_DIRECT="on" if tool == "direct" else "off")
                x = Opticc(T, env, True, a.nodirect_opticc if (tool == "nodirect" and a.nodirect_opticc) else OPTICC).build(d, a.j); res[tool]["fresh-client"] = x
                print(f"{tool:6s} fresh-client {x['wall']:7.2f} s wall {x['cpu']:7.1f} CPU-s | remote {x['remote']} (worker cache) local {x['local']} bloom-affinity {x.get('bloom')} coord-cpu {x['coord_cpu']:.1f}", flush=True)
            bad_o = sum(1 for o in objs if open(f"{T}/ref/{o}", "rb").read() != open(f"{d}/{o}", "rb").read())
            bad_d = sum(1 for x in deps if open(f"{T}/ref/{x}", "rb").read() != open(f"{d}/{x}", "rb").read())
            res[tool]["fresh_mismatch"] = [bad_o, bad_d]
            print(f"{tool:6s} fresh-client correctness: objects differing {bad_o}, dep files differing {bad_d}", flush=True)
        json.dump(res, open(f"{T}/results.json", "w"), indent=1)
        print(f"# results: {T}/results.json")
    finally:
        if worker: worker.kill()
        if not a.keep and not a.dir: shutil.rmtree(T, ignore_errors=True)


if __name__ == "__main__":
    main()
