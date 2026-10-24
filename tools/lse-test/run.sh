#!/bin/bash
# Build and run the tests of the ARMv8.1 atomics emulation (osfmk/arm64/lse_emulate.c of the kernel
# tree) on this machine, under the address and undefined-behaviour sanitizers. The kernel's
# interfaces (saved state, copyin_atomic*, copy_cas_user*) are replaced by a fake user memory in
# test_lse.c; the instructions are encoded by llvm-mc from cases.in (gen.py).
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=$ROOT/xnu-src/xnu-8796.121.2                 # the kernel tree being worked in, or else the copies that are in git
[ -f "$K/osfmk/arm64/lse_emulate.c" ] || K=$ROOT/xnu/files
B=$ROOT/xnu-deps/build/lse-test
rm -rf "$B"
mkdir -p "$B/stub/arm" "$B/stub/kern" "$B/stub/mach/machine" "$B/stub/os" "$B/stub/sys"
# the kernel headers lse_emulate.c includes: test_lse.c defines what it needs of them
for h in arm/thread.h kern/debug.h kern/misc_protos.h kern/startup.h mach/machine/thread_status.h os/atomic_private.h; do
	: > "$B/stub/$h"
done
echo '#include <errno.h>' > "$B/stub/sys/errno.h"
python3 -I "$HERE/gen.py" "$HERE/cases.in" "$B/cases.h"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B/stub" -I"$B" -I"$K/osfmk/arm64" -o "$B/test" "$HERE/test_lse.c"
"$B/test"
