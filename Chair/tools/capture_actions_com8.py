#!/usr/bin/env python3
"""在固定 COM8 上采集清闲椅的 3 秒动作窗口。

本工具不会枚举、探测或尝试任何其他串口。它与
``Chair/firmware/data_collection`` 固件配套，只把完整动作窗口内的
``SAMPLE`` 记录写入 ``Chair/data`` 下的 CSV。
"""

from __future__ import annotations

import argparse
import csv
import math
import queue
import random
import re
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:  # pragma: no cover - 给未安装依赖时提供中文错误
    serial = None


PORT = "COM8"
BAUD = 115200
WINDOW_MS = 3000
FIRMWARE_PROTOCOL = "CHAIR_DATA_COLLECTION_V1"
CSV_SCHEMA = "CHAIR_ACTION_SAMPLES_V1"
OUTPUT_DIR = Path(__file__).resolve().parents[1] / "data"

ACTIONS = {
    "n": ("NEUTRAL", "neutral", "正常坐姿"),
    "l": ("LEFT", "left_stretch", "向左拉伸"),
    "r": ("RIGHT", "right_stretch", "向右拉伸"),
    "c": ("CHEST", "chest_extension", "胸椎舒展"),
}

ACTION_CUES = {
    "n": "身体居中坐直，肩膀放松，保持自然正常坐姿。",
    "l": "从正常坐姿向左侧拉伸，身体不要前倾或转身。",
    "r": "从正常坐姿向右侧拉伸，身体不要前倾或转身。",
    "c": "挺胸并向后收肩胛，做胸椎舒展，左右保持对称。",
}

SAMPLE_FIELDS = (
    "record_type",
    "t_ms",
    "frame_id",
    "window_id",
    "label",
    "window_elapsed_ms",
    "frame_span_ms",
    "hc1_cm",
    "hc2_cm",
    "hc3_cm",
    "hc4_cm",
    "hc5_cm",
    "range_valid_mask",
    "mpu_ok",
    "ax_g",
    "ay_g",
    "az_g",
    "gx_dps",
    "gy_dps",
    "gz_dps",
)

CSV_FIELDS = (
    "record_type",
    "csv_schema",
    "firmware_protocol",
    "participant_id",
    "session_id",
    "session_started_at",
    "capture_index",
    "collection_mode",
    "guided_seed",
    "guided_trial_index",
    "guided_trial_total",
    "action_repetition",
    "action_code",
    "action_name",
    "action_name_zh",
    "capture_started_at",
    "host_received_at",
    "port",
    "baud",
    "window_duration_ms",
    "usable_for_training",
    *SAMPLE_FIELDS[1:],
)


class ProtocolError(RuntimeError):
    """开发板返回的采集协议不符合预期。"""


@dataclass(frozen=True)
class GuidedTrial:
    action_code: str
    repetition: int


@dataclass(frozen=True)
class Sample:
    values: dict[str, str]
    received_at: str

    @property
    def window_id(self) -> int:
        return int(self.values["window_id"])

    @property
    def frame_id(self) -> int:
        return int(self.values["frame_id"])

    @property
    def label(self) -> str:
        return self.values["label"]


@dataclass(frozen=True)
class SerialEvent:
    line: str
    received_at: str
    sample: Sample | None = None
    error: Exception | None = None


def local_iso_now() -> str:
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def safe_id(value: str, fallback: str) -> str:
    """生成可放入文件名和 CSV 的匿名短编号。"""

    cleaned = re.sub(r"[^0-9A-Za-z_\-\u4e00-\u9fff]+", "_", value.strip())
    return cleaned.strip("._-")[:48] or fallback


def make_guided_plan(repetitions: int, seed: int) -> list[GuidedTrial]:
    """生成包含四种显式标签的、可由 seed 复现的随机顺序。"""

    if repetitions < 1:
        raise ValueError("repetitions 必须至少为 1")
    plan = [
        GuidedTrial(action_code=action_code, repetition=repetition)
        for repetition in range(1, repetitions + 1)
        for action_code in ACTIONS
    ]
    random.Random(seed).shuffle(plan)
    return plan


def parse_sample(line: str, received_at: str) -> Sample | None:
    """严格校验固件的固定位置 SAMPLE 行，保留原始数值文本。"""

    parts = line.split(",")
    if not parts or parts[0] != "SAMPLE":
        return None
    if len(parts) != len(SAMPLE_FIELDS):
        raise ProtocolError(
            f"SAMPLE 列数错误：应为 {len(SAMPLE_FIELDS)}，实际为 {len(parts)}"
        )

    values = dict(zip(SAMPLE_FIELDS, parts))
    try:
        integer_values = {
            name: int(values[name], 10)
            for name in (
                "t_ms",
                "frame_id",
                "window_id",
                "window_elapsed_ms",
                "frame_span_ms",
                "range_valid_mask",
                "mpu_ok",
            )
        }
        for name in (
            "hc1_cm",
            "hc2_cm",
            "hc3_cm",
            "hc4_cm",
            "hc5_cm",
            "ax_g",
            "ay_g",
            "az_g",
            "gx_dps",
            "gy_dps",
            "gz_dps",
        ):
            number = float(values[name])
            if not math.isfinite(number) and values[name].lower() != "nan":
                raise ValueError(f"{name} 不是有限值或 NaN")
    except ValueError as error:
        raise ProtocolError(f"SAMPLE 数值格式错误：{error}") from error

    if integer_values["window_id"] < 0:
        raise ProtocolError("SAMPLE window_id 不能为负数")
    if integer_values["t_ms"] < 0 or integer_values["frame_id"] < 0:
        raise ProtocolError("SAMPLE t_ms/frame_id 不能为负数")
    if integer_values["frame_span_ms"] < 0:
        raise ProtocolError("SAMPLE frame_span_ms 不能为负数")
    if not 0 <= integer_values["range_valid_mask"] <= 0x1F:
        raise ProtocolError("SAMPLE range_valid_mask 超出 5 路有效位范围")
    if integer_values["mpu_ok"] not in (0, 1):
        raise ProtocolError("SAMPLE mpu_ok 只能为 0 或 1")
    if values["label"] not in {"UNLABELED", "NEUTRAL", "LEFT", "RIGHT", "CHEST"}:
        raise ProtocolError(f"未知 SAMPLE 标签：{values['label']}")
    if integer_values["window_id"] == 0 and (
        values["label"] != "UNLABELED"
        or integer_values["window_elapsed_ms"] != -1
    ):
        raise ProtocolError("窗外 SAMPLE 必须使用 UNLABELED/-1")
    if integer_values["window_id"] > 0 and values["label"] == "UNLABELED":
        raise ProtocolError("窗口内 SAMPLE 不能标为 UNLABELED")

    return Sample(values=values, received_at=received_at)


class DeviceReader:
    """持续清空 COM8 输入，避免操作员等待时串口缓存堆积。"""

    def __init__(self, connection: object) -> None:
        self.connection = connection
        self.events: queue.Queue[SerialEvent] = queue.Queue()
        self.stop_requested = threading.Event()
        self.saw_valid_sample = threading.Event()
        self.malformed_samples = 0
        self.discarded_unlabeled = 0
        self._thread = threading.Thread(
            target=self._run, name="chair-com8-reader", daemon=True
        )
        self._thread.start()

    def _run(self) -> None:
        while not self.stop_requested.is_set():
            try:
                raw = self.connection.readline()
            except Exception as error:  # pragma: no cover - 需要硬件断连
                if not self.stop_requested.is_set():
                    self.events.put(
                        SerialEvent("", local_iso_now(), error=error)
                    )
                return

            if not raw:
                continue
            received_at = local_iso_now()
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            if line.startswith("SAMPLE,"):
                try:
                    sample = parse_sample(line, received_at)
                except ProtocolError:
                    self.malformed_samples += 1
                    continue
                if sample is None:
                    continue
                self.saw_valid_sample.set()
                if sample.window_id == 0 and sample.label == "UNLABELED":
                    self.discarded_unlabeled += 1
                    continue
                self.events.put(SerialEvent(line, received_at, sample=sample))
                continue

            self.events.put(SerialEvent(line, received_at))

    def get(self, timeout: float) -> SerialEvent:
        event = self.events.get(timeout=timeout)
        if event.error is not None:
            raise RuntimeError(f"COM8 读取失败：{event.error}") from event.error
        return event

    def drain(self) -> None:
        while True:
            try:
                self.events.get_nowait()
            except queue.Empty:
                return

    def close(self) -> None:
        self.stop_requested.set()
        try:
            self.connection.close()
        finally:
            self._thread.join(timeout=1.0)


def parse_window_event(line: str) -> tuple[str, int, str, int, int] | None:
    parts = line.split(",")
    if len(parts) != 6 or parts[0] != "WINDOW":
        return None
    event_name, label = parts[1], parts[3]
    if event_name not in {"START", "END", "CANCEL"}:
        return None
    try:
        return event_name, int(parts[2]), label, int(parts[4]), int(parts[5])
    except ValueError as error:
        raise ProtocolError(f"WINDOW 数值格式错误：{line}") from error


def wait_for_firmware(reader: DeviceReader, timeout: float = 8.0) -> None:
    """确认正在运行正确固件；已错过启动头时用严格 SAMPLE 格式确认。"""

    deadline = time.monotonic() + timeout
    saw_boot = False
    saw_ready = False
    while time.monotonic() < deadline:
        if saw_boot and saw_ready:
            return
        try:
            event = reader.get(timeout=min(0.20, max(0.01, deadline - time.monotonic())))
        except queue.Empty:
            if reader.saw_valid_sample.is_set():
                print("已收到有效 SAMPLE 数据（本次连接可能错过了启动标识）。")
                return
            continue

        if event.line == f"BOOT,{FIRMWARE_PROTOCOL}":
            saw_boot = True
        elif event.line.startswith("READY,COMMANDS,") and "DURATION_MS=3000" in event.line:
            saw_ready = True

    if reader.saw_valid_sample.is_set():
        print("已收到有效 SAMPLE 数据（未看到完整启动标识）。")
        return
    raise TimeoutError(
        "8 秒内未收到清闲椅采集固件的 BOOT/READY/SAMPLE；"
        "请确认已烧录 data_collection 固件并按一下开发板 RESET。"
    )


def next_event(reader: DeviceReader, deadline: float) -> SerialEvent:
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("等待开发板响应超时")
        try:
            return reader.get(timeout=min(0.25, remaining))
        except queue.Empty:
            continue


def cancel_active_window(connection: object) -> None:
    try:
        connection.write(b"Q\n")
        connection.flush()
    except Exception:
        pass


def collect_window(
    connection: object,
    reader: DeviceReader,
    action_code: str,
) -> tuple[int, str, list[Sample]]:
    """让固件采集一个固定 3000ms 窗口，并返回完整窗口样本。"""

    expected_label = ACTIONS[action_code][0]
    reader.drain()
    connection.write((action_code.upper() + "\n").encode("ascii"))
    connection.flush()

    start_deadline = time.monotonic() + 3.0
    window_id: int | None = None
    capture_started_at = ""
    while window_id is None:
        event = next_event(reader, start_deadline)
        if event.line.startswith("STATUS,COMMAND,REJECTED,BUSY"):
            raise ProtocolError("开发板仍在采集上一个动作，请等待提示音结束后重试")
        window_event = parse_window_event(event.line)
        if window_event is None:
            continue
        event_name, candidate_id, label, _, duration_ms = window_event
        if event_name != "START":
            continue
        if label != expected_label or duration_ms != WINDOW_MS:
            cancel_active_window(connection)
            raise ProtocolError(
                f"窗口参数不匹配：期望 {expected_label}/{WINDOW_MS}ms，"
                f"收到 {label}/{duration_ms}ms"
            )
        window_id = candidate_id
        capture_started_at = event.received_at

    samples: list[Sample] = []
    seen_frames: set[int] = set()
    end_seen = False
    end_deadline = time.monotonic() + 6.0
    trailing_deadline = end_deadline
    try:
        while True:
            event = next_event(reader, trailing_deadline if end_seen else end_deadline)
            if event.sample is not None:
                sample = event.sample
                if sample.window_id != window_id:
                    continue
                if sample.label != expected_label:
                    raise ProtocolError(
                        f"窗口 {window_id} 的 SAMPLE 标签异常：{sample.label}"
                    )
                elapsed_ms = int(sample.values["window_elapsed_ms"])
                if not 0 <= elapsed_ms < WINDOW_MS:
                    raise ProtocolError(
                        f"窗口 {window_id} 的 SAMPLE 时间越界：{elapsed_ms}ms"
                    )
                if sample.frame_id not in seen_frames:
                    seen_frames.add(sample.frame_id)
                    samples.append(sample)
                continue

            window_event = parse_window_event(event.line)
            if window_event is not None:
                event_name, candidate_id, label, _, final_value = window_event
                if candidate_id != window_id:
                    continue
                if event_name == "CANCEL":
                    raise ProtocolError(
                        f"窗口 {window_id} 被开发板取消（已采 {final_value}ms）"
                    )
                if event_name == "END":
                    if label != expected_label or final_value != WINDOW_MS:
                        raise ProtocolError(f"窗口结束参数异常：{event.line}")
                    end_seen = True
                    # 一帧可能在 3000ms 边界前开始、边界后才输出。
                    trailing_deadline = time.monotonic() + 0.35
                    continue

            if end_seen and time.monotonic() >= trailing_deadline:
                break
    except TimeoutError:
        if not end_seen:
            cancel_active_window(connection)
            raise TimeoutError(f"窗口 {window_id} 未在预期时间内结束")

    if not samples:
        raise ProtocolError(f"窗口 {window_id} 没有有效 SAMPLE，未写入 CSV")
    samples.sort(key=lambda item: (int(item.values["t_ms"]), item.frame_id))
    return window_id, capture_started_at, samples


def write_window(
    writer: csv.DictWriter,
    csv_file: object,
    samples: list[Sample],
    *,
    participant: str,
    session_id: str,
    session_started_at: str,
    capture_index: int,
    action_code: str,
    capture_started_at: str,
    collection_mode: str = "manual",
    guided_seed: int | None = None,
    guided_trial_index: int | None = None,
    guided_trial_total: int | None = None,
    action_repetition: int = 1,
) -> None:
    _, action_name, action_name_zh = ACTIONS[action_code]
    for sample in samples:
        elapsed_ms = int(sample.values["window_elapsed_ms"])
        frame_span_ms = int(sample.values["frame_span_ms"])
        writer.writerow(
            {
                "record_type": "SAMPLE",
                "csv_schema": CSV_SCHEMA,
                "firmware_protocol": FIRMWARE_PROTOCOL,
                "participant_id": participant,
                "session_id": session_id,
                "session_started_at": session_started_at,
                "capture_index": capture_index,
                "collection_mode": collection_mode,
                "guided_seed": "" if guided_seed is None else guided_seed,
                "guided_trial_index": (
                    "" if guided_trial_index is None else guided_trial_index
                ),
                "guided_trial_total": (
                    "" if guided_trial_total is None else guided_trial_total
                ),
                "action_repetition": action_repetition,
                "action_code": action_code,
                "action_name": action_name,
                "action_name_zh": action_name_zh,
                "capture_started_at": capture_started_at,
                "host_received_at": sample.received_at,
                "port": PORT,
                "baud": BAUD,
                "window_duration_ms": WINDOW_MS,
                # 原始窗口全部保留；训练时排除提示音/边界跨帧。
                "usable_for_training": int(
                    elapsed_ms >= 250
                    and elapsed_ms + frame_span_ms <= WINDOW_MS
                ),
                **{name: sample.values[name] for name in SAMPLE_FIELDS[1:]},
            }
        )
    csv_file.flush()


def print_help() -> None:
    print("\n键盘命令（输入字母后按 Enter）：")
    print("  n = 正常坐姿，采集 3 秒")
    print("  l = 向左拉伸，采集 3 秒")
    print("  r = 向右拉伸，采集 3 秒")
    print("  c = 胸椎舒展，采集 3 秒")
    print("  h = 再看一次帮助")
    print("  q = 保存并退出")
    print("每次都从正常坐姿开始；准备好后再输入动作键。")


def readiness_prompt(trial_index: int, total: int, action_code: str) -> bool:
    """显示下一动作，Enter 确认；q/EOF 安全结束引导采集。"""

    action_name_zh = ACTIONS[action_code][2]
    print("\n" + "=" * 58)
    print(f"下一项 [{trial_index}/{total}]：{action_name_zh}")
    print(f"动作要求：{ACTION_CUES[action_code]}")
    print("先回到正常坐姿并做好准备；倒数结束后立即开始动作。")
    print("=" * 58)
    while True:
        try:
            answer = input("准备好后按 Enter（输入 q 结束本场）：").strip().lower()
        except EOFError:
            return False
        if not answer:
            return True
        if answer == "q":
            return False
        print("请输入 q 结束，或直接按 Enter 开始。")


def countdown() -> None:
    for number in (3, 2, 1):
        print(f"  {number}", flush=True)
        time.sleep(1.0)
    print("  开始！保持 3 秒。", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="只打开 COM8，以固件固定的 3 秒窗口采集清闲椅动作数据。",
        epilog=(
            "引导采集示例：python Chair\\tools\\capture_actions_com8.py "
            "--guided --participant P01 --session S01 "
            "--repetitions 1 --seed 20260829"
        ),
    )
    parser.add_argument("--participant", help="匿名参与者编号，例如 P01")
    parser.add_argument("--session", help="场次编号，例如 S01")
    parser.add_argument(
        "--guided",
        action="store_true",
        help="自动逐项提示正常、左、右、胸椎四种 3 秒动作",
    )
    parser.add_argument(
        "--repetitions",
        type=int,
        default=1,
        help="引导模式中每种动作的次数（默认 1）",
    )
    parser.add_argument(
        "--seed",
        type=int,
        help="引导模式随机顺序种子；相同种子产生相同顺序",
    )
    return parser.parse_args()


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")

    args = parse_args()
    if args.repetitions < 1:
        print("--repetitions 必须至少为 1。", file=sys.stderr)
        return 2
    try:
        participant_raw = args.participant or input("匿名参与者编号（例如 P01）：")
        session_raw = args.session or input("场次编号（例如 S01）：")
    except (EOFError, KeyboardInterrupt):
        print("\n未开始采集。")
        return 130

    participant = safe_id(participant_raw, "PILOT")
    session_id = safe_id(session_raw, "S01")
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    csv_path = OUTPUT_DIR / f"chair_{participant}_{session_id}_{timestamp}.csv"
    session_started_at = local_iso_now()

    if serial is None or not hasattr(serial, "Serial"):
        print(
            "缺少 pyserial。请执行：python -m pip install pyserial==3.5",
            file=sys.stderr,
        )
        return 2

    print(f"\n只打开固定串口 {PORT}，波特率 {BAUD}；不会枚举或访问其他串口。")
    connection = None
    try:
        connection = serial.Serial(
            PORT,
            BAUD,
            timeout=0.20,
            write_timeout=1.0,
            rtscts=False,
            dsrdtr=False,
        )
        connection.dtr = False
        connection.rts = False
    except Exception as error:
        if connection is not None:
            connection.close()
        print(f"无法打开 {PORT}：{error}", file=sys.stderr)
        print("请连接开发板，并关闭占用 COM8 的串口监视器。", file=sys.stderr)
        return 2

    reader = DeviceReader(connection)
    rows_written = 0
    completed = Counter()
    capture_index = 0
    exit_code = 0
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    try:
        print("等待开发板采集固件就绪……")
        wait_for_firmware(reader)
        print(f"数据文件：{csv_path}")
        with csv_path.open("w", newline="", encoding="utf-8-sig") as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()
            csv_file.flush()

            def capture_and_save(
                command: str,
                *,
                collection_mode: str,
                action_repetition: int,
                guided_seed: int | None = None,
                guided_trial_index: int | None = None,
                guided_trial_total: int | None = None,
            ) -> None:
                nonlocal capture_index, rows_written
                action_name_zh = ACTIONS[command][2]
                print(f"采集中：{action_name_zh}，请保持 3 秒……")
                window_id, capture_started_at, samples = collect_window(
                    connection, reader, command
                )
                capture_index += 1
                write_window(
                    writer,
                    csv_file,
                    samples,
                    participant=participant,
                    session_id=session_id,
                    session_started_at=session_started_at,
                    capture_index=capture_index,
                    action_code=command,
                    capture_started_at=capture_started_at,
                    collection_mode=collection_mode,
                    guided_seed=guided_seed,
                    guided_trial_index=guided_trial_index,
                    guided_trial_total=guided_trial_total,
                    action_repetition=action_repetition,
                )
                rows_written += len(samples)
                completed[command] += 1
                valid_ranges = sum(
                    bin(int(sample.values["range_valid_mask"])).count("1")
                    for sample in samples
                )
                possible_ranges = len(samples) * 5
                print(
                    f"完成：窗口 {window_id}，保存 {len(samples)} 帧；"
                    f"超声有效 {valid_ranges}/{possible_ranges}。"
                )

            if args.guided:
                effective_seed = (
                    args.seed
                    if args.seed is not None
                    else time.time_ns() & 0xFFFFFFFF
                )
                plan = make_guided_plan(args.repetitions, effective_seed)
                print(
                    f"\n引导采集：共 {len(plan)} 项，每种动作 "
                    f"{args.repetitions} 次；随机种子 {effective_seed}。"
                )
                print("程序每次只显示下一项；可在准备提示处输入 q 结束。")
                for trial_index, trial in enumerate(plan, start=1):
                    if not readiness_prompt(
                        trial_index, len(plan), trial.action_code
                    ):
                        print("引导采集已安全结束；此前完整动作均已保存。")
                        break
                    countdown()
                    capture_and_save(
                        trial.action_code,
                        collection_mode="guided",
                        action_repetition=trial.repetition,
                        guided_seed=effective_seed,
                        guided_trial_index=trial_index,
                        guided_trial_total=len(plan),
                    )
                else:
                    print("\n本轮引导动作已全部完成。")
            else:
                print_help()
                while True:
                    try:
                        command = input(
                            "\n动作命令 [n/l/r/c，q退出]："
                        ).strip().lower()
                    except EOFError:
                        command = "q"
                    if command == "q":
                        break
                    if command in {"h", "help", "?"}:
                        print_help()
                        continue
                    if command not in ACTIONS:
                        print("无效命令。请输入 n、l、r、c 或 q。")
                        continue
                    capture_and_save(
                        command,
                        collection_mode="manual",
                        action_repetition=completed[command] + 1,
                    )
    except KeyboardInterrupt:
        cancel_active_window(connection)
        print("\n收到 Ctrl-C：已取消正在进行的窗口，完整窗口数据已保存。")
        exit_code = 130
    except (ProtocolError, TimeoutError, RuntimeError) as error:
        cancel_active_window(connection)
        print(f"\n采集停止：{error}", file=sys.stderr)
        exit_code = 3
    except Exception as error:
        cancel_active_window(connection)
        print(f"\nCOM8 通信失败：{error}", file=sys.stderr)
        exit_code = 3
    finally:
        malformed = reader.malformed_samples
        reader.close()

    print("\n=== 本场采集汇总 ===")
    print(f"参与者：{participant}；场次：{session_id}")
    print(
        "完整窗口："
        + ", ".join(
            f"{ACTIONS[code][2]} {completed[code]} 次" for code in ACTIONS
        )
    )
    print(f"共保存 {rows_written} 条 SAMPLE；畸形 SAMPLE 丢弃 {malformed} 条。")
    print(f"CSV：{csv_path}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
