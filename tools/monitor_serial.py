"""Monitor and validate the ESP32-S3 + MPU60x0/MPU6500 hardware demo."""

from __future__ import annotations

import argparse
import sys
import time

import serial
from serial.tools import list_ports


def choose_port(requested: str | None) -> str:
    ports = list(list_ports.comports())
    if requested:
        return requested

    if not ports:
        raise RuntimeError("没有发现串口。请确认 USB 数据线已连接到开发板 COM 口。")

    likely = [
        port
        for port in ports
        if any(
            marker in f"{port.description} {port.manufacturer} {port.hwid}".lower()
            for marker in ("usb", "ch340", "ch343", "cp210", "serial", "jtag")
        )
    ]
    candidates = likely or ports
    if len(candidates) != 1:
        details = "\n".join(
            f"  {port.device}: {port.description} ({port.hwid})" for port in ports
        )
        raise RuntimeError(
            "发现多个串口，请使用 --port COMx 指定：\n" + details
        )
    return candidates[0].device


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="例如 COM7；省略时自动选择")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    try:
        port = choose_port(args.port)
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    print(f"打开 {port}，波特率 {args.baud}，监视 {args.seconds:g} 秒")
    saw_address = False
    saw_identity = False
    saw_ready = False
    error_lines: list[str] = []
    timestamps: list[int] = []

    try:
        with serial.Serial(port, args.baud, timeout=0.25) as device:
            device.reset_input_buffer()
            device.dtr = False
            device.rts = False
            deadline = time.monotonic() + args.seconds

            while time.monotonic() < deadline:
                raw = device.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line)

                saw_address |= line == "INFO,I2C_DEVICE,0x68"
                saw_identity |= line in {
                    "INFO,WHO_AM_I,0x68",
                    "INFO,WHO_AM_I,0x69",
                    "INFO,WHO_AM_I,0x70",
                }
                saw_ready |= line == "INFO,IMU_READY"
                if line.startswith("ERROR,"):
                    error_lines.append(line)
                if line.startswith("DATA,"):
                    fields = line.split(",")
                    if len(fields) == 12:
                        try:
                            timestamps.append(int(fields[1]))
                            for value in fields[2:]:
                                float(value)
                        except ValueError:
                            error_lines.append("ERROR,INVALID_DATA_LINE")
    except serial.SerialException as error:
        print(f"ERROR: 无法打开串口 {port}: {error}", file=sys.stderr)
        print("请关闭 PuTTY 和 Arduino Serial Monitor 后重试。", file=sys.stderr)
        return 2

    sample_rate = 0.0
    if len(timestamps) >= 2 and timestamps[-1] > timestamps[0]:
        sample_rate = 1000.0 * (len(timestamps) - 1) / (
            timestamps[-1] - timestamps[0]
        )

    print("\n=== 硬件自检结果 ===")
    print(f"I2C 地址 0x68: {'通过' if saw_address else '未发现'}")
    print(f"WHO_AM_I: {'通过' if saw_identity else '未通过'}")
    print(f"IMU 初始化: {'通过' if saw_ready else '未通过'}")
    print(f"有效 DATA 行: {len(timestamps)}")
    print(f"估算采样率: {sample_rate:.1f} Hz")
    if error_lines:
        print("错误：")
        for line in sorted(set(error_lines)):
            print(f"  {line}")

    passed = (
        saw_address
        and saw_identity
        and saw_ready
        and len(timestamps) >= 100
        and 90.0 <= sample_rate <= 110.0
        and not error_lines
    )
    print("结论：" + ("硬件链路通过" if passed else "硬件链路未完全通过"))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
