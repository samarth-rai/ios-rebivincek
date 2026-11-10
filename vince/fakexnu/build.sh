#!/bin/bash
# Build the stand-in kernel (flat blob + Mach-O). Needs clang, ld.lld, llvm-objcopy, llvm-readelf, python3.
set -euo pipefail
cd "$(dirname "$0")"
OUT=${1:-out}
mkdir -p "$OUT"
CFLAGS="--target=aarch64-none-elf -O2 -Wall -Wextra -ffreestanding -nostdlib -fno-pic -fno-pie \
 -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fno-jump-tables"
clang $CFLAGS -c fake_start.S -o "$OUT/fstart.o"
clang $CFLAGS -c fake_main.c -o "$OUT/fmain.o"
clang $CFLAGS -c console.c -o "$OUT/fcon.o"
clang $CFLAGS -c ../shim/reset.c -o "$OUT/freset.o"
ld.lld -T fake.ld -N --no-rosegment -o "$OUT/fake.elf" "$OUT"/fstart.o "$OUT"/fmain.o "$OUT"/fcon.o "$OUT"/freset.o
llvm-objcopy -O binary --only-section=.text --only-section=.rodata --only-section=.data "$OUT/fake.elf" "$OUT/fake.bin"
n=$(llvm-readelf -r "$OUT"/fstart.o "$OUT"/fmain.o "$OUT"/fcon.o "$OUT"/freset.o | grep -c 'R_AARCH64_ABS' || true)
[ "$n" = 0 ] || { echo "FAIL: $n absolute relocations"; exit 1; }
python3 -I mkfake_macho.py "$OUT/fake.bin" "$OUT/fakexnu.macho"
echo "built $OUT/fakexnu.macho ($(stat -c %s "$OUT/fakexnu.macho") bytes)"
