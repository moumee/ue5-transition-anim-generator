"""Convert LAFAN1-model 135D output into UE5 Manny pose frames.

The model state contains 22 local 6D rotations followed by the LAFAN-space
Hips XYZ position.  This module reverses the coordinate/rest-pose mapping and
emits component-space rotations for the 22 mapped Manny bones.  When a Manny
skeleton export is supplied it also emits an 89-bone local pose: unmapped
twist, IK and finger bones retain their reference transforms.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import numpy as np

from manny_lafan_transform import (
    LAFAN_TO_UE,
    UE_TO_LAFAN,
    globals_to_locals,
    lafan_position_to_ue,
    locals_to_globals,
    matrix6d_to_9d,
    matrix_to_quat_xyzw,
    quat_to_matrix,
    ue_position_to_lafan,
    validate_calibration,
)
from official_context_preprocess import denormalize, load_stats, reverse_start_center_root


HERE = Path(__file__).resolve().parent
JOINT_COUNT = 22
STATE_SIZE = JOINT_COUNT * 6 + 3


def load_json(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def _vector3(value, field_name):
    if isinstance(value, dict):
        result = np.array([value["x"], value["y"], value["z"]], dtype=np.float64)
    else:
        result = np.asarray(value, dtype=np.float64)
    if result.shape != (3,) or not np.isfinite(result).all():
        raise ValueError(field_name + " must be a finite XYZ vector")
    return result


def _quaternion(value, field_name):
    if isinstance(value, dict):
        result = np.array([value["x"], value["y"], value["z"], value["w"]], dtype=np.float64)
    else:
        result = np.asarray(value, dtype=np.float64)
    if result.shape != (4,) or not np.isfinite(result).all() or np.linalg.norm(result) < 1e-10:
        raise ValueError(field_name + " must be a finite nonzero XYZW quaternion")
    return result / np.linalg.norm(result)


def _angle_degrees(a, b):
    relative = np.swapaxes(a, -1, -2) @ b
    cosine = np.clip((np.trace(relative, axis1=-2, axis2=-1) - 1.0) * 0.5, -1.0, 1.0)
    return np.degrees(np.arccos(cosine))


def _continuous_quaternion(rotation, previous):
    """Choose the quaternion hemisphere nearest the previous animation key."""
    quaternion = matrix_to_quat_xyzw(rotation)
    if previous is not None and float(np.dot(quaternion, previous)) < 0.0:
        quaternion *= -1.0
    return quaternion


def extract_states(document, stats_path=None):
    """Read model states and undo z-score normalization when declared.

    Accepted state fields are ``predictions_tx135``, ``vectors_tx135`` and
    ``output_tx135``.  Normalized data must explicitly declare
    ``normalization_applied: true`` and provide model statistics via
    ``stats_path``; silent guessing is intentionally rejected.
    """
    key = next(
        (name for name in ("predictions_tx135", "vectors_tx135", "output_tx135") if name in document),
        None,
    )
    if key is None:
        raise ValueError("JSON must contain predictions_tx135, vectors_tx135, or output_tx135")
    states = np.asarray(document[key], dtype=np.float64)
    if states.ndim != 2 or states.shape[1] != STATE_SIZE or len(states) < 1:
        raise ValueError("Model output must have shape (T, 135) with at least one frame")
    if not np.isfinite(states).all():
        raise ValueError("Model output contains non-finite values")

    normalized = bool(document.get("normalization_applied", False))
    if normalized:
        if stats_path is None:
            raise ValueError("Normalized model output requires --stats")
        mean, std = load_stats(stats_path)
        states = denormalize(states, mean, std)
    elif stats_path is not None:
        raise ValueError("--stats was supplied but normalization_applied is false")
    return states, key, normalized


def decode_lafan_states(states, calibration, placement=None):
    """Decode raw 135D states into Manny component-space transforms.

    ``placement`` may contain either/both of the following inverse transforms:

    * ``heading_position_offset_xz`` and ``heading_rotation_offset`` undo the
      official context heading alignment.
    * ``root_position_offset_lafan`` restores horizontal world placement used
      by :mod:`convert_manny_sequence_135`.
    """
    vectors = np.asarray(states, dtype=np.float64)
    if vectors.ndim != 2 or vectors.shape[1] != STATE_SIZE or not np.isfinite(vectors).all():
        raise ValueError("states must be a finite (T, 135) array")
    corrections = validate_calibration(calibration)
    parents = calibration["parents"]

    model_locals = matrix6d_to_9d(vectors[:, :132].reshape(-1, JOINT_COUNT, 6))
    root_positions = vectors[:, -3:].copy()
    placement = placement or {}

    heading_position = placement.get("heading_position_offset_xz")
    heading_rotation = placement.get("heading_rotation_offset")
    if (heading_position is None) != (heading_rotation is None):
        raise ValueError("Heading reversal requires both heading offsets")
    if heading_position is not None:
        root_positions, model_locals = reverse_start_center_root(
            root_positions,
            model_locals,
            np.asarray(heading_position, dtype=np.float64),
            np.asarray(heading_rotation, dtype=np.float64),
        )

    root_offset = np.asarray(
        placement.get("root_position_offset_lafan", [0.0, 0.0, 0.0]),
        dtype=np.float64,
    )
    if root_offset.shape != (3,) or not np.isfinite(root_offset).all():
        raise ValueError("root_position_offset_lafan must be a finite XYZ vector")
    root_positions = root_positions + root_offset

    corrected_globals = locals_to_globals(model_locals, parents)
    # Forward mapping is corrected = basis_changed_ue @ correction.
    lafan_axis_globals = corrected_globals @ np.swapaxes(corrections, -1, -2)
    ue_globals = np.einsum(
        "ab,tjbc,cd->tjad", LAFAN_TO_UE, lafan_axis_globals, UE_TO_LAFAN
    )
    pelvis_positions_ue = lafan_position_to_ue(root_positions)

    # Verify the inverse against the exact matrices actually emitted.
    remapped_lafan = np.einsum(
        "ab,tjbc,cd->tjad", UE_TO_LAFAN, ue_globals, LAFAN_TO_UE
    )
    remapped_corrected = remapped_lafan @ corrections
    remapped_locals = globals_to_locals(remapped_corrected, parents)
    rotation_errors = _angle_degrees(remapped_locals, model_locals)
    position_errors = np.abs(ue_position_to_lafan(pelvis_positions_ue) - root_positions)
    determinants = np.linalg.det(ue_globals)
    validation = {
        "shape_matches_sequence": list(vectors.shape) == [len(vectors), STATE_SIZE],
        "all_values_finite": bool(
            np.isfinite(ue_globals).all() and np.isfinite(pelvis_positions_ue).all()
        ),
        "max_inverse_rotation_roundtrip_error_degrees": float(np.max(rotation_errors)),
        "max_inverse_position_roundtrip_error_cm": float(np.max(position_errors)),
        "max_rotation_determinant_error": float(np.max(np.abs(determinants - 1.0))),
    }
    validation["passed"] = bool(
        validation["shape_matches_sequence"]
        and validation["all_values_finite"]
        and validation["max_inverse_rotation_roundtrip_error_degrees"] < 0.001
        and validation["max_inverse_position_roundtrip_error_cm"] < 1e-8
        and validation["max_rotation_determinant_error"] < 1e-8
    )
    return pelvis_positions_ue, ue_globals, validation


def component_pose_frames(pelvis_positions_ue, ue_globals, calibration, frame_indices, sample_rate_hz):
    """Build the compact 22-bone payload consumed by the UE/plugin side."""
    positions = np.asarray(pelvis_positions_ue, dtype=np.float64)
    rotations = np.asarray(ue_globals, dtype=np.float64)
    if rotations.shape != (len(positions), JOINT_COUNT, 3, 3):
        raise ValueError("Rotation/position sequence lengths do not match")
    if len(frame_indices) != len(positions):
        raise ValueError("frame_indices length does not match model output")
    if not np.isfinite(sample_rate_hz) or sample_rate_hz <= 0:
        raise ValueError("sample_rate_hz must be positive and finite")

    names = calibration["manny_bone_order"]
    frames = []
    previous_quaternions = [None] * len(names)
    for frame_number, source_index in enumerate(frame_indices):
        bone_payloads = []
        for joint, name in enumerate(names):
            quaternion = _continuous_quaternion(
                rotations[frame_number, joint], previous_quaternions[joint]
            )
            previous_quaternions[joint] = quaternion
            bone_payloads.append(
                {
                    "name": name,
                    "component_rotation_xyzw": quaternion.tolist(),
                }
            )
        frames.append(
            {
                "frame_index": source_index,
                "time_seconds": frame_number / float(sample_rate_hz),
                "pelvis_component_translation_cm": positions[frame_number].tolist(),
                "bones": bone_payloads,
            }
        )
    return frames


def build_full_local_pose_frames(compact_frames, skeleton, calibration):
    """Expand compact frames to the Manny mesh hierarchy using reference pose.

    The 22 modelled bones exactly match their target component rotations.
    Unmapped bones keep their reference local rotations/translations.  This is
    deterministic but does not synthesize twist/finger motion.
    """
    bones = skeleton.get("bones")
    if not isinstance(bones, list) or not bones:
        raise ValueError("Skeleton JSON must contain a non-empty bones list")
    names = [bone.get("name") for bone in bones]
    if len(set(names)) != len(names) or any(not name for name in names):
        raise ValueError("Skeleton bone names must be unique and non-empty")
    index_by_name = {name: index for index, name in enumerate(names)}
    mapped_names = calibration["manny_bone_order"]
    missing = [name for name in mapped_names if name not in index_by_name]
    if missing:
        raise ValueError("Mapped Manny bones missing from skeleton: " + ", ".join(missing))

    parents = []
    reference_rotations = []
    reference_translations = []
    reference_scales = []
    for index, bone in enumerate(bones):
        parent_name = bone.get("parent")
        parent = -1 if parent_name in (None, "", "None") else index_by_name.get(parent_name, -2)
        if parent == -2 or parent >= index:
            raise ValueError("Skeleton bones must be in parent-before-child order")
        parents.append(parent)
        transform = bone["reference_local_transform"]
        reference_translations.append(_vector3(transform["translation_cm"], names[index] + " translation"))
        reference_scales.append(_vector3(transform["scale"], names[index] + " scale"))
        q = _quaternion(transform["rotation_quaternion"], names[index] + " rotation")
        reference_rotations.append(quat_to_matrix(dict(zip(("x", "y", "z", "w"), q))))

    reference_rotations = np.asarray(reference_rotations)
    reference_translations = np.asarray(reference_translations)
    reference_scales = np.asarray(reference_scales)
    if not np.allclose(reference_scales, 1.0, atol=1e-6, rtol=0):
        raise ValueError("Non-unit reference bone scale is unsupported")
    mapped_set = set(mapped_names)
    expanded = []
    max_component_error = 0.0
    previous_local_quaternions = [None] * len(names)

    for compact in compact_frames:
        target_by_name = {
            bone["name"]: quat_to_matrix(
                dict(zip(("x", "y", "z", "w"), bone["component_rotation_xyzw"]))
            )
            for bone in compact["bones"]
        }
        local_rotations = reference_rotations.copy()
        local_translations = reference_translations.copy()
        component_rotations = []
        component_translations = []

        for index, name in enumerate(names):
            parent = parents[index]
            parent_rotation = np.eye(3) if parent < 0 else component_rotations[parent]
            parent_translation = np.zeros(3) if parent < 0 else component_translations[parent]

            if name in mapped_set:
                local_rotations[index] = parent_rotation.T @ target_by_name[name]
            if name == "pelvis":
                target_position = np.asarray(compact["pelvis_component_translation_cm"], dtype=np.float64)
                local_translations[index] = parent_rotation.T @ (target_position - parent_translation)

            component_rotations.append(parent_rotation @ local_rotations[index])
            component_translations.append(
                parent_translation + parent_rotation @ local_translations[index]
            )

        for name in mapped_names:
            index = index_by_name[name]
            max_component_error = max(
                max_component_error,
                float(_angle_degrees(component_rotations[index], target_by_name[name])),
            )

        local_bone_payloads = []
        for index, name in enumerate(names):
            quaternion = _continuous_quaternion(
                local_rotations[index], previous_local_quaternions[index]
            )
            previous_local_quaternions[index] = quaternion
            local_bone_payloads.append(
                {
                    "name": name,
                    "local_translation_cm": local_translations[index].tolist(),
                    "local_rotation_xyzw": quaternion.tolist(),
                    "local_scale": reference_scales[index].tolist(),
                }
            )

        expanded.append(
            {
                "frame_index": compact["frame_index"],
                "time_seconds": compact["time_seconds"],
                "bones": local_bone_payloads,
            }
        )

    return expanded, {
        "bone_count": len(names),
        "mapped_bone_count": len(mapped_names),
        "unmapped_bones_use_reference_pose": True,
        "max_mapped_component_rotation_error_degrees": max_component_error,
        "passed": max_component_error < 0.001,
    }


def convert_document(document, calibration, stats_path=None, skeleton=None, sample_rate_hz=None):
    states, state_key, normalized = extract_states(document, stats_path=stats_path)
    rate = float(
        document.get("sample_rate_hz", 30.0)
        if sample_rate_hz is None
        else sample_rate_hz
    )
    frame_indices = document.get("frame_indices", list(range(len(states))))

    placement = {
        key: document[key]
        for key in (
            "root_position_offset_lafan",
            "heading_position_offset_xz",
            "heading_rotation_offset",
        )
        if key in document
    }
    pelvis_positions, ue_globals, inverse_validation = decode_lafan_states(
        states, calibration, placement=placement
    )
    compact = component_pose_frames(
        pelvis_positions, ue_globals, calibration, frame_indices, rate
    )
    result = {
        "schema_version": 1,
        "description": "LAFAN1 model output mapped to UE5 Manny component-space poses.",
        "source_state_field": state_key,
        "source_was_normalized": normalized,
        "sample_rate_hz": rate,
        "frame_count": len(compact),
        "frame_indices": frame_indices,
        "manny_bone_order": calibration["manny_bone_order"],
        "coordinate_space": "UE component space: +X forward, +Y right, +Z up, centimetres",
        "frames": compact,
        "validation": {"inverse_mapping": inverse_validation},
        "limitations": [
            "The model drives 22 mapped bones only.",
            "Twist, IK and finger bones retain the Manny reference pose when a skeleton export is supplied.",
            "The prototype rest-pose calibration still requires final UE visual approval.",
        ],
    }
    if skeleton is not None:
        full_frames, full_validation = build_full_local_pose_frames(
            compact, skeleton, calibration
        )
        result["skeletal_mesh"] = skeleton.get("skeletal_mesh")
        result["skeleton"] = skeleton.get("skeleton")
        result["full_local_pose_frames"] = full_frames
        result["validation"]["full_skeleton_expansion"] = full_validation
    result["validation"]["passed"] = all(
        section.get("passed", False) for section in result["validation"].values()
        if isinstance(section, dict)
    )
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Convert LAFAN1 model 135D output to UE5 Manny pose JSON."
    )
    parser.add_argument("input", help="model-output JSON")
    parser.add_argument(
        "-c",
        "--calibration",
        default=str(HERE / "rest_pose_corrections_22_prototype.json"),
    )
    parser.add_argument("--stats", help="official model statistics pickle for normalized output")
    parser.add_argument("--skeleton", help="Manny skeleton JSON exported in UE; enables 89-bone local poses")
    parser.add_argument("--sample-rate-hz", type=float, help="override output sample rate")
    parser.add_argument(
        "-o",
        "--output",
        default=str(HERE / "lafan_output_manny_pose.json"),
    )
    args = parser.parse_args()

    document = load_json(args.input)
    calibration = load_json(args.calibration)
    skeleton = load_json(args.skeleton) if args.skeleton else None
    result = convert_document(
        document,
        calibration,
        stats_path=args.stats,
        skeleton=skeleton,
        sample_rate_hz=args.sample_rate_hz,
    )
    if not result["validation"]["passed"]:
        raise ValueError("Inverse mapping validation failed: " + json.dumps(result["validation"]))
    with open(args.output, "w", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print(
        json.dumps(
            {
                "output": os.path.abspath(args.output),
                "frame_count": result["frame_count"],
                "full_skeleton_expanded": "full_local_pose_frames" in result,
                "validation": result["validation"],
            },
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
