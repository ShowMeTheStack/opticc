#!/usr/bin/env python3
"""Make a gen_kbuild_tree.py tree compilable (benchmarks that compile, not just scan): define the computed-include
macro to a stub header and create empty stubs for every missing include, iterating `clang -E` until every TU
preprocesses cleanly. Usage: make_compilable.py TREE_ROOT TUS_JSON  (prints the extra flag to add)"""
import json, os, re, subprocess, sys
root, tusf = sys.argv[1], sys.argv[2]
tus = json.load(open(tusf))
os.makedirs(os.path.join(root, "include"), exist_ok=True)
open(os.path.join(root, "include", "stub_macro.h"), "w").write("#pragma once\n")
extra = ['-D__H_MACRO=<stub_macro.h>']
pat = re.compile(r"^(.*?):\d+:\d+: fatal error: '([^']+)' file not found")
for it in range(60):
    created = 0
    for t in tus:
        argv = t["argv"][:]; i = argv.index("-c"); src = argv[i + 1]
        cmd = [argv[0]] + extra + [x for k, x in enumerate(argv[1:], 1) if k not in (i, i + 1, argv.index("-o"), argv.index("-o") + 1)] + ["-E", src, "-o", "/dev/null"]
        r = subprocess.run(cmd, cwd=t["cwd"], capture_output=True, text=True)
        for line in r.stderr.splitlines():
            m = pat.match(line)
            if not m: continue
            includer, name = m.group(1), m.group(2)
            # quoted include -> includer's directory; angled -> include/
            txt = open(os.path.join(t["cwd"], includer) if not os.path.isabs(includer) else includer, errors="replace").read()
            target = os.path.join(os.path.dirname(includer if os.path.isabs(includer) else os.path.join(t["cwd"], includer)), name) if f'"{name}"' in txt else os.path.join(root, "include", name)
            if not os.path.exists(target):
                os.makedirs(os.path.dirname(target), exist_ok=True); open(target, "w").write("#pragma once\n"); created += 1
    if created == 0: break
print(" ".join(extra))
