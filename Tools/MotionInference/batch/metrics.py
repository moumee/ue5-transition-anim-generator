"""Shared FK and motion metrics for the batch evaluation tools (NumPy only).

Joint offsets come from the Manny reference skeleton exported by ue_export_manny_clips.py,
expressed in the team calibration's corrected (model) frames, so FK of a 135-D state gives
Manny joint positions in the model's LAFAN axes (cm, Y up).
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[2]
sys.path[:0] = [str(PROJECT / "Tools/MannyLafanMapping")]
from manny_lafan_transform import UE_TO_LAFAN, matrix6d_to_9d, quat_to_matrix, validate_calibration  # noqa: E402

CALIBRATION_FILE = PROJECT / "Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json"
CALIBRATION = json.loads(CALIBRATION_FILE.read_text(encoding="utf-8"))
NAMES, PARENTS = CALIBRATION["joint_order"], CALIBRATION["parents"]
FOOT = [3, 4, 7, 8]          # LeftFoot, LeftToe(ball_l), RightFoot, RightToe(ball_r)
TOES = [4, 8]
GROUPS = {"legs": [1, 2, 3, 5, 6, 7], "spine/head": [9, 10, 11, 12, 13], "clavicles": [14, 18],
          "arms": [15, 16, 17, 19, 20, 21]}


def _vector(value):
    return np.array([value["x"], value["y"], value["z"]] if isinstance(value, dict) else value, dtype=np.float64)


def _quat(value):
    q = [value["x"], value["y"], value["z"], value["w"]] if isinstance(value, dict) else value
    return quat_to_matrix(dict(zip("xyzw", q)))


def offsets_from_skeleton(skeleton_path):
    """22 child offsets (cm) in the corrected parent frames, from the reference pose."""
    skeleton = json.loads(Path(skeleton_path).read_text(encoding="utf-8"))
    rotation, position = {}, {}
    for bone in skeleton["bones"]:                 # exporter writes parents before children
        local = bone["reference_local_transform"]
        r, t = _quat(local["rotation_quaternion"]), _vector(local["translation_cm"])
        parent = bone["parent"]
        rotation[bone["name"]] = r if parent is None else rotation[parent] @ r
        position[bone["name"]] = t if parent is None else position[parent] + rotation[parent] @ t
    B = validate_calibration(CALIBRATION)
    G = np.stack([UE_TO_LAFAN @ rotation[m] @ UE_TO_LAFAN.T @ B[j] for j, m in enumerate(CALIBRATION["manny_bone_order"])])
    X = np.stack([UE_TO_LAFAN @ position[m] for m in CALIBRATION["manny_bone_order"]])
    offsets = np.zeros((22, 3))
    for j, p in enumerate(PARENTS):
        if p >= 0:
            offsets[j] = G[p].T @ (X[j] - X[p])
    return offsets


def decode(states):
    states = np.asarray(states, dtype=np.float64)
    return states[:, -3:].copy(), matrix6d_to_9d(states[:, :132].reshape(-1, 22, 6))


def load_states(path):
    return decode(json.loads(Path(path).read_text(encoding="utf-8-sig"))["vectors_tx135"])


def fk(rotations, positions, offsets):
    T = len(positions)
    G, X = np.zeros((T, 22, 3, 3)), np.zeros((T, 22, 3))
    for j, p in enumerate(PARENTS):
        if p < 0:
            G[:, j], X[:, j] = rotations[:, j], positions
        else:
            G[:, j] = G[:, p] @ rotations[:, j]
            X[:, j] = X[:, p] + np.einsum("tij,j->ti", G[:, p], offsets[j])
    return G, X


def rotvec(R):
    cos = np.clip((np.trace(R, axis1=-2, axis2=-1) - 1) / 2, -1, 1)
    angle = np.arccos(cos)
    w = np.stack([R[..., 2, 1] - R[..., 1, 2], R[..., 0, 2] - R[..., 2, 0], R[..., 1, 0] - R[..., 0, 1]], -1)
    s = np.sin(angle)
    return w * np.where(s > 1e-8, angle / (2 * np.maximum(s, 1e-12)), 0.5)[..., None]


def geodesic_degrees(A, B):
    R = np.swapaxes(A, -1, -2) @ B
    return np.degrees(np.arccos(np.clip((np.trace(R, axis1=-2, axis2=-1) - 1) / 2, -1, 1)))


def reconstruction_errors(pred_p, pred_r, true_p, true_r, frames, offsets):
    """Mean joint position (cm) and rotation (deg) error over the given frame slice."""
    _, X = fk(pred_r, pred_p, offsets)
    _, Y = fk(true_r, true_p, offsets)
    position = np.linalg.norm(X[frames] - Y[frames], axis=-1)
    rotation = geodesic_degrees(pred_r[frames], true_r[frames])
    out = {"position_cm": float(position.mean()), "root_cm": float(position[:, 0].mean()),
           "hands_cm": float(position[:, [17, 21]].mean())}
    out.update({f"rotation_deg_{g}": float(rotation[:, idx].mean()) for g, idx in GROUPS.items()})
    return out


def transition_metrics(positions, rotations, first, last, offsets, ground, margin_cm=3.0):
    """Plausibility proxies for generated frames first..last (inclusive, last = target)."""
    _, X = fk(rotations, positions, offsets)
    feet = X[:, FOOT]
    contact = feet[:, :, 1] < ground + margin_cm
    step = np.linalg.norm(np.diff(feet[:, :, [0, 2]], axis=0), axis=-1)            # t-1 -> t
    mask = contact[1:] & contact[:-1]
    window = np.zeros_like(mask)
    window[first - 1:last] = True
    slide = step[mask & window]
    speed = np.linalg.norm(np.diff(positions[:, [0, 2]], axis=0), axis=1)[first - 1:last]
    change = np.diff(speed)
    keep = [j for j in range(22) if j not in TOES]
    w = rotvec(np.swapaxes(rotations[:-1, keep], -1, -2) @ rotations[1:, keep])
    acceleration = np.degrees(np.linalg.norm(np.diff(w, axis=0), axis=-1))[first - 2:last - 1]
    return {
        "foot_slide_cm_per_frame": float(slide.mean()) if slide.size else 0.0,
        "foot_contact_pairs": int(slide.size),
        "root_speed_cm_per_frame": speed.round(2).tolist(),
        "root_speed_reversals": int(((change[:-1] > .15) & (change[1:] < -.15)).sum()
                                    + ((change[:-1] < -.15) & (change[1:] > .15)).sum()),
        "joint_acceleration_deg": float(acceleration.mean()),
        "min_toe_height_cm": float(X[first:last + 1, TOES, 1].min() - ground[[1, 3]].min()),
    }
