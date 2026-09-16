#!/bin/bash
# flash.sh - the only way to flash the tank. Archives the battery log and the
# tank state first (tools/preflight.py -> docs/batlog/), then builds and
# flashes the app; `--model` also writes the model partition. Refuses to run
# without a preflight archive: battery measurements run in parallel with
# feature work, and a reflash used to wipe the night's log (2026-09-15).
#
#   tools/flash.sh              # preflight, build, flash app
#   tools/flash.sh --model      # ... and model/out/model_q4.bin at 0x290000
#   tools/flash.sh --no-build   # preflight, flash the existing build
set -u
cd "$(dirname "$0")/.."
BUILD=~/.cache/pocket-tank/fw-build
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "flash: no /dev/cu.usbmodem* - wake the tank (BOOT) first"; exit 1; }
tools/preflight.py || exit 1
. ~/esp/esp-idf/export.sh > /dev/null 2>&1 || { echo "flash: ESP-IDF export failed"; exit 1; }
cd firmware
if [[ " $* " != *" --no-build "* ]]; then
  idf.py -B "$BUILD" build 2>&1 | grep -E "error|binary size|build complete"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "flash: BUILD FAILED - nothing flashed"; exit 1; }
fi
if [[ " $* " == *" --model "* ]]; then
  python -m esptool --chip esp32s3 -p "$PORT" -b 460800 write_flash 0x290000 ../model/out/model_q4.bin 2>&1 | grep -E "Wrote|verified|rror"
  sleep 3
fi
idf.py -B "$BUILD" -p "$PORT" flash 2>&1 | grep -E "verified|Hard resetting|rror" | tail -2
echo "flash: done - the tank boots now; docs/batlog has the pre-flash archive (commit it)"
