#!/bin/sh
# Runs zvatest.elf on the same QEMU (neoverse-v1) with VPHONE_INLINE_ZVA=1 (no EL2: EL1 DC ZVA
# skips the access check), 0 (QEMU's helpers) and 1 with virtualization=on (EL2 present, the
# check stays); all must pass and print the same lines.
#   QEMU=path (default ../../../build/qemu-system-aarch64)
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_INLINE_ZVA=$1 timeout "${TIMEOUT:-300}" "$QEMU" -M virt$3 -accel tcg -cpu neoverse-v1 \
        -m 256M -nographic -no-reboot -kernel "$here/zvatest.elf" | tr -d '\r' > "$OUTDIR/zva$2.txt"
}
# 1: inline, no EL2 (EL1 skips the access check); 0: helpers; e: inline with EL2 present
run 1 1 & run 0 0 & run 1 e ,virtualization=on &
wait
cat "$OUTDIR/zva1.txt"
for m in 1 0 e; do
    grep -q '^TOTAL ok' "$OUTDIR/zva$m.txt" || { echo "FAILED (VPHONE_INLINE_ZVA=$m): $OUTDIR"; cat "$OUTDIR/zva$m.txt"; exit 1; }
done
for m in 0 e; do
    cmp -s "$OUTDIR/zva1.txt" "$OUTDIR/zva$m.txt" || { echo "DIFFER (1, $m): $OUTDIR"; exit 1; }
done
echo "MATCH"
