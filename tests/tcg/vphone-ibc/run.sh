#!/bin/sh
# Runs ibctest.elf on the same QEMU (single vCPU, neoverse-v1: PAC, no BTI) with
#   A: the indirect-branch cache and the cross-page refill on (statistics logged),
#   B: both off, C: the cache on and the refill off, D: no checked cross-page links
#   and no cache (QEMU's own ways),
# and compares the outputs.
#   QEMU=path (default ../../../build/qemu-system-aarch64)
set -e
here=$(cd "$(dirname "$0")" && pwd)
QEMU=${QEMU:-$here/../../../build/qemu-system-aarch64}
OUTDIR=${OUTDIR:-$(mktemp -d)}
run() {
    VPHONE_XPAGE_LINKS=$2 VPHONE_IBC=$3 VPHONE_XPAGE_REFILL=$4 VPHONE_IBC_STATS=$5 \
    VPHONE_LOOKUP_STATS=$5 \
        timeout "${TIMEOUT:-600}" "$QEMU" -M virt \
        -accel tcg -cpu neoverse-v1 -m 256M -nographic -no-reboot \
        -d guest_errors -D "$OUTDIR/log$1.txt" -kernel "$here/ibctest.elf" \
        | tr -d '\r' > "$OUTDIR/ibc$1.txt"
}
run A 1 1 1 14 &
run B 1 0 0 0 &
run C 1 1 0 0 &
run D 0 0 0 0 &
wait
cat "$OUTDIR/ibcA.txt"
grep ibcstat "$OUTDIR/logA.txt" | tail -1
grep xpagestat "$OUTDIR/logA.txt" | tail -1 || true
for m in A B C D; do
    grep -q '^TOTAL ok' "$OUTDIR/ibc$m.txt" || { echo "FAILED ($m): $OUTDIR"; exit 1; }
    cmp -s "$OUTDIR/ibcA.txt" "$OUTDIR/ibc$m.txt" || { echo "DIFFER (A, $m): $OUTDIR"; exit 1; }
done
echo "MATCH"
