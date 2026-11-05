#!/bin/sh
# Runs fencetest.elf (2 vCPUs, neoverse-v1: LSE, RCPC) with VPHONE_FENCE=1 and 2, which must
# pass (no forbidden store-buffering outcome, and the plain pair reordered so the test can see
# one), and with VPHONE_FENCE=0 (QEMU's mapping), whose result is only reported: on an x86
# host it lets STLR;LDAR reorder.
#   QEMU=path (default ../../../build/qemu-system-aarch64)
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_FENCE=$1 timeout "${TIMEOUT:-600}" "$QEMU" -M virt -accel tcg -cpu neoverse-v1 \
        -smp 2 -m 256M -nographic -no-reboot -kernel "$here/fencetest.elf" \
        | tr -d '\r' > "$OUTDIR/fence$1.txt"
}
run 0 & run 1 & run 2 &
wait
rc=0
for m in 0 1 2; do
    echo "== VPHONE_FENCE=$m"
    cat "$OUTDIR/fence$m.txt"
done
for m in 1 2; do
    grep -q '^TOTAL ok' "$OUTDIR/fence$m.txt" || { echo "FAILED (VPHONE_FENCE=$m): $OUTDIR"; rc=1; }
done
[ $rc = 0 ] && echo "PASS (modes 1 and 2)"
exit $rc
