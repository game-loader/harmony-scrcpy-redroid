#!/usr/bin/env python3
"""scrcpy TCP gateway for the HarmonyOS client.

Each client session opens two TCP connections to LISTEN (video, then control).
On the video connection the gateway (re)starts scrcpy-server on the device via
`adb forward`, consumes the forward-tunnel dummy byte, and then pipes bytes
untouched in both directions. The client therefore sees the plain scrcpy
v5.0.1 stream: 64-byte device name, codec id, session packets, media packets.

The client->device direction of the video socket is unused by scrcpy, so the
client uses it to send resize requests (b"RSZ1" + u16 width + u16 height +
u16 dpi, big-endian). The gateway applies them with `wm size`/`wm density`;
scrcpy-server notices the display change and starts a new capture session.
"""
import asyncio
import logging
import os
import random
import struct
import subprocess

ADB = os.environ.get("ADB", os.path.expanduser("~/redroid/scrcpy-linux-x86_64-v5.0.1/adb"))
SERIAL = os.environ.get("SERIAL", "127.0.0.1:5555")
SERVER_JAR = os.environ.get("SERVER_JAR", os.path.expanduser("~/redroid/scrcpy-linux-x86_64-v5.0.1/scrcpy-server"))
SERVER_VERSION = "5.0.1"
LISTEN_HOST = os.environ.get("LISTEN_HOST")  # required, e.g. this server's Tailscale IP
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "27183"))
FORWARD_PORT = int(os.environ.get("FORWARD_PORT", "27199"))
MAX_SIZE = os.environ.get("MAX_SIZE", "1920")
BIT_RATE = os.environ.get("BIT_RATE", "8000000")
MAX_FPS = os.environ.get("MAX_FPS", "60")
CONTROL_WAIT_S = 10
RESIZE_MAGIC = b"RSZ1"
RESIZE_DEBOUNCE_S = 0.4

log = logging.getLogger("gateway")


def adb(*args, check=True):
    return subprocess.run([ADB, "-s", SERIAL, *args], check=check, capture_output=True, text=True)


class Session:
    def __init__(self):
        self.proc = None
        self.pending_control = False
        self.video_ready = asyncio.Event()

    def stop(self):
        if self.proc and self.proc.returncode is None:
            self.proc.terminate()
        self.proc = None

    async def start_server(self):
        self.stop()
        subprocess.run([ADB, "connect", SERIAL], capture_output=True)
        scid = "%08x" % random.randint(0, 0x7FFFFFFF)
        adb("push", SERVER_JAR, "/data/local/tmp/scrcpy-server.jar")
        adb("forward", "--remove-all", check=False)
        adb("forward", f"tcp:{FORWARD_PORT}", f"localabstract:scrcpy_{scid}")
        cmd = [ADB, "-s", SERIAL, "shell",
               "CLASSPATH=/data/local/tmp/scrcpy-server.jar", "app_process", "/",
               "com.genymobile.scrcpy.Server", SERVER_VERSION,
               f"scid={scid}", "log_level=info", "tunnel_forward=true",
               "audio=false", "control=true", "video_codec=h264",
               f"max_size={MAX_SIZE}", f"video_bit_rate={BIT_RATE}", f"max_fps={MAX_FPS}",
               "stay_awake=true", "clipboard_autosync=false", "cleanup=true"]
        self.proc = await asyncio.create_subprocess_exec(*cmd)
        log.info("scrcpy-server started scid=%s", scid)

    async def open_device_socket(self, expect_dummy):
        for _ in range(100):
            try:
                r, w = await asyncio.open_connection("127.0.0.1", FORWARD_PORT)
                if not expect_dummy:
                    return r, w
                b = await asyncio.wait_for(r.readexactly(1), 2)
                if b == b"\x00":
                    return r, w
            except (OSError, asyncio.IncompleteReadError, asyncio.TimeoutError):
                pass
            await asyncio.sleep(0.1)
        raise RuntimeError("scrcpy-server did not accept the connection")


session = Session()


def clamp_size(w, h, dpi):
    w = min(max(w, 320), 4096) & ~7
    h = min(max(h, 320), 4096) & ~7
    return w, h, min(max(dpi, 100), 800)


async def apply_resize(w, h, dpi):
    w, h, dpi = clamp_size(w, h, dpi)
    log.info("resize display to %dx%d @%ddpi", w, h, dpi)
    await asyncio.to_thread(adb, "shell", f"wm size {w}x{h} && wm density {dpi}", check=False)


async def read_resize_requests(reader):
    """Consume resize requests, applying only the last one of a burst
    (folding/unfolding emits several surface sizes in a row)."""
    pending = None
    task = None

    async def apply_later(req):
        await asyncio.sleep(RESIZE_DEBOUNCE_S)
        await apply_resize(*req)

    try:
        while True:
            msg = await reader.readexactly(10)
            if msg[:4] != RESIZE_MAGIC:
                log.warning("unexpected data on video socket, ignoring the rest")
                await reader.read()
                return
            pending = struct.unpack(">HHH", msg[4:])
            if task:
                task.cancel()
            task = asyncio.create_task(apply_later(pending))
    except (asyncio.IncompleteReadError, ConnectionError):
        pass


async def pipe(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(client_r, client_w):
    peer = client_w.get_extra_info("peername")
    # Roles are assigned synchronously at accept time: the client always opens
    # video first, then control, possibly before the video setup completes.
    is_control = session.pending_control
    session.pending_control = not is_control
    try:
        if not is_control:
            log.info("video connection from %s", peer)
            session.video_ready.clear()
            await session.start_server()
            dev_r, dev_w = await session.open_device_socket(expect_dummy=True)
            session.video_ready.set()
        else:
            log.info("control connection from %s", peer)
            await asyncio.wait_for(session.video_ready.wait(), CONTROL_WAIT_S)
            dev_r, dev_w = await session.open_device_socket(expect_dummy=False)
    except Exception as e:
        log.error("session setup failed: %s", e)
        session.pending_control = False
        client_w.close()
        return
    upstream = read_resize_requests(client_r) if not is_control else pipe(client_r, dev_w)
    await asyncio.gather(pipe(dev_r, client_w), upstream)
    log.info("%s connection closed", "control" if is_control else "video")
    if not is_control:
        session.stop()


async def main():
    if not LISTEN_HOST:
        raise SystemExit("LISTEN_HOST is required, e.g. LISTEN_HOST=<tailscale-ip>")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    srv = await asyncio.start_server(handle, LISTEN_HOST, LISTEN_PORT)
    log.info("listening on %s:%d", LISTEN_HOST, LISTEN_PORT)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
