#!/usr/bin/env python3
"""Hardware checks for framing, bounds, timeout isolation and reconnect.
Requires bleak. Explicit address only; changes a small canvas region, no OTA writes.
"""
import argparse
import asyncio
import json
from bleak import BleakClient
from ble_benchmark import RX, TX


class Session:
    def __init__(self):
        self.buffer = bytearray()
        self.queue = asyncio.Queue()

    def receive(self, sender, data):
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            line, _, self.buffer = self.buffer.partition(b"\n")
            try:
                value = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                raise AssertionError(f"Invalid notification: {line!r}")
            if "evt" not in value:
                self.queue.put_nowait(value)

    async def response(self, predicate, timeout=8):
        async def find():
            while True:
                value = await self.queue.get()
                if predicate(value):
                    return value
        return await asyncio.wait_for(find(), timeout)


async def run(address):
    session = Session()
    async with BleakClient(address) as client:
        await client.start_notify(TX, session.receive)
        # Every header fragment and subsequent JSON must retain its boundary.
        for part in (b'{"cmd":', b'"rtos_', b'stats"}', b'\n'):
            await client.write_gatt_char(RX, part, response=True)
        await session.response(lambda m: m.get("cmd") == "rtos_stats")
        print("PASS fragmented_header", flush=True)
        await client.write_gatt_char(RX, b'{"cmd":"set_standby","frames":2147483647,"fps":8,"size":8112}\n', response=True)
        await session.response(lambda m: m.get("cmd") == "set_standby" and m.get("reason") == "invalid_params")
        print("PASS standby_bounds", flush=True)
        # Send an oversized line with a valid-looking JSON suffix: no suffix execution.
        oversized = b'x' * 1100 + b'{"cmd":"mode","mode":"sand"}\n'
        for offset in range(0, len(oversized), 100):
            await client.write_gatt_char(RX, oversized[offset:offset + 100], response=True)
        await session.response(lambda m: m.get("reason") == "line_too_long")
        await client.write_gatt_char(RX, b'{"cmd":"rtos_stats"}\n', response=True)
        await session.response(lambda m: m.get("cmd") == "rtos_stats")
        print("PASS oversized_line_recovery", flush=True)
        # Force a partial binary transfer. Embedded LF/CR/NUL are pixels, not JSON.
        await client.write_gatt_char(RX, b'{"cmd":"recv","size":8112}\n' + bytes([0, 10, 13, 255]), response=True)
        await session.response(lambda m: m.get("cmd") == "recv" and m.get("reason") == "recv_fail")
        print("PASS binary_timeout_without_false_ack", flush=True)
    await asyncio.sleep(.8)
    session = Session()
    async with BleakClient(address) as client:
        await client.start_notify(TX, session.receive)
        await client.write_gatt_char(RX, b'{"cmd":"rtos_stats"}\n', response=True)
        stats = await session.response(lambda m: m.get("cmd") == "rtos_stats")
        assert stats["rx_overflows"] == 0
        assert stats["input_drops"] == 0 and stats["command_drops"] == 0
        print("PASS reconnect_after_binary_timeout", flush=True)
        print(json.dumps(stats), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    asyncio.run(run(parser.parse_args().address))
