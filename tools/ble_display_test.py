#!/usr/bin/env python3
"""Display a static diagnostic pattern and report scan configuration over BLE.

Changes the current image and brightness (10% by default), without writing a
standby file or firmware. A camera/oscilloscope is needed to measure scan rate.
"""
import argparse
import asyncio
import json
import time

from ble_benchmark import RX, TX, Replies


def pattern_rgb(pattern):
    bars = ((255, 0, 0), (0, 255, 0), (0, 0, 255),
            (255, 255, 0), (0, 255, 255), (255, 0, 255), (255, 255, 255))
    frame = bytearray()
    for y in range(52):
        for x in range(52):
            if pattern == "bars":
                rgb = bars[min(x * len(bars) // 52, len(bars) - 1)]
            elif pattern == "gradient":
                value = x * 255 // 51
                rgb = ((value, 0, 0), (0, value, 0), (0, 0, value),
                       (value, value, value))[y // 13]
            else:
                rgb = (255, 255, 255) if (x + y) % 2 else (0, 0, 0)
            frame.extend(rgb)
    return bytes(frame)


async def run(options):
    from bleak import BleakClient
    replies = Replies()
    async with BleakClient(options.address) as client:
        await client.start_notify(TX, replies.receive)
        characteristic = client.services.get_characteristic(RX)
        for _ in range(20):
            if characteristic.max_write_without_response_size > 20:
                break
            await asyncio.sleep(.05)
        chunk = min(244, 512, characteristic.max_write_without_response_size)
        print(json.dumps({"att_payload_bytes": chunk}), flush=True)
        # All writes share one serialized NUS stream, including the RGB body.
        brightness = json.dumps({"cmd": "bright", "val": options.brightness}) + "\n"
        await client.write_gatt_char(characteristic, brightness.encode(), response=True)
        await client.write_gatt_char(characteristic, b'{"cmd":"recv","size":8112}\n', response=True)
        frame = pattern_rgb(options.pattern)
        started = time.monotonic()
        for offset in range(0, len(frame), chunk):
            await client.write_gatt_char(characteristic, frame[offset:offset + chunk], response=False)
        print(json.dumps({"sent_bytes": len(frame), "send_seconds": round(time.monotonic() - started, 3)}), flush=True)
        await replies.wait("recv")
        # ACK acknowledges staging. Allow a render iteration and ring switch.
        await asyncio.sleep(.1)
        for command in ("display_stats", "rtos_stats"):
            await client.write_gatt_char(characteristic, (json.dumps({"cmd": command}) + "\n").encode(), response=True)
            _, stats = await replies.wait(command)
            print(json.dumps(stats), flush=True)
        print(f"Showing {options.pattern}, brightness={options.brightness}%; scan_hz_est is not a measurement.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--pattern", choices=("bars", "gradient", "checker"), default="bars")
    parser.add_argument("--brightness", type=int, default=10)
    options = parser.parse_args()
    if not 1 <= options.brightness <= 100:
        parser.error("brightness must be 1..100")
    asyncio.run(run(options))


if __name__ == "__main__":
    main()
