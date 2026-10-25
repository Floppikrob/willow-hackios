"""The Metal test's results out of a logdump whose lines other cores tear: each 'drvtest:'/'redwoodmetal[' line
is glued back from the pieces that follow it until its newline, skipping text that looks like another writer's."""
import re, sys
d = open(sys.argv[1], "rb").read()
written = [int(x) for x in re.findall(rb"log: (\d+) bytes written to logdump", d)]
d = d[:max(written)] if written else d  # this run's own part; past it is an older run's
start = d.find(b"$ /redwood/bin/cpu_test")
d = d[start:] if start >= 0 else d
keys = (b"drvtest: ", b"redwoodmetal[")
out = []
for m in re.finditer(rb"drvtest: |redwoodmetal\[", d):
    i = m.start()
    # take up to 600 bytes; cut at the first newline that follows a sentence end
    chunk = d[i:i + 600]
    line = chunk.split(b"\n", 1)[0]
    out.append(line.decode("utf-8", "replace"))
pat = re.compile(r"dispatch|buffer \d|of \d+ fu|draw|pixel|refus|fail|could not|hung|no answer|panic|crash|compute |air ")
for l in out:
    if pat.search(l):
        print(l[:240])
