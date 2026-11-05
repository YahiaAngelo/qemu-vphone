#!/bin/sh
# Runs cnttest.elf with VPHONE_FAST_CNT=1 and 0 on the same QEMU; both must pass and print the
# same lines.   QEMU=path (default ../../../build/qemu-system-aarch64)
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_FAST_CNT=$1 timeout "${TIMEOUT:-300}" "$QEMU" -M virt -accel tcg -cpu neoverse-v1 \
        -m 256M -nographic -no-reboot -kernel "$here/cnttest.elf" | tr -d '\r' > "$OUTDIR/cnt$1.txt"
}
run 1 & run 0 &
wait
cat "$OUTDIR/cnt1.txt"
for m in 1 0; do
    grep -q '^TOTAL ok' "$OUTDIR/cnt$m.txt" || { echo "FAILED (VPHONE_FAST_CNT=$m): $OUTDIR"; exit 1; }
done
cmp -s "$OUTDIR/cnt1.txt" "$OUTDIR/cnt0.txt" || { echo "DIFFER: $OUTDIR"; exit 1; }
echo "MATCH"
