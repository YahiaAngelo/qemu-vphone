#!/bin/sh
# Builds fencetest.elf (bare-metal AArch64 for QEMU virt, 2 vCPUs). clang + ld.lld.
set -e
cd "$(dirname "$0")"
CFLAGS="--target=aarch64-none-elf -march=armv8.5-a+rcpc -mstrict-align
  -ffreestanding -fno-builtin -nostdlib -O2 -Wall $EXTRA_CFLAGS"
clang $CFLAGS -c start.S -o start.o
clang $CFLAGS -c main.c -o main.o
ld.lld -T link.ld start.o main.o -o fencetest.elf
rm -f start.o main.o
echo "built fencetest.elf"
