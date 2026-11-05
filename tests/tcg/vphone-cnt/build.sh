#!/bin/sh
# Builds cnttest.elf (bare-metal AArch64 for QEMU virt, 1 vCPU). clang + ld.lld.
set -e
cd "$(dirname "$0")"
CFLAGS="--target=aarch64-none-elf -march=armv8.5-a -mstrict-align
  -ffreestanding -fno-builtin -nostdlib -O2 -Wall $EXTRA_CFLAGS"
clang $CFLAGS -c start.S -o start.o
clang $CFLAGS -c main.c -o main.o
ld.lld -T link.ld start.o main.o -o cnttest.elf
rm -f start.o main.o
echo "built cnttest.elf"
