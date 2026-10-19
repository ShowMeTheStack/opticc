#!/usr/bin/env python3
"""Synthesise a kbuild-like include graph for the coordinator closure-path benchmark.
Shape targets measured on Linux v6.12 defconfig: ~12,000 headers spread over 9 include directories (with shadowed
basenames across arch/generic/uapi dirs), ~600-700-file closures per TU, 3,000 TUs in nested source dirs, some
`#include MACRO` (computed) directives, include guards, comments and conditionals.
Usage: gen_kbuild_tree.py OUTDIR [--headers 12000] [--tus 3000] [--seed 1]
Writes OUTDIR/tree/... and OUTDIR/tus.json: [{"cwd": ..., "argv": [...], "src": ...}] (argv = clang -c with the 9 -I dirs).
"""
import argparse, json, os, random

DIRS = ["arch/x86/include", "arch/x86/include/generated", "include", "build/include", "arch/x86/include/uapi",
        "arch/x86/include/generated/uapi", "include/uapi", "build/include/generated/uapi", "tools/include"]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--headers", type=int, default=12000)
    ap.add_argument("--tus", type=int, default=3000); ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--fanout", type=int, default=7); ap.add_argument("--tu-includes", type=int, default=14)
    a = ap.parse_args(); rnd = random.Random(a.seed)
    root = os.path.join(a.out, "tree"); os.makedirs(root, exist_ok=True)
    subs = ["linux", "asm", "net", "uapi/linux", "trace", "crypto", "acpi", "xen"]
    headers = []   # (dir_index, relname)
    for i in range(a.headers):
        d = rnd.choices(range(len(DIRS)), weights=[18, 3, 40, 4, 8, 2, 15, 2, 8])[0]
        rel = f"{rnd.choice(subs)}/h{i}.h"
        headers.append((d, rel))
    # shadowed basenames: 3% of headers also exist (as a different file) in a later search dir
    shadows = [(min(len(DIRS) - 1, d + 1 + rnd.randrange(3)), rel) for d, rel in rnd.sample(headers, a.headers * 3 // 100)]
    def popular_lt(i):   # earlier headers are more "core" (like linux/types.h): skew towards low indices
        return min(int(i * rnd.random() ** 3), i - 1)
    for i, (d, rel) in enumerate(headers):
        p = os.path.join(root, DIRS[d], rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        lines = [f"/* SPDX-License-Identifier: GPL-2.0 */", f"#ifndef _H{i}_", f"#define _H{i}_"]
        if i > 0:
            for _ in range(rnd.randint(a.fanout - 3, a.fanout + 3)):
                j = popular_lt(i); lines.append(f"#include <{headers[j][1]}>")
            if rnd.random() < 0.15: lines.append(f'#include "{os.path.basename(headers[popular_lt(i)][1])}"')   # quoted, includer dir
            if rnd.random() < 0.02: lines.append("#include __H_MACRO")                                          # computed
            if rnd.random() < 0.3: lines.append(f"#ifdef CONFIG_X{i}\n#include <{headers[popular_lt(i)][1]}>\n#endif")
        lines += [f"struct s{i} {{ int a; }}; // comment with #include <not/really.h>", "/* block", "#include <also/not.h>", "*/", f"#endif"]
        open(p, "w").write("\n".join(lines) + "\n")
    for d, rel in shadows:
        p = os.path.join(root, DIRS[d], rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.exists(p): open(p, "w").write("/* shadow */\n#pragma once\n")
    tus = []
    srcdirs = ["kernel", "mm", "fs/ext4", "net/ipv4", "drivers/net/ethernet/intel/e1000", "drivers/gpu/drm", "lib", "crypto", "block", "security/selinux"]
    for t in range(a.tus):
        sd = rnd.choice(srcdirs); src = os.path.join(root, sd, f"t{t}.c"); os.makedirs(os.path.dirname(src), exist_ok=True)
        inc = [headers[a.headers - 1 - int((a.headers // 3) * rnd.random())][1] for _ in range(a.tu_includes)]
        local = os.path.join(root, sd, "local.h")
        if not os.path.exists(local): open(local, "w").write("#pragma once\n#include <linux/h0.h>\n")
        open(src, "w").write("".join(f"#include <{h}>\n" for h in inc) + '#include "local.h"\nint f%d(void){return 0;}\n' % t)
        argv = ["clang", "-nostdinc"] + [x for d in DIRS for x in ("-I", os.path.join(root, d))] + \
               ["-include", os.path.join(root, "include/linux/h0.h") if os.path.exists(os.path.join(root, "include/linux/h0.h")) else os.path.join(root, DIRS[headers[0][0]], headers[0][1]),
                "-c", src, "-o", src[:-2] + ".o"]
        tus.append({"cwd": root, "argv": argv, "src": src})
    json.dump(tus, open(os.path.join(a.out, "tus.json"), "w"))
    print(f"wrote {a.headers} headers (+{len(shadows)} shadows), {a.tus} TUs under {root}")

if __name__ == "__main__":
    main()
