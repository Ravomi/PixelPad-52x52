#!/usr/bin/env python3
"""Verify one-artwork PSRAM preload, cached selection and safe replacement.

Replaces the selected board's picture with dim RGB fixtures, without Flash/NVS
writes. Physical knob counts are covered by host tests and separate phone logs.
"""
import argparse
import asyncio
import json
import time
from bleak import BleakClient
from ble_benchmark import Replies, RX, TX, summary
from ble_image_latency import frame_hash


async def run(address):
    replies = Replies()
    async with BleakClient(address, timeout=15) as client:
        await client.start_notify(TX, replies.receive)
        acquire = getattr(client._backend, '_acquire_mtu', None)
        if acquire:
            await acquire()
        chunk = min(client.mtu_size - 3, 480)

        async def send(body):
            raw = (json.dumps(body, separators=(',', ':')) + '\n').encode()
            for offset in range(0, len(raw), chunk):
                await client.write_gatt_char(RX, raw[offset:offset+chunk], response=True)

        async def command(body):
            await send(body)
            return await replies.wait(body['cmd'])

        async def upload(session, region, data):
            _, ready = await command({'cmd': 'rotary_frame_store',
                'session': session, 'region': region, 'size': len(data)})
            assert ready['status'] == 'ready', ready
            for offset in range(0, len(data), chunk):
                await client.write_gatt_char(RX, data[offset:offset+chunk], response=False)
            _, done = await replies.wait('rotary_frame_store')
            assert done['status'] == 'ok', done

        async def verify(frame):
            deadline = time.perf_counter() + 2
            while True:
                _, stats = await command({'cmd': 'display_stats'})
                if stats['shown_hash'] == frame_hash(frame):
                    return stats
                assert time.perf_counter() < deadline, stats
                await asyncio.sleep(.02)

        _, before = await command({'cmd': 'rtos_stats'})
        _, capabilities = await command({'cmd': 'capabilities'})
        assert capabilities['rotary_frames_v1']
        order = [7, 2, 19]
        base = bytes([8, 8, 8]) * 2704
        frames = {region: (bytes(rgb) * 2704, bytes([c // 2 for c in rgb]) * 2704)
                  for region, rgb in zip(order, [(48, 0, 0), (0, 48, 0), (0, 0, 48)])}
        await command({'cmd': 'rotary_frame_begin', 'session': 1234,
                       'order': order, 'wrap': True})
        await upload(1234, -1, base)
        for region in order:
            await upload(1234, region, b''.join(frames[region]))
        _, stats = await command({'cmd': 'rotary_stats'})
        assert stats['loaded'] == 3 and not stats['armed'], stats
        print(json.dumps({'preloaded': stats, 'bytes': len(base) * 7}), flush=True)
        samples = []
        for region in order * 3:
            started = time.perf_counter()
            ended, _ = await command({'cmd': 'rotary_frame_select', 'session': 1234,
                'region': region, 'code': True, 'hold_ms': 600, 'fade_ms': 200})
            samples.append((ended - started) * 1000)
            await verify(frames[region][0])
            await verify(frames[region][1])
        # A previous screen's delayed dispose cannot clear the current artwork.
        await command({'cmd': 'rotary_frame_clear', 'session': 999})
        _, stats = await command({'cmd': 'rotary_stats'})
        assert stats['loaded'] == 3 and stats['armed'], stats
        # New artwork replaces the cache, never accumulates multiple drawings.
        await command({'cmd': 'rotary_frame_begin', 'session': 1235,
                       'order': [4], 'wrap': False})
        _, stats = await command({'cmd': 'rotary_stats'})
        assert stats['loaded'] == 0 and stats['count'] == 1 and not stats['armed'], stats
        await command({'cmd': 'rotary_frame_clear', 'session': 1235})
        _, stats = await command({'cmd': 'rotary_stats'})
        assert stats['count'] == 0 and stats['loaded'] == 0, stats
        _, after = await command({'cmd': 'rtos_stats'})
        for key in ('rx_overflows', 'rx_oversize', 'tx_drops', 'input_drops', 'command_drops'):
            assert before[key] == after[key], (key, before, after)
        print(json.dumps({'result': 'PASS', 'cached_select_ack': summary(samples),
                          'after': after}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True)
    asyncio.run(run(parser.parse_args().address))
