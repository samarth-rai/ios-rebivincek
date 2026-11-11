#!/usr/bin/env python3
"""Apple Device Tree for the Redmi 5 (rosy, SDM450, 3 GB) that the SWEET xnu boots with.

The same tree as mkadt_vince.py (the two phones share the MSM8953 platform: GICv2 at 0x0b000000,
virtual timer PPI 4 = INTID 20, eight Cortex-A53 with MPIDR 0-3 and 0x100-0x103, 19.2 MHz timebase;
all of that is also what rosy's own device tree says), with rosy's memory and panel:

  RAM     the phone has System RAM 0x40000000-0x849fffff (plus 0x86800000-0x86bfffff and
          0x8ee00000-0xffffffff). LK loads the loader at 0x40080000 and is expected to put the device
          tree and the payload ramdisk near 0x43400000 / 0x43600000 (DDR base + the vince offsets).
          xnu gets 0x50000000-0x7fffffff (768 MiB): above the payload, and below 0x80000000 because
          the early framebuffer mapping of start.s takes L1 slot 2 (0x80000000-0xbfffffff).
  panel   720 x 1440 (ft8613_ebbg_5p7_720p_video), splash buffer at 0x90001000 like vince; assumed
          packed 24 bpp with stride 2160 until the loader test confirms it.

  mkadt_rosy.py OUT.adt [--dump]
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import adt  # noqa: E402
import mkadt_vince as base  # noqa: E402
from mkadt import cstr, node, u32  # noqa: E402

PHYS_BASE = 0x50000000
MEM_SIZE = 0x30000000	# 768 MiB: 0x50000000 - 0x7fffffff
# Kernel RAM layouts (vince_pack.py --ram). Both end at 0x80000000, which the vince patches assume.
#   low   0x50000000 + 768 MiB. Patch 0987 carves the GPU memory out only above 768 MiB, so nothing is
#         carved: all 768 MiB are the kernel's, the GPU (adreno=, gpu=) must stay off, vinceram3g does nothing.
#   wide  0x48000000 + 896 MiB, the vince arrangement: 0987 keeps the top 196 MiB (0x73c00000-0x7fffffff) for
#         the GPU, which leaves 700 MiB, and boot-arg vinceram3g=1 (0995) adds 0xa0000000-0xffffffff (1536 MiB).
#         The payload must then fit in 0x43600000-0x48000000 (74 MiB; the packer checks).
LAYOUTS = {'low': (0x50000000, 0x30000000), 'wide': (0x48000000, 0x38000000)}

# Addresses iBoot puts into /chosen from the phone's syscfg (the real D22AP tree has the properties
# mac-address-wifi0, mac-address-bluetooth0 and mac-address-ethernet0, 6 bytes each). Without a Wi-Fi
# address MobileGestalt looks for a wlan device (IONameMatch sdio, wlan, marconi-wifi), finds none and
# waits for a network controller for ever; on rosy that stopped lsd while it registered the built-in
# apps (MobileInstallation asks MobileGestalt), and SpringBoard never got past the Apple logo.
# Synthetic, locally administered addresses: not the phone's.
MAC_WIFI = bytes.fromhex('02524f535901')        # 02:52:4f:53:59:01
MAC_BLUETOOTH = bytes.fromhex('02524f535902')
MAC_ETHERNET = bytes.fromhex('02524f535903')
# vince_pack.py --wlan-node: also a /arm-io/wlan and /arm-io/bluetooth node with local-mac-address, laid
# out like the D22AP ones (6 bytes of address, 6 of zeroes). Off by default: launchd starts wifid when the
# tree has a wlan node (tools/mkadt.py), and there is no Wi-Fi driver here.
WLAN_NODE = False


def set_wlan_node(on):
    global WLAN_NODE
    WLAN_NODE = bool(on)


CANVASES = {'x': (1125, 2436, 3), 'x2': (750, 1624, 2), 'native': (720, 1440, 2)}


def set_canvas(name):
    """What iOS draws (/arm-io/disp0 canvas size, /chosen display-scale); the kernel scales it to the panel.
    'x'      the iPhone X layout, 1125x2436 at 3x (375x812 points): 2.74 M pixels
    'x2'     the same 375x812 points at 2x, 750x1624: 1.22 M pixels. Needs boot-arg sweetfb_logical=750x1624
    'native' the panel, 720x1440 at 2x (360x720 points): fastest, but narrower than any layout iOS 16 has
             (most of the UI does not fit). Needs boot-arg sweetfb_native=1"""
    global CANVAS_W, CANVAS_H, DISPLAY_SCALE
    CANVAS_W, CANVAS_H, DISPLAY_SCALE = CANVASES[name]


def set_layout(name):
    global PHYS_BASE, MEM_SIZE
    PHYS_BASE, MEM_SIZE = LAYOUTS[name]
FB_W, FB_H, FB_STRIDE = 720, 1440, 2160
FB, FB_SIZE = 0x90001000, 0x2f8000	# 720 * 1440 * 3 bytes, rounded up to 4 KiB
SWEET_ECID = 0x524f535900000001	# 'ROSY' + 1, synthetic
# What iOS draws: the iPhone X layout (SWEET_FB_LOGICAL_W/H of SweetFramebuffer.cpp), scaled to the panel
CANVAS_W, CANVAS_H = 1125, 2436
DISPLAY_SCALE = 3	# /chosen display-scale (MobileGestalt's screen scale), see set_canvas()
# Where LK is expected to put the ramdisk (DDR base 0x40000000 + 0x3600000); the packer checks that
# the payload ends below PHYS_BASE.
LK_RAMDISK_PA = 0x43600000

_OVERRIDES = ('PHYS_BASE', 'MEM_SIZE', 'FB', 'FB_SIZE', 'FB_W', 'FB_H', 'FB_STRIDE', 'SWEET_ECID')


def build(seed=None):
    saved = {k: getattr(base, k) for k in _OVERRIDES}
    try:
        for k in _OVERRIDES:
            setattr(base, k, globals()[k])
        root = base.build(seed)
    finally:
        for k, v in saved.items():
            setattr(base, k, v)
    comp = (cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,rosy') + cstr('xiaomi,sweet') +
            cstr('qcom,sdm450') + cstr('qcom,msm8953') + cstr('AppleARM'))
    root.props = [(k, f, comp if k == 'compatible' else v) for k, f, v in root.props]
    for c in root.children:
        if c.name == 'arm-io' and not any(k.name == 'disp0' for k in c.children):
            # Main display. IOMobileFramebuffer.framework counts the /arm-io children with this device_type
            # (none: CoreAnimation finds no main display and SpringBoard stops), reads the canvas size from
            # the node, and the kernel's AppleCLCD (SweetFramebuffer.cpp, patch 0968 for this panel) matches
            # the compatible string. The canvas is the iPhone X layout; the kernel scales it to the panel.
            # mkadt_vince.py has no such node (it predates 0968); tools/mkadt.py is where this comes from.
            c.children.append(node('disp0', [
                ('AAPL,phandle', u32(900)),
                ('device_type', cstr('display-subsystem')),
                ('compatible', cstr('sweet,display')),
                ('canvas-width', u32(CANVAS_W)),
                ('canvas-height', u32(CANVAS_H)),
            ]))
        if c.name == 'arm-io' and WLAN_NODE and not any(k.name == 'wlan' for k in c.children):
            c.children.append(node('wlan', [
                ('AAPL,phandle', u32(901)),
                ('device_type', cstr('wlan')),
                ('compatible', cstr('rosy,wlan-stub')),
                ('local-mac-address', MAC_WIFI + bytes(6)),
            ]))
            c.children.append(node('bluetooth', [
                ('AAPL,phandle', u32(902)),
                ('device_type', cstr('bluetooth')),
                ('compatible', cstr('rosy,bluetooth-stub')),
                ('local-mac-address', MAC_BLUETOOTH + bytes(6)),
            ]))
        if c.name == 'chosen':
            c.props = [(k, f, cstr('rosy-loader') if k in ('firmware-version', 'system-firmware-version') else
                        u32(DISPLAY_SCALE) if k == 'display-scale' else v)
                       for k, f, v in c.props]
            have = {k for k, f, v in c.props}
            for k, v in (('mac-address-wifi0', MAC_WIFI), ('mac-address-bluetooth0', MAC_BLUETOOTH),
                         ('mac-address-ethernet0', MAC_ETHERNET)):
                if k not in have:
                    c.props.append((k, 0, v))
    return root


if __name__ == '__main__':
    blob = adt.serialize(build())
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
