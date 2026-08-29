"""Terminal-owned COM8 bridge for the Qingxian chair web app.

The first connected app client starts chair recognition. The last one pauses
it. Every recognized chair action is forwarded over localhost WebSocket so the
app can both credit a grain and display its two-second action caption.
"""
from __future__ import annotations

import asyncio
import datetime
import json
import os
import queue
import threading

import serial
from websockets.asyncio.server import serve

PORT, BAUD, clients = "COM8", 115200, set()
line_queue: queue.Queue[str] = queue.Queue()
device: serial.Serial | None = None
LOG_PATH = os.path.join(os.environ.get("TEMP", "."), "pecky-chair-serial-bridge.log")
ACTIONS = {1: ("left_stretch", "向左拉伸"), 2: ("right_stretch", "向右拉伸"), 3: ("chest_extension", "胸椎舒展")}


def log(message: str) -> None:
    line = f"{datetime.datetime.now().isoformat(timespec='seconds')} {message}"
    print(line, flush=True)
    with open(LOG_PATH, "a", encoding="utf-8") as output:
        output.write(line + "\n")


def command(value: str) -> None:
    if device:
        device.write(f"{value}\n".encode("ascii"))
        device.flush()
        log(f"COMMAND {value}")


def reader() -> None:
    while device and device.is_open:
        raw = device.readline().decode("utf-8", "replace").strip()
        if raw:
            if raw.startswith(("ACTION,", "STATUS,", "ERROR,")):
                log(f"SERIAL {raw}")
            line_queue.put(raw)


async def broadcast(line: str) -> None:
    if not line.startswith("ACTION,"):
        return
    try:
        _, sequence_text, code_text = line.split(",", 2)
        sequence, code = int(sequence_text), int(code_text)
        action = ACTIONS.get(code)
        if sequence < 0 or action is None:
            return
        action_id, label = action
        # Chair actions deliberately have no `action` field in the monetary
        # event: only cap actions are part of the persisted PeckyAction enum.
        event = {
            "eventId": f"QINGXIAN-CHAIR-BRIDGE-{sequence}",
            "deviceId": "QINGXIAN-CHAIR-USB-BRIDGE",
            "sequence": sequence,
            "peckCount": 1,
            "amountDelta": 1,
            "occurredAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        }
        message = json.dumps({"type": "event", "event": event, "recognizedAction": {"action": action_id, "label": label}})
        await asyncio.gather(*(client.send(message) for client in clients), return_exceptions=True)
        log(f"EVENT_BROADCAST {sequence} {action_id}")
    except (ValueError, TypeError):
        return


async def handler(socket) -> None:
    first = not clients
    clients.add(socket)
    log("APP_CONNECTED")
    if first:
        command("START")
    try:
        await socket.wait_closed()
    finally:
        clients.discard(socket)
        if not clients:
            command("PAUSE")
        log("APP_DISCONNECTED")


async def main() -> None:
    global device
    device = serial.Serial(port=None, baudrate=BAUD, timeout=.25, write_timeout=1)
    device.dtr = device.rts = False
    device.port = PORT
    device.open()
    device.reset_input_buffer()
    threading.Thread(target=reader, daemon=True).start()
    log("CHAIR_BRIDGE_READY ws://127.0.0.1:8766 COM8")
    async with serve(handler, "127.0.0.1", 8766):
        while True:
            try:
                line = await asyncio.to_thread(line_queue.get, True, .5)
                await broadcast(line)
            except queue.Empty:
                pass


asyncio.run(main())
