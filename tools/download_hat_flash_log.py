#!/usr/bin/env python3
"""Read a completed Pecky Cap CSV session back from ESP32 LittleFS.

The hat firmware keeps logging automatically on every boot. Therefore this
tool deliberately excludes the *currently active* file and downloads the most
recent previous session by default. It never erases flash storage.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("缺少 pyserial。请先执行：python -m pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "data" / "raw"
FILE_PATTERN = re.compile(r"^FILE,/?(hat_\d+\.csv),(\d+)$", re.MULTILINE)
ACTIVE_PATTERN = re.compile(r"^STATUS,.*FILE=(/hat_\d+\.csv),", re.MULTILINE)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="从 Pecky 帽子内部闪存下载已完成的 CSV 数据。")
    parser.add_argument("--port", default="COM7", help="串口，默认 COM7")
    parser.add_argument("--baud", type=int, default=115200, help="波特率，默认 115200")
    parser.add_argument("--file", help="指定下载，例如 /hat_0003.csv")
    parser.add_argument("--all", action="store_true", help="下载全部已完成会话")
    parser.add_argument("--list", action="store_true", help="只列出文件，不下载")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="本地保存目录")
    return parser.parse_args()


def open_port(port: str, baud: int) -> serial.Serial:
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = baud
    connection.timeout = 0.25
    connection.write_timeout = 2
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def read_until(connection: serial.Serial, marker: bytes, timeout_s: float) -> bytes:
    deadline = time.monotonic() + timeout_s
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = connection.read(512)
        if chunk:
            data.extend(chunk)
            if marker in data:
                return bytes(data)
    raise TimeoutError(f"等待设备响应超时：{marker.decode('utf-8', 'replace')}")


def send_and_read(connection: serial.Serial, command: str, marker: bytes, timeout_s: float = 8.0) -> bytes:
    connection.reset_input_buffer()
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    return read_until(connection, marker, timeout_s)


def list_files(connection: serial.Serial) -> tuple[str | None, list[tuple[str, int]]]:
    status = send_and_read(connection, "STATUS", b"STATUS,")
    status_text = status.decode("utf-8", errors="replace").replace("\r", "")
    active_match = ACTIVE_PATTERN.search(status_text)
    active_file = active_match.group(1) if active_match else None

    listing = send_and_read(connection, "LIST", b"FILES_END")
    listing_text = listing.decode("utf-8", errors="replace").replace("\r", "")
    files = [(f"/{name}", int(size)) for name, size in FILE_PATTERN.findall(listing_text)]
    return active_file, sorted(files)


def dump_file(connection: serial.Serial, filename: str) -> bytes:
    connection.reset_input_buffer()
    connection.write((f"DUMP {filename}\n").encode("ascii"))
    connection.flush()
    end_marker = f"FILE_END,{filename}".encode("ascii")
    # At 115200 baud a one-minute raw session can take around ten seconds;
    # older sessions may be larger, so leave generous headroom.
    response = read_until(connection, end_marker, timeout_s=90.0)
    header = f"FILE_BEGIN,{filename},".encode("ascii")
    header_start = response.find(header)
    if header_start < 0:
        raise RuntimeError(response.decode("utf-8", errors="replace"))
    data_start = response.find(b"\n", header_start) + 1
    line_start = response.rfind(b"\n", 0, response.find(end_marker))
    data_end = line_start
    if data_start <= 0 or data_end < data_start:
        raise RuntimeError("设备返回的文件边界不完整")
    return response[data_start:data_end]


def main() -> int:
    args = parse_args()
    try:
        connection = open_port(args.port, args.baud)
    except (serial.SerialException, OSError) as exc:
        print(f"无法打开 {args.port}：{exc}", file=sys.stderr)
        print("请关闭 PuTTY、Arduino 串口监视器等占用 COM7 的程序后重试。", file=sys.stderr)
        return 2

    try:
        # A USB connection can reset the board. Give its 3-second neutral
        # calibration time a little margin before requesting files.
        time.sleep(4.5)
        active_file, files = list_files(connection)
        if not files:
            print("芯片中没有找到 hat_XXXX.csv。请先让帽子上电采集一次。")
            return 3

        print("芯片内文件：")
        for name, size in files:
            suffix = "  <当前新会话，不作为默认下载目标>" if name == active_file else ""
            print(f"  {name:16} {size:8d} bytes{suffix}")

        if args.list:
            return 0

        completed = [(name, size) for name, size in files if name != active_file]
        if args.file and args.all:
            print("--file 与 --all 不能同时使用。", file=sys.stderr)
            return 4
        if args.file:
            targets = [args.file]
        elif args.all:
            targets = [name for name, _ in completed]
        elif completed:
            targets = [completed[-1][0]]
        else:
            print("还没有已完成会话。让帽子断电并完成一次采集后再下载。")
            return 4

        output_dir = args.output.expanduser().resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        known_names = {name for name, _ in files}
        for target in targets:
            if target not in known_names:
                print(f"找不到指定文件：{target}", file=sys.stderr)
                return 5
            content = dump_file(connection, target)
            if not content.startswith(b"seq,t_ms,"):
                print(f"下载内容不是预期的 CSV：{target}", file=sys.stderr)
                return 6
            output_file = output_dir / target.lstrip("/")
            output_file.write_bytes(content)
            rows = max(0, content.count(b"\n") - 1)
            print(f"已下载 {target}：{rows} 行 -> {output_file}")
        return 0
    except (TimeoutError, RuntimeError, serial.SerialException, OSError) as exc:
        print(f"下载失败：{exc}", file=sys.stderr)
        return 7
    finally:
        connection.close()


if __name__ == "__main__":
    raise SystemExit(main())
