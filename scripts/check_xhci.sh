#!/usr/bin/env bash
# Build is performed by the workflow before this smoke test.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-45}"
SERIAL_LOG="$ROOT/serial-xhci.log"
QEMU_LOG="$ROOT/qemu-xhci.log"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-/usr/share/OVMF/OVMF_VARS_4M.fd}"

fail() {
  echo "[check_xhci] FAIL: $*" >&2
  echo "--- serial log (last 250 lines) ---" >&2
  tail -n 250 "$SERIAL_LOG" 2>/dev/null || true
  echo "--- QEMU log (last 100 lines) ---" >&2
  tail -n 100 "$QEMU_LOG" 2>/dev/null || true
  exit 1
}

command -v "$QEMU_BIN" >/dev/null 2>&1 || fail "QEMU executable not found: $QEMU_BIN"
[[ -f aurora.iso ]] || fail "aurora.iso missing; run make iso first"
[[ -f "$OVMF_CODE" ]] || fail "OVMF code firmware missing: $OVMF_CODE"
[[ -f "$OVMF_VARS_SRC" ]] || fail "OVMF vars template missing: $OVMF_VARS_SRC"
cp "$OVMF_VARS_SRC" "$ROOT/OVMF_VARS-xhci.fd" || fail "cannot copy OVMF vars"
rm -f "$SERIAL_LOG" "$QEMU_LOG"

set +e
timeout --signal=TERM --kill-after=5s "${TIMEOUT_SECONDS}s" \
  "$QEMU_BIN" \
  -machine q35 \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$ROOT/OVMF_VARS-xhci.fd" \
  -m 512M -smp 4 -cpu max \
  -cdrom "$ROOT/aurora.iso" \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0,port=1 \
  -device usb-mouse,bus=xhci.0,port=2 \
  -display none -monitor none \
  -serial "file:$SERIAL_LOG" \
  -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1
qemu_rc=$?
set -e

# QEMU is expected to be stopped by timeout because the OS keeps running.
if [[ "$qemu_rc" -ne 0 && "$qemu_rc" -ne 124 && "$qemu_rc" -ne 137 ]]; then
  fail "QEMU exited unexpectedly with status $qemu_rc"
fi
[[ -s "$SERIAL_LOG" ]] || fail "serial log is empty; kernel may not have booted"

grep -Fq "[xHCI] inicializado" "$SERIAL_LOG" || fail "xHCI did not report successful initialization"
if grep -Eiq '\[xHCI\].*cmd timeout|\[xHCI\].*timeout esperando USB Legacy Handoff|\[xHCI\].*no se detuvo antes del reset' "$SERIAL_LOG"; then
  fail "xHCI reported a command/ownership/reset timeout"
fi
grep -Eq '\[xHCI\] new device slot=' "$SERIAL_LOG" || fail "no USB device reached the xHCI slot-creation path"

# A slot alone is not proof of successful enumeration. Require real device
# descriptors and successful HID report descriptor parsing for both devices
# this test attaches to QEMU's xHCI controller.
grep -Eq '\[USB\] device [[:xdigit:]]{4}:[[:xdigit:]]{4}' "$SERIAL_LOG" ||
  fail "no USB device descriptor was parsed"
if grep -Eiq '\[USB\].*(descriptor malformado|descriptor incompleto|GET_DESCRIPTOR\(DEVICE\) falló)' "$SERIAL_LOG"; then
  fail "USB device descriptor transfer or parsing failed"
fi
grep -Eq '\[HID\] usbhid-kbd-[0-9]+: Report Descriptor OK' "$SERIAL_LOG" ||
  fail "USB keyboard HID report descriptor was not parsed"
grep -Eq '\[HID\] usbhid-mouse-[0-9]+: Report Descriptor OK' "$SERIAL_LOG" ||
  fail "USB mouse HID report descriptor was not parsed"

echo "[check_xhci] PASS: USB descriptors and keyboard/mouse HID report descriptors parsed"
echo "[check_xhci] Serial log: $SERIAL_LOG"
