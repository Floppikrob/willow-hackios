"""Frames a second and driver times from the display's 5-second lines of one or more logdumps (torn lines: by fields)."""
import re, sys
pat = re.compile(rb"(\d+) frames in (\d+) ms \((\d+) a second\), (\d+) us each in the driver, after (\d+) us waiting for the GPU"
                 rb"(?: and, (\d+) of them, (\d+) us for the next refresh)?; (\d+) swaps waited for the frame before, (\d+) gave up; "
                 rb"(\d+) swaps came a refresh late themselves(?:; (\d+) copied by the GPU)?")
wpat = re.compile(rb"redwood-gl: frame (\d+): (\d+) us \((\d+) handing on, (\d+) waiting")
gpat = re.compile(rb"RedwoodGPU: (\d+) submissions in (\d+) ms: .{0,80}?buffers named, (\d+) KB out of the cache in (\d+) us a submission; "
                  rb"the GPU worked (\d+) us on each of (\d+)")
ppat = re.compile(rb"cpu-prep: (\d+) asked, (\d+) waited for the GPU, (\d+) us each")
opat = re.compile(rb"osm: core [0-9a-f]+: [^\n]{0,120}")
for name in sys.argv[1:]:
    d = open(name, "rb").read()
    # this run's own part: the loader says how much it wrote; past that is an older run's
    written = [int(x) for x in re.findall(rb"log: (\d+) bytes written to logdump", d)]
    d = d[:max(written)] if written else d
    rows = [m for m in pat.finditer(d) if int(m.group(2)) < 6000 and int(m.group(1)) > 20]
    fps = [int(m.group(1)) * 1000 / int(m.group(2)) for m in rows]
    print(name, "windows", len(rows))
    if fps:
        s = sorted(fps)
        print("  fps min %.1f median %.1f mean %.1f max %.1f" % (s[0], s[len(s) // 2], sum(s) / len(s), s[-1]))
        print("  fps by window", [round(x, 1) for x in fps])
        print("  driver us", [int(m.group(4)) for m in rows][:20])
        print("  gpu wait us", [int(m.group(5)) for m in rows][:20])
        print("  waited-before/late", [(int(m.group(8)), int(m.group(10))) for m in rows][:20])
    avg = re.findall(rb"()(\d+) us a frame, of which (\d+) handing the commands on, (\d+) waiting", d)
    avg = [(int(a[1]), int(a[2]), int(a[3])) for a in avg if a[1]]
    if avg:
        print("  window server, average per frame so far (us: frame / handing on / waiting):",
              " ".join("%d/%d/%d" % x for x in avg[:10]))
    w = [(int(m.group(2)), int(m.group(3)), int(m.group(4))) for m in wpat.finditer(d)]
    if w:
        tot = sorted(x[0] for x in w)
        print("  window-server frames sampled", len(w), "median us", tot[len(tot) // 2], "handing-on median",
              sorted(x[1] for x in w)[len(w) // 2], "waiting median", sorted(x[2] for x in w)[len(w) // 2])
    g = [m for m in gpat.finditer(d)]
    if g:
        print("  gpu: subs/5s", [int(m.group(1)) for m in g][:20])
        print("  gpu: KB flushed/sub", [int(m.group(3)) for m in g][:20], "us/sub", [int(m.group(4)) for m in g][:20])
        print("  gpu: worked us/sub", [int(m.group(5)) for m in g][:20])
    for m in list(ppat.finditer(d))[:20]:
        print("  prep", m.group(0).decode())
    for m in list(opat.finditer(d))[:10]:
        print("  ", m.group(0).decode(errors="replace"))
    for t in sorted(set(re.findall(rb"redwood: status bar[^\n]{0,200}", d)))[:12]:
        print("  ", t.decode(errors="replace"))
    for t in sorted(set(re.findall(rb"No status bar visual provider[^\n]{0,160}", d)))[:3]:
        print("  ", t.decode(errors="replace"))
    print("  kernel returned" if b"kernel returned" in d else "  KERNEL DID NOT RETURN")
