#!/usr/bin/env bash
# precondition-probe.sh — read the DSC 0x10 0x02 precondition bitmap
# via SSM-A8 24-bit RAM-truncation, decode against round-11 expected
# values (all bytes must equal 0x01).
#
# Usage:
#   precondition-probe.sh [--device COM4|COM5]
#
# Wire reference:
#   - Round-10 analyst spec atlas-sids-and-firmware-dispatch-round-10.md §3.1
#   - Round-11 analyst spec dsc-precondition-bitmap-per-byte-decode-round-11.md
#
# Reads:
#   - 21 bytes at RAM 0xFFF8A6CE..0xFFF8A6EA (DSC handler reads non-contiguous:
#     0xCE/CF/D0 + 0xD9..0xEA, but we read all 21 for context)
#   - 1 byte at RAM 0xFFF89216 (22nd outlier)
#
# Expected (all OK = DSC 0x10 0x02 should grant):
#   - All 21 bytes == 0x01
#   - 0xFFF89216 typically == 0xA0 (not a blocker per round-11 §4)

set -euo pipefail

DEVICE="COM4"
if [[ "${1:-}" == "--device" && -n "${2:-}" ]]; then
    DEVICE="$2"
fi

CLI="$(cd "$(dirname "$0")/.." && pwd)/build/win-mingw/bin/subuwutuner-cli.exe"
if [[ ! -x "$CLI" ]]; then
    echo "error: cli not built at $CLI" >&2
    exit 1
fi

# Build multi-addr A8 payload: PAD + 21 × 3-byte addrs covering 0xCE..0xEA
PAYLOAD="00"
for hex in CE CF D0 D1 D2 D3 D4 D5 D6 D7 D8 D9 DA DB DC DD DE DF E0 E1 E2 E3 E4 E5 E6 E7 E8 E9 EA; do
    PAYLOAD="${PAYLOAD}F8A6${hex}"
done

run_a8() {
    local payload="$1"
    "$CLI" subaru-ssm-cmd --transport obdx --device "$DEVICE" \
        --cmd 0xA8 --payload "$payload" \
        --no-authenticate --no-enter-dsc --verbose --timeout-ms 2000 2>&1 \
        | grep -oE "\[obdx-rx\] E8 [0-9A-F ]+\([0-9]+ B\)" \
        | head -1 | sed 's/.*\[obdx-rx\] E8 //; s/ ([0-9]* B)//'
}

echo "=== DSC 0x10 0x02 precondition probe ==="
echo "Device: $DEVICE"
echo ""

BITMAP_RAW=$(run_a8 "$PAYLOAD")
OUTLIER_RAW=$(run_a8 "00F89216")

if [[ -z "$BITMAP_RAW" ]]; then
    echo "error: no response from bench (adapter open failed or ECU not powered?)" >&2
    exit 1
fi

# Decode bitmap: 29 bytes from 0xCE..0xEA inclusive
read -r -a BYTES <<< "$BITMAP_RAW"

# Indices the DSC handler actually checks (per round-11 §2.2):
#   relative 0..2 (0xCE/CF/D0) and 11..28 (0xD9..0xEA)
DSC_CHECKED_INDICES=(0 1 2 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28)
DSC_CHECKED_LABELS=(CE CF D0 D9 DA DB DC DD DE DF E0 E1 E2 E3 E4 E5 E6 E7 E8 E9 EA)

set_count=0
total=${#DSC_CHECKED_INDICES[@]}
echo "DSC handler precondition flags (round-11 §2.2 — must all == 0x01):"
for i in "${!DSC_CHECKED_INDICES[@]}"; do
    idx="${DSC_CHECKED_INDICES[$i]}"
    lbl="${DSC_CHECKED_LABELS[$i]}"
    val="${BYTES[$idx]}"
    if [[ "$val" == "01" ]]; then
        marker="[OK ]"
        set_count=$((set_count + 1))
    else
        marker="[!! ]"
    fi
    echo "  $marker 0xFFF8A6$lbl = 0x$val"
done

echo ""
echo "Gap bytes 0xD1..0xD8 (not in DSC literal pool):"
for i in 3 4 5 6 7 8 9 10; do
    lbl_hex=$(printf '%02X' $((0xD1 + i - 3)))
    val="${BYTES[$i]}"
    echo "  [.. ] 0xFFF8A6$lbl_hex = 0x$val"
done

echo ""
echo "22nd outlier (separate region):"
if [[ -n "$OUTLIER_RAW" ]]; then
    read -r -a O <<< "$OUTLIER_RAW"
    val="${O[0]:-??}"
    if [[ "$val" == "A0" ]]; then
        echo "  [OK ] 0xFFF89216 = 0x$val (typical bench value)"
    else
        echo "  [?? ] 0xFFF89216 = 0x$val (unexpected)"
    fi
fi

echo ""
echo "Summary: $set_count / $total precondition flags set"
if [[ "$set_count" -eq "$total" ]]; then
    echo "DSC 0x10 0x02 should grant."
elif [[ "$set_count" -eq 0 ]]; then
    echo "Bench cold — no heartbeat injection populating these IDs."
    echo "Round-12 analyst trace needed for per-byte CAN-ID mapping."
else
    echo "Partial — $((total - set_count)) flag(s) still missing."
fi
