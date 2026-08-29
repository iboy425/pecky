"""Control Pecky cap (COM7) and Qingxian chair (COM8) from one terminal.

Examples:
  py -3 tools/control_devices.py all status
  py -3 tools/control_devices.py hat start
  py -3 tools/control_devices.py chair pause
  py -3 tools/control_devices.py all calibrate
"""
from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("请安装 pyserial：py -3 -m pip install pyserial") from exc

PORTS = {"hat": "COM7", "chair": "COM8"}
COMMANDS = {"start": "START", "pause": "PAUSE", "status": "STATUS", "calibrate": "CALIBRATE"}

def run(device: str, action: str) -> int:
    targets = PORTS.items() if device == "all" else [(device, PORTS[device])]
    for name, port in targets:
        try:
            # ESP32-S3 dev boards commonly wire DTR/RTS to EN/BOOT. Set both
            # inactive *before* opening, otherwise each terminal command can
            # reset a board and silently cancel the session being controlled.
            connection = serial.Serial(port=None, baudrate=115200, timeout=0.35, write_timeout=1)
            connection.dtr = False
            connection.rts = False
            connection.port = port
            connection.open()
            with connection:
                connection.reset_input_buffer()
                connection.write(f"{COMMANDS[action]}\n".encode("ascii"))
                connection.flush()
                deadline = time.monotonic() + (6 if action == "calibrate" else 1.5)
                lines: list[str] = []
                while time.monotonic() < deadline:
                    line = connection.readline().decode("utf-8", "replace").strip()
                    if line:
                        lines.append(line)
                        if line.startswith(("CONTROL,", "STATUS,", "ERROR,")):
                            break
                print(f"{name} ({port}): {lines[-1] if lines else '未收到响应'}")
        except (serial.SerialException, OSError) as exc:
            print(f"{name} ({port}): 无法连接：{exc}", file=sys.stderr)
            return 1
    return 0

parser = argparse.ArgumentParser(description="控制帽子和椅子的本地识别")
parser.add_argument("device", choices=("hat", "chair", "all"))
parser.add_argument("action", choices=tuple(COMMANDS))
args = parser.parse_args()
raise SystemExit(run(args.device, args.action))
