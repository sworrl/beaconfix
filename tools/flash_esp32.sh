#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Flashes BeaconFix monitor node firmware to a plugged-in ESP32
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-}"

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/serial/by-id/* 2>/dev/null | grep -iE "cp210|ch340|ftdi|uart|esp32" | head -n 1 || true)
    if [ -z "$PORT" ]; then
        PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1 || true)
    fi
fi

if [ -z "$PORT" ]; then
    echo "Error: No ESP32 serial port detected. Please plug in an ESP32 or specify port (e.g. /dev/ttyUSB0)." >&2
    exit 1
fi

cleanup() {
    systemctl --user start beaconfix-esp32-bridge.service 2>/dev/null || true
}
trap cleanup EXIT

echo "==> Using port: $PORT"
systemctl --user stop beaconfix-esp32-bridge.service 2>/dev/null || true

if [ "${2:-}" != "--no-compile" ]; then
    echo "==> Compiling ESP32 firmware with min_spiffs (OTA enabled)..."
    arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --build-path "$DIR/firmware/esp32_node/build" "$DIR/firmware/esp32_node"
    echo "==> Signing firmware binary cryptographically..."
    python3 "$DIR/tools/sign_firmware.py" "$DIR/firmware/esp32_node/build/esp32_node.ino.bin"
fi

echo "==> Flashing ESP32 on $PORT..."
arduino-cli upload -p "$PORT" --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --input-dir "$DIR/firmware/esp32_node/build"

echo "==> Done! Starting live telemetry stream..."
"$DIR/tools/esp32_gateway.py" --port "$PORT" --status

