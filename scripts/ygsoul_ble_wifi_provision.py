#!/usr/bin/env python3
"""BLE WIFI_CONFIG restore for YGSoul ESP32S3. Password only from env, never printed."""
from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "00001910-0000-1000-8000-00805f9b34fb"
WRITE = "00002b11-0000-1000-8000-00805f9b34fb"
NOTIFY = "00002b10-0000-1000-8000-00805f9b34fb"
QUERY = 0x01
DEV_INFO = 0x02
WIFI_CONFIG = 0x03
NETCFG_RESULT = 0x04


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ 0xA001 if value & 1 else value >> 1
    return value


def encode_tlv(parts: list[str]) -> bytes:
    out = bytearray()
    for part in parts:
        body = part.encode("utf-8")
        if len(body) > 255:
            raise ValueError("TLV field too long")
        out.append(len(body))
        out.extend(body)
    return bytes(out)


def encode_frame(command: int, payload: bytes = b"") -> bytes:
    raw = bytes([0xAA, command, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) + payload
    checksum = crc16(raw[1:])
    return raw + bytes([checksum & 0xFF, checksum >> 8])


def decode_frames(buffer: bytearray) -> list[tuple[int, bytes]]:
    frames = []
    while True:
        try:
            start = buffer.index(0xAA)
        except ValueError:
            buffer.clear()
            return frames
        if start:
            del buffer[:start]
        if len(buffer) < 4:
            return frames
        length = (buffer[2] << 8) | buffer[3]
        total = length + 6
        if length > 238 or len(buffer) < total:
            return frames
        checksum = buffer[total - 2] | (buffer[total - 1] << 8)
        if checksum != crc16(bytes(buffer[1:total - 2])):
            del buffer[0]
            continue
        frames.append((buffer[1], bytes(buffer[4:4 + length])))
        del buffer[:total]
    return frames


async def provision(ssid: str, password: str, name_hint: str, timeout: float) -> int:
    if not ssid or not password:
        print("SSID/password missing", file=sys.stderr)
        return 2
    print(f"scanning BLE for {name_hint} / service 1910", file=sys.stderr)
    device = None
    for _ in range(8):
        found = await BleakScanner.discover(timeout=3.0, return_adv=True)
        for dev, adv in found.values():
            name = (dev.name or adv.local_name or "")
            uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            if name_hint.lower() in name.lower() or SERVICE in uuids or "1910" in "".join(uuids):
                device = dev
                print(f"found {name} {dev.address}", file=sys.stderr)
                break
        if device:
            break
    if device is None:
        print("device not advertising", file=sys.stderr)
        return 3

    notify_buf = bytearray()
    got = asyncio.Event()
    results: list[tuple[int, bytes]] = []

    def on_notify(_handle, data: bytearray):
        notify_buf.extend(data)
        frames = decode_frames(notify_buf)
        if frames:
            results.extend(frames)
            got.set()

    async with BleakClient(device, timeout=timeout) as client:
        await client.start_notify(NOTIFY, on_notify)
        await client.write_gatt_char(WRITE, encode_frame(QUERY), response=True)
        try:
            await asyncio.wait_for(got.wait(), timeout=8)
        except asyncio.TimeoutError:
            print("no DEV_INFO", file=sys.stderr)
        for command, payload in results:
            if command == DEV_INFO:
                print(f"devinfo {payload.decode('utf-8', 'replace')}", file=sys.stderr)
        got.clear()
        results.clear()
        payload = encode_tlv([ssid, password])
        await client.write_gatt_char(WRITE, encode_frame(WIFI_CONFIG, payload), response=True)
        deadline = asyncio.get_event_loop().time() + 50
        while asyncio.get_event_loop().time() < deadline:
            try:
                await asyncio.wait_for(got.wait(), timeout=5)
            except asyncio.TimeoutError:
                continue
            frames = list(results)
            results.clear()
            got.clear()
            for command, body in frames:
                text = body.decode("utf-8", "replace")
                print(f"notify cmd=0x{command:02x} {text}", file=sys.stderr)
                if command == NETCFG_RESULT and ("SUCCEEDED" in text or '"status":"SUCCEEDED"' in text):
                    print("WIFI_CONFIG SUCCEEDED", file=sys.stderr)
                    return 0
                if command == NETCFG_RESULT and "FAILED" in text:
                    print("WIFI_CONFIG FAILED", file=sys.stderr)
                    return 4
        print("WIFI_CONFIG timeout", file=sys.stderr)
        return 5


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ssid", required=True)
    parser.add_argument("--name", default="YGSOUL-34:30:7c")
    parser.add_argument("--timeout", type=float, default=20)
    args = parser.parse_args()
    password = os.environ.get("YGSOUL_WIFI_PASSWORD", "")
    return asyncio.run(provision(args.ssid, password, args.name, args.timeout))


if __name__ == "__main__":
    raise SystemExit(main())
