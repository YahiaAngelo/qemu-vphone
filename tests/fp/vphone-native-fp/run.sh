#!/bin/sh
# Runs fptest.elf twice on the same QEMU, native FP on and off, and compares.
#   QEMU=path (default ../../../build/qemu-system-aarch64)  ELF=path  CPU=max
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
ELF=${ELF:-$here/fptest.elf}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_NATIVE_FP=$1 timeout "${TIMEOUT:-1800}" "$QEMU" -M virt -accel tcg \
        -cpu "${CPU:-max}" -m 256M -nographic -no-reboot -kernel "$ELF" \
        | tr -d '\r' > "$OUTDIR/native$1.txt"
}
run 1 &
run 0 &
wait
if ! grep -q '^TOTAL' "$OUTDIR/native1.txt" || ! grep -q '^TOTAL' "$OUTDIR/native0.txt"; then
    echo "a run did not finish: $OUTDIR"; exit 2
fi
if cmp -s "$OUTDIR/native1.txt" "$OUTDIR/native0.txt"; then
    echo "MATCH ($(grep -c . "$OUTDIR/native1.txt") lines) $(grep TOTAL "$OUTDIR/native1.txt")"
else
    echo "MISMATCH: $(diff "$OUTDIR/native0.txt" "$OUTDIR/native1.txt" | grep -c '^>') forms differ"
    diff "$OUTDIR/native0.txt" "$OUTDIR/native1.txt" | grep '^>' | head -40
    exit 1
fi
