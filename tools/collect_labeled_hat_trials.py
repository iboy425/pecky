#!/usr/bin/env python3
"""Pecky 帽子事件级动作采集器。

必须与 ``05_event_labeled_collector`` 固件配套。电脑端操作员在真实动作
边界按 Enter；开发板把当前 trial/action/stage 写入下一帧原始传感器数据。
这样等待、讲解和走动都保持 EXCLUDE，不会再污染动作标签。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import queue
import random
import re
import statistics
import sys
import threading
import time
from collections import Counter, defaultdict, deque
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

try:
    import serial
except ImportError:  # pragma: no cover
    serial = None

try:
    import winsound
except ImportError:  # pragma: no cover
    winsound = None


BAUDRATE = 115200
FIRMWARE_TOKEN = "PECKY_EVENT_COLLECTOR_V1"
DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "data" / "labeled"

STAGE_NAMES = {
    0: "exclude",
    1: "prepare",
    2: "action",
    3: "hold",
    4: "return",
    5: "rest",
}


@dataclass(frozen=True)
class ActionSpec:
    action_id: int
    name: str
    zh: str
    cue: str
    target: bool
    hold_seconds: float
    full_repetitions: int
    pilot_repetitions: int


ACTIONS = (
    ActionSpec(1, "neck_extension", "后仰脖子", "头向后仰；不要耸肩或转头", True, 2.0, 6, 2),
    ActionSpec(2, "chin_tuck", "收下巴", "头部水平向后缩出双下巴；不要低头", True, 2.0, 6, 2),
    ActionSpec(3, "head_resistance", "抱头抗阻", "双手抱后脑向前施力，头向后抗阻", True, 5.0, 6, 2),
    ActionSpec(10, "look_down", "负样本：低头", "只低头看下方，不要收下巴", False, 2.0, 3, 1),
    ActionSpec(11, "nod", "负样本：点头", "做一次自然点头", False, 0.5, 3, 1),
    ActionSpec(12, "turn_left", "负样本：左转头", "自然向左转头", False, 2.0, 3, 1),
    ActionSpec(13, "turn_right", "负样本：右转头", "自然向右转头", False, 2.0, 3, 1),
    ActionSpec(14, "shrug", "负样本：耸肩", "头保持正直，双肩上耸", False, 2.0, 3, 1),
    ActionSpec(15, "walk_in_place", "负样本：原地走", "戴帽原地自然走动", False, 5.0, 3, 1),
    ActionSpec(16, "drink", "负样本：喝水", "模拟一次自然喝水", False, 2.0, 3, 1),
    ActionSpec(17, "phone", "负样本：看手机", "自然低头看手机", False, 3.0, 3, 1),
    ActionSpec(18, "posture_adjust", "负样本：坐姿调整", "自然调整一次坐姿", False, 2.0, 3, 1),
)
ACTIONS_BY_ID = {item.action_id: item for item in ACTIONS}


@dataclass(frozen=True)
class Trial:
    number: int
    action_id: int
    repetition: int

    @property
    def spec(self) -> ActionSpec:
        return ACTIONS_BY_ID[self.action_id]


class UserAbort(RuntimeError):
    """操作员主动终止当前采集。"""


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def safe_fragment(value: str, fallback: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z_\-\u4e00-\u9fff]+", "_", value.strip())
    return cleaned.strip("._-")[:60] or fallback


def make_trial_plan(mode: str, seed: int) -> list[Trial]:
    selected = [item for item in ACTIONS if mode != "targets" or item.target]
    instances: list[int] = []
    for action in selected:
        repetitions = (
            action.full_repetitions if mode in {"full", "targets"}
            else action.pilot_repetitions
        )
        instances.extend([action.action_id] * repetitions)

    rng = random.Random(seed)
    for _ in range(1000):
        rng.shuffle(instances)
        if all(
            not (instances[i] == instances[i - 1] == instances[i - 2])
            for i in range(2, len(instances))
        ):
            break
    repetitions_seen: Counter[int] = Counter()
    trials: list[Trial] = []
    for number, action_id in enumerate(instances, start=1):
        repetitions_seen[action_id] += 1
        trials.append(Trial(number, action_id, repetitions_seen[action_id]))
    return trials


def parse_raw(line: str) -> dict[str, int] | None:
    parts = line.split(",")
    if len(parts) != 15 or parts[0] != "RAW":
        return None
    try:
        values = [int(value, 10) for value in parts[1:]]
    except ValueError:
        return None
    keys = (
        "device_ms", "seq", "trial", "action_id", "repetition", "stage_id",
        "ax_raw", "ay_raw", "az_raw", "gx_raw", "gy_raw", "gz_raw",
        "pressure_raw", "dt_ms",
    )
    return dict(zip(keys, values))


def parse_key_values(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for part in line.split(",")[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            result[key] = value
    return result


class Device:
    def __init__(self, port: str, output_path: Path | None = None) -> None:
        if serial is None or not hasattr(serial, "Serial"):
            raise RuntimeError(
                "当前安装的是错误的 serial 包。请执行："
                "py -3 -m pip uninstall -y serial && py -3 -m pip install pyserial==3.5"
            )
        self.connection = serial.Serial(port, BAUDRATE, timeout=0.20, write_timeout=1)
        self.port = port
        self.started_monotonic = time.monotonic()
        self.output_path = output_path
        self._messages: queue.Queue[str] = queue.Queue()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._samples: deque[tuple[int, dict[str, int]]] = deque(maxlen=600)
        self._sample_index = 0
        self._csv_file = None
        self._writer = None
        self.rows_written = 0
        self.malformed_rows = 0
        self.sequence_gaps = 0
        self.large_time_gaps = 0
        self.good_dt_rows = 0
        self.stage_counts: Counter[tuple[int, int]] = Counter()
        self.device_first_ms: int | None = None
        self.device_last_ms: int | None = None
        self._last_seq: int | None = None
        self.calibration: dict[str, str] = {}
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            try:
                line = self.connection.readline().decode("utf-8", errors="replace").strip()
            except Exception as exc:  # pragma: no cover - hardware failure
                self._messages.put(f"HOST_SERIAL_ERROR,{exc}")
                return
            if not line:
                continue
            parsed = parse_raw(line)
            if parsed is None:
                if line.startswith("RAW,"):
                    self.malformed_rows += 1
                self._messages.put(line)
                continue
            self._record_sample(parsed)

    def _record_sample(self, sample: dict[str, int]) -> None:
        received = now_iso()
        with self._lock:
            self._sample_index += 1
            self._samples.append((self._sample_index, sample.copy()))
            if self._last_seq is not None and sample["seq"] != self._last_seq + 1:
                self.sequence_gaps += max(1, sample["seq"] - self._last_seq - 1)
            self._last_seq = sample["seq"]
            if 35 <= sample["dt_ms"] <= 45:
                self.good_dt_rows += 1
            if sample["dt_ms"] > 80:
                self.large_time_gaps += 1
            self.device_first_ms = self.device_first_ms or sample["device_ms"]
            self.device_last_ms = sample["device_ms"]
            self.stage_counts[(sample["trial"], sample["stage_id"])] += 1

            if self._writer is not None:
                action = ACTIONS_BY_ID.get(sample["action_id"])
                self._writer.writerow(
                    {
                        "host_received_at": received,
                        "host_elapsed_s": f"{time.monotonic() - self.started_monotonic:.6f}",
                        **sample,
                        "action_name": action.name if action else "exclude",
                        "stage_name": STAGE_NAMES.get(sample["stage_id"], "unknown"),
                        "ax_g": f"{sample['ax_raw'] / 16384.0:.7f}",
                        "ay_g": f"{sample['ay_raw'] / 16384.0:.7f}",
                        "az_g": f"{sample['az_raw'] / 16384.0:.7f}",
                        "gx_dps": f"{sample['gx_raw'] / 131.0:.7f}",
                        "gy_dps": f"{sample['gy_raw'] / 131.0:.7f}",
                        "gz_dps": f"{sample['gz_raw'] / 131.0:.7f}",
                        "pressure_delta": sample["pressure_raw"]
                        - int(self.calibration.get("P0", "0")),
                    }
                )
                self.rows_written += 1
                if self.rows_written % 25 == 0:
                    self._csv_file.flush()

    def start_csv(self) -> None:
        if self.output_path is None:
            return
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        fieldnames = (
            "host_received_at", "host_elapsed_s", "device_ms", "seq", "trial",
            "action_id", "action_name", "repetition", "stage_id", "stage_name",
            "ax_raw", "ay_raw", "az_raw", "gx_raw", "gy_raw", "gz_raw",
            "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps",
            "pressure_raw", "pressure_delta", "dt_ms",
        )
        with self._lock:
            self._csv_file = self.output_path.open("w", newline="", encoding="utf-8-sig")
            self._writer = csv.DictWriter(self._csv_file, fieldnames=fieldnames)
            self._writer.writeheader()
            self._csv_file.flush()

    def stop_csv(self) -> None:
        with self._lock:
            self._writer = None
            if self._csv_file is not None:
                self._csv_file.flush()
                self._csv_file.close()
                self._csv_file = None

    def send(self, command: str) -> None:
        self.connection.write((command + "\n").encode("ascii"))
        self.connection.flush()

    def wait_for(self, predicate: Callable[[str], bool], timeout: float = 8.0) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._messages.get(timeout=min(0.25, deadline - time.monotonic()))
            except queue.Empty:
                continue
            if line.startswith(("ERROR,", "NACK,", "HOST_SERIAL_ERROR,")):
                raise RuntimeError(f"设备返回错误：{line}")
            if predicate(line):
                return line
        raise TimeoutError("等待开发板响应超时")

    def command(self, command: str, expected_prefix: str, timeout: float = 8.0) -> str:
        self.send(command)
        return self.wait_for(lambda line: line.startswith(expected_prefix), timeout)

    def sample_marker(self) -> int:
        with self._lock:
            return self._sample_index

    def samples_after(self, marker: int) -> list[dict[str, int]]:
        with self._lock:
            return [sample.copy() for index, sample in self._samples if index > marker]

    def close(self) -> None:
        self.stop_csv()
        self._stop.set()
        try:
            self.connection.close()
        finally:
            self._reader.join(timeout=1.0)

    def summary(self) -> dict[str, object]:
        duration_ms = (
            (self.device_last_ms - self.device_first_ms)
            if self.device_first_ms is not None and self.device_last_ms is not None
            else 0
        )
        sample_rate = (
            (self.rows_written - 1) * 1000.0 / duration_ms
            if self.rows_written > 1 and duration_ms > 0
            else 0.0
        )
        return {
            "rows": self.rows_written,
            "sample_rate_hz": round(sample_rate, 3),
            "good_dt_ratio": round(self.good_dt_rows / max(1, self.rows_written), 4),
            "large_time_gaps": self.large_time_gaps,
            "sequence_gaps": self.sequence_gaps,
            "malformed_rows": self.malformed_rows,
        }


def beep(frequency: int = 1000, duration_ms: int = 220) -> None:
    if winsound is not None:
        winsound.Beep(frequency, duration_ms)
    else:  # pragma: no cover - Windows is the intended COM environment
        print("\a", end="", flush=True)


def countdown(seconds: float, message: str) -> None:
    print(message, flush=True)
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        remaining = max(0, math.ceil(deadline - time.monotonic()))
        if remaining != last:
            print(f"  {remaining}...", flush=True)
            last = remaining
        time.sleep(0.05)


def boundary(prompt: str) -> None:
    value = input(f"{prompt}（Enter=继续，x=终止整场）：").strip().lower()
    if value == "x":
        raise UserAbort("操作员终止采集")


def median_pressure(samples: list[dict[str, int]]) -> float:
    values = [sample["pressure_raw"] for sample in samples]
    if len(values) < 20:
        raise RuntimeError(f"压力测试样本不足：{len(values)}")
    return float(statistics.median(values))


def pressure_check(device: Device) -> dict[str, object]:
    print("\n--- 压力通道主动检查（这部分自动标为 EXCLUDE）---")
    results: dict[str, object] = {}
    markers: list[int] = []
    for instruction in (
        "完全松开帽内压力片，按 Enter 后保持 2 秒",
        "用抱头抗阻时的力度压住压力片，按 Enter 后保持 2 秒",
        "再次完全松开，按 Enter 后保持 2 秒",
    ):
        boundary(instruction)
        marker = device.sample_marker()
        countdown(2.0, "保持不动")
        markers.append(marker)

    released_1 = device.samples_after(markers[0])
    pressed = [s for s in device.samples_after(markers[1]) if s["device_ms"] <=
               (device.samples_after(markers[1])[0]["device_ms"] + 2200)]
    released_2 = [s for s in device.samples_after(markers[2]) if s["device_ms"] <=
                  (device.samples_after(markers[2])[0]["device_ms"] + 2200)]
    base = median_pressure(released_1[:55])
    press = median_pressure(pressed[:55])
    returned = median_pressure(released_2[:55])
    delta = press - base
    noise = max(1.0, float(device.calibration.get("PMAD", "1")))
    threshold = max(100.0, 5.0 * noise)
    return_error = abs(returned - base)
    passed = abs(delta) >= threshold and return_error <= max(150.0, abs(delta) * 0.35)
    results.update(
        baseline_median=base,
        pressed_median=press,
        released_again_median=returned,
        delta=delta,
        threshold=threshold,
        return_error=return_error,
        passed=passed,
        polarity="rising" if delta >= 0 else "falling",
    )
    print(
        f"压力检查：基线 {base:.0f}，按压 {press:.0f}，变化 {delta:+.0f}，"
        f"释放后 {returned:.0f} -> {'PASS' if passed else 'FAIL'}"
    )
    return results


def set_label(device: Device, command_id: int, trial: Trial | None, stage: int) -> int:
    if trial is None:
        values = (0, 0, 0)
    else:
        values = (trial.number, trial.action_id, trial.repetition)
    command = f"LABEL,{command_id},{values[0]},{values[1]},{values[2]},{stage}"
    device.command(command, f"ACK,LABEL,{command_id},")
    return command_id + 1


def run_trial(device: Device, trial: Trial, command_id: int) -> int:
    spec = trial.spec
    print("\n" + "=" * 66)
    print(
        f"试次 {trial.number}：{spec.zh}  "
        f"({spec.name} 第 {trial.repetition} 次)"
    )
    print(f"动作要求：{spec.cue}")
    print("=" * 66)

    command_id = set_label(device, command_id, trial, 1)
    countdown(2.0, "保持中立姿态，准备")
    boundary("观察员：表演者真正开始动作的瞬间按 Enter")
    command_id = set_label(device, command_id, trial, 2)
    boundary("观察员：动作到位的瞬间按 Enter")
    command_id = set_label(device, command_id, trial, 3)
    beep(1150)
    countdown(spec.hold_seconds, f"保持 {spec.hold_seconds:g} 秒")
    beep(800)
    print("现在回到中立姿态")
    command_id = set_label(device, command_id, trial, 4)
    boundary("观察员：完全回到中立姿态时按 Enter")
    command_id = set_label(device, command_id, trial, 5)
    countdown(2.0, "中立休息；不要提前做下一个动作")
    return set_label(device, command_id, None, 0)


def print_plan(trials: list[Trial]) -> None:
    counts = Counter(trial.action_id for trial in trials)
    print(f"本场共 {len(trials)} 个有效试次：")
    for action in ACTIONS:
        if counts[action.action_id]:
            print(f"  {action.zh}: {counts[action.action_id]} 次")
    print("顺序：" + " -> ".join(trial.spec.name for trial in trials))


def probe(port: str) -> int:
    print(f"只检查帽子串口 {port}，不会采集或擦除数据。")
    device = Device(port)
    try:
        time.sleep(1.8)
        hello = device.command("HELLO", "PONG,", timeout=5)
        status = device.command("STATUS", "STATUS,", timeout=3)
        print("设备握手：", hello)
        print("设备状态：", status)
        if FIRMWARE_TOKEN not in hello or "IMU=OK" not in status:
            print("自检失败：固件版本或 IMU 状态不正确。", file=sys.stderr)
            return 3
        print("串口和 IMU 自检通过。压力片需要佩戴后做主动按压测试。")
        return 0
    finally:
        device.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Pecky 帽子逐动作标注采集")
    parser.add_argument("--port", default="COM7", help="帽子串口（默认 COM7）")
    parser.add_argument("--subject", default="pilot01", help="匿名参与者编号")
    parser.add_argument("--mode", choices=("pilot", "full", "targets"), default="pilot")
    parser.add_argument("--seed", type=int, default=None, help="随机试次种子")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--probe", action="store_true", help="只握手和检查 IMU")
    parser.add_argument("--dry-run", action="store_true", help="只打印试次表")
    parser.add_argument("--skip-pressure-check", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.probe:
        return probe(args.port)

    seed = args.seed if args.seed is not None else int(time.time())
    trials = make_trial_plan(args.mode, seed)
    print_plan(trials)
    if args.dry_run:
        return 0

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    subject = safe_fragment(args.subject, "pilot")
    session_name = f"hat_event_{timestamp}_{subject}_{args.mode}"
    output_dir = args.output_dir.expanduser().resolve()
    csv_path = output_dir / f"{session_name}.csv"
    manifest_path = output_dir / f"{session_name}.manifest.json"
    device = Device(args.port, csv_path)
    command_id = 1
    started_at = now_iso()
    pressure_result: dict[str, object] | None = None
    completed_trials: list[int] = []
    aborted = False

    try:
        time.sleep(1.8)
        hello = device.command("HELLO", "PONG,", timeout=5)
        status = device.command("STATUS", "STATUS,", timeout=3)
        if FIRMWARE_TOKEN not in hello or "IMU=OK" not in status:
            raise RuntimeError(f"帽子握手失败：{hello} / {status}")

        print("\n请戴好帽子，坐直保持中立，双手不要碰帽子和压力片。")
        boundary("准备好后按 Enter，接下来 3 秒严格保持不动")
        device.send(f"CAL,{subject}")
        calibration_line = device.wait_for(
            lambda line: line.startswith(("CAL_OK,", "CAL_FAIL,")), timeout=8
        )
        if not calibration_line.startswith("CAL_OK,"):
            raise RuntimeError(f"静止校准失败，请重新运行：{calibration_line}")
        device.calibration = parse_key_values(calibration_line)
        print("静止校准通过：", calibration_line)

        device.start_csv()
        device.command(f"BEGIN,{subject},{session_name}", "ACK,BEGIN,")
        command_id = set_label(device, command_id, None, 0)

        if not args.skip_pressure_check:
            pressure_result = pressure_check(device)
            if not pressure_result["passed"]:
                raise RuntimeError(
                    "压力通道主动检查失败。请检查压力片位置和 GPIO4 接线后重新运行；"
                    "如果只采后仰/收下巴，可加 --skip-pressure-check。"
                )

        print("\n正式试次开始。建议一人佩戴、一人观察并按键。")
        for trial in trials:
            try:
                command_id = run_trial(device, trial, command_id)
                completed_trials.append(trial.number)
            except UserAbort:
                device.command(
                    f"ABORT,{command_id},{trial.number}", f"ACK,ABORT,{command_id},"
                )
                command_id += 1
                raise

    except (KeyboardInterrupt, UserAbort):
        aborted = True
        print("\n已终止本场；已经写入的原始数据仍会保留。")
    except Exception as exc:
        aborted = True
        print(f"\n采集失败：{exc}", file=sys.stderr)
    finally:
        try:
            status_line = device.command(f"END,{command_id}", f"ACK,END,{command_id},", 3)
        except Exception as exc:
            status_line = f"END_NOT_ACKNOWLEDGED:{exc}"
        time.sleep(0.25)
        device.stop_csv()
        summary = device.summary()
        device.close()

    expected_stages = {1, 2, 3, 4, 5}
    missing_stages = {
        str(trial.number): sorted(
            expected_stages
            - {stage for (number, stage), count in device.stage_counts.items()
               if number == trial.number and count > 0}
        )
        for trial in trials
        if trial.number in completed_trials
    }
    missing_stages = {key: value for key, value in missing_stages.items() if value}
    quality_pass = (
        not aborted
        and len(completed_trials) == len(trials)
        and not missing_stages
        and summary["sample_rate_hz"] >= 24.0
        and summary["sample_rate_hz"] <= 26.0
        and summary["good_dt_ratio"] >= 0.98
        and summary["large_time_gaps"] == 0
        and summary["sequence_gaps"] == 0
        and summary["malformed_rows"] == 0
    )
    manifest = {
        "protocol": FIRMWARE_TOKEN,
        "port": args.port,
        "subject": subject,
        "mode": args.mode,
        "seed": seed,
        "started_at": started_at,
        "finished_at": now_iso(),
        "aborted": aborted,
        "end_response": status_line,
        "calibration": device.calibration,
        "pressure_check": pressure_result,
        "trial_plan": [asdict(trial) | {"action_name": trial.spec.name}
                       for trial in trials],
        "completed_trials": completed_trials,
        "missing_stages": missing_stages,
        "stream_quality": summary,
        "quality_pass": quality_pass,
        "csv_path": str(csv_path),
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    print("\n--- 本场验收 ---")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"完成试次：{len(completed_trials)}/{len(trials)}")
    print(f"总体验收：{'PASS' if quality_pass else 'FAIL / 需检查'}")
    print(f"原始数据：{csv_path}")
    print(f"验收清单：{manifest_path}")
    return 0 if quality_pass else 4


if __name__ == "__main__":
    raise SystemExit(main())
