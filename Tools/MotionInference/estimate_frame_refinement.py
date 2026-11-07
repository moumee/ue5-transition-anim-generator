"""Estimate the Manny model-frame refinement (bone-axis rolls) from exported inputs.

The team calibration (rest_pose_corrections_22_prototype.json) aligns each Manny
joint frame with LAFAN1 from a single pose pair. That fixes bone directions but
not the roll about the bone, which only becomes visible when a joint bends.
LAFAN1 elbows and knees bend about their local +Z axis, so this script measures
the bend axis in real Manny motion and rolls the parent and child frames about
+X until the axis has no Y component. The result is applied by infer.py as
B' = B @ Rx(roll) just before the model and undone right after it.

Usage:
  python estimate_frame_refinement.py --inputs <model_input.json> ... \
      --output configs/manny_frame_refinement_v1.json [--hinges elbows knees] [--stats train_stats_context.pkl]
Inputs must be unnormalized model_input.json files exported by the plugin.
"""
from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
sys.path[:0] = [str(PROJECT / "Tools/MannyLafanMapping"), str(PROJECT / "Plugins/MotionInbetweeningHello/Scripts")]
from manny_lafan_transform import matrix6d_to_9d  # noqa: E402
from prepare_manny_input import file_hash  # noqa: E402

CALIBRATION_FILE = PROJECT / "Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json"
HINGES = {
    "elbows": [("LeftArm", "LeftForeArm"), ("RightArm", "RightForeArm")],
    "knees": [("LeftUpLeg", "LeftLeg"), ("RightUpLeg", "RightLeg")],
}
MIN_BEND_DEGREES = 5.0


def rotvec(R):
    cos = np.clip((np.trace(R, axis1=-2, axis2=-1) - 1) / 2, -1, 1)
    angle = np.arccos(cos)
    w = np.stack([R[..., 2, 1] - R[..., 1, 2], R[..., 0, 2] - R[..., 2, 0], R[..., 1, 0] - R[..., 0, 1]], -1)
    s = np.sin(angle)
    return w * np.where(s > 1e-8, angle / (2 * np.maximum(s, 1e-12)), 0.5)[..., None]


def geodesic(A, B):
    R = np.swapaxes(A, -1, -2) @ B
    return np.arccos(np.clip((np.trace(R, axis1=-2, axis2=-1) - 1) / 2, -1, 1))


def load_inputs(paths, calibration):
    clips = []
    calibration_sha = file_hash(CALIBRATION_FILE)
    for path in paths:
        document = json.loads(Path(path).read_text(encoding="utf-8-sig"))
        if document.get("schema") != "motion_inbetweening.manny_input.v1":
            raise ValueError(f"{path}: not a plugin model_input.json")
        if document.get("normalization_applied") is not False:
            raise ValueError(f"{path}: export without Training Statistics (unnormalized input) is required")
        if document.get("joint_order") != calibration["joint_order"]:
            raise ValueError(f"{path}: joint order differs from the calibration")
        recorded = (document.get("mapping_source_sha256") or {}).get(CALIBRATION_FILE.name)
        if recorded is not None and recorded != calibration_sha:
            raise ValueError(f"{path}: exported with a different calibration file")
        states = np.asarray(document["vectors_tx135"], dtype=np.float64)
        clips.append((document.get("source_animation", str(path)), matrix6d_to_9d(states[:, :132].reshape(-1, 22, 6))))
    return clips


def hinge_axes(local):
    """Bend axis of a joint in its parent's frame and in its own frame (unit, +Z hemisphere)."""
    sample = local[np.linspace(0, len(local) - 1, min(len(local), 400)).astype(int)]
    medoid = sample[np.argmin(geodesic(sample[:, None], sample[None]).sum(axis=1))]
    result = []
    for relative in (local @ medoid.T, medoid.T @ local):            # parent frame, child frame
        w = rotvec(relative)
        w = w[np.linalg.norm(w, axis=1) > np.radians(MIN_BEND_DEGREES)]
        if len(w) < 10:
            raise ValueError("Not enough bending to estimate a hinge axis; export clips that bend this joint.")
        _, singular, vt = np.linalg.svd(w, full_matrices=False)
        axis = vt[0] * np.sign(vt[0][2])
        share = float(singular[0] ** 2 / np.sum(singular ** 2))
        result.append((axis, share))
    bend = float(np.degrees(geodesic(local, np.repeat(medoid[None], len(local), 0)).max()))
    return result, bend


def roll_for(axis):
    """Roll about +X that removes the Y component of ``axis``: Rx(roll)^T axis lies in the X-Z plane."""
    return float(np.degrees(np.arctan2(-axis[1], axis[2])))


def roll_matrix(degrees):
    a = np.radians(degrees)
    return np.array([[1, 0, 0], [0, np.cos(a), -np.sin(a)], [0, np.sin(a), np.cos(a)]])


def estimate(clips, joint_order, hinge_groups):
    rolls, details = {}, {}
    rotations = np.concatenate([r for _, r in clips])
    for group in hinge_groups:
        for parent, child in HINGES[group]:
            axes, bend = hinge_axes(rotations[:, joint_order.index(child)])
            (parent_axis, parent_share), (child_axis, child_share) = axes
            rolls[parent] = round(roll_for(parent_axis), 3)
            rolls[child] = round(roll_for(child_axis), 3)
            details[child] = {
                "hinge_parent": parent,
                "axis_in_parent_frame": parent_axis.round(4).tolist(),
                "axis_in_child_frame": child_axis.round(4).tolist(),
                "tilt_from_z_degrees": [round(float(np.degrees(np.arccos(np.clip(a[2], -1, 1)))), 2)
                                        for a in (parent_axis, child_axis)],
                "after_roll_axis_in_parent_frame": (roll_matrix(rolls[parent]).T @ parent_axis).round(4).tolist(),
                "after_roll_axis_in_child_frame": (roll_matrix(rolls[child]).T @ child_axis).round(4).tolist(),
                "first_component_variance_share": [round(parent_share, 3), round(child_share, 3)],
                "max_bend_degrees": round(bend, 1),
            }
    return rolls, details


def z_report(clips, joint_order, rolls, stats_path):
    sys.path.insert(0, str(HERE))
    from infer import refine_rotations  # local import keeps the script usable without torch
    from official_context_preprocess import load_stats
    mean, std = load_stats(stats_path)
    calibration = json.loads(CALIBRATION_FILE.read_text(encoding="utf-8"))
    frames = np.repeat(np.eye(3)[None], 22, 0)
    for name, value in rolls.items():
        frames[joint_order.index(name)] = roll_matrix(value)
    report = {}
    for label, rot_fn in (("before", lambda r: r), ("after", lambda r: refine_rotations(r, frames, calibration["parents"]))):
        values = []
        for _, r in clips:
            six = rot_fn(r)[..., :2].reshape(len(r), 22, 6)
            z = np.abs((six.reshape(len(r), -1) - mean[:132]) / np.where(std[:132] < 1e-6, np.inf, std[:132]))
            values.append(z.reshape(len(r), 22, 6))
        z = np.concatenate(values)
        report[label] = {name: {"mean": round(float(z[:, j].mean()), 2), "max": round(float(z[:, j].max()), 2)}
                         for j, name in enumerate(joint_order) if name in rolls}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inputs", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--hinges", nargs="+", choices=sorted(HINGES), default=["elbows"])
    parser.add_argument("--stats", help="train_stats_context.pkl, only for the z-score report")
    args = parser.parse_args()
    calibration = json.loads(CALIBRATION_FILE.read_text(encoding="utf-8"))
    joint_order = calibration["joint_order"]
    clips = load_inputs(args.inputs, calibration)
    rolls, details = estimate(clips, joint_order, args.hinges)
    document = {
        "schema": "motion_inbetweening.frame_refinement.v1",
        "description": ("Per-joint roll (degrees) about the bone's local +X axis applied on top of the team "
                        "calibration, B' = B @ Rx(roll). Estimated so that the measured bend axis of each hinge "
                        "joint lies on LAFAN1's local +Z hinge. infer.py applies it before the model and undoes "
                        "it after, so exported inputs and the inverse mapper are unchanged."),
        "calibration_file": CALIBRATION_FILE.name,
        "calibration_sha256": file_hash(CALIBRATION_FILE),
        "joint_order": joint_order,
        "roll_degrees": rolls,
        "estimation": {
            "method": "hinge_axis_alignment",
            "script": Path(__file__).name,
            "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "hinges": args.hinges,
            "clips": [name for name, _ in clips],
            "frames": int(sum(len(r) for _, r in clips)),
            "joints": details,
        },
    }
    if args.stats:
        document["estimation"]["z_report"] = z_report(clips, joint_order, rolls, args.stats)
    output = Path(args.output)
    output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "roll_degrees": rolls,
                      "z_report": document["estimation"].get("z_report")}, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
