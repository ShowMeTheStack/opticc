#!/usr/bin/env python3
"""Size statistics of published direct manifests (opticc/rdirect.hpp "OPR1" inside "OPRZ" zstd files) in a worker's
<object-cache>/rd directory: entries, positives, negative directories and probed names per entry, bytes.
Usage: rdirect_manifest_stats.py <dir> [--zstd /path/to/zstd]"""
import os, struct, subprocess, sys

def entries(raw):
    assert raw[:4] == b"OPR1"; i = 4
    def u32():
        nonlocal i; v = struct.unpack_from("<I", raw, i)[0]; i += 4; return v
    def s():
        nonlocal i; n = u32(); v = raw[i:i + n]; i += n; return v
    out = []
    for _ in range(u32()):
        s(); s(); i += 8 + 1
        npos = u32()
        for _ in range(npos): s(); s(); i += 8
        nneg = u32(); names = 0
        for _ in range(nneg):
            s(); nn = u32(); names += nn
            for _ in range(nn): s()
        out.append((npos, nneg, names))
    return out

d = sys.argv[1]
rows = []
for sub in sorted(os.listdir(d)):
    for f in sorted(os.listdir(os.path.join(d, sub))):
        b = open(os.path.join(d, sub, f), "rb").read()
        raw_len = struct.unpack(">I", b[4:8])[0]
        raw = subprocess.run(["zstd", "-d", "-c"], input=b[8:], capture_output=True).stdout
        assert len(raw) == raw_len, (len(raw), raw_len)
        for e in entries(raw): rows.append((len(b), raw_len) + e)
n = len(rows)
avg = lambda k: sum(r[k] for r in rows) / max(1, n)
print(f"{n} entries: zstd {avg(0)/1024:.1f} KiB, raw {avg(1)/1024:.1f} KiB, positives {avg(2):.0f}, negative dirs {avg(3):.0f}, probed names {avg(4):.0f}")
