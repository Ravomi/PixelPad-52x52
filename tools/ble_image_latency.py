#!/usr/bin/env python3
"""FM6373C image/patch regression: ACK latency and pixels handed to active DMA.

Requires bleak and an explicitly selected board. Replaces its canvas with dim
RGB test frames. No Flash/OTA writes. --chunk 20 exercises small-packet clients;
--gap-ms 15 makes a complete image take longer than the old 5-second deadline.
This checks the encoded frame's source hash, not optical panel output.
"""
import argparse
import asyncio
import json
import time
from bleak import BleakClient
from ble_benchmark import Replies, RX, TX, summary


def frame_hash(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


async def run(options):
    replies = Replies()
    samples = []
    async with BleakClient(options.address, timeout=15) as client:
        await client.start_notify(TX, replies.receive)
        rx = client.services.get_characteristic(RX)
        # BlueZ can retain a cached 20-byte property between connections.
        # Bleak's Linux MTU helper obtains the actual negotiated ATT MTU via
        # AcquireWrite and immediately releases the acquired descriptor.
        if options.negotiate_mtu:
            acquire = getattr(client._backend, "_acquire_mtu", None)
            if acquire:
                await asyncio.wait_for(acquire(), timeout=5)
        for _ in range(20):
            if rx.max_write_without_response_size > 20:
                break
            await asyncio.sleep(.05)
        payload = client.mtu_size - 3 if options.negotiate_mtu else rx.max_write_without_response_size
        chunk = min(options.chunk, payload, 512)
        if options.require_chunk:
            assert chunk == options.chunk, f"Negotiated {chunk}, requested {options.chunk}"
        print(json.dumps({"chunk": chunk, "gap_ms": options.gap_ms,
                          "header_delay_ms": options.header_delay_ms}), flush=True)

        async def send(data, response=False):
            # Also fragment JSON at the requested size; binary boundaries must
            # survive a 20-byte ATT payload without relying on long writes.
            for offset in range(0, len(data), chunk):
                await client.write_gatt_char(rx, data[offset:offset+chunk], response=response)

        async def query(command):
            await send((json.dumps({"cmd": command}) + "\n").encode(), True)
            _, result = await replies.wait(command)
            return result

        async def verify_pixels(frame):
            expected = frame_hash(frame)
            deadline = time.perf_counter() + 1
            while True:
                stats = await query("display_stats")
                assert stats.get("driver") == "fm6373c" and stats.get("ready"), stats
                assert "shown_hash" in stats, "Firmware lacks DMA image verification"
                if stats["shown_hash"] == expected:
                    return stats
                assert time.perf_counter() < deadline, (expected, stats)
                await asyncio.sleep(.02)

        before = await query("rtos_stats")
        print(json.dumps({"before": before}), flush=True)
        display_before = await query("display_stats") if options.fade_ms else None
        for iteration in range(options.frames):
            frame = bytearray([48 if iteration % 2 else 0, 0, 0 if iteration % 2 else 48]) * 2704
            started = time.perf_counter()
            header = {"cmd": "recv", "size": 8112}
            if options.fade_ms:
                header["fade_ms"] = options.fade_ms
            await send((json.dumps(header) + "\n").encode(), True)
            if options.header_delay_ms:
                await asyncio.sleep(options.header_delay_ms / 1000)
            for offset in range(0, len(frame), chunk):
                await client.write_gatt_char(rx, frame[offset:offset+chunk], response=False)
                if options.gap_ms:
                    await asyncio.sleep(options.gap_ms / 1000)
            sent = time.perf_counter()
            ended, _ = await replies.wait("recv", timeout=10)
            elapsed = (ended - started) * 1000
            if not options.ack_only:
                await verify_pixels(frame)
            print(json.dumps({"image": iteration, "sent_ms": round((sent-started)*1000, 2),
                              "ack_ms": round(elapsed, 2),
                              "verified_ms": round((time.perf_counter()-started)*1000, 2)}), flush=True)
            assert elapsed < options.max_ms, f"Image ACK took {elapsed:.1f} ms, limit {options.max_ms}"
            samples.append(elapsed)
            # A subsequent color command must still work after a slow image.
            frame[:3] = bytes([0, 48, 0])
            await send(b'{"cmd":"px_patch","ops":[[0,0,0,48,0]]}\n', True)
            if not options.ack_only:
                await verify_pixels(frame)
        after = await query("rtos_stats")
        print(json.dumps({"after": after}), flush=True)
        for key in ("rx_overflows", "rx_oversize", "tx_drops", "command_drops", "input_drops"):
            assert after[key] == before[key], (key, before, after)
        if options.fade_ms:
            display_after = await query("display_stats")
            fade_frames = display_after["transition_frames"] - display_before["transition_frames"]
            assert fade_frames >= options.frames * 2, (fade_frames, display_after)
            assert not display_after["fading"], display_after
            assert display_after["shown"] == display_after["committed"], display_after
            print(json.dumps({"local_fade_frames": fade_frames, "display_after": display_after}), flush=True)
        print(json.dumps({"result": "PASS", "verification": "ACK" if options.ack_only else "DMA image and patch",
                          **summary(samples)}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--chunk", type=int, default=20)
    parser.add_argument("--gap-ms", type=float, default=0)
    parser.add_argument("--header-delay-ms", type=float, default=50,
                        help="Legacy header wait; firmware streamRecv capability permits zero")
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--max-ms", type=float, default=2000)
    parser.add_argument("--ack-only", action="store_true", help="Baseline for older firmware without shown_hash")
    parser.add_argument("--negotiate-mtu", action="store_true", help="Acquire the actual MTU on BlueZ for a large-packet test")
    parser.add_argument("--require-chunk", action="store_true", help="Fail rather than silently test smaller packets")
    parser.add_argument("--fade-ms", type=int, default=0, help="Verify on-board transition without sending intermediate frames")
    options = parser.parse_args()
    if not 1 <= options.chunk <= 512 or options.gap_ms < 0 or options.header_delay_ms < 0 or options.frames < 1 or options.max_ms <= 0:
        parser.error("Require chunk 1..512, frames >= 1, gap >= 0, max-ms > 0")
    if not 0 <= options.fade_ms <= 500 or (options.fade_ms and options.ack_only):
        parser.error("fade-ms requires 0..500 and DMA verification")
    asyncio.run(run(options))
