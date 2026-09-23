#!/usr/bin/env python3
"""Serves the phone page from src/web_page.h and replays a recorded stream to it, like the Cardputer.

Record a stream with test/run_tests.sh <rom>, then:
    tools/replay_server.py test/out/<game>.stream [port]
and open http://localhost:8080 (or the Mac's address from a phone on the same Wi-Fi).
Button presses from the page's touch pad are printed.
"""
import base64
import hashlib
import pathlib
import re
import socket
import struct
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = re.search(r'R"html\((.*)\)html"', (ROOT / "src/web_page.h").read_text(), re.S).group(1).encode()
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_stream(path):
    data, out, i = pathlib.Path(path).read_bytes(), [], 0
    while i + 4 <= len(data):
        (n,) = struct.unpack_from("<I", data, i)
        out.append(data[i + 4: i + 4 + n])
        i += 4 + n
    return out


def ws_frame(payload, opcode=2):
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + payload


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError
        buf += chunk
    return buf


def reader(conn):
    try:
        while True:
            b0, b1 = recv_exact(conn, 2)
            n = b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack(">H", recv_exact(conn, 2))
            elif n == 127:
                (n,) = struct.unpack(">Q", recv_exact(conn, 8))
            mask = recv_exact(conn, 4) if b1 & 0x80 else b"\0\0\0\0"
            data = bytes(c ^ mask[i % 4] for i, c in enumerate(recv_exact(conn, n)))
            if b0 & 0x0F == 8:
                return
            if len(data) == 2 and data[0] == 2:
                names = [nm for bit, nm in zip(range(8), "A B SELECT START RIGHT LEFT UP DOWN".split()) if data[1] >> bit & 1]
                print("buttons:", " ".join(names) or "-", flush=True)
    except (ConnectionError, OSError):
        pass


def serve_ws(conn, frames):
    threading.Thread(target=reader, args=(conn,), daemon=True).start()
    sent = 0
    try:
        while True:
            for f in frames:  # the recording starts with a full frame, so each loop repaints
                conn.sendall(ws_frame(f))
                sent += len(f)
                time.sleep(1 / 30)
    except OSError:
        print(f"viewer left after {sent // 1024} KB", flush=True)


def handle(conn, frames):
    req = b""
    while b"\r\n\r\n" not in req:
        chunk = conn.recv(4096)
        if not chunk:
            return conn.close()
        req += chunk
    head = req.decode(errors="replace")
    path = head.split(" ")[1]
    key = re.search(r"Sec-WebSocket-Key: *(\S+)", head, re.I)
    if path == "/ws" and key:
        accept = base64.b64encode(hashlib.sha1((key.group(1) + GUID).encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        print("viewer connected", flush=True)
        serve_ws(conn, frames)
    elif path == "/":
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     + f"Content-Length: {len(PAGE)}\r\nConnection: close\r\n\r\n".encode() + PAGE)
    else:
        conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    conn.close()


def main():
    frames = read_stream(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    total = sum(map(len, frames))
    print(f"{len(frames)} messages, {total // 1024} KB, {total / len(frames) / 1024:.1f} KB avg; http://localhost:{port}")
    srv = socket.create_server(("", port), reuse_port=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, frames), daemon=True).start()


if __name__ == "__main__":
    main()
