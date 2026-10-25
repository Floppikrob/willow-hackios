/*
 * Apple's HFS (hfs-627.100.6, the core/ of github.com/apple-oss-distributions/hfs) built into this
 * port's kernel, for a filesystem iOS's data can live in on the flash: on willow, userdata, written
 * through the loader (bsd/dev/redwood_ufs.c, ufs_write=data). On a phone HFS.kext is an extension
 * that IOKit starts; this kernel loads none, so what the kext's start() does -- core/hfs_iokit.cpp --
 * is done here once at boot (redwood_hfs_init(), from bsd_init), and the kext's few questions to
 * IOKit get plain answers. Configuration: kext-config.h beside this file.
 */
#include <mach/mach_types.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "hfs_iokit.h"
#include "hfs.h"
#include "hfs_dbg.h"
#include "hfs_cnode.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

#if DEBUG
bool hfs_corruption_panics = true;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern struct vfsops hfs_vfsops;

static vfstable_t hfs_vfs_handle;

/* What com_apple_filesystems_hfs::start() does: HFS into the table of filesystems. */
void
redwood_hfs_init(void)
{
	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};
#ifndef VFS_TBLVNOP_SECLUDE_RENAME
#define VFS_TBLVNOP_SECLUDE_RENAME 0
#endif
	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED
		    | VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2
		    | VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT | VFS_TBLVNOP_SECLUDE_RENAME
		    | VFS_TBLNATIVEXATTR)
	};
	int ret;

	if (hfs_vfs_handle != NULL) {
		return;
	}
#ifndef panic_on_assert
	panic_on_assert = PE_i_can_has_kernel_configuration() & kPEICanHasAssertions;
#endif
	ret = vfs_fsadd(&vfe, &hfs_vfs_handle);
	if (ret != 0) {
		printf("hfs: vfs_fsadd failed: %d\n", ret);
		hfs_vfs_handle = NULL;
		return;
	}
	hfs_init_zones();
	/*
	 * (Not hfs_sysctl_register(): built into the kernel, HFS's SYSCTL_ nodes are the kernel's own,
	 * registered at boot from their linker set, and registering them again panics: "Use
	 * sysctl_register_oid_early to register permanent nodes", willow-gl36.)
	 */
	printf("hfs: HFS (hfs-627.100.6) is in the table of filesystems\n");
}

/* Nothing of this port's storage is ejectable. */
int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

/* Media are not IOKit objects here: there is nothing to iterate. */
void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg), __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(__unused char *serial_number_str, __unused uint32_t len)
{
	return KERN_FAILURE;
}
