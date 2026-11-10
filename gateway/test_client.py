#!/usr/bin/env python3
"""Protocol test that mirrors the HarmonyOS client: read the video stream,
dump raw H.264 to out.h264, send a tap via the control socket."""
import socket
import struct
import sys
import time

if len(sys.argv) < 2:
    sys.exit("usage: test_client.py <gateway-host>")
host = sys.argv[1]
port = 27183


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


v = socket.create_connection((host, port))
c = socket.create_connection((host, port))
name = recv_exact(v, 64).rstrip(b"\0").decode()
codec = recv_exact(v, 4)
print("device:", name, "codec:", codec)
w = h = 0
frames = keys = 0
out = open("out.h264", "wb")
t0 = time.time()
tapped = False
while time.time() - t0 < 6:
    hdr = recv_exact(v, 12)
    if hdr[0] & 0x80:
        w, h = struct.unpack(">II", hdr[4:12])
        print("session", w, h)
        continue
    pts_flags, size = struct.unpack(">QI", hdr)
    data = recv_exact(v, size)
    out.write(data)
    if not pts_flags & (1 << 62):
        frames += 1
        keys += bool(pts_flags & (1 << 61))
    if not tapped and frames > 5 and w:
        # tap the screen center: ACTION_DOWN then ACTION_UP
        for action in (0, 1):
            msg = struct.pack(">BBqiiHHHii", 2, action, -2, w // 2, h // 2, w, h,
                              0xFFFF if action == 0 else 0, 1, 1 if action == 0 else 0)
            c.sendall(msg)
        # KEYCODE_HOME down/up
        for action in (0, 1):
            c.sendall(struct.pack(">BBiii", 0, action, 3, 0, 0))
        tapped = True
out.close()
print(f"frames={frames} keyframes={keys} fps~{frames/6:.1f} control_sent={tapped}")
