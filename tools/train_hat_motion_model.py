#!/usr/bin/env python3
"""Train and quality-gate the Pecky Cap motion model.

This is deliberately a *participant-grouped* training pipeline.  It never
randomly splits overlapping windows from one participant into both the train
and validation sets, because doing that would report an unrealistically high
score.

The current hat logger records a clock phase rather than a true per-repetition
marker.  Consequently the generated model is a V0 research artifact: the
script saves it locally, reports its real hold-out score, and only marks it as
deployable when the validation balanced accuracy reaches the configured gate.
It does not silently turn a weak model into firmware.

The network is intentionally small enough for an ESP32-S3 deployment after
the data quality gate is passed:

    50 samples (2 s at 25 Hz) x 7 channels
    Conv1D(7 -> 12) -> depthwise-separable Conv1D(12 -> 16)
    global average pool -> Dense(16 -> 4)

It has roughly 1.2k trainable parameters.  Inputs are neutral-calibrated so
the same per-session calibration can be performed on the cap.
"""

from __future__ import annotations

import argparse
import copy
import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np
import pandas as pd
import torch
from sklearn.metrics import (
    accuracy_score,
    balanced_accuracy_score,
    confusion_matrix,
    f1_score,
    precision_recall_fscore_support,
)
from torch import Tensor, nn
from torch.utils.data import DataLoader, Dataset


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA_DIR = ROOT / "data" / "raw"
DEFAULT_OUT_DIR = ROOT / "models" / "hat_motion_v0"

# These are the fully completed participant protocols available at the time
# this pipeline was introduced.  The explicit split is stable and reproducible.
DEFAULT_TRAIN_IDS = (9, 10, 11, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26)
DEFAULT_VALIDATION_IDS = (27, 28, 29)
DEFAULT_TEST_IDS = (30, 31)

CLASS_NAMES = ("background", "neck_extension", "chin_tuck", "head_resistance")
PHASE_TO_CLASS = {
    0: 0,
    1: 1,
    2: 0,
    3: 2,
    4: 0,
    5: 3,
    6: 0,
}
REQUIRED_COLUMNS = (
    "seq",
    "t_ms",
    "phase",
    "ax_raw",
    "ay_raw",
    "az_raw",
    "gx_raw",
    "gy_raw",
    "gz_raw",
    "pressure_raw",
)


@dataclass(frozen=True)
class SessionWindows:
    session_id: int
    windows: np.ndarray
    labels: np.ndarray


class WindowDataset(Dataset[tuple[Tensor, Tensor]]):
    def __init__(self, windows: np.ndarray, labels: np.ndarray) -> None:
        self.windows = torch.from_numpy(windows.astype(np.float32, copy=False))
        self.labels = torch.from_numpy(labels.astype(np.int64, copy=False))

    def __len__(self) -> int:
        return len(self.labels)

    def __getitem__(self, index: int) -> tuple[Tensor, Tensor]:
        return self.windows[index], self.labels[index]


class TinyDepthwiseCnn(nn.Module):
    """A tiny 1-D depthwise-separable CNN suitable for later int8 export."""

    def __init__(self) -> None:
        super().__init__()
        self.stem = nn.Sequential(
            nn.Conv1d(7, 12, kernel_size=5, padding=2, bias=False),
            nn.BatchNorm1d(12),
            nn.ReLU(inplace=True),
        )
        self.depthwise = nn.Sequential(
            nn.Conv1d(12, 12, kernel_size=5, padding=2, groups=12, bias=False),
            nn.BatchNorm1d(12),
            nn.ReLU(inplace=True),
        )
        self.pointwise = nn.Sequential(
            nn.Conv1d(12, 16, kernel_size=1, bias=False),
            nn.BatchNorm1d(16),
            nn.ReLU(inplace=True),
        )
        self.classifier = nn.Linear(16, len(CLASS_NAMES))

    def forward(self, inputs: Tensor) -> Tensor:
        outputs = self.stem(inputs)
        outputs = self.depthwise(outputs)
        outputs = self.pointwise(outputs)
        outputs = outputs.mean(dim=2)
        return self.classifier(outputs)


def parse_id_list(value: str) -> tuple[int, ...]:
    if not value.strip():
        return ()
    return tuple(int(token.strip()) for token in value.split(",") if token.strip())


def session_path(data_dir: Path, session_id: int) -> Path:
    return data_dir / f"hat_{session_id:04d}.csv"


def require_completed_protocol(dataframe: pd.DataFrame, source: Path) -> None:
    missing = [column for column in REQUIRED_COLUMNS if column not in dataframe.columns]
    if missing:
        raise ValueError(f"{source.name}: missing required columns: {', '.join(missing)}")
    observed = set(int(value) for value in dataframe["phase"].dropna().unique())
    missing_phases = set(range(7)) - observed
    if missing_phases:
        raise ValueError(
            f"{source.name}: incomplete protocol; missing phase(s) {sorted(missing_phases)}"
        )
    if len(dataframe) < 1400:
        raise ValueError(f"{source.name}: too few rows for a complete one-minute protocol")


def load_session_windows(
    data_dir: Path,
    session_id: int,
    window_samples: int,
    stride_samples: int,
) -> SessionWindows:
    source = session_path(data_dir, session_id)
    if not source.exists():
        raise FileNotFoundError(f"Required session is unavailable: {source}")
    dataframe = pd.read_csv(source)
    require_completed_protocol(dataframe, source)

    # Phase 0 immediately follows the three-second on-device calibration and
    # gives a second neutral reference for the recorded session.  Do not leak
    # its phase value to the network: it is only used to normalise sensor axes.
    neutral = dataframe[dataframe["phase"] == 0]
    neutral_accel = neutral[["ax_raw", "ay_raw", "az_raw"]].to_numpy(np.float32).mean(axis=0)
    neutral_gyro = neutral[["gx_raw", "gy_raw", "gz_raw"]].to_numpy(np.float32).mean(axis=0)
    neutral_pressure = float(neutral["pressure_raw"].median())

    # Convert sensor XYZ into a participant-neutral head coordinate frame.
    # vertical is measured gravity; board Y is approximately left/right in the
    # fixed cap mounting and is projected orthogonal to gravity; forward is the
    # remaining sagittal axis. This removes most cap pitch/roll placement offset
    # before the temporal network sees a window.
    vertical = neutral_accel / max(float(np.linalg.norm(neutral_accel)), 1.0)
    board_y = np.array([0.0, 1.0, 0.0], dtype=np.float32)
    lateral = board_y - float(np.dot(board_y, vertical)) * vertical
    if float(np.linalg.norm(lateral)) < 0.1:
        board_z = np.array([0.0, 0.0, 1.0], dtype=np.float32)
        lateral = board_z - float(np.dot(board_z, vertical)) * vertical
    lateral /= max(float(np.linalg.norm(lateral)), 1e-6)
    forward = np.cross(vertical, lateral)
    forward /= max(float(np.linalg.norm(forward)), 1e-6)
    head_axes = np.stack((forward, lateral, vertical), axis=0)

    acceleration = dataframe[["ax_raw", "ay_raw", "az_raw"]].to_numpy(np.float32)
    acceleration = (acceleration / 16384.0 - neutral_accel / 16384.0) @ head_axes.T
    gyro = dataframe[["gx_raw", "gy_raw", "gz_raw"]].to_numpy(np.float32)
    gyro = (gyro / 131.0 - neutral_gyro / 131.0) @ head_axes.T
    pressure = dataframe["pressure_raw"].to_numpy(np.float32)
    # The 12-bit ADC scaling prevents pressure magnitude from overwhelming the
    # six IMU channels.  It remains a model feature, not a hard requirement.
    pressure = np.clip((pressure - neutral_pressure) / 4095.0, -1.0, 1.0)
    signals = np.column_stack((acceleration, gyro, pressure))

    phases = dataframe["phase"].to_numpy(np.int16)
    labels = np.array([PHASE_TO_CLASS.get(int(phase), -1) for phase in phases], dtype=np.int64)
    windows: list[np.ndarray] = []
    window_labels: list[int] = []
    for start in range(0, len(signals) - window_samples + 1, stride_samples):
        window_labels_slice = labels[start : start + window_samples]
        # A transition window has no reliable action label.  Phase 7 is free
        # activity after the protocol and is intentionally excluded.
        if np.any(window_labels_slice < 0) or not np.all(window_labels_slice == window_labels_slice[0]):
            continue
        windows.append(signals[start : start + window_samples].T)
        window_labels.append(int(window_labels_slice[0]))

    if not windows:
        raise ValueError(f"{source.name}: no complete windows after preprocessing")
    return SessionWindows(
        session_id=session_id,
        windows=np.stack(windows).astype(np.float32),
        labels=np.asarray(window_labels, dtype=np.int64),
    )


def concatenate_sessions(sessions: Iterable[SessionWindows]) -> tuple[np.ndarray, np.ndarray]:
    session_list = list(sessions)
    return (
        np.concatenate([session.windows for session in session_list], axis=0),
        np.concatenate([session.labels for session in session_list], axis=0),
    )


def class_histogram(labels: np.ndarray) -> dict[str, int]:
    return {CLASS_NAMES[index]: int((labels == index).sum()) for index in range(len(CLASS_NAMES))}


def make_metrics(labels: np.ndarray, predictions: np.ndarray) -> dict[str, object]:
    precision, recall, f1, support = precision_recall_fscore_support(
        labels,
        predictions,
        average=None,
        labels=range(len(CLASS_NAMES)),
        zero_division=0,
    )
    return {
        "accuracy": round(float(accuracy_score(labels, predictions)), 6),
        "balanced_accuracy": round(float(balanced_accuracy_score(labels, predictions)), 6),
        "macro_f1": round(float(f1_score(labels, predictions, average="macro", zero_division=0)), 6),
        "per_class_f1": {
            CLASS_NAMES[index]: round(float(score), 6) for index, score in enumerate(f1)
        },
        "per_class_precision": {
            CLASS_NAMES[index]: round(float(score), 6) for index, score in enumerate(precision)
        },
        "per_class_recall": {
            CLASS_NAMES[index]: round(float(score), 6) for index, score in enumerate(recall)
        },
        "per_class_samples": {
            CLASS_NAMES[index]: int(count) for index, count in enumerate(support)
        },
        "confusion_matrix_rows_actual_columns_predicted": confusion_matrix(
            labels, predictions, labels=range(len(CLASS_NAMES))
        ).tolist(),
    }


@torch.no_grad()
def predict(model: nn.Module, loader: DataLoader[tuple[Tensor, Tensor]], device: torch.device) -> tuple[np.ndarray, np.ndarray]:
    model.eval()
    all_labels: list[np.ndarray] = []
    all_predictions: list[np.ndarray] = []
    for inputs, labels in loader:
        logits = model(inputs.to(device))
        all_predictions.append(torch.argmax(logits, dim=1).cpu().numpy())
        all_labels.append(labels.numpy())
    return np.concatenate(all_labels), np.concatenate(all_predictions)


def write_deployment_header(model: nn.Module, destination: Path) -> None:
    """Write human-readable float weights for later firmware conversion.

    This export is deliberately produced only after the validation quality gate
    succeeds.  It is not committed, and it is not silently flashed to a cap.
    A follow-up firmware task must use the same neutral calibration and replace
    these arrays with int8/TFLM or equivalent fixed-point tensors.
    """

    destination.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// Auto-generated by tools/train_hat_motion_model.py.\n",
        "// Do not edit by hand; source model is model.pt.\n",
        "#pragma once\n",
        "namespace pecky_model {\n",
    ]
    for name, tensor in model.state_dict().items():
        if "num_batches_tracked" in name:
            continue
        identifier = name.replace(".", "_")
        values = tensor.detach().cpu().numpy().reshape(-1)
        shape = ", ".join(str(size) for size in tensor.shape)
        values_text = ", ".join(f"{float(value):.8g}f" for value in values)
        lines.append(f"// {name}: [{shape}]\n")
        lines.append(f"constexpr float {identifier}[] = {{{values_text}}};\n")
    lines.append("}  // namespace pecky_model\n")
    destination.write_text("".join(lines), encoding="utf-8")


def train_model(
    train_windows: np.ndarray,
    train_labels: np.ndarray,
    validation_windows: np.ndarray,
    validation_labels: np.ndarray,
    epochs: int,
    batch_size: int,
    learning_rate: float,
    seed: int,
    device: torch.device,
) -> tuple[TinyDepthwiseCnn, dict[str, object]]:
    torch.manual_seed(seed)
    model = TinyDepthwiseCnn().to(device)
    train_loader = DataLoader(WindowDataset(train_windows, train_labels), batch_size=batch_size, shuffle=True)
    validation_loader = DataLoader(WindowDataset(validation_windows, validation_labels), batch_size=batch_size)

    class_counts = np.bincount(train_labels, minlength=len(CLASS_NAMES)).astype(np.float32)
    class_weights = class_counts.sum() / (len(CLASS_NAMES) * np.maximum(class_counts, 1.0))
    criterion = nn.CrossEntropyLoss(weight=torch.from_numpy(class_weights).to(device))
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=1e-4)

    best_state: dict[str, Tensor] | None = None
    best_metrics: dict[str, object] | None = None
    best_epoch = 0
    stale_epochs = 0
    for epoch in range(1, epochs + 1):
        model.train()
        for inputs, labels in train_loader:
            inputs = inputs.to(device)
            labels = labels.to(device)
            # Very small input noise makes the model slightly less sensitive to
            # ADC jitter without changing the action sequence semantics.
            noisy_inputs = inputs + torch.randn_like(inputs) * 0.002
            optimizer.zero_grad(set_to_none=True)
            loss = criterion(model(noisy_inputs), labels)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()

        if epoch % 2 != 0 and epoch != epochs:
            continue
        labels, predictions = predict(model, validation_loader, device)
        metrics = make_metrics(labels, predictions)
        metric = float(metrics["macro_f1"])
        if best_metrics is None or metric > float(best_metrics["macro_f1"]):
            best_state = copy.deepcopy(model.state_dict())
            best_metrics = metrics
            best_epoch = epoch
            stale_epochs = 0
        else:
            stale_epochs += 2
            if stale_epochs >= 24:
                break

    if best_state is None or best_metrics is None:
        raise RuntimeError("Training did not produce a validation checkpoint")
    model.load_state_dict(best_state)
    return model, {"best_epoch": best_epoch, "validation": best_metrics}


def evaluate_quality_gate(
    validation_metrics: dict[str, object],
    minimum_balanced_accuracy: float,
    minimum_neck_extension_f1: float,
    minimum_chin_tuck_f1: float,
    minimum_head_resistance_precision: float,
    minimum_head_resistance_recall: float,
) -> tuple[bool, dict[str, dict[str, object]]]:
    """Apply the product acceptance criteria without hiding class imbalance.

    The product requirement names percentages per action.  We use precision and
    recall for resistance instead of its raw accuracy: because non-resistance
    windows are plentiful, raw binary accuracy could look high even if the cap
    never recognised a real hands-behind-head exercise.
    """

    per_f1 = validation_metrics["per_class_f1"]
    per_precision = validation_metrics["per_class_precision"]
    per_recall = validation_metrics["per_class_recall"]
    criteria = {
        "overall_balanced_accuracy": {
            "actual": float(validation_metrics["balanced_accuracy"]),
            "minimum": minimum_balanced_accuracy,
        },
        "neck_extension_f1": {
            "actual": float(per_f1["neck_extension"]),
            "minimum": minimum_neck_extension_f1,
        },
        "chin_tuck_f1": {
            "actual": float(per_f1["chin_tuck"]),
            "minimum": minimum_chin_tuck_f1,
        },
        "head_resistance_precision": {
            "actual": float(per_precision["head_resistance"]),
            "minimum": minimum_head_resistance_precision,
        },
        "head_resistance_recall": {
            "actual": float(per_recall["head_resistance"]),
            "minimum": minimum_head_resistance_recall,
        },
    }
    for criterion in criteria.values():
        criterion["passed"] = bool(float(criterion["actual"]) >= float(criterion["minimum"]))
    return all(bool(criterion["passed"]) for criterion in criteria.values()), criteria


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--train-sessions", default=",".join(map(str, DEFAULT_TRAIN_IDS)))
    parser.add_argument("--validation-sessions", default=",".join(map(str, DEFAULT_VALIDATION_IDS)))
    parser.add_argument("--test-sessions", default=",".join(map(str, DEFAULT_TEST_IDS)))
    parser.add_argument("--window-samples", type=int, default=50, help="2 seconds at the current 25 Hz logger rate")
    parser.add_argument("--stride-samples", type=int, default=5, help="0.2-second inference hop")
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=0.001)
    parser.add_argument("--seed", type=int, default=20260828)
    parser.add_argument(
        "--minimum-balanced-accuracy",
        type=float,
        default=0.50,
        help="Product overall metric; balanced accuracy avoids background-window inflation.",
    )
    parser.add_argument("--minimum-neck-extension-f1", type=float, default=0.60)
    parser.add_argument("--minimum-chin-tuck-f1", type=float, default=0.60)
    parser.add_argument(
        "--minimum-head-resistance-precision",
        type=float,
        default=0.80,
        help="Resistance must not be triggered by ordinary pressure/noise.",
    )
    parser.add_argument(
        "--minimum-head-resistance-recall",
        type=float,
        default=0.80,
        help="Resistance must detect at least 80%% of labelled valid holds.",
    )
    parser.add_argument(
        "--enforce-quality-gate",
        action="store_true",
        help="Exit with status 2 if any independent validation criterion is below its minimum.",
    )
    args = parser.parse_args()

    if args.window_samples < 10 or args.stride_samples < 1:
        parser.error("window-samples must be >= 10 and stride-samples must be >= 1")

    train_ids = parse_id_list(args.train_sessions)
    validation_ids = parse_id_list(args.validation_sessions)
    test_ids = parse_id_list(args.test_sessions)
    split_sets = (set(train_ids), set(validation_ids), set(test_ids))
    if any(not split for split in split_sets) or any(
        left & right for index, left in enumerate(split_sets) for right in split_sets[index + 1 :]
    ):
        parser.error("train, validation, and test participant lists must be non-empty and disjoint")

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"INFO,DEVICE,{device.type}")
    print("INFO,LABELS,clock_phase_weak_labels; phase is never an input feature")

    all_ids = (*train_ids, *validation_ids, *test_ids)
    sessions = {
        session_id: load_session_windows(args.data_dir, session_id, args.window_samples, args.stride_samples)
        for session_id in all_ids
    }
    train_windows, train_labels = concatenate_sessions(sessions[session_id] for session_id in train_ids)
    validation_windows, validation_labels = concatenate_sessions(
        sessions[session_id] for session_id in validation_ids
    )
    test_windows, test_labels = concatenate_sessions(sessions[session_id] for session_id in test_ids)

    print(f"INFO,TRAIN_WINDOWS,{len(train_labels)},{class_histogram(train_labels)}")
    print(f"INFO,VALIDATION_WINDOWS,{len(validation_labels)},{class_histogram(validation_labels)}")
    print(f"INFO,TEST_WINDOWS,{len(test_labels)},{class_histogram(test_labels)}")

    model, fit_summary = train_model(
        train_windows,
        train_labels,
        validation_windows,
        validation_labels,
        args.epochs,
        args.batch_size,
        args.learning_rate,
        args.seed,
        device,
    )
    validation_metrics = fit_summary["validation"]
    test_loader = DataLoader(WindowDataset(test_windows, test_labels), batch_size=args.batch_size)
    test_actual, test_predictions = predict(model, test_loader, device)
    test_metrics = make_metrics(test_actual, test_predictions)

    parameter_count = sum(parameter.numel() for parameter in model.parameters())
    eligible, quality_criteria = evaluate_quality_gate(
        validation_metrics,
        args.minimum_balanced_accuracy,
        args.minimum_neck_extension_f1,
        args.minimum_chin_tuck_f1,
        args.minimum_head_resistance_precision,
        args.minimum_head_resistance_recall,
    )
    report = {
        "model_name": "TinyDepthwiseCnn",
        "model_version": "hat-motion-v0",
        "architecture": "Conv1D(7->12,k5) + depthwise Conv1D(12,k5) + pointwise(12->16) + GAP + Dense(16->4)",
        "trainable_parameters": parameter_count,
        "sample_rate_hz": 25,
        "window_samples": args.window_samples,
        "window_seconds": round(args.window_samples / 25.0, 3),
        "stride_samples": args.stride_samples,
        "classes": list(CLASS_NAMES),
        "input_channels": [
            "acc_forward_g_minus_neutral",
            "acc_lateral_g_minus_neutral",
            "acc_vertical_g_minus_neutral",
            "gyro_about_forward_dps_minus_neutral",
            "gyro_about_lateral_dps_minus_neutral",
            "gyro_about_vertical_dps_minus_neutral",
            "pressure_delta_adc_over_4095",
        ],
        "label_source": "logger clock phase; weak label, not manually delimited repetitions",
        "split": {
            "train_participants": list(train_ids),
            "validation_participants": list(validation_ids),
            "test_participants": list(test_ids),
            "participant_overlap": False,
        },
        "windows": {
            "train": int(len(train_labels)),
            "validation": int(len(validation_labels)),
            "test": int(len(test_labels)),
            "train_histogram": class_histogram(train_labels),
            "validation_histogram": class_histogram(validation_labels),
            "test_histogram": class_histogram(test_labels),
        },
        "best_epoch_by_validation_macro_f1": fit_summary["best_epoch"],
        "validation": validation_metrics,
        "test": test_metrics,
        "deployment_quality_gate": {
            "metric": "participant-held-out action-specific acceptance criteria",
            "criteria": quality_criteria,
            "passed": eligible,
            "result": "ELIGIBLE_FOR_FIRMWARE_EXPORT" if eligible else "BLOCKED_NEEDS_CLEANER_LABELS_AND_HARD_NEGATIVES",
        },
        "known_limitations": [
            "The logger phase is a clock cue. Each action block contains repetitions and rests, so current labels are weak.",
            "Pressure contact is inconsistent across participants and cannot be a universal hard gate until the mechanical mounting is fixed.",
            "No labelled look-down, nod, turn, walking, phone, drink, shrug, or posture-adjust hard negatives are in this training split.",
        ],
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "state_dict": model.cpu().state_dict(),
            "class_names": CLASS_NAMES,
            "input_channels": report["input_channels"],
            "window_samples": args.window_samples,
        },
        args.out_dir / "model.pt",
    )
    np.savez_compressed(
        args.out_dir / "model_weights.npz",
        **{name: tensor.detach().cpu().numpy() for name, tensor in model.state_dict().items()},
    )
    (args.out_dir / "quality_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if eligible:
        write_deployment_header(model, args.out_dir / "model_generated.h")
    else:
        generated_header = args.out_dir / "model_generated.h"
        if generated_header.exists():
            generated_header.unlink()

    print("RESULT,VALIDATION," + json.dumps(validation_metrics, ensure_ascii=False))
    print("RESULT,TEST," + json.dumps(test_metrics, ensure_ascii=False))
    print(
        "RESULT,QUALITY_GATE,"
        + ("PASS" if eligible else "BLOCKED")
        + ",criteria="
        + json.dumps(quality_criteria, ensure_ascii=False)
    )
    print(f"INFO,ARTIFACTS,{args.out_dir}")
    if args.enforce_quality_gate and not eligible:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
