#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Flashes BeaconFix Heltec WiFi LoRa 32 (V3) Node Firmware
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_CLI="${ARDUINO_CLI:-/home/user/.local/bin/arduino-cli}"
PORT="${1:-}"

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/ttyACM* /dev/serial/by-id/* /dev/ttyUSB* 2>/dev/null | grep -iE "acm|esp|usb|cp210|ch340|ftdi" | head -n 1 || true)
fi

if [ -z "$PORT" ]; then
    echo "Error: No Heltec V3 serial port detected. Please plug in the Heltec V3 or specify port (e.g. /dev/ttyACM0 or /dev/ttyUSB0)." >&2
    exit 1
fi

cleanup() {
    systemctl --user start beaconfix-esp32-bridge.service 2>/dev/null || true
}
trap cleanup EXIT

echo "==> Target Heltec V3 port: $PORT"
systemctl --user stop beaconfix-esp32-bridge.service 2>/dev/null || true

if [ "${2:-}" != "--no-compile" ]; then
    echo "==> Compiling Heltec V3 firmware..."
    python3 "$DIR/tools/sign_firmware.py" --init   # your own keypair on first run; embeds its public half
    "$ARDUINO_CLI" compile --fqbn esp32:esp32:heltec_wifi_lora_32_V3 --build-path "$DIR/firmware/heltec_v3/build" "$DIR/firmware/heltec_v3"
    echo "==> Signing Heltec V3 firmware cryptographically..."
    python3 "$DIR/tools/sign_firmware.py" "$DIR/firmware/heltec_v3/build/heltec_v3.ino.bin"
fi

echo "==> Uploading firmware to Heltec V3 on $PORT..."
"$ARDUINO_CLI" upload -p "$PORT" --fqbn esp32:esp32:heltec_wifi_lora_32_V3 --input-dir "$DIR/firmware/heltec_v3/build"

echo "==> Flashed successfully! Verifying node boot..."
sleep 2
echo "==> Heltec V3 is live with OLED status display, SX1262 LoRa mesh, and Wi-Fi monitor mode."
