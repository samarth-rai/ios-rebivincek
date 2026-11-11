#!/usr/bin/env python3
"""Turn the xnu kernel built by GitHub Actions into a boot image for `fastboot boot` on the vince.

  python3 vince/tools/vince_pack.py KERNEL OUT.img [--cmdline "..."] [--ramdisk ROOT.img]

KERNEL is the Mach-O from the `xnu-kernel` artifact (kernel.development.sweet). The script builds the
Apple Device Tree, packs kernel + ADT + boot-args with tools/mkpayload.py, and wraps the loader
(vince/prebuilt/loader.Image) plus the phone's own device tree and the payload into an Android
boot image (header v0, page 2048) shaped like the stock one. LK hands the ramdisk to the loader
whole; the loader copies the kernel into RAM and starts it. Nothing is flashed: use

  fastboot boot OUT.img
"""
import argparse, gzip, hashlib, os, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))	# the fork's tools/ (adt.py, mkadt.py, mkpayload.py)
sys.path.insert(0, os.path.join(HERE, '..', 'tools-fork'))
import mkadt_vince  # noqa: E402
import mkpayload  # noqa: E402
import adt  # noqa: E402

PAGE = 2048
KERNEL_ADDR = 0x80008000
RAMDISK_ADDR = 0x81000000
SECOND_ADDR = 0x80f00000
TAGS_ADDR = 0x80000100
OS_VERSION = 0x2000019B  # 16.0.0, patch 2025-11 (as the stock image)
CMDLINE = (b"androidboot.hardware=qcom msm_rtb.filter=0x237 ehci-hcd.park=3 "
           b"lpm_levels.sleep_disabled=1 androidboot.bootdevice=7824900.sdhci loop.max_part=7 "
           b"androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on console=ttyMSM0,115200,n8 "
           b"androidboot.console=ttyMSM0 earlycon=msm_serial_dm,0x78af000 ")
# Where LK puts the ramdisk in the measured runs (M1/M2) and where xnu's RAM starts: the payload must fit between
LK_RAMDISK_PA = 0x13600000
# --board: Apple Device Tree module, boot image name, ramdisk address (None = that module's LK_RAMDISK_PA)
BOARDS = {'vince': ('mkadt_vince', b'vince-m3b', LK_RAMDISK_PA), 'rosy': ('mkadt_rosy', b'rosy-m3b', None)}
DEFAULT_CMDLINE = 'debug=0xc serial=3 cpus=1 keepsyms=1'


def pad(b):
    return b + b"\0" * (-len(b) % PAGE)


def make_boot_img(loader_image, dtb, ramdisk, name=b"vince-m3b"):
    kz = gzip.compress(loader_image, 9, mtime=0) + dtb
    h = b"ANDROID!"
    h += struct.pack("<10I", len(kz), KERNEL_ADDR, len(ramdisk), RAMDISK_ADDR, 0, SECOND_ADDR, TAGS_ADDR, PAGE, 0, OS_VERSION)
    h += name.ljust(16, b"\0")
    h += CMDLINE.ljust(512, b"\0")
    sha = hashlib.sha1()
    for blob in (kz, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    h += sha.digest().ljust(32, b"\0")
    h += b"\0" * 1024
    assert len(h) == 1632, len(h)
    return pad(h) + pad(kz) + pad(ramdisk)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('kernel')
    ap.add_argument('out')
    ap.add_argument('--cmdline', default=DEFAULT_CMDLINE)
    ap.add_argument('--ramdisk', help='root image for md0 (boot with rd=md0), optional')
    ap.add_argument('--loader', default=os.path.join(HERE, '..', 'prebuilt', 'loader.Image'))
    ap.add_argument('--dtb', default=os.path.join(HERE, '..', 'prebuilt', 'stock-appended.dtb'))
    ap.add_argument('--board', default='vince', choices=sorted(BOARDS),
                    help='phone: selects the Apple Device Tree (RAM, panel). rosy has no prebuilt loader or '
                         'device tree in the repository: pass --loader and --dtb')
    ap.add_argument('--ram', default='low', choices=['low', 'wide'],
                    help='rosy only: kernel RAM layout. low = 0x50000000 + 768 MiB (no GPU memory carved out); '
                         'wide = 0x48000000 + 896 MiB, of which the kernel keeps 700 MiB, for use with '
                         'boot-arg vinceram3g=1 (adds 1536 MiB) and for the GPU')
    ap.add_argument('--wlan-node', action='store_true',
                    help='rosy only: also put /arm-io/wlan and /arm-io/bluetooth nodes with a MAC address in the '
                         'Apple Device Tree (the addresses are always in /chosen). launchd then starts wifid')
    ap.add_argument('--native', action='store_true',
                    help='rosy only: iOS draws at the panel size (720x1440 at 2x) instead of the iPhone X layout '
                         'scaled down. Boot with sweetfb_native=1 (appended to the command line here if missing)')
    a = ap.parse_args()
    modname, img_name, lk_ramdisk = BOARDS[a.board]
    board = __import__(modname)
    if a.ram != 'low':
        if not hasattr(board, 'set_layout'):
            sys.exit(f'--ram {a.ram} is not available for --board {a.board}')
        board.set_layout(a.ram)
    if a.wlan_node:
        if not hasattr(board, 'set_wlan_node'):
            sys.exit(f'--wlan-node is not available for --board {a.board}')
        board.set_wlan_node(True)
    if a.native:
        if not hasattr(board, 'set_canvas'):
            sys.exit(f'--native is not available for --board {a.board}')
        board.set_canvas('native')
        if 'sweetfb_native=' not in a.cmdline:
            a.cmdline += ' sweetfb_native=1'
    if lk_ramdisk is None:
        lk_ramdisk = board.LK_RAMDISK_PA
    if a.board != 'vince' and (a.loader == ap.get_default('loader') or a.dtb == ap.get_default('dtb')):
        sys.exit(f'--board {a.board} needs --loader and --dtb (the prebuilt ones are for vince)')

    loader = open(a.loader, 'rb').read()
    assert loader[56:60] == b'ARM\x64', 'loader is not an arm64 Image'
    dtb = open(a.dtb, 'rb').read()
    assert dtb[:4] == b'\xd0\x0d\xfe\xed', 'dtb is not a flattened device tree'

    with tempfile.TemporaryDirectory() as td:
        adt_path = os.path.join(td, 'vince.adt')
        open(adt_path, 'wb').write(adt.serialize(board.build()))
        payload, info = mkpayload.build(a.kernel, adt_path, a.cmdline, a.ramdisk)

    phys_base, mem_size = board.PHYS_BASE, board.MEM_SIZE
    limit = phys_base - lk_ramdisk
    if len(payload) > limit:
        sys.exit(f'payload is {len(payload):#x} bytes, but only {limit:#x} fit between the ramdisk address '
                 f'{lk_ramdisk:#x} and the kernel RAM at {phys_base:#x}')
    open(a.out, 'wb').write(make_boot_img(loader, dtb, payload, img_name))
    print(f"payload   {len(payload):#x} bytes at ramdisk {lk_ramdisk:#x}..{lk_ramdisk + len(payload):#x}")
    print(f"kernel RAM {phys_base:#x}..{phys_base + mem_size:#x}  virt_base {info['virt_base']}  entry {info['entry']}")
    print(f"cmdline   {a.cmdline}")
    print(f"wrote     {a.out} ({os.path.getsize(a.out)} bytes), sha256 {hashlib.sha256(open(a.out, 'rb').read()).hexdigest()}")
    print("next:     fastboot boot " + a.out)


if __name__ == '__main__':
    main()
