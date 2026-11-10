#!/bin/bash
# Build the loader (vince/shim) and the stand-in kernel (vince/fakexnu) for one phone, on macOS or Linux.
#   bash vince/build-rosy.sh OUTDIR [rosy|vince]
# Same compiler and linker flags as shim/build.sh and fakexnu/build.sh. Needs clang, ld.lld, llvm-objcopy,
# llvm-readelf (Homebrew: brew install lld) and python3. Writes only into OUTDIR.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build-rosy.sh OUTDIR [rosy|vince]}
BOARD=${2:-rosy}
case "$BOARD" in
rosy) DEF=-DROSY ;;
vince) DEF= ;;
*) echo "unknown board $BOARD"; exit 1 ;;
esac
for d in /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin; do [ -d "$d" ] && PATH="$PATH:$d"; done
mkdir -p "$OUT/shim" "$OUT/fakexnu"
CFLAGS="--target=aarch64-none-elf -O2 -Wall -Wextra -ffreestanding -nostdlib -fno-pic -fno-pie \
 -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
 -fno-jump-tables $DEF"

# ---- loader ----
S="$HERE/shim"; O="$OUT/shim"
clang $CFLAGS -c "$S/start.S" -o "$O/start.o"
for f in loader console fdt; do clang $CFLAGS -c "$S/$f.c" -o "$O/$f.o"; done
ld.lld -T "$S/link.ld" -N --no-rosegment -o "$O/loader.elf" "$O"/start.o "$O"/loader.o "$O"/console.o "$O"/fdt.o
llvm-objcopy -O binary "$O/loader.elf" "$O/loader.Image"
end=$(llvm-readelf -s "$O/loader.elf" | awk '$8=="_end"{print "0x"$2}')
python3 -I - "$O/loader.Image" "$end" <<'PYEOF'
import struct, sys
path, end = sys.argv[1], int(sys.argv[2], 16)
b = open(path, 'rb').read()
assert len(b) <= end, 'image longer than _end'
b = b.ljust(end, b'\0')		# bss and stack: the header's image_size covers them
assert b[56:60] == b'ARM\x64', 'bad magic'
assert struct.unpack_from('<Q', b, 16)[0] == len(b), 'image_size != file size'
open(path, 'wb').write(b)
PYEOF
# position independence: the only absolute relocation allowed is the Image header's image_size
n=$(llvm-readelf -r "$O"/start.o "$O"/loader.o "$O"/console.o "$O"/fdt.o | grep -c 'R_AARCH64_ABS' || true)
[ "$n" = 1 ] || { echo "FAIL: $n absolute relocations in the loader (expected exactly 1: image_size)"; exit 1; }
if llvm-readelf -r "$O/loader.elf" | grep -qE 'R_AARCH64'; then echo "FAIL: runtime relocations left"; exit 1; fi

# ---- stand-in kernel ----
S="$HERE/fakexnu"; O="$OUT/fakexnu"
clang $CFLAGS -c "$S/fake_start.S" -o "$O/fstart.o"
clang $CFLAGS -c "$S/fake_main.c" -o "$O/fmain.o"
clang $CFLAGS -c "$S/console.c" -o "$O/fcon.o"
ld.lld -T "$S/fake.ld" -N --no-rosegment -o "$O/fake.elf" "$O"/fstart.o "$O"/fmain.o "$O"/fcon.o
llvm-objcopy -O binary --only-section=.text --only-section=.rodata --only-section=.data "$O/fake.elf" "$O/fake.bin"
n=$(llvm-readelf -r "$O"/fstart.o "$O"/fmain.o "$O"/fcon.o | grep -c 'R_AARCH64_ABS' || true)
[ "$n" = 0 ] || { echo "FAIL: $n absolute relocations in the stand-in kernel"; exit 1; }
python3 -I "$S/mkfake_macho.py" "$O/fake.bin" "$O/fakexnu.macho"

echo "board $BOARD"
ls -l "$O/fakexnu.macho" "$OUT/shim/loader.Image"
shasum -a 256 "$O/fakexnu.macho" "$OUT/shim/loader.Image"
