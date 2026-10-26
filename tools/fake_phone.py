#!/usr/bin/env python3
"""Pretends to be the fyewebcam Android app, so the Windows client can be tested without a phone.

Streams an ffmpeg test pattern (or an existing Annex B .h264 file) using the fyewebcam protocol.

    python tools/fake_phone.py                 # needs ffmpeg on PATH
    python tools/fake_phone.py --file clip.h264
"""
import argparse
import socket
import struct
import subprocess
import threading
import time

MAGIC = b"PWCM"
FRAME_CONFIG, FRAME_KEY, FRAME_DELTA = 1, 2, 3
CTRL_BYE = 0x1F


def make_test_stream(width, height, fps, seconds=10):
    cmd = [
        "ffmpeg", "-loglevel", "error", "-f", "lavfi",
        "-i", f"testsrc2=size={width}x{height}:rate={fps}",
        "-t", str(seconds), "-c:v", "libx264", "-profile:v", "baseline", "-bf", "0",
        "-g", str(fps * 2), "-pix_fmt", "yuv420p", "-f", "h264", "-",
    ]
    return subprocess.run(cmd, check=True, capture_output=True).stdout


def split_nals(data):
    """Splits an Annex B byte stream into NAL units, each keeping its start code."""
    starts = []
    i = 0
    while True:
        i = data.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        start = i - 1 if i > 0 and data[i - 1] == 0 else i
        starts.append(start)
        i += 3
    starts.append(len(data))
    return [data[starts[k]:starts[k + 1]] for k in range(len(starts) - 1)]


def nal_type(nal):
    offset = 4 if nal[2] == 0 else 3
    return nal[offset] & 0x1F


def access_units(data):
    """Groups NAL units the way MediaCodec delivers them: config (SPS+PPS), then one buffer per frame."""
    config, frame, units = b"", b"", []
    for nal in split_nals(data):
        t = nal_type(nal)
        if t in (7, 8):
            config += nal
        elif t in (1, 5):
            if config:
                units.append((FRAME_CONFIG, config))
                config = b""
            units.append((FRAME_KEY if t == 5 else FRAME_DELTA, frame + nal))
            frame = b""
        else:
            frame += nal  # SEI, AUD: attach to the next picture
    return units


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("client closed")
        buf += chunk
    return buf


def serve_client(conn, args, stream_cache):
    hello = recv_exact(conn, 11)
    if hello[:4] != MAGIC:
        conn.sendall(MAGIC + bytes([1]) + b"\x00" * 6)
        return
    _, version, width, height, fps, camera = struct.unpack("<4sBHHBB", hello)
    width, height, fps = width or 1280, height or 720, fps or 30
    print(f"client asked for {width}x{height}@{fps} camera={camera}")

    if args.file:
        data = open(args.file, "rb").read()
    else:
        key = (width, height, fps)
        if key not in stream_cache:
            stream_cache[key] = make_test_stream(width, height, fps)
        data = stream_cache[key]
    units = access_units(data)

    conn.sendall(struct.pack("<4sBHHBB", MAGIC, 0, width, height, fps, 1))

    stop = threading.Event()

    def read_controls():
        try:
            while not stop.is_set():
                t, length = struct.unpack("<BH", recv_exact(conn, 3))
                payload = recv_exact(conn, length)
                print(f"control 0x{t:02x} payload={payload.hex()}")
                if t == CTRL_BYE:
                    break
        except (ConnectionError, OSError):
            pass
        stop.set()

    threading.Thread(target=read_controls, daemon=True).start()

    pts = 0
    frame_us = 1_000_000 // fps
    try:
        while not stop.is_set():
            for kind, payload in units:
                if stop.is_set():
                    break
                conn.sendall(struct.pack("<BQI", kind, pts, len(payload)) + payload)
                if kind != FRAME_CONFIG:
                    pts += frame_us
                    time.sleep(frame_us / 1_000_000)
    except OSError:
        pass
    stop.set()
    print("client disconnected")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=4747)
    parser.add_argument("--file", help="Annex B H.264 file to stream instead of a test pattern")
    args = parser.parse_args()

    cache = {}
    with socket.create_server(("0.0.0.0", args.port), reuse_port=False) as server:
        print(f"fake phone listening on port {args.port}")
        while True:
            conn, addr = server.accept()
            print(f"connection from {addr[0]}")
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with conn:
                try:
                    serve_client(conn, args, cache)
                except (ConnectionError, OSError) as e:
                    print(f"client error: {e}")


if __name__ == "__main__":
    main()
