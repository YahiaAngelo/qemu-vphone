#!/bin/sh
# Runs rastest.elf with the return-address stack on and off on the same QEMU
# (single vCPU, and the 'ras' and 'lookup' statistics logged with it on).
#   QEMU=path (default ../../../build/qemu-system-aarch64)
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_RAS=$1 VPHONE_RAS_STATS=$(($1 * 16)) timeout "${TIMEOUT:-600}" "$QEMU" -M virt \
        -accel tcg -cpu max -m 256M -nographic -no-reboot \
        -d guest_errors -D "$OUTDIR/log$1.txt" -kernel "$here/rastest.elf" \
        | tr -d '\r' > "$OUTDIR/ras$1.txt"
}
run 1 &
run 0 &
wait
cat "$OUTDIR/ras1.txt"
grep rasstat "$OUTDIR/log1.txt" | tail -1
if ! grep -q '^TOTAL ok' "$OUTDIR/ras1.txt" || ! grep -q '^TOTAL ok' "$OUTDIR/ras0.txt"; then
    echo "FAILED: $OUTDIR"; exit 1
fi
cmp -s "$OUTDIR/ras1.txt" "$OUTDIR/ras0.txt" || { echo "DIFFER: $OUTDIR"; exit 1; }
echo "MATCH"
