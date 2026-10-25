/*
 * This port's configuration of Apple's HFS (hfs-627.100.6, core/kext-config.h), built into the
 * kernel rather than as HFS.kext: iOS's own choices less what there is nothing here for --
 * content protection (no AppleKeyStore to wrap a key)
 * and quotas; compression as on iOS (decmpfs is in this kernel).
 */
#ifndef _hfs_config_
#define _hfs_config_

/* (the kernel's own options come as -D flags: these, for HFS's files alone, take their place) */
/*
 * A kext's copies are checked against the whole object, the kernel's own against the member
 * (osfmk/libsa/string.h, __xnu_bos_strict), and HFS copies a Finder info of 32 bytes across two
 * members of 16 on purpose (hfs_catalog.c, buildrecord): that panicked willow-gl37 at the first
 * mount. HFS's files without the checked builtins, as a kext is built.
 */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0
#undef HFS_COMPRESSION
#define HFS_COMPRESSION 1
#undef FIFO
#define FIFO 1
#undef QUOTA
#define QUOTA 0
#undef CONFIG_PROTECT
#define CONFIG_PROTECT 0
#undef CONFIG_SECLUDED_RENAME
#define CONFIG_SECLUDED_RENAME 0

#include <sys/types.h>
/* (declarations a kext has from Kernel.framework) */
#define ubc_create_upl ubc_create_upl_external    /* (a kext's ubc_create_upl is the _external one: config/BSDKernel.exports) */
__BEGIN_DECLS int sysctlbyname(const char *, void *, size_t *, void *, size_t); __END_DECLS
#endif /* defined(_hfs_config_) */
