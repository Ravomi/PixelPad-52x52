#!/usr/bin/env python3
"""BLE latency/transfer benchmark. Run manually against an explicitly selected board.

Install bleak in a virtual environment. No firmware flashing or OTA is performed.
Image modes replace the displayed picture with dim RGB test data.
"""
import argparse
import asyncio
import json
import math
import statistics
import time

RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
FRAME_BYTES = 52 * 52 * 3


class Replies:
    def __init__(self):
        self.buffer = bytearray()
        self.messages = asyncio.Queue()
        self.events = []

    def receive(self, _sender, data):
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            line, _, self.buffer = self.buffer.partition(b"\n")
            try:
                message = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                print("Invalid notification:", repr(line))
                continue
            now = time.perf_counter()
            if "evt" in message:
                self.events.append((now, message))
                print("input:", message)
            else:
                self.messages.put_nowait((now, message))

    async def wait(self, cmd, timeout=10):
        async def match():
            while True:
                timestamp, message = await self.messages.get()
                # Original 52x52 firmware uses a bare {status:ok} for image ACK.
                if message.get("status") == "err":
                    raise RuntimeError(message)
                if message.get("cmd") == cmd or (cmd == "recv" and message.get("status") == "ok" and "cmd" not in message):
                    return timestamp, message
        return await asyncio.wait_for(match(), timeout)


def summary(samples):
    ordered = sorted(samples)
    return {"n": len(samples), "median_ms": round(statistics.median(samples), 3),
            "p95_ms": round(ordered[math.ceil(len(samples) * .95) - 1], 3),
            "max_ms": round(max(samples), 3)}


async def run(options):
    from bleak import BleakClient
    replies = Replies()
    samples = []
    async with BleakClient(options.address) as client:
        await client.start_notify(TX, replies.receive)
        characteristic = client.services.get_characteristic(RX)
        # Respect the ATT payload negotiated by this host. The device accepts
        # at most 512 bytes per characteristic value, even with an MTU of 517.
        # BlueZ may publish the negotiated MTU shortly after service discovery.
        # Wait briefly instead of freezing the initial fallback of 20 bytes.
        for _ in range(20):
            if characteristic.max_write_without_response_size > 20:
                break
            await asyncio.sleep(.05)
        chunk = min(options.chunk, characteristic.max_write_without_response_size, 512)
        print(json.dumps({"att_payload_bytes": chunk}), flush=True)
        if options.mode == "input":
            print("Turn the knob / press buttons during the 2-second transfer pause.")
        for iteration in range(options.iterations):
            if options.mode in ("stats", "display"):
                command = "display_stats" if options.mode == "display" else "rtos_stats"
                start = time.perf_counter()
                await client.write_gatt_char(RX, (json.dumps({"cmd": command}) + "\n").encode(), response=True)
                end, stats = await replies.wait(command)
                print(json.dumps(stats, ensure_ascii=False))
            else:
                frame = bytes([options.level if iteration % 2 else 0, 0, options.level]) * (52 * 52)
                start = time.perf_counter()
                await client.write_gatt_char(RX, b'{"cmd":"recv","size":8112}\n', response=True)
                paused = False
                for offset in range(0, FRAME_BYTES, chunk):
                    if options.mode == "input" and not paused and offset >= FRAME_BYTES // 2:
                        paused = True
                        pause_start = time.perf_counter()
                        await asyncio.sleep(2)
                        pause_end = time.perf_counter()
                        during = [m for t, m in replies.events if pause_start <= t <= pause_end]
                        print(json.dumps({"events_during_incomplete_transfer": len(during)}))
                    await client.write_gatt_char(characteristic, frame[offset:offset + chunk], response=False)
                    if options.gap_ms:
                        await asyncio.sleep(options.gap_ms / 1000)
                end, _ = await replies.wait("recv")
            samples.append((end - start) * 1000)
            if (iteration + 1) % 10 == 0:
                print(json.dumps({"completed": iteration + 1, **summary(samples)}), flush=True)
            await asyncio.sleep(.05)
        await client.stop_notify(TX)
    print(json.dumps({"mode": options.mode, **summary(samples)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True, help="Explicit BLE address or macOS peripheral UUID")
    parser.add_argument("--mode", choices=("image", "stats", "display", "input"), default="stats")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--chunk", type=int, default=244)
    parser.add_argument("--gap-ms", type=float, default=1)
    parser.add_argument("--level", type=int, default=12, help="Image RGB level; use 96 with 10%% brightness to exercise the 4-bit LUT visibly")
    options = parser.parse_args()
    if options.iterations < 1 or not 1 <= options.chunk <= 512 or options.gap_ms < 0 or not 0 <= options.level <= 255:
        parser.error("Require iterations >= 1, chunk 1..512, gap-ms >= 0, level 0..255")
    asyncio.run(run(options))


if __name__ == "__main__":
    main()
