"""Terminal-owned COM7 bridge for the Pecky web app.

It starts cap recognition when an app client connects, forwards every firmware
EVENT over localhost WebSocket, and pauses recognition when the last client
disconnects. Run on Windows (the WSL wrapper invokes it for you).
"""
from __future__ import annotations

import asyncio, json, os, queue, threading
import serial
from websockets.asyncio.server import serve

PORT, BAUD, clients = "COM7", 115200, set()
line_queue: queue.Queue[str] = queue.Queue()
device: serial.Serial | None = None
LOG_PATH = os.path.join(os.environ.get("TEMP", "."), "pecky-serial-bridge.log")

def log(message: str) -> None:
    line = f"{__import__('datetime').datetime.now().isoformat(timespec='seconds')} {message}"
    print(line, flush=True)
    with open(LOG_PATH, "a", encoding="utf-8") as output: output.write(line + "\n")

def command(value: str) -> None:
    if device:
        device.write(f"{value}\n".encode("ascii")); device.flush()
        log(f"COMMAND {value}")

def reader() -> None:
    while device and device.is_open:
        raw = device.readline().decode("utf-8", "replace").strip()
        if raw:
            if raw.startswith(("EVENT,", "识别成功", "ERROR,", "PROGRESS,")):
                log(f"SERIAL {raw}")
            line_queue.put(raw)

async def broadcast(line: str) -> None:
    if not line.startswith("EVENT,"): return
    at = line.find("{")
    if at < 0: return
    try:
        payload = json.loads(line[at:])
        if payload.get("t") != "r": return
        code = payload.get("c")
        action = {1: "neck_extension", 2: "chin_tuck", 3: "head_resistance"}.get(code)
        if not action: return
        event = {"eventId": f"PECKY-BRIDGE-{payload['q']}", "deviceId": "PECKY-USB-BRIDGE", "sequence": payload["q"], "peckCount": 1, "amountDelta": 1, "action": action, "occurredAt": __import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat()}
        message = json.dumps({"type": "event", "event": event})
        await asyncio.gather(*(client.send(message) for client in clients), return_exceptions=True)
    except (KeyError, ValueError, TypeError): pass

async def handler(socket) -> None:
    first = not clients; clients.add(socket)
    log("APP_CONNECTED")
    if first: command("START")
    try: await socket.wait_closed()
    finally:
        clients.discard(socket)
        if not clients: command("PAUSE")
        log("APP_DISCONNECTED")

async def main() -> None:
    global device
    device = serial.Serial(port=None, baudrate=BAUD, timeout=.25, write_timeout=1)
    device.dtr = device.rts = False; device.port = PORT; device.open(); device.reset_input_buffer()
    threading.Thread(target=reader, daemon=True).start()
    log("BRIDGE_READY ws://127.0.0.1:8765 COM7")
    async with serve(handler, "127.0.0.1", 8765):
        while True:
            try: line = await asyncio.to_thread(line_queue.get, True, .5); await broadcast(line)
            except queue.Empty: pass

asyncio.run(main())
