"""NumPy equivalent of the official context-model preprocessing.

The official implementation lives in
``official_motion_inbetweening/packages/motion_inbetween/data/utils_torch.py``.
This module keeps the same convention without requiring PyTorch: the last
context frame (index ``context_len - 1``) is moved to the horizontal origin
and the root joint's local +Y axis is turned toward world +X.
"""

from __future__ import annotations

import pickle
from pathlib import Path

import numpy as np

from manny_lafan_transform import matrix6d_to_9d, matrix9d_to_6d


JOINT_COUNT = 22
STATE_SIZE = JOINT_COUNT * 6 + 3


def yaw_matrix(angle_radians: float) -> np.ndarray:
    """Return the same Y-axis matrix used by official euler_to_matrix9D."""
    if not np.isfinite(angle_radians):
        raise ValueError("Yaw angle must be finite")
    c, s = np.cos(angle_radians), np.sin(angle_radians)
    return np.array([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])


def get_root_heading_offset(root_rotation: np.ndarray,
                            forward_axis: str = "x") -> tuple[np.ndarray, float]:
    """Compute the official yaw offset from the root's local +Y direction."""
    root_rotation = np.asarray(root_rotation, dtype=np.float64)
    if root_rotation.shape != (3, 3) or not np.isfinite(root_rotation).all():
        raise ValueError("root_rotation must be a finite 3x3 matrix")

    heading = root_rotation @ np.array([0.0, 1.0, 0.0])
    heading[1] = 0.0
    length = np.linalg.norm(heading)
    if length < 1e-10:
        raise ValueError("Root local +Y has no stable horizontal projection")
    heading /= length

    if forward_axis == "x":
        forward = np.array([1.0, 0.0, 0.0])
    elif forward_axis == "z":
        forward = np.array([0.0, 0.0, 1.0])
    else:
        raise ValueError("forward_axis must be 'x' or 'z'")

    dot = float(np.dot(heading, forward))
    cross = np.cross(heading, forward)
    angle = float(np.arctan2(np.dot(cross, [0.0, 1.0, 0.0]), dot))
    return yaw_matrix(angle), angle


def start_center_root(root_positions: np.ndarray,
                      local_rotations: np.ndarray,
                      context_len: int = 10,
                      forward_axis: str = "x"):
    """Apply official start centering to the model-relevant root data."""
    positions = np.asarray(root_positions, dtype=np.float64)
    rotations = np.asarray(local_rotations, dtype=np.float64)
    if positions.ndim != 2 or positions.shape[1] != 3:
        raise ValueError("root_positions must have shape (T, 3)")
    if rotations.shape != (len(positions), JOINT_COUNT, 3, 3):
        raise ValueError("local_rotations must have shape (T, 22, 3, 3)")
    if not np.isfinite(positions).all() or not np.isfinite(rotations).all():
        raise ValueError("Input contains non-finite values")
    if not isinstance(context_len, int) or context_len < 1 or len(positions) < context_len:
        raise ValueError("context_len must select an existing frame")

    frame = context_len - 1
    pos_offset_xz = positions[frame, [0, 2]].copy()
    centered_positions = positions.copy()
    centered_positions[:, 0] -= pos_offset_xz[0]
    centered_positions[:, 2] -= pos_offset_xz[1]

    rot_offset, yaw_angle = get_root_heading_offset(
        rotations[frame, 0], forward_axis=forward_axis
    )
    centered_positions = np.einsum("ij,tj->ti", rot_offset, centered_positions)
    centered_rotations = rotations.copy()
    centered_rotations[:, 0] = np.einsum(
        "ij,tjk->tik", rot_offset, centered_rotations[:, 0]
    )
    return centered_positions, centered_rotations, pos_offset_xz, rot_offset, yaw_angle


def reverse_start_center_root(centered_positions: np.ndarray,
                              centered_rotations: np.ndarray,
                              pos_offset_xz: np.ndarray,
                              rot_offset: np.ndarray):
    """Undo :func:`start_center_root` for numerical validation/output recovery."""
    positions = np.asarray(centered_positions, dtype=np.float64).copy()
    rotations = np.asarray(centered_rotations, dtype=np.float64).copy()
    pos_offset_xz = np.asarray(pos_offset_xz, dtype=np.float64)
    rot_offset = np.asarray(rot_offset, dtype=np.float64)
    if positions.ndim != 2 or positions.shape[1] != 3:
        raise ValueError("centered_positions must have shape (T, 3)")
    if rotations.shape != (len(positions), JOINT_COUNT, 3, 3):
        raise ValueError("centered_rotations must have shape (T, 22, 3, 3)")
    if pos_offset_xz.shape != (2,) or rot_offset.shape != (3, 3):
        raise ValueError("Invalid offset shape")

    inverse = rot_offset.T
    positions = np.einsum("ij,tj->ti", inverse, positions)
    rotations[:, 0] = np.einsum("ij,tjk->tik", inverse, rotations[:, 0])
    positions[:, 0] += pos_offset_xz[0]
    positions[:, 2] += pos_offset_xz[1]
    return positions, rotations


def pack_state(local_rotations: np.ndarray, root_positions: np.ndarray) -> np.ndarray:
    rotations = np.asarray(local_rotations, dtype=np.float64)
    positions = np.asarray(root_positions, dtype=np.float64)
    if rotations.shape != (len(positions), JOINT_COUNT, 3, 3):
        raise ValueError("Rotation/position shapes do not match")
    return np.concatenate([matrix9d_to_6d(rotations).reshape(len(positions), -1),
                           positions], axis=-1)


def unpack_raw_json(raw: dict) -> tuple[np.ndarray, np.ndarray]:
    vector_data = raw.get("vectors_tx135", raw.get("vectors_10x135"))
    if vector_data is None:
        raise ValueError("Raw JSON has no vectors_tx135 or vectors_10x135 field")
    vectors = np.asarray(vector_data, dtype=np.float64)
    positions = np.asarray(raw["root_positions_before_centering"], dtype=np.float64)
    if vectors.ndim != 2 or vectors.shape[1] != STATE_SIZE or len(vectors) != len(positions):
        raise ValueError("Raw JSON must contain matching T x 135 states and T x 3 roots")
    rotations = matrix6d_to_9d(vectors[:, :132].reshape(-1, JOINT_COUNT, 6))
    return positions, rotations


def load_stats(path: str | Path) -> tuple[np.ndarray, np.ndarray]:
    with open(path, "rb") as stream:
        stats = pickle.load(stream)
    mean = np.asarray(stats["mean"], dtype=np.float64)
    std = np.asarray(stats["std"], dtype=np.float64)
    if mean.shape != (STATE_SIZE,) or std.shape != (STATE_SIZE,):
        raise ValueError("Statistics must each have shape (135,)")
    if not np.isfinite(mean).all() or not np.isfinite(std).all() or np.any(std <= 0):
        raise ValueError("Statistics must be finite and standard deviations positive")
    return mean, std


def normalize(states: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    states = np.asarray(states, dtype=np.float64)
    mean = np.asarray(mean, dtype=np.float64)
    std = np.asarray(std, dtype=np.float64)
    if states.ndim != 2 or states.shape[1] != STATE_SIZE:
        raise ValueError("states must have shape (T, 135)")
    if mean.shape != (STATE_SIZE,) or std.shape != (STATE_SIZE,):
        raise ValueError("mean and std must each have shape (135,)")
    if not np.isfinite(mean).all() or not np.isfinite(std).all() or np.any(std <= 0):
        raise ValueError("mean and std must be finite and std must be positive")
    return (states - mean) / std


def project_static_channels(states: np.ndarray, mean: np.ndarray, std: np.ndarray,
                            threshold: float = 1e-6) -> tuple[np.ndarray, np.ndarray]:
    """Set channels absent from LAFAN1 variation to their training mean.

    In the supplied LAFAN1 statistics these are exactly the twelve 6D
    rotation channels of LeftToe and RightToe.  Dividing Manny toe motion by
    a ~1e-8 standard deviation would otherwise create inputs in the millions.
    This is an explicit domain-adapter rule, not a modification of z-score
    normalization.
    """
    values = np.asarray(states, dtype=np.float64)
    if values.ndim != 2 or values.shape[1] != STATE_SIZE:
        raise ValueError("states must have shape (T, 135)")
    if not np.isfinite(threshold) or threshold <= 0:
        raise ValueError("threshold must be positive and finite")
    static_mask = np.asarray(std) < threshold
    result = values.copy()
    result[:, static_mask] = np.asarray(mean)[static_mask]
    return result, np.flatnonzero(static_mask)


def denormalize(states: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    states = np.asarray(states, dtype=np.float64)
    mean = np.asarray(mean, dtype=np.float64)
    std = np.asarray(std, dtype=np.float64)
    if states.ndim != 2 or states.shape[1] != STATE_SIZE:
        raise ValueError("states must have shape (T, 135)")
    if mean.shape != (STATE_SIZE,) or std.shape != (STATE_SIZE,):
        raise ValueError("mean and std must each have shape (135,)")
    if not np.isfinite(mean).all() or not np.isfinite(std).all() or np.any(std <= 0):
        raise ValueError("mean and std must be finite and std must be positive")
    return states * std + mean
