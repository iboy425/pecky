#!/usr/bin/env python3
"""Mine head-relative spatial features from the collected Pecky Cap sessions.

The report is intentionally based on participant-grouped splits.  `phase` is
used only as a weak offline label and is never included as an input feature.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import ExtraTreesClassifier
from sklearn.feature_selection import mutual_info_classif
from sklearn.metrics import (
    accuracy_score,
    average_precision_score,
    f1_score,
    precision_recall_fscore_support,
    roc_auc_score,
)
from sklearn.model_selection import GroupKFold


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA_DIR = ROOT / "data" / "raw"
DEFAULT_OUTPUT = ROOT / "models" / "hat_motion_v0" / "spatial_feature_report.json"
TRAIN_IDS = (9, 10, 11, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26)
VALIDATION_IDS = (27, 28, 29)
TEST_IDS = (30, 31)
CLASS_NAMES = ("background", "neck_extension", "chin_tuck", "head_resistance")
PHASE_TO_CLASS = {0: 0, 1: 1, 2: 0, 3: 2, 4: 0, 5: 3, 6: 0}


def ema(values: np.ndarray, alpha: float = 0.90) -> np.ndarray:
    result = np.empty_like(values, dtype=np.float32)
    result[0] = values[0]
    for index in range(1, len(values)):
        result[index] = alpha * result[index - 1] + (1.0 - alpha) * values[index]
    return result


def head_relative_signals(dataframe: pd.DataFrame) -> dict[str, np.ndarray]:
    neutral = dataframe[dataframe["phase"] == 0]
    neutral_accel = neutral[["ax_raw", "ay_raw", "az_raw"]].to_numpy(np.float32).mean(axis=0) / 16384.0
    neutral_gyro = neutral[["gx_raw", "gy_raw", "gz_raw"]].to_numpy(np.float32).mean(axis=0) / 131.0
    pressure_baseline = float(neutral["pressure_raw"].median())

    vertical = neutral_accel / max(float(np.linalg.norm(neutral_accel)), 1e-6)
    board_y = np.array([0.0, 1.0, 0.0], dtype=np.float32)
    lateral = board_y - float(np.dot(board_y, vertical)) * vertical
    if float(np.linalg.norm(lateral)) < 0.1:
        board_z = np.array([0.0, 0.0, 1.0], dtype=np.float32)
        lateral = board_z - float(np.dot(board_z, vertical)) * vertical
    lateral /= max(float(np.linalg.norm(lateral)), 1e-6)
    forward = np.cross(vertical, lateral)
    forward /= max(float(np.linalg.norm(forward)), 1e-6)
    axes = np.stack((forward, lateral, vertical), axis=0)

    accel_sensor = dataframe[["ax_raw", "ay_raw", "az_raw"]].to_numpy(np.float32) / 16384.0
    gyro_sensor = dataframe[["gx_raw", "gy_raw", "gz_raw"]].to_numpy(np.float32) / 131.0
    gravity_sensor = ema(accel_sensor)
    linear_head = (accel_sensor - gravity_sensor) @ axes.T
    gyro_head = (gyro_sensor - neutral_gyro) @ axes.T

    gravity_unit = gravity_sensor / np.maximum(np.linalg.norm(gravity_sensor, axis=1, keepdims=True), 1e-6)
    dot = np.clip(gravity_unit @ vertical, -1.0, 1.0)
    cross = np.cross(np.broadcast_to(vertical, gravity_unit.shape), gravity_unit)
    pitch = np.degrees(np.arctan2(cross @ lateral, dot))
    roll = np.degrees(np.arctan2(cross @ forward, dot))
    tilt = np.degrees(np.arccos(dot))
    pressure = dataframe["pressure_raw"].to_numpy(np.float32) - pressure_baseline

    return {
        "pitch_deg": pitch,
        "roll_deg": roll,
        "tilt_deg": tilt,
        "linear_forward_g": linear_head[:, 0],
        "linear_lateral_g": linear_head[:, 1],
        "linear_vertical_g": linear_head[:, 2],
        "gyro_roll_dps": gyro_head[:, 0],
        "gyro_pitch_dps": gyro_head[:, 1],
        "gyro_yaw_dps": gyro_head[:, 2],
        "gyro_norm_dps": np.linalg.norm(gyro_head, axis=1),
        "pressure_delta": pressure,
    }


def longest_fraction_above(values: np.ndarray, threshold: float) -> float:
    longest = current = 0
    for active in values > threshold:
        current = current + 1 if active else 0
        longest = max(longest, current)
    return longest / max(len(values), 1)


def ordered_biphasic_features(values: np.ndarray) -> tuple[float, float, float]:
    """Return ordered opposite-pulse strength, gap, and balance.

    A chin tuck must contain a forward/back pulse *sequence*.  Taking the
    independent positive and negative maxima loses ordering and makes walking
    vibration look deceptively similar.  Board-forward sign is intentionally
    allowed in either direction, but the opposite peak must arrive 0.24--1.6 s
    later.
    """

    minimum_gap = 6
    maximum_gap = min(40, len(values) - 1)
    best_strength = 0.0
    best_gap = 0
    best_balance = 0.0
    for first_index in range(0, max(0, len(values) - minimum_gap)):
        first = float(values[first_index])
        if abs(first) < 1e-6:
            continue
        stop = min(len(values), first_index + maximum_gap + 1)
        for second_index in range(first_index + minimum_gap, stop):
            second = float(values[second_index])
            if first * second >= 0.0:
                continue
            strength = min(abs(first), abs(second))
            if strength <= best_strength:
                continue
            best_strength = strength
            best_gap = second_index - first_index
            best_balance = min(abs(first), abs(second)) / max(abs(first), abs(second))
    return best_strength, best_gap / 25.0, best_balance


def window_features(signals: dict[str, np.ndarray], start: int, size: int) -> dict[str, float]:
    features: dict[str, float] = {}
    for name, values in signals.items():
        window = values[start : start + size]
        features[f"{name}_mean"] = float(np.mean(window))
        features[f"{name}_std"] = float(np.std(window))
        features[f"{name}_min"] = float(np.min(window))
        features[f"{name}_max"] = float(np.max(window))
        features[f"{name}_range"] = float(np.ptp(window))
        features[f"{name}_rms"] = float(np.sqrt(np.mean(np.square(window))))
        features[f"{name}_end_minus_start"] = float(window[-1] - window[0])
    sagittal = signals["linear_forward_g"][start : start + size]
    pressure = signals["pressure_delta"][start : start + size]
    ordered_strength, ordered_gap_s, ordered_balance = ordered_biphasic_features(sagittal)
    features["chin_ordered_biphasic_strength"] = ordered_strength
    features["chin_ordered_peak_gap_s"] = ordered_gap_s
    features["chin_ordered_pulse_balance"] = ordered_balance
    features["chin_forward_jerk_rms"] = float(np.sqrt(np.mean(np.square(np.diff(sagittal)))))
    features["pressure_positive_area"] = float(np.maximum(pressure, 0.0).sum() / 25.0)
    features["pressure_rise_max"] = float(np.max(np.diff(pressure)))
    features["pressure_above_600_fraction"] = float(np.mean(pressure > 600.0))
    features["pressure_above_600_longest_fraction"] = longest_fraction_above(pressure, 600.0)
    return features


def build_windows(data_dir: Path, session_ids: tuple[int, ...], window_size: int, stride: int) -> tuple[pd.DataFrame, np.ndarray, np.ndarray]:
    records: list[dict[str, float]] = []
    labels: list[int] = []
    groups: list[int] = []
    for session_id in session_ids:
        source = data_dir / f"hat_{session_id:04d}.csv"
        dataframe = pd.read_csv(source)
        signals = head_relative_signals(dataframe)
        phases = dataframe["phase"].to_numpy(np.int16)
        mapped = np.array([PHASE_TO_CLASS.get(int(phase), -1) for phase in phases], dtype=np.int16)
        for start in range(0, len(dataframe) - window_size + 1, stride):
            label_slice = mapped[start : start + window_size]
            if np.any(label_slice < 0) or not np.all(label_slice == label_slice[0]):
                continue
            records.append(window_features(signals, start, window_size))
            labels.append(int(label_slice[0]))
            groups.append(session_id)
    return pd.DataFrame.from_records(records), np.asarray(labels), np.asarray(groups)


def action_report(
    action_index: int,
    train_features: pd.DataFrame,
    train_labels: np.ndarray,
    validation_features: pd.DataFrame,
    validation_labels: np.ndarray,
) -> dict[str, object]:
    train_target = (train_labels == action_index).astype(np.int8)
    validation_target = (validation_labels == action_index).astype(np.int8)
    mutual_information = mutual_info_classif(
        train_features.to_numpy(), train_target, random_state=20260829
    )
    mi_order = np.argsort(mutual_information)[::-1][:12]

    model = ExtraTreesClassifier(
        n_estimators=160,
        max_depth=7,
        min_samples_leaf=4,
        class_weight="balanced",
        random_state=20260829,
        n_jobs=-1,
    )
    model.fit(train_features, train_target)
    predictions = model.predict(validation_features)
    precision, recall, f1, _ = precision_recall_fscore_support(
        validation_target, predictions, labels=[1], average=None, zero_division=0
    )
    importance_order = np.argsort(model.feature_importances_)[::-1][:12]
    return {
        "action": CLASS_NAMES[action_index],
        "top_mutual_information": [
            {
                "feature": train_features.columns[index],
                "score": round(float(mutual_information[index]), 6),
            }
            for index in mi_order
        ],
        "top_extra_trees_importance": [
            {
                "feature": train_features.columns[index],
                "score": round(float(model.feature_importances_[index]), 6),
            }
            for index in importance_order
        ],
        "validation": {
            "accuracy": round(float(accuracy_score(validation_target, predictions)), 6),
            "precision": round(float(precision[0]), 6),
            "recall": round(float(recall[0]), 6),
            "f1": round(float(f1[0]), 6),
            "positive_windows": int(validation_target.sum()),
        },
    }


def feature_families(columns: pd.Index) -> dict[str, list[str]]:
    def prefixed(*prefixes: str) -> list[str]:
        return [column for column in columns if column.startswith(prefixes)]

    pose = prefixed("pitch_deg_", "roll_deg_", "tilt_deg_")
    gyro = prefixed("gyro_roll_dps_", "gyro_pitch_dps_", "gyro_yaw_dps_", "gyro_norm_dps_")
    linear = prefixed("linear_forward_g_", "linear_lateral_g_", "linear_vertical_g_", "chin_")
    pressure = prefixed("pressure_delta_", "pressure_")
    return {
        "pose": pose,
        "gyro": gyro,
        "linear_acceleration": linear,
        "pressure": pressure,
        "pose_plus_gyro": pose + gyro,
        "all_imu": pose + gyro + linear,
        "all_imu_plus_pressure": pose + gyro + linear + pressure,
    }


def grouped_family_report(
    action_index: int,
    features: pd.DataFrame,
    labels: np.ndarray,
    groups: np.ndarray,
) -> dict[str, object]:
    """Leakage-resistant feature-family ablation on development participants."""

    target = (labels == action_index).astype(np.int8)
    fold = GroupKFold(n_splits=min(5, len(np.unique(groups))))
    reports: dict[str, object] = {}
    for family_name, columns in feature_families(features.columns).items():
        probabilities = np.zeros(len(target), dtype=np.float32)
        for train_indices, validation_indices in fold.split(features, target, groups):
            model = ExtraTreesClassifier(
                n_estimators=120,
                max_depth=7,
                min_samples_leaf=4,
                class_weight="balanced",
                random_state=20260829,
                n_jobs=-1,
            )
            model.fit(features.iloc[train_indices][columns], target[train_indices])
            probabilities[validation_indices] = model.predict_proba(
                features.iloc[validation_indices][columns]
            )[:, 1]
        predictions = (probabilities >= 0.5).astype(np.int8)
        precision, recall, _, _ = precision_recall_fscore_support(
            target, predictions, labels=[1], average=None, zero_division=0
        )
        reports[family_name] = {
            "roc_auc": round(float(roc_auc_score(target, probabilities)), 6),
            "average_precision": round(float(average_precision_score(target, probabilities)), 6),
            "precision_at_0_5": round(float(precision[0]), 6),
            "recall_at_0_5": round(float(recall[0]), 6),
            "f1_at_0_5": round(float(f1_score(target, predictions, zero_division=0)), 6),
            "feature_count": len(columns),
        }
    return {"action": CLASS_NAMES[action_index], "families": reports}


def pressure_data_quality(data_dir: Path, session_ids: tuple[int, ...]) -> dict[str, object]:
    session_reports: list[dict[str, object]] = []
    reliable_sessions: list[int] = []
    for session_id in session_ids:
        dataframe = pd.read_csv(data_dir / f"hat_{session_id:04d}.csv")
        neutral_pressure = dataframe.loc[dataframe["phase"] == 0, "pressure_raw"].to_numpy(np.float32)
        baseline = float(np.median(neutral_pressure))
        noise = max(1.0, 1.4826 * float(np.median(np.abs(neutral_pressure - baseline))))
        threshold = baseline + max(600.0, 8.0 * noise)
        phase_five = dataframe.loc[dataframe["phase"] == 5, "pressure_raw"].to_numpy(np.float32)
        high_samples = int(np.sum(phase_five >= threshold))
        reliable = high_samples >= 10
        if reliable:
            reliable_sessions.append(session_id)
        session_reports.append(
            {
                "session": session_id,
                "phase5_high_samples": high_samples,
                "phase5_max_delta": round(float(np.max(phase_five) - baseline), 3),
                "reliable_for_resistance": reliable,
            }
        )
    return {
        "criterion": "at least 10 phase-5 samples above P0 + max(600 ADC, 8*MAD-noise)",
        "reliable_session_count": len(reliable_sessions),
        "total_session_count": len(session_ids),
        "reliable_sessions": reliable_sessions,
        "sessions": session_reports,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--window-samples", type=int, default=50)
    parser.add_argument("--stride-samples", type=int, default=50)
    args = parser.parse_args()

    train_x, train_y, train_groups = build_windows(
        args.data_dir, TRAIN_IDS, args.window_samples, args.stride_samples
    )
    validation_x, validation_y, validation_groups = build_windows(
        args.data_dir, VALIDATION_IDS, args.window_samples, args.stride_samples
    )
    test_x, test_y, _ = build_windows(
        args.data_dir, TEST_IDS, args.window_samples, args.stride_samples
    )
    reports = [
        action_report(index, train_x, train_y, validation_x, validation_y)
        for index in range(1, len(CLASS_NAMES))
    ]
    development_x = pd.concat((train_x, validation_x), ignore_index=True)
    development_y = np.concatenate((train_y, validation_y))
    development_groups = np.concatenate((train_groups, validation_groups))
    grouped_ablation = [
        grouped_family_report(index, development_x, development_y, development_groups)
        for index in range(1, len(CLASS_NAMES))
    ]
    report = {
        "coordinate_frame": {
            "vertical": "neutral gravity",
            "lateral": "board Y projected orthogonal to neutral gravity",
            "forward": "vertical cross lateral",
        },
        "label_warning": "phase is a weak clock-block label containing repetitions and rests; it is never an input feature",
        "causal_warning": "feature importance is association, not causation; physical action constraints control the firmware design",
        "split": {
            "train_participants": list(TRAIN_IDS),
            "validation_participants": list(VALIDATION_IDS),
            "untouched_test_participants": list(TEST_IDS),
        },
        "windows": {
            "train": len(train_y),
            "validation": len(validation_y),
            "test_reserved_not_scored": len(test_y),
        },
        "actions": reports,
        "development_grouped_feature_ablation": grouped_ablation,
        "pressure_data_quality": pressure_data_quality(
            args.data_dir, TRAIN_IDS + VALIDATION_IDS + TEST_IDS
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for action in reports:
        strongest = ", ".join(
            item["feature"] for item in action["top_mutual_information"][:5]
        )
        metrics = action["validation"]
        print(
            f"RESULT,{action['action']},P={metrics['precision']:.3f},"
            f"R={metrics['recall']:.3f},F1={metrics['f1']:.3f},TOP={strongest}"
        )
    for action in grouped_ablation:
        best_family, best_metrics = max(
            action["families"].items(), key=lambda item: item[1]["roc_auc"]
        )
        print(
            f"GROUPED,{action['action']},BEST={best_family},"
            f"AUC={best_metrics['roc_auc']:.3f},AP={best_metrics['average_precision']:.3f},"
            f"F1={best_metrics['f1_at_0_5']:.3f}"
        )
    print(f"INFO,REPORT,{args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
