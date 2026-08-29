#!/usr/bin/env python3
"""可视化口令引导的 RFP602 压力通道验收。"""

from __future__ import annotations

import csv
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import capture_motion as capture

try:
    import winsound
except ImportError:  # pragma: no cover
    winsound = None


PHASES = (
    ("release_1", "完全松开压力片", 5),
    ("light_hold", "轻轻按住圆形区域", 5),
    ("release_2", "完全松开压力片", 5),
    ("normal_hold_1", "用正常训练力度按住", 5),
    ("release_3", "完全松开压力片", 5),
    ("firm_hold", "较用力但不要过度地按住", 5),
    ("release_4", "完全松开压力片", 5),
    ("normal_hold_2", "再次用正常训练力度按住", 5),
    ("release_5", "完全松开压力片", 5),
    ("normal_hold_3", "第三次用正常训练力度按住", 5),
    ("final_release", "最后完全松开压力片", 5),
)


def announce(text: str) -> None:
    if winsound is not None:
        winsound.Beep(1000, 250)
    print("\n" + "=" * 58, flush=True)
    print(f"现在：{text}", flush=True)
    print("=" * 58, flush=True)


def main() -> int:
    output_dir = Path(__file__).resolve().parent.parent / "data" / "raw"
    output_path, session_id = capture.make_output_path(
        output_dir, "bench", "pressure_guided_visible"
    )
    fieldnames = (*capture.CSV_FIELDS, "test_phase", "phase_elapsed_s")

    print("Pecky 压力传感器引导测试", flush=True)
    print("正在打开 COM7。校准完成前：不要碰板子、IMU 和压力片。", flush=True)
    try:
        connection = capture.open_read_only_serial("COM7", 460800)
    except Exception as exc:
        print(f"无法打开 COM7：{exc}", file=sys.stderr, flush=True)
        return 2

    try:
        calibration_deadline = time.monotonic() + 20
        while time.monotonic() < calibration_deadline:
            line = connection.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("ERROR,"):
                print(line, file=sys.stderr, flush=True)
            if line.startswith("HEADER,"):
                break
        else:
            print("20 秒内没有等到固件校准完成。", file=sys.stderr, flush=True)
            return 3

        sample_count = 0
        malformed_count = 0
        with output_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
            writer.writeheader()
            test_started = time.monotonic()

            for phase_name, instruction, duration in PHASES:
                announce(instruction)
                phase_started = time.monotonic()
                next_countdown = duration
                while time.monotonic() - phase_started < duration:
                    remaining = duration - (time.monotonic() - phase_started)
                    shown = max(1, int(remaining + 0.999))
                    if shown <= next_countdown:
                        print(f"  剩余 {shown} 秒", flush=True)
                        next_countdown = shown - 1

                    line = connection.readline().decode(
                        "utf-8", errors="replace"
                    ).strip()
                    parsed = capture.parse_data_line(line)
                    if parsed is None:
                        if line.startswith("DATA,"):
                            malformed_count += 1
                        continue
                    received = time.monotonic()
                    writer.writerow(
                        {
                            "session_id": session_id,
                            "captured_at": datetime.now(timezone.utc)
                            .astimezone()
                            .isoformat(timespec="milliseconds"),
                            "host_elapsed_s": f"{received - test_started:.6f}",
                            "subject": "bench",
                            "label": "pressure_guided_visible",
                            "port": "COM7",
                            "baudrate": 460800,
                            **parsed,
                            "test_phase": phase_name,
                            "phase_elapsed_s": f"{received - phase_started:.6f}",
                        }
                    )
                    sample_count += 1
            csv_file.flush()

        announce("测试完成，请不要再按压力片")
        print(f"有效样本：{sample_count}；异常行：{malformed_count}", flush=True)
        print(f"数据文件：{output_path}", flush=True)
        print("窗口将在 5 秒后关闭。", flush=True)
        time.sleep(5)
        return 0 if sample_count >= 5000 and malformed_count == 0 else 4
    finally:
        connection.close()


if __name__ == "__main__":
    raise SystemExit(main())
