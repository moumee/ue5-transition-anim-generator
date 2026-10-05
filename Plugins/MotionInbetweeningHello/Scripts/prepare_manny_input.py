"""External NumPy adapter. Reuses team mapping code without modifying it."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import traceback


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    temporary.replace(path)


def file_hash(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(job):
    import numpy as np

    mapping_directory = Path(job["mapping_directory"])
    sys.path.insert(0, str(mapping_directory))
    from convert_manny_sequence_135 import convert_sequence
    from official_context_preprocess import (
        load_stats, normalize, pack_state, project_static_channels,
        start_center_root, unpack_raw_json,
    )

    directory = Path(job["output_directory"])
    source_path = directory / "source_manny.json"
    source = json.loads(source_path.read_text(encoding="utf-8"))
    calibration_path = mapping_directory / "rest_pose_corrections_22_prototype.json"
    calibration = json.loads(calibration_path.read_text(encoding="utf-8"))
    if source.get("sample_rate_hz") != 30.0 or source.get("frame_count", 0) < 10:
        raise ValueError("The model input requires at least 10 samples at 30 fps.")
    times = np.asarray(source.get("sample_times_seconds", []), dtype=np.float64)
    if times.shape != (source["frame_count"],) or not np.isfinite(times).all() or not np.allclose(np.diff(times), 1 / 30, atol=1e-6, rtol=0):
        raise ValueError("Exported sample times must have a uniform 30 fps interval.")

    raw = convert_sequence(source, calibration,
                           max_root_step_cm=float(job["max_root_step_cm"]),
                           max_joint_step_degrees=float(job["max_joint_step_degrees"]))
    if not raw["validation"]["passed"]:
        raise ValueError("The team mapper rejected this motion: " + json.dumps(raw["validation"]))
    positions, rotations = unpack_raw_json(raw)
    positions, rotations, position_offset, rotation_offset, yaw = start_center_root(
        positions, rotations, context_len=10, forward_axis="x"
    )
    states = pack_state(rotations, positions)
    stats_path = job.get("statistics_file", "")
    projected_channels = []
    statistics = None
    if stats_path:
        mean, std = load_stats(stats_path)
        states, channels = project_static_channels(states, mean, std)
        projected_channels = channels.tolist()
        states = normalize(states, mean, std)
        statistics = {"file": str(Path(stats_path).resolve()), "sha256": file_hash(stats_path)}
    if states.shape != (source["frame_count"], 135) or not np.isfinite(states).all():
        raise ValueError("Model input must be a finite T x 135 array.")

    save_json(directory / "raw_135.json", raw)
    output = {
        "schema": "motion_inbetweening.manny_input.v1",
        "description": "Plugin adapter output for the input-mapping stage; no model inference has run.",
        "source_animation": source["animation"],
        "sample_rate_hz": 30.0,
        "sample_count": source["frame_count"],
        "sample_times_seconds": source["sample_times_seconds"],
        "context_len": 10,
        "shape": list(states.shape),
        "layout": raw["layout"],
        "joint_order": raw["joint_order"],
        "parents": calibration["parents"],
        "rotation_space": "parent_local",
        "position_space": "lafan_start_centered",
        "source_position_unit": "cm",
        "heading_alignment_applied": True,
        "normalization_applied": bool(stats_path),
        "statistics": statistics,
        "projected_static_channels": projected_channels,
        "alignment": {"reference_frame": 9, "position_offset_xz": position_offset.tolist(),
                      "rotation_offset": rotation_offset.tolist(), "yaw_radians": float(yaw)},
        "vectors_tx135": states.tolist(),
        "source_sha256": file_hash(source_path),
        "mapping_source_sha256": {name: file_hash(mapping_directory / name) for name in [
            "manny_lafan_transform.py", "convert_manny_sequence_135.py",
            "official_context_preprocess.py", "rest_pose_corrections_22_prototype.json"]},
        "validation": raw["validation"],
    }
    save_json(directory / "model_input.json", output)
    return {"success": True, "sample_count": source["frame_count"],
            "normalization_applied": bool(stats_path),
            "model_input": str(directory / "model_input.json")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True)
    args = parser.parse_args()
    job_path = Path(args.job)
    job = json.loads(job_path.read_text(encoding="utf-8-sig"))
    directory = Path(job["output_directory"])
    try:
        result = prepare(job)
    except Exception as error:
        (directory / "error.log").write_text(traceback.format_exc(), encoding="utf-8")
        save_json(directory / "result.json", {"success": False, "error": str(error)})
        return 1
    save_json(directory / "result.json", result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
