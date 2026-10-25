import os, re
p = os.path.expanduser("~/hackios/xnu-deps/build/mesa/mesa-turnip/src/freedreno/vulkan/tu_knl.cc")
s = open(p).read()
if "hw.cachelinesize" in s:
    print("already patched")
    raise SystemExit(0)
m = re.search(r"#if DETECT_ARCH_AARCH64 &&\s*\\\n\s*\(!defined\(_SC_LEVEL1_DCACHE_LINESIZE\) \|\| DETECT_OS_ANDROID\)", s)
assert m, "pattern"
new = """#if DETECT_ARCH_AARCH64 && defined(__APPLE__)
   /* Darwin on redwood: XNU leaves SCTLR_EL1.UCT clear, so CTR_EL0 is not readable from user space
    * (an mrs of it is an undefined instruction); the kernel has the line size. dc civac / dc cvac
    * are allowed (SCTLR_EL1.UCI is set). */
   uint32_t line = 0;
   size_t length = sizeof(line);
   if (sysctlbyname("hw.cachelinesize", &line, &length, NULL, 0) != 0 || line == 0)
      line = 64;
   return line;
#elif DETECT_ARCH_AARCH64 && \\
   (!defined(_SC_LEVEL1_DCACHE_LINESIZE) || DETECT_OS_ANDROID)"""
s = s[:m.start()] + new + s[m.end():]
first = s.index("#include")
s = s[:first] + "#ifdef __APPLE__\n#include <sys/sysctl.h>\n#endif\n" + s[first:]
open(p, "w").write(s)
print("patched")
