# HackiOS on the Redmi Note 8T (willow, Snapdragon 665)

A port of [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) — XNU and the iOS 16.5 user space
running on Android phones — to the Xiaomi Redmi Note 8T (`willow`, SM6125 "trinket", Adreno 610).

This repository has two branches, mirroring the two upstream repositories:

| Branch     | Upstream                                                                        | What is in it                                      |
|------------|---------------------------------------------------------------------------------|----------------------------------------------------|
| `main`     | [hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel)           | the kernel patches, the XNU loader, UEFI, tools; `willow/` (this folder) |
| `userland` | [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland)       | init, test programs, job lists, Mesa/Metal glue, the turnip patches |

Upstream's README (one directory up) explains the project as a whole; this file is about the willow
port only. Nothing from iOS, no firmware and no dumps of the phone are in this repository: they come
from your own device and your own IPSW, as upstream describes.

## Status

Works on the phone:

- **Boot**: Mu-Silicium UEFI (the HackiOS patch, `fastboot boot` from RAM) → the XNU loader →
  XNU on all 8 cores (two clusters; MPIDR table per core) → iOS 16.5 (iPhone10,3 user space) from
  the `system` partition, up to the home screen.
- **CPU without LSE atomics**: the Kryo 260 has no ARMv8.1 atomics, which iOS's libraries use
  unconditionally; the kernel emulates them in its undefined-instruction handler. 4 KB granule.
- **Storage**: eMMC through the loader; Apple HFS+ built into the kernel, `/private/var` on the
  `userdata` partition (`ufs_write=data var_disk=1`).
- **Input**: Novatek touchscreen, power and volume keys.
- **GPU (Adreno 610)**: the kernel powers it up, loads the zap shader, leaves secure mode and keeps
  it on (`gpu=1`); Mesa's freedreno draws the iOS interface (OpenGL ES, `GL=1`), the frame goes to
  the panel through the GPU's 2D engine (`fb_gpu=1`). Apple's Metal runs through this port's Metal
  driver (all MTLDevice methods, compute kernels, a triangle checked against softpipe).
- **Vulkan**: Mesa's turnip on the A610 (`userland/src/vk_test.c`): device, memory, fill/copy with a
  fence, a compute shader — every word checked. Vulkan 1.0 (turnip's level for the A610).
- **CPU frequency**: Silver cluster at 1.8 GHz through OSM. The Gold cluster's OSM is on but its
  cores run at 300 MHz (read-only investigation, `osm_dump=1`).
- **USB (device mode)**: DWC3 + QUSB2 PHY from scratch (`usb=1…8`): Windows enumerates the phone
  as "Redmi Note 8T (iOS)", 1209:0001. Step 9 (`usb=9`) is a CDC-NCM network adapter behind `en0`:
  Windows binds its in-box UsbNcm driver; traffic is not working yet.
- **Modem (MPSS)**: loaded and started through TrustZone (PAS), SMEM/SMP2P/GLINK/QRTR up
  (`mss_step=1…11`). Step 11 serves the modem what Linux's user space would: the service
  locator (pd-mapper), RMTFS (its EFS, kept in memory — never written back) with the shared buffer
  assigned to the modem by TrustZone (MP_ASSIGN), and TFTP over QRTR (tqftpserv). With these the
  modem comes up fully (about 40 QMI services) and loads the Wi-Fi firmware (`wlanmdsp.mbn`).

Not working (yet):

- **Wi-Fi**: the Wi-Fi domain (`wlan_pd`) starts and asserts immediately:
  `EF:wlan_process:0x1:qmi_platform:0xa6:cmnos_thread.c:3968:Asserted in platform_main.c:platform_main_task:230`.
  Ruled out: the WCN3990's supplies (L8, L9, L16, L17, L23 through the RPM), TFTP delivery (seek/rsize),
  a servreg listener for the domain. Open leads: the MSA region (`wlan_msa_region@53300000`),
  `tms/pdr_enabled`, the modem's GLINK channels (DS, DATA*), and comparing with stock Android's boot.
- USB networking, Gold-cluster frequency, sound, cameras, battery charging control.
- An occasional `backboardd` SIGBUS with `glasync=1` and `/var` on the disk.

## Hardware facts

| | |
|---|---|
| SoC | SM6125 (trinket), 4× Kryo 260 Gold (`0x800`) + 4× Silver (`0x801`), no LSE, no FP16 |
| RAM | 4 GB (3.55 GiB visible) |
| Storage | eMMC (`sdhci@4744000`) |
| Panel | Novatek NT36672A (Tianma), video mode, 1080×2340 |
| Touch | Novatek on SPI |
| GPU | Adreno 610, chip ID 0x6010000 |
| USB | DWC3 3.30a (`0x5533330a`) at 0x4e00000, high-speed only, QUSB2 PHY at 0x1613000 |
| Wi-Fi | WCN3990, firmware on the modem (`wlanmdsp.mbn`), service wlfw (QMI 0x45) |

## Building

Linux (WSL Ubuntu here), with upstream's toolchain set up (`common/setup.sh`, theos with the iPhoneOS
16.5 SDK):

```bash
WIFI_ATH11K=0 ./xnu-build.sh -j8                        # the kernel
FLASHBOOT=1 CRYPTEXES=flash REST=1 GL=1 userland/build.sh   # the small image (branch userland)
willow/scripts/iosimg.sh 240 "BOOT-ARGS"               # UEFI + loader + kernel -> out/willow-ios.img
```

`./xnu/export.sh` writes the kernel tree back into `xnu/files` and `xnu/redwood.patch` before a commit.
Set `WILLOW_OUT` to a directory to have `iosimg.sh` copy the image there (e.g. the Windows side).

The best command for the desktop so far:

```bash
willow/scripts/iosimg.sh 240 "gpu=1 fb_gpu=1 osm=1 touchscreen=1 fbwidth=1080 fbheight=2340 fb_beat=0 sb_statusbar=1 ufs_write=data var_disk=1"
```

## Running a probe

Everything is booted from RAM with `fastboot boot`; nothing is flashed for a probe. The loader
returns to the bootloader after the given number of seconds, the phone comes back in TWRP, and the
kernel's log is read from the `logdump` partition:

```powershell
powershell -ExecutionPolicy Bypass -File willow\scripts\trial.ps1 -Name willow-try1
```

`trial.ps1` runs `adb reboot bootloader`, `fastboot boot out/willow-ios.img`, waits for TWRP and runs
`read-diag.ps1`. The text stops at the first zero byte; the whole log is
`head -c N out/NAME-logdump.bin | tr -d '\000'`, N from the last "log: N bytes written".

Other scripts in `willow/scripts/`: `speed.py` (boot timing marks), `fps-sum.py` and `metal-sum.py`
(summaries of GPU runs), `hfs-extents.py`, `winwatch.ps1` (what Windows sees on the USB cable during a
probe), `vk-build.sh` / `vk-image.sh` (the Vulkan test), `turnip-fix-ctr.py` (the turnip fix below).

## Boot arguments of this port (device=willow)

| Argument | |
|---|---|
| `gpu=1` | power up the Adreno 610 and keep it for the system (`a610_keep`), RPM buses on |
| `fb_gpu=1` | frames onto the panel by the GPU's 2D engine |
| `glasync=1` | asynchronous GL flush in the window server |
| `osm=1` | Silver cluster at full speed through OSM; `osm_dump=1` reads both clusters' OSM |
| `touchscreen=1` | the Novatek touchscreen |
| `ufs_write=data var_disk=1` | `/private/var` on HFS+ in `userdata` |
| `usb=N` | USB bring-up, steps 1…9 (9: CDC-NCM network adapter, `en0`) |
| `mss_step=N` | modem bring-up, steps 1…11; `mss_listen=S`, `mss_serve=S`, `mss_wlan_power=0`, `mss_servreg=0` |
| `net_trace=N` | log the first N frames of `en0` |

## Turnip on Darwin

`tools/mesa/turnip/` (branch `userland`) has the changes that make Mesa 25.0.7's turnip build for
and run on this kernel: `turnip-over-gl.patch` over the GL tree of `tools/mk-mesa.sh`, or
`turnip.patch` from the pristine 25.0.7 tarball. Among them: the device is the kernel's `RedwoodGPU`
service through `redwood-drm` (libdrm's stand-in, with syncobjs emulated in-process), and the L1 cache
line comes from `sysctlbyname("hw.cachelinesize")`, because XNU leaves `SCTLR_EL1.UCT` clear and
`mrs ctr_el0` traps in user space.

## Safety

The phone's `persist`, `modemst1/2`, `fsg`, `fsc` hold its IMEI, MAC addresses and calibration.
Keep your backups of them private; the modem driver here reads them and never writes them back.

## License

As upstream: see `LICENSE` in each branch.
