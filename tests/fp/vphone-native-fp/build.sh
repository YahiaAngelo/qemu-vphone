#!/bin/sh
# Builds fptest.elf (bare-metal AArch64 for QEMU virt). clang + ld.lld (LLVM 21).
#   EXTRA_CFLAGS="-DITERS=200 -DDUMP"  for a dump of every result.
set -e
cd "$(dirname "$0")"
OUT=${OUT:-fptest.elf}
python3 tests.py tests.inc
CFLAGS="--target=aarch64-none-elf -march=armv8.5-a+fp16 -mstrict-align
  -ffreestanding -fno-builtin -nostdlib -O2 -Wall $EXTRA_CFLAGS"
clang $CFLAGS -c start.S -o start.o
clang $CFLAGS -c main.c -o main.o
ld.lld -T link.ld start.o main.o -o "$OUT"
rm -f start.o main.o
echo "built $OUT"
