# willow/: probe scripts

What works and what does not, how to build: the [README at the top](../README.md).

| Script | |
|---|---|
| `scripts/iosimg.sh SECONDS "BOOT-ARGS"` | UEFI + loader + kernel + the small image → `out/willow-ios.img` (set `WILLOW_OUT` to copy it elsewhere) |
| `scripts/trial.ps1 -Name NAME` | one RAM-only probe: `fastboot boot`, wait for TWRP, read the log (`read-diag.ps1`) |
| `read-diag.ps1 -Blocks 16384 -Name NAME` | the kernel's log from the `logdump` partition, through TWRP's adb |
| `scripts/winwatch.ps1` | what Windows sees on the USB cable during a probe |
| `scripts/speed.py`, `fps-sum.py`, `metal-sum.py` | boot timing marks; summaries of GPU runs |
| `scripts/hfs-extents.py` | the extents of a file on the HFS+ `userdata` |
| `scripts/vk-build.sh`, `vk-image.sh` | the Vulkan test (`userland/src/vk_test.c`) and an image for it |
| `scripts/turnip-fix-ctr.py` | the turnip change for `ctr_el0` (also in `tools/mesa/turnip/`, branch `userland`) |

The whole log of a probe: `head -c N out/NAME-logdump.bin | tr -d '\000'`, N from the last
"log: N bytes written" line; the text `read-diag.ps1` saves stops at the first zero byte.
