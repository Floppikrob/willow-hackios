"""The speed marks of one boot out of a logdump: cpu_test, backboardd's startup, the frames.
usage: speed.py LOGDUMP.bin
Only the part after the last boot-args line of this run (its epoch=), and kernel lines only while
their timestamps go up: what follows is left over from an earlier run."""
import re
import sys

d = open(sys.argv[1], "rb").read()
epochs = re.findall(rb"epoch=(\d+)", d)
epoch = epochs[0]                       # the loader's log comes first: this run's cmdline
start = d.find(b"epoch=" + epoch)
lines = [l.decode("latin1") for l in re.split(rb"[\n\0\r]+", d[start:]) if l.strip()]

stamp = re.compile(r"^\[\s*([0-9.]+)\]")
last = 0.0
kept = []
for l in lines:
    m = stamp.match(l)
    if m:
        t = float(m.group(1))
        if t + 5 < last:                # time went back: an earlier run's
            break
        last = t
    kept.append(l)

print("epoch", epoch.decode(), "kernel log up to %.1f s" % last)
# lines without a timestamp (userland's) are not cut off where the run ends: of those, only the first
# is this run's (an earlier run's may follow it)
marks = [
    (r"cpu_test: one thread", "first"),
    (r"startup complete", "first"),
    (r"System shell connected", "first"),
    (r"MiniIOMobileFramebuffer: swap \d+ \(#", None),
    (r"denied attempt to escape CPU monitor", "count"),
    (r"vfs_switch_root", None),
    (r"processors running", None),
    (r"panic", None),
]
for pattern, how in marks:
    found = [l for l in kept if re.search(pattern, l)]
    if how == "count":
        print("%-40s %d lines" % (pattern[:40], len(found)))
        continue
    for l in found[:1 if how == "first" else 12]:
        print("  " + l[:170])
