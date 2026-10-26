#!/usr/bin/env python3
"""Connects to the phone like the Windows client does and saves the stream to a playable .h264 file.

    python tools/test_client.py 192.168.1.42 --seconds 5 --out stream.h264
    ffplay stream.h264
"""
import argparse
import socket
import struct
import time

MAGIC = b"PWCM"
NAMES = {1: "config", 2: "key", 3: "delta"}


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("phone closed the connection")
        buf += chunk
    return buf


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=4747)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--front", action="store_true", help="use the front camera")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--out", default="stream.h264")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.sendall(struct.pack("<4sBHHBB", MAGIC, 1, args.width, args.height, args.fps, 1 if args.front else 0))
    magic, status, width, height, fps, codec = struct.unpack("<4sBHHBB", recv_exact(sock, 11))
    if magic != MAGIC or status != 0:
        raise SystemExit(f"handshake failed: magic={magic!r} status={status}")
    print(f"streaming {width}x{height} @ {fps} fps, codec {codec}")

    counts = {1: 0, 2: 0, 3: 0}
    total = 0
    started = time.monotonic()
    with open(args.out, "wb") as out:
        while time.monotonic() - started < args.seconds:
            kind, pts, length = struct.unpack("<BQI", recv_exact(sock, 13))
            payload = recv_exact(sock, length)
            counts[kind] = counts.get(kind, 0) + 1
            total += length
            out.write(payload)
    sock.sendall(struct.pack("<BH", 0x1F, 0))
    sock.close()

    elapsed = time.monotonic() - started
    frames = counts.get(2, 0) + counts.get(3, 0)
    summary = ", ".join(f"{counts[k]} {NAMES.get(k, k)}" for k in counts)
    print(f"{summary}; {frames / elapsed:.1f} fps, {total * 8 / elapsed / 1e6:.2f} Mbps -> {args.out}")


if __name__ == "__main__":
    main()
