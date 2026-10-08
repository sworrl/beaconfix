#!/usr/bin/env python3
import asyncio
import sys
import argparse
try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("Please install bleak: pip install bleak")
    sys.exit(1)

UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
UART_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
UART_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mac", help="MAC address of the ESP32 node", required=False)
    parser.add_argument("--scan", action="store_true", help="Scan for nodes")
    parser.add_argument("--cmd", help="Command to send over BLE")
    args = parser.parse_args()

    if args.scan or not args.mac:
        print("Scanning for BeaconFix nodes...")
        devices = await BleakScanner.discover()
        for d in devices:
            if d.name and ("Node" in d.name or "Beacon" in d.name):
                print(f"Found {d.name} at {d.address} (RSSI: {d.rssi})")
        if args.scan:
            return
        if not args.mac:
            print("Please specify a --mac address to connect.")
            return

    print(f"Connecting to {args.mac}...")
    async with BleakClient(args.mac) as client:
        print("Connected! Store-and-Forward queue should dump automatically.")
        
        def handle_rx(_, data):
            print(f"[BLE RX] {data.decode('utf-8', 'replace').strip()}")

        await client.start_notify(UART_TX_CHAR_UUID, handle_rx)
        
        if args.cmd:
            print(f"Sending command: {args.cmd}")
            cmd_bytes = (args.cmd + "\n").encode()
            # Chunking if needed
            for i in range(0, len(cmd_bytes), 256):
                await client.write_gatt_char(UART_RX_CHAR_UUID, cmd_bytes[i:i+256])
        
        print("Listening for 10 seconds to catch all telemetry...")
        await asyncio.sleep(10)
        await client.stop_notify(UART_TX_CHAR_UUID)

if __name__ == "__main__":
    asyncio.run(main())
