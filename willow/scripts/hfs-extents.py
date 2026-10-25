#!/usr/bin/env python3
"""The parts of a sparse filesystem image that hold data (SEEK_DATA / SEEK_HOLE), packed for a partition.

  hfs-extents.py IMAGE OUTDIR

Writes OUTDIR/chunk-NNN.bin (each a run of data, at most 16 MB) and OUTDIR/write.sh, a script for the
phone's recovery that puts every chunk at its place in the partition given as its first argument
(dd, blocks of 4096, conv=notrunc) and then reads each back and compares. What a filesystem's maker
leaves as holes is space it never wrote -- free space, whose contents nothing reads -- so the
partition need not be cleared first: willow's userdata, 43 GiB, takes some 150 MB this way.
"""
import os
import sys

image, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
CHUNK = 16 << 20
fd = os.open(image, os.O_RDONLY)
size = os.fstat(fd).st_size
runs = []
pos = 0
while pos < size:
    try:
        start = os.lseek(fd, pos, os.SEEK_DATA)
    except OSError:
        break
    end = os.lseek(fd, start, os.SEEK_HOLE)
    start -= start % 512
    end += (-end) % 512
    runs.append((start, min(end, size)))
    pos = end
lines = ["#!/system/bin/sh", "# written by scripts/hfs-extents.py from %s" % os.path.basename(image),
         "set -e", 'DEV="$1"', '[ -b "$DEV" ] || { echo "no block device: $DEV"; exit 1; }', 'cd "$(dirname "$0")"']
n = 0
total = 0
for start, end in runs:
    at = start
    while at < end:
        length = min(CHUNK, end - at)
        name = "chunk-%03d.bin" % n
        with open(os.path.join(out, name), "wb") as f:
            f.write(os.pread(fd, length, at))
        # blocks of 4096 where the run allows, else of 512 (the end of a partition need not be a multiple of 4096:
        # the alternate volume header sits in its last 1024 bytes)
        bs = 4096 if at % 4096 == 0 and length % 4096 == 0 else 512
        lines.append("dd if=%s of=$DEV bs=%d seek=%d count=%d conv=notrunc 2>/dev/null" % (name, bs, at // bs, length // bs))
        lines.append('[ "$(dd if=$DEV bs=%d skip=%d count=%d 2>/dev/null | md5sum | cut -d" " -f1)" = "$(md5sum %s | cut -d" " -f1)" ] '
                     '|| { echo "MISMATCH %s"; exit 1; }' % (bs, at // bs, length // bs, name, name))
        n += 1
        total += length
        at += length
lines.append('sync; echo "all %d chunks written and read back the same (%d bytes)"' % (n, total))
with open(os.path.join(out, "write.sh"), "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("%d runs of data, %d chunks, %d MB; image %d bytes" % (len(runs), n, total >> 20, size))
for start, end in runs[:12]:
    print("  %12d .. %12d  (%d KB)" % (start, end, (end - start) >> 10))
if len(runs) > 12:
    print("  ... %d more; last %d .. %d" % (len(runs) - 12, runs[-1][0], runs[-1][1]))
