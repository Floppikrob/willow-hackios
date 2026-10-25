# HackiOS on the Redmi Note 8T (willow)

iOS 16.5 (XNU + the iPhone10,3 user space) on the Xiaomi Redmi Note 8T — `willow`, Snapdragon 665
(SM6125 "trinket"), Adreno 610 — ported from [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel).

> Branches: **`main`** — kernel patches, XNU loader,
> UEFI, tools and [`willow/`](willow/) (probe scripts); **`userland`** — init, test programs, job lists,
> Mesa/Metal glue, the turnip patches. Upstream's own README: [docs/hackios-upstream-README.md](docs/hackios-upstream-README.md).

## What works and what does not

| Part | Status | Notes |
|---|---|---|
| Boot (UEFI → XNU loader → XNU) | ✅ Works | Mu-Silicium UEFI with the HackiOS patch, booted from RAM by `fastboot boot` |
| All 8 CPU cores | ✅ Works | two clusters (MPIDR per core), PSCI |
| iOS 16.5 to the home screen | ✅ Works | root on the `system` partition, launchd, SpringBoard |
| CPU without LSE atomics | ✅ Works | the Kryo 260 lacks ARMv8.1 atomics; the kernel emulates them |
| Storage (eMMC) | ✅ Works | through the loader; GPT partitions as block devices |
| Writable `/private/var` | ✅ Works | Apple HFS+ in the kernel, on `userdata` (`ufs_write=data var_disk=1`) |
| Display (1080×2340) | ✅ Works | NT36672A video mode, the UEFI framebuffer |
| Touchscreen | ✅ Works | Novatek NT36xxx over SPI (`touchscreen=1`) |
| Power and volume keys | ✅ Works | PM6125 PON, gpio-keys |
| GPU (Adreno 610) | ✅ Works | power-up, zap shader, secure mode exit, kept on for the system (`gpu=1`) |
| UI on the GPU (OpenGL ES) | ✅ Works | Mesa freedreno draws CoreAnimation (`GL=1`), 4–26 fps; frame to panel by the GPU's 2D engine (`fb_gpu=1`) |
| Metal | ✅ Works | this port's Metal driver: all MTLDevice methods, compute kernels, a triangle checked against softpipe |
| Vulkan | ✅ Works (test) | Mesa turnip, Vulkan 1.0: fill/copy/compute checked word by word; no WSI yet |
| CPU frequency, Silver cluster | ✅ Works | 1.8 GHz through OSM (`osm=1`) |
| CPU frequency, Gold cluster | ⚠️ Partly | OSM is on, but the Gold cores run at 300 MHz |
| USB device mode | ✅ Works | DWC3 + QUSB2 from scratch; Windows enumerates "Redmi Note 8T (iOS)" (1209:0001) |
| USB networking (CDC-NCM) | ⚠️ Partly | `usb=9`: Windows binds UsbNcm, frames arrive, nothing goes out yet; no address on `en0` |
| SSH | ❌ No | needs USB networking or Wi-Fi first |
| Modem (MPSS) boot | ✅ Works | TrustZone PAS, SMEM, SMP2P, GLINK, QRTR (`mss_step=1…10`) |
| Modem's services from our side | ✅ Works | `mss_step=11`: service locator (pd-mapper), RMTFS (EFS in memory, never written back), TFTP over QRTR; the modem comes up with ~40 QMI services |
| Wi-Fi (WCN3990) | ❌ No | the modem loads `wlanmdsp.mbn`, the Wi-Fi domain starts and asserts at once (`platform_main.c:platform_main_task:230`) |
| Mobile network, calls, SMS | ❌ No | the modem runs, nothing on our side talks to it |
| Bluetooth | ❌ No | |
| Sound | ❌ No | |
| Cameras | ❌ No | |
| Battery / charging | ❌ No | no fuel gauge or charger driver |
| Sensors, NFC, vibration | ❌ No | |
| Booting without a PC | ❌ No | every run is `fastboot boot` from RAM; nothing is flashed but `system` (the iOS volume) |

Known bug: `backboardd` sometimes dies with SIGBUS (out of a 32 KB malloc buffer) with `glasync=1`
and `/var` on the disk.

## Wi-Fi: where it stands

What the kernel does in `bsd/dev/redwood_mss.c` (`mss_step=11`), and what the modem answers:

1. We announce three QRTR services: the locator (0x40), RMTFS (0x0e), TFTP (0x1000).
2. RMTFS: the modem opens `/boot/modem_fs1`, `modem_fs2`, `modem_fsg`, gets a 2 MB buffer that
   TrustZone has assigned to it (MP_ASSIGN), reads its EFS → it announces ~40 services.
3. Locator: `tms/servreg`, `kernel/elf_loader` → the Wi-Fi domain `msm/modem/wlan_pd` is started.
4. TFTP: `/readwrite/server_check.txt` written, `wlanmdsp.mbn` (3.7 MB) read from the `modem` partition.
5. servreg notifier: the Wi-Fi domain goes `UP` — and 1.6 ms later the modem reports
   `EF:wlan_process:0x1:qmi_platform:0xa6:cmnos_thread.c:3968:Asserted in platform_main.c:platform_main_task:230`.

Ruled out: the WCN3990's supplies (L8, L9, L16, L17, L23 through the RPM), TFTP delivery (`seek`,
`rsize`), a servreg listener. Open leads: the MSA region (`wlan_msa_region@53300000`),
`tms/pdr_enabled`, the GLINK channels the modem opens (DS, DATA*), and comparing with stock Android.

## Hardware

| | |
|---|---|
| SoC | SM6125, 4× Kryo 260 Gold + 4× Silver, no LSE, no FP16 |
| RAM | 4 GB (3.55 GiB visible) |
| Storage | eMMC |
| Panel | Novatek NT36672A (Tianma), video mode, 1080×2340 |
| GPU | Adreno 610, chip ID 0x6010000 |
| USB | DWC3 3.30a at 0x4e00000, high-speed only, QUSB2 PHY |
| Wi-Fi | WCN3990, firmware on the modem |

## Building and running

On Linux (WSL Ubuntu was used), with upstream's toolchain (`common/setup.sh`, theos with the
iPhoneOS 16.5 SDK) and your own iOS 16.5 files, as upstream describes:

```bash
WIFI_ATH11K=0 ./xnu-build.sh -j8                            # the kernel
FLASHBOOT=1 CRYPTEXES=flash REST=1 GL=1 userland/build.sh   # the small image (branch userland)
willow/scripts/iosimg.sh 240 "gpu=1 fb_gpu=1 osm=1 touchscreen=1 fbwidth=1080 fbheight=2340 fb_beat=0 sb_statusbar=1 ufs_write=data var_disk=1"
```

Then, with TWRP on the phone (Windows):

```powershell
powershell -ExecutionPolicy Bypass -File willow\scripts\trial.ps1 -Name try1
```

`trial.ps1` reboots to the bootloader, `fastboot boot`s `out/willow-ios.img`, waits for the phone to
come back in TWRP and reads the kernel's log from the `logdump` partition. See [willow/](willow/) for
the other scripts and the boot arguments.

## Boot arguments (device=willow)

| Argument | |
|---|---|
| `gpu=1`, `fb_gpu=1`, `glasync=1` | the GPU kept on; frames by its 2D engine; asynchronous GL flush |
| `osm=1`, `osm_dump=1` | Silver at full speed; read both clusters' OSM |
| `touchscreen=1` | the touchscreen |
| `ufs_write=data var_disk=1` | `/private/var` on HFS+ in `userdata` |
| `usb=N` | USB bring-up, steps 1…9 (9: CDC-NCM, `en0`) |
| `mss_step=N` | modem bring-up, steps 1…11 (`mss_listen`, `mss_serve`, `mss_wlan_power`, `mss_servreg`) |
| `net_trace=N` | log the first N frames of `en0` |

## Safety

The phone's `persist`, `modemst1/2`, `fsg` and `fsc` hold its IMEI, MAC addresses and calibration.
Keep backups of them private. Nothing from iOS, no firmware and no dumps of the phone are in this
repository.

## License

As upstream: see [LICENSE](LICENSE).
