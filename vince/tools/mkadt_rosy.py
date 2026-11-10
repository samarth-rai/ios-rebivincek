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
from mkadt import cstr  # noqa: E402

PHYS_BASE = 0x50000000
MEM_SIZE = 0x30000000	# 768 MiB: 0x50000000 - 0x7fffffff
FB_W, FB_H, FB_STRIDE = 720, 1440, 2160
FB, FB_SIZE = 0x90001000, 0x2f8000	# 720 * 1440 * 3 bytes, rounded up to 4 KiB
SWEET_ECID = 0x524f535900000001	# 'ROSY' + 1, synthetic
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
        if c.name == 'chosen':
            c.props = [(k, f, cstr('rosy-loader') if k in ('firmware-version', 'system-firmware-version') else v)
                       for k, f, v in c.props]
    return root


if __name__ == '__main__':
    blob = adt.serialize(build())
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
