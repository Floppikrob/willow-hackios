"""cases.in -> cases.h: every @instruction@ replaced by its encoding from llvm-mc."""
import re, subprocess, sys

src = open(sys.argv[1]).read()


def encode(asm):
    r = subprocess.run(["llvm-mc", "-triple=aarch64", "-mattr=+lse,+rcpc,+fullfp16", "--show-encoding"],
                       input=asm + "\n", capture_output=True, text=True)
    if r.returncode:
        sys.exit("llvm-mc: %s\n%s" % (asm, r.stderr))
    out = r.stdout
    b = [int(x, 16) for x in re.search(r"encoding: \[([^\]]*)\]", out).group(1).split(",")]
    return "0x%08x" % (b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24)


open(sys.argv[2], "w").write(re.sub(r"@([^@\n]+)@", lambda m: encode(m.group(1)), src))
