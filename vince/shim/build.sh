#!/bin/bash
# Build the vince XNU loader (Image). Needs clang, ld.lld, llvm-objcopy, llvm-readelf.
#   bash build.sh [outdir]
set -euo pipefail
cd "$(dirname "$0")"
OUT=${1:-out}
mkdir -p "$OUT"
CFLAGS="--target=aarch64-none-elf -O2 -Wall -Wextra -ffreestanding -nostdlib -fno-pic -fno-pie \
 -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
 -fno-jump-tables"
clang $CFLAGS -c start.S -o "$OUT/start.o"
for f in loader console fdt reset; do
	clang $CFLAGS -c $f.c -o "$OUT/$f.o"
done
ld.lld -T link.ld -N --no-rosegment -o "$OUT/loader.elf" "$OUT"/start.o "$OUT"/loader.o "$OUT"/console.o "$OUT"/fdt.o "$OUT"/reset.o
llvm-objcopy -O binary "$OUT/loader.elf" "$OUT/loader.Image"
end=$(llvm-readelf -s "$OUT/loader.elf" | awk '$8=="_end"{print "0x"$2}')
truncate -s "$((end))" "$OUT/loader.Image"
# position independence: the only absolute relocation allowed is the Image header's image_size
n=$(llvm-readelf -r "$OUT"/start.o "$OUT"/loader.o "$OUT"/console.o "$OUT"/fdt.o "$OUT"/reset.o | grep -c 'R_AARCH64_ABS' || true)
[ "$n" = 1 ] || { echo "FAIL: $n absolute relocations (expected exactly 1: image_size)"; exit 1; }
if llvm-readelf -r "$OUT/loader.elf" | grep -qE 'R_AARCH64'; then echo "FAIL: runtime relocations left"; exit 1; fi
python3 -I - "$OUT/loader.Image" <<'PYEOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[56:60] == b'ARM\x64', 'bad magic'
assert struct.unpack_from('<Q', b, 16)[0] == len(b), 'image_size != file size'
PYEOF
echo "built $OUT/loader.Image ($(stat -c %s "$OUT/loader.Image") bytes)"
