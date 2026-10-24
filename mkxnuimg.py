#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from prebuilt/uefi/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

Another device (common/devices/<name>.conf, read by common/build.sh) sets these in the environment:
  UEFI_TARGET   Mu-Silicium device name (default redwood)    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE                              addresses from its UEFI port's memory map
  BOOT_HEADER   2: pack like a stock Android 10/11 boot image instead -- header v2, page 4096,
                base 0, the DTB in its own section (DTB is then the stock boot image's dtb section)
                and not appended to the kernel; TEXT_OFFSET (e.g. 0x80000) gives the shim a stock
                Linux Image header. willow's ABL hangs on the default v1 image before it runs a
                single instruction of ours.
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'prebuilt', 'uefi')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
FD_BASE, FD_SIZE = env_int('FD_BASE', 0x9FD00000), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000
BOOT_HEADER, TEXT_OFFSET = env_int('BOOT_HEADER', 1), env_int('TEXT_OFFSET', 0)

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

payload = b''
if args.payload:
    payload = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert payload[:4] == b'XNUP' and len(payload) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    cflags = f'--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'
    if TEXT_OFFSET:
        # the Image's memory footprint: shim, FD and payload, in 2 MiB units
        image_size = (0x1000 + FD_SIZE + len(payload) + 0x1FFFFF) & ~0x1FFFFF
        cflags += f' -DTEXT_OFFSET={TEXT_OFFSET:#x} -DIMAGE_SIZE={image_size:#x}'
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS={cflags}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    assert not TEXT_OFFSET, 'TEXT_OFFSET needs a Mu-Silicium tree to build the shim'
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'redwood.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if payload:
    blob += payload
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(payload)} bytes, bytesum={sum(payload) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
if BOOT_HEADER == 2:
    open(kernel, 'wb').write(gzip.compress(blob, 6))
    layout = ['--header_version', '2', '--pagesize', '4096', '--base', '0x0', '--kernel_offset', '0x8000',
              '--ramdisk_offset', '0x1000000', '--tags_offset', '0x100', '--dtb_offset', '0x1f00000',
              '--dtb', parts['dtb']]
else:
    open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
    layout = ['--header_version', '1']
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           *layout, '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
