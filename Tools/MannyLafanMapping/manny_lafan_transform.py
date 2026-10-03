"""First verified Manny -> LAFAN1 transform primitives.

Scope: coordinate basis, rotation basis, official 6D layout, and numerical
round-trip checks.  Rest-pose calibration offsets are intentionally supplied
by the caller because a LAFAN BVH file has motion-channel rotations rather
than a neutral T/A-pose rotation table.
"""

from __future__ import annotations

import json
from typing import Dict

import numpy as np


# UE component/world: +X forward, +Y right, +Z up (left-handed coordinates)
# Model/LAFAN world: +X forward, +Y up, +Z right (right-handed matrix math)
UE_TO_LAFAN = np.array(
    [[1.0, 0.0, 0.0],
     [0.0, 0.0, 1.0],
     [0.0, 1.0, 0.0]],
    dtype=np.float64,
)
LAFAN_TO_UE = UE_TO_LAFAN.T


LAFAN_TO_MANNY: Dict[str, str] = {
    "Hips": "pelvis",
    "LeftUpLeg": "thigh_l",
    "LeftLeg": "calf_l",
    "LeftFoot": "foot_l",
    "LeftToe": "ball_l",
    "RightUpLeg": "thigh_r",
    "RightLeg": "calf_r",
    "RightFoot": "foot_r",
    "RightToe": "ball_r",
    "Spine": "spine_01",
    "Spine1": "spine_02",
    "Spine2": "spine_03",
    "Neck": "neck_01",
    "Head": "head",
    "LeftShoulder": "clavicle_l",
    "LeftArm": "upperarm_l",
    "LeftForeArm": "lowerarm_l",
    "LeftHand": "hand_l",
    "RightShoulder": "clavicle_r",
    "RightArm": "upperarm_r",
    "RightForeArm": "lowerarm_r",
    "RightHand": "hand_r",
}


def ue_position_to_lafan(position_ue: np.ndarray) -> np.ndarray:
    """Convert UE XYZ position to LAFAN XYZ without changing centimetres."""
    return np.asarray(position_ue) @ UE_TO_LAFAN.T


def lafan_position_to_ue(position_lafan: np.ndarray) -> np.ndarray:
    return np.asarray(position_lafan) @ LAFAN_TO_UE.T


def ue_rotation_to_lafan(rotation_ue: np.ndarray) -> np.ndarray:
    """Basis-change a 3x3 rotation; never swap quaternion components."""
    rotation_ue = np.asarray(rotation_ue)
    return UE_TO_LAFAN @ rotation_ue @ LAFAN_TO_UE


def lafan_rotation_to_ue(rotation_lafan: np.ndarray) -> np.ndarray:
    rotation_lafan = np.asarray(rotation_lafan)
    return LAFAN_TO_UE @ rotation_lafan @ UE_TO_LAFAN


def matrix9d_to_6d(rotation: np.ndarray) -> np.ndarray:
    """Exactly matches official utils_np.matrix9D_to_6D()."""
    rotation = np.asarray(rotation)
    return rotation[..., :-1].reshape(*rotation.shape[:-2], 6)


def matrix6d_to_9d(rotation_6d: np.ndarray) -> np.ndarray:
    """Exactly matches the official Gram-Schmidt 6D decoder."""
    raw = np.asarray(rotation_6d, dtype=np.float64)
    if raw.ndim < 1 or raw.shape[-1] != 6 or not np.isfinite(raw).all():
        raise ValueError('6D rotations must be finite arrays with last dimension 6')
    value = raw.reshape(*raw.shape[:-1], 3, 2).copy()
    col0 = value[..., 0]
    norm0 = np.linalg.norm(col0, axis=-1, keepdims=True)
    if np.any(norm0 < 1e-10):
        raise ValueError('6D rotation has a zero first column')
    col0 /= norm0
    col1 = value[..., 1] - np.sum(col0 * value[..., 1], axis=-1, keepdims=True) * col0
    norm1 = np.linalg.norm(col1, axis=-1, keepdims=True)
    if np.any(norm1 < 1e-10):
        raise ValueError('6D rotation columns are parallel or degenerate')
    col1 /= norm1
    col2 = np.cross(col0, col1)
    return np.stack([col0, col1, col2], axis=-1)


def rebuild_local_rotation(global_rotation: np.ndarray,
                           parent_global_rotation: np.ndarray | None) -> np.ndarray:
    """Collapse skipped Manny bones by rebuilding locals from model parents."""
    if parent_global_rotation is None:
        return global_rotation
    return np.swapaxes(parent_global_rotation, -1, -2) @ global_rotation


def apply_global_rest_correction(rotation_in_lafan_axes: np.ndarray,
                                 correction: np.ndarray) -> np.ndarray:
    """Right-side joint-axis correction B: UE_ref @ B == LAFAN_ref.

    Requires right_local_v2 calibration, not legacy left-side matrices.
    """
    return rotation_in_lafan_axes @ correction


def quat_to_matrix(q: dict) -> np.ndarray:
    x, y, z, w = q["x"], q["y"], q["z"], q["w"]
    if not np.isfinite([x, y, z, w]).all() or x*x+y*y+z*z+w*w < 1e-20:
        raise ValueError('Quaternion must be finite and nonzero')
    s = 2.0 / (x*x + y*y + z*z + w*w)
    return np.array([
        [1-s*(y*y+z*z), s*(x*y-z*w), s*(x*z+y*w)],
        [s*(x*y+z*w), 1-s*(x*x+z*z), s*(y*z-x*w)],
        [s*(x*z-y*w), s*(y*z+x*w), 1-s*(x*x+y*y)],
    ])


def globals_to_locals(rotations, parents):
    rotations = np.asarray(rotations, dtype=np.float64)
    return np.stack([rebuild_local_rotation(rotations[..., j, :, :],
        None if p < 0 else rotations[..., p, :, :])
        for j, p in enumerate(parents)], axis=-3)


def locals_to_globals(rotations, parents):
    result = []
    for j, p in enumerate(parents):
        local = rotations[..., j, :, :]
        result.append(local if p < 0 else result[p] @ local)
    return np.stack(result, axis=-3)


def validate_calibration(calibration):
    if calibration.get('convention') != 'right_local_v2':
        raise ValueError('Legacy calibration: regenerate with build_fullbody_calibration_135.py')
    if calibration['joint_order'] != list(LAFAN_TO_MANNY) or calibration['manny_bone_order'] != list(LAFAN_TO_MANNY.values()):
        raise ValueError('Joint order does not match the 22-joint model layout')
    parents = calibration['parents']
    if len(parents) != 22 or parents[0] != -1 or any(not isinstance(p, int) or not 0 <= p < j for j,p in enumerate(parents[1:], 1)):
        raise ValueError('Invalid parent hierarchy')
    matrices = np.asarray(calibration['correction_matrices'], dtype=np.float64)
    if matrices.shape != (22,3,3) or not np.isfinite(matrices).all():
        raise ValueError('Invalid correction matrices')
    if not np.allclose(matrices @ matrices.swapaxes(-1,-2), np.eye(3), atol=1e-8, rtol=0) or not np.allclose(np.linalg.det(matrices), 1, atol=1e-8, rtol=0):
        raise ValueError('Corrections must be proper orthogonal rotations')
    return matrices


def _self_test() -> dict:
    max_position_error = 0.0
    max_rotation_error = 0.0
    max_6d_error = 0.0
    test_positions = [
        [0.0, 0.0, 0.0],
        [12.5, -4.0, 93.25],
        [-150.0, 32.0, 2.5],
    ]
    test_quaternions = [
        {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        {"x": 0.0, "y": 0.0, "z": 0.382683432365, "w": 0.923879532511},
        {"x": 0.258819045103, "y": 0.0, "z": 0.0, "w": 0.965925826289},
    ]
    for position, quaternion in zip(test_positions, test_quaternions):
        p = np.asarray(position, dtype=np.float64)
        r = quat_to_matrix(quaternion)

        p_rt = lafan_position_to_ue(ue_position_to_lafan(p))
        r_lafan = ue_rotation_to_lafan(r)
        r_rt = lafan_rotation_to_ue(r_lafan)
        r_6d_rt = matrix6d_to_9d(matrix9d_to_6d(r_lafan))

        max_position_error = max(max_position_error, float(np.max(np.abs(p_rt - p))))
        max_rotation_error = max(max_rotation_error, float(np.max(np.abs(r_rt - r))))
        max_6d_error = max(max_6d_error, float(np.max(np.abs(r_6d_rt - r_lafan))))

    result = {
        "tested_cases": len(test_positions),
        "position_roundtrip_max_abs_error": max_position_error,
        "rotation_basis_roundtrip_max_abs_error": max_rotation_error,
        "rotation_6d_roundtrip_max_abs_error": max_6d_error,
        "passed": max(max_position_error, max_rotation_error, max_6d_error) < 1e-9,
    }
    return result


if __name__ == "__main__":
    print(json.dumps(_self_test(), ensure_ascii=False, indent=2))
