#!/usr/bin/env python3
"""从 ESP32 动作识别固件只读采集 MPU6050 数据并保存为 CSV。

当前固件输出协议（共 18 个逗号分隔字段）：
DATA,seq,t_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,temp_c,pitch_deg,roll_deg,acc_mag_g,pressure_raw,pressure_filtered,pressure_baseline,pressure_delta,pressure_state

本工具只读取串口：不会向开发板发送命令，也不会主动复位硬件。
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import serial
except ImportError:  # pragma: no cover - 仅在用户环境缺少依赖时触发
    print(
        "错误：未安装 pyserial。请先执行：python -m pip install pyserial",
        file=sys.stderr,
    )
    raise SystemExit(2)


BAUDRATE = 115200
DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "data" / "raw"

LEGACY_DEVICE_FIELDS = (
    "device_t_ms",
    "ax_g",
    "ay_g",
    "az_g",
    "gx_dps",
    "gy_dps",
    "gz_dps",
    "temp_c",
    "pitch_deg",
    "roll_deg",
    "acc_mag_g",
)

DEVICE_FIELDS = (
    "seq",
    *LEGACY_DEVICE_FIELDS,
    "pressure_raw",
    "pressure_filtered",
    "pressure_baseline",
    "pressure_delta",
    "pressure_state",
)

CSV_FIELDS = (
    "session_id",
    "captured_at",
    "host_elapsed_s",
    "subject",
    "label",
    "port",
    "baudrate",
    *DEVICE_FIELDS,
)


def positive_seconds(value: str) -> float:
    """argparse 类型：只接受大于 0 的采集时长。"""
    try:
        seconds = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("时长必须是数字") from exc
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("时长必须大于 0 秒")
    return seconds


def non_empty(value: str) -> str:
    """argparse 类型：拒绝空标签或空受试者编号。"""
    cleaned = value.strip()
    if not cleaned:
        raise argparse.ArgumentTypeError("不能是空字符串")
    return cleaned


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="只读采集 ESP32/MPU6050 动作数据并写入 CSV。"
    )
    parser.add_argument("--port", default="COM7", help="串口名称（默认：COM7）")
    parser.add_argument("--label", required=True, type=non_empty, help="动作标签")
    parser.add_argument(
        "--seconds",
        type=positive_seconds,
        default=15.0,
        help="采集时长，单位秒（默认：15）",
    )
    parser.add_argument(
        "--subject",
        type=non_empty,
        default="unknown",
        help="受试者编号（默认：unknown）",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"CSV 输出目录（默认：{DEFAULT_OUTPUT}）",
    )
    return parser.parse_args()


def safe_filename_fragment(value: str, fallback: str) -> str:
    """将用户输入变成适合 Windows/Linux 的短文件名片段。"""
    value = re.sub(r"[^0-9A-Za-z_\-\u4e00-\u9fff]+", "_", value.strip())
    value = value.strip("._-")
    return (value[:60] or fallback)


def make_output_path(output_dir: Path, subject: str, label: str) -> tuple[Path, str]:
    output_dir = output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    safe_subject = safe_filename_fragment(subject, "unknown")
    safe_label = safe_filename_fragment(label, "unlabeled")
    session_id = f"{timestamp}_{safe_subject}_{safe_label}"
    return output_dir / f"motion_{session_id}.csv", session_id


def parse_data_line(line: str) -> dict[str, int | float | str | None] | None:
    """解析一行 DATA；非 DATA、字段错误或非有限数值均返回 None。"""
    if not line.startswith("DATA,"):
        # INFO、HEADER、ESP-ROM 启动日志和空行都在这里被安全忽略。
        return None

    parts = [part.strip() for part in line.split(",")]
    try:
        if len(parts) == 18:
            seq = int(parts[1], 10)
            device_t_ms = int(parts[2], 10)
            floats = [float(value) for value in parts[3:17]]
            pressure_state = parts[17]
            if pressure_state not in {"PRESSED", "RELEASED"}:
                return None
            if seq < 0 or device_t_ms < 0 or not all(
                math.isfinite(value) for value in floats
            ):
                return None
            return dict(zip(DEVICE_FIELDS, [seq, device_t_ms, *floats, pressure_state]))

        if len(parts) == 12:
            device_t_ms = int(parts[1], 10)
            floats = [float(value) for value in parts[2:]]
            if device_t_ms < 0 or not all(math.isfinite(value) for value in floats):
                return None
            legacy = dict(zip(LEGACY_DEVICE_FIELDS, [device_t_ms, *floats]))
            return {
                "seq": None,
                **legacy,
                "pressure_raw": None,
                "pressure_filtered": None,
                "pressure_baseline": None,
                "pressure_delta": None,
                "pressure_state": None,
            }
    except ValueError:
        return None

    return None


def looks_like_port_busy(exc: BaseException) -> bool:
    message = str(exc).lower()
    busy_markers = (
        "access is denied",
        "permissionerror",
        "permission denied",
        "resource busy",
        "拒绝访问",
        "设备正在使用",
    )
    return any(marker in message for marker in busy_markers)


def open_read_only_serial(port: str) -> serial.Serial:
    """打开只读串口；在 open 前保持 DTR/RTS 为低，避免主动复位板卡。"""
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = BAUDRATE
    connection.bytesize = serial.EIGHTBITS
    connection.parity = serial.PARITY_NONE
    connection.stopbits = serial.STOPBITS_ONE
    connection.timeout = 0.25
    connection.write_timeout = 0

    # 必须在 open() 前设置。脚本从不调用 write()，也不脉冲 DTR/RTS。
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def estimate_rate(
    sample_count: int,
    first_device_ms: int | None,
    last_device_ms: int | None,
    first_host_time: float | None,
    last_host_time: float | None,
) -> float:
    if sample_count < 2:
        return 0.0

    if (
        first_device_ms is not None
        and last_device_ms is not None
        and last_device_ms > first_device_ms
    ):
        return (sample_count - 1) * 1000.0 / (last_device_ms - first_device_ms)

    if (
        first_host_time is not None
        and last_host_time is not None
        and last_host_time > first_host_time
    ):
        return (sample_count - 1) / (last_host_time - first_host_time)

    return 0.0


def print_summary(
    output_path: Path,
    sample_count: int,
    malformed_count: int,
    sample_rate_hz: float,
    interrupted: bool,
) -> None:
    print("\n采集结束。" if not interrupted else "\n已收到 Ctrl+C，数据已安全保存。")
    print(f"有效样本数：{sample_count}")
    print(f"估算采样率：{sample_rate_hz:.2f} Hz")
    if malformed_count:
        print(f"忽略的异常 DATA 行：{malformed_count}")
    print(f"CSV 绝对路径：{output_path.resolve()}")


def collect(args: argparse.Namespace) -> int:
    try:
        output_path, session_id = make_output_path(
            args.output, args.subject, args.label
        )
    except OSError as exc:
        print(
            f"错误：无法创建输出目录 {args.output.expanduser()}。\n"
            f"系统信息：{exc}",
            file=sys.stderr,
        )
        return 2

    try:
        connection = open_read_only_serial(args.port)
    except (serial.SerialException, OSError) as exc:
        if looks_like_port_busy(exc):
            print(
                f"错误：串口 {args.port} 被占用或没有访问权限。\n"
                "请关闭 PuTTY、Arduino 串口监视器和其他占用该端口的软件后重试。",
                file=sys.stderr,
            )
        else:
            print(
                f"错误：无法打开串口 {args.port}。请检查 USB 连接和端口号。\n"
                f"系统信息：{exc}",
                file=sys.stderr,
            )
        return 2

    sample_count = 0
    malformed_count = 0
    first_device_ms: int | None = None
    last_device_ms: int | None = None
    first_host_time: float | None = None
    last_host_time: float | None = None
    interrupted = False
    read_failed = False
    output_failed = False
    started = time.monotonic()
    deadline = started + args.seconds

    print(
        f"开始采集：端口={args.port}，标签={args.label}，"
        f"受试者={args.subject}，时长={args.seconds:g}s"
    )
    print("提示：按 Ctrl+C 可提前结束，已采集数据仍会保存。")

    try:
        with output_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()

            try:
                while time.monotonic() < deadline:
                    raw_line = connection.readline()
                    if not raw_line:
                        continue

                    line = raw_line.decode("utf-8", errors="replace").strip()
                    parsed = parse_data_line(line)
                    if parsed is None:
                        if line.startswith("DATA,"):
                            malformed_count += 1
                        continue

                    received = time.monotonic()
                    device_t_ms = int(parsed["device_t_ms"])
                    if first_device_ms is None:
                        first_device_ms = device_t_ms
                        first_host_time = received
                    last_device_ms = device_t_ms
                    last_host_time = received

                    row: dict[str, Any] = {
                        "session_id": session_id,
                        "captured_at": datetime.now(timezone.utc)
                        .astimezone()
                        .isoformat(timespec="milliseconds"),
                        "host_elapsed_s": f"{received - started:.6f}",
                        "subject": args.subject,
                        "label": args.label,
                        "port": args.port,
                        "baudrate": BAUDRATE,
                        **parsed,
                    }
                    writer.writerow(row)
                    sample_count += 1

                    # 周期性落盘，意外断线时也尽量保留最近数据。
                    if sample_count % 100 == 0:
                        csv_file.flush()

            except KeyboardInterrupt:
                interrupted = True
            except serial.SerialException as exc:
                read_failed = True
                print(f"\n错误：采集中串口连接中断：{exc}", file=sys.stderr)
            finally:
                csv_file.flush()
    except (OSError, csv.Error) as exc:
        output_failed = True
        print(f"\n错误：CSV 写入失败：{exc}", file=sys.stderr)
    finally:
        if connection.is_open:
            connection.close()

    if output_failed:
        print(f"未能完整保存到：{output_path.resolve()}", file=sys.stderr)
        return 5

    rate = estimate_rate(
        sample_count,
        first_device_ms,
        last_device_ms,
        first_host_time,
        last_host_time,
    )
    print_summary(output_path, sample_count, malformed_count, rate, interrupted)

    if read_failed:
        return 3
    if sample_count == 0:
        print(
            "警告：没有收到有效 DATA。请确认固件正在以 115200 波特率持续输出。",
            file=sys.stderr,
        )
        return 4
    return 0


def main() -> int:
    return collect(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
