set -e
# willow: UEFI + loader + kernel + the RAM part of a flash boot; the root is the iOS volume in system
# usage: iosimg.sh SECONDS "EXTRA ARGS"      env BOOT_CPUS (default "bootcpu=3 cpus=8": start on Silver)
cd ~/hackios
source common/devices/willow.conf
SECS=${1:-300}
CMD="${2:--v} ${BOOT_CPUS:-bootcpu=3 cpus=8} ufs=2 root_apfs=ufs_system loader_return_after=$SECS epoch=$(date +%s)"
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
LOADER_BASE=$LOADER_BASE xnuloader/build.sh > /dev/null
xnuloader/mkpayload.py -o out/willow-ios-payload.bin loader.efi=xnuloader/loader.efi kernel=$K \
	ramdisk=userland/root.apfs resident=userland/flashboot.apfs --cmdline "$CMD"
UEFI_TARGET=$UEFI_TARGET FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE DTB=$DTB \
	BOOT_HEADER=$BOOT_HEADER TEXT_OFFSET=$TEXT_OFFSET \
	python3 mkxnuimg.py --mode RELEASE -p out/willow-ios-payload.bin -o out/willow-ios.img > /dev/null
# WILLOW_OUT: a directory to copy the image to as well (e.g. the Windows side, for fastboot)
if [ -n "$WILLOW_OUT" ]; then cp out/willow-ios.img "$WILLOW_OUT/"; fi
sha256sum out/willow-ios.img | cut -c1-16
echo "cmdline: $CMD"
