"""Convert an exported Manny pose sequence to raw LAFAN1 135D states."""

import argparse
import json
import os

import numpy as np

from manny_lafan_transform import (
    UE_TO_LAFAN,
    matrix6d_to_9d,
    matrix9d_to_6d,
    quat_to_matrix,
    apply_global_rest_correction,
    globals_to_locals,
    locals_to_globals,
    validate_calibration,
)


HERE = os.path.dirname(os.path.abspath(__file__))


def angle_degrees(a, b):
    relative = a.T @ b
    cosine = np.clip((np.trace(relative) - 1.0) * 0.5, -1.0, 1.0)
    return float(np.degrees(np.arccos(cosine)))


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def convert_sequence(sequence, calibration, max_root_step_cm=5.0, max_joint_step_degrees=45.0):
    """Idle diagnostic limits are per sample, not universal motion limits.

    Input timing must be established before adopting speed-based thresholds.
    Returns raw unnormalized states and validation; never writes files.
    """
    if max_root_step_cm <= 0 or max_joint_step_degrees <= 0 or not np.isfinite([max_root_step_cm, max_joint_step_degrees]).all():
        raise ValueError('Continuity limits must be positive and finite')

    names = calibration["joint_order"]
    manny_names = calibration["manny_bone_order"]
    parents = calibration["parents"]
    corrections = validate_calibration(calibration)
    frames = sequence['frames']
    indices = [f['frame_index'] for f in frames]
    if len(frames) < 2 or sequence['frame_indices'] != indices or any(b-a != 1 for a,b in zip(indices, indices[1:])):
        raise ValueError('At least two consecutive, consistently indexed frames required')
    if sequence.get('frame_count', len(frames)) != len(frames):
        raise ValueError('frame_count mismatch')

    states = []
    corrected_globals_by_frame = []
    original_ue_globals_by_frame = []
    root_positions = []

    for frame in sequence["frames"]:
        bones = {bone["name"]: bone for bone in frame["bones"]}
        if len(bones) != len(frame['bones']) or any(n not in bones for n in manny_names):
            raise ValueError('Missing or duplicate mapped bone')
        for name in manny_names:
            transform = bones[name]['world']
            position = np.asarray(transform['translation'], dtype=float)
            quaternion = np.asarray(transform['rotation_xyzw'], dtype=float)
            if position.shape != (3,) or not np.isfinite(position).all() or quaternion.shape != (4,):
                raise ValueError('Invalid transform for ' + name)
            if not np.allclose(transform.get('scale', [1,1,1]), [1,1,1], atol=1e-6):
                raise ValueError('Non-unit scale is unsupported: ' + name)
        corrected_globals = []
        original_ue_globals = []
        for joint_index, manny_name in enumerate(manny_names):
            q = bones[manny_name]["world"]["rotation_xyzw"]
            ue_global = quat_to_matrix(dict(zip(("x", "y", "z", "w"), q)))
            in_lafan_axes = UE_TO_LAFAN @ ue_global @ UE_TO_LAFAN.T
            corrected_globals.append(apply_global_rest_correction(in_lafan_axes, corrections[joint_index]))
            original_ue_globals.append(ue_global)

        corrected_globals = np.asarray(corrected_globals)
        local_rotations = globals_to_locals(corrected_globals, parents)
        rotation_6d = matrix9d_to_6d(local_rotations)

        pelvis_position_ue = np.asarray(
            bones["pelvis"]["world"]["translation"], dtype=np.float64
        )
        root_positions.append(UE_TO_LAFAN @ pelvis_position_ue)
        corrected_globals_by_frame.append(corrected_globals)
        original_ue_globals_by_frame.append(np.asarray(original_ue_globals))
        states.append({
            "frame_index": frame["frame_index"],
            "rotation_6d": rotation_6d,
        })

    # Match the repository's context convention: the final context frame is the
    # horizontal origin. Height is preserved.
    root_positions = np.asarray(root_positions)
    centered_positions = root_positions.copy()
    centered_positions[:, 0] -= root_positions[-1, 0]
    centered_positions[:, 2] -= root_positions[-1, 2]

    vectors = []
    decoded_local_errors = []
    reconstructed_ue_errors = []
    determinants = []
    for frame_index, state in enumerate(states):
        rotation_6d = state["rotation_6d"]
        decoded_locals = matrix6d_to_9d(rotation_6d)
        determinants.extend(np.linalg.det(decoded_locals).tolist())

        # Recreate corrected globals from decoded local rotations.
        decoded_globals = locals_to_globals(decoded_locals, parents)

        for joint_index in range(len(names)):
            decoded_local_errors.append(
                angle_degrees(
                    decoded_globals[joint_index],
                    corrected_globals_by_frame[frame_index][joint_index],
                )
            )
            recovered_lafan_axes = decoded_globals[joint_index] @ corrections[joint_index].T
            recovered_ue = UE_TO_LAFAN.T @ recovered_lafan_axes @ UE_TO_LAFAN
            reconstructed_ue_errors.append(
                angle_degrees(
                    recovered_ue,
                    original_ue_globals_by_frame[frame_index][joint_index],
                )
            )

        vectors.append(
            np.concatenate([rotation_6d.reshape(-1), centered_positions[frame_index]])
        )

    vectors = np.asarray(vectors)
    root_offset = root_positions[-1].copy()
    root_offset[1] = 0
    restored_positions = (vectors[:, -3:] + root_offset) @ UE_TO_LAFAN
    source_positions = np.array([next(b for b in f['bones'] if b['name']=='pelvis')['world']['translation'] for f in frames])
    root_roundtrip_error = float(np.max(np.abs(restored_positions-source_positions)))
    root_step_distances = np.linalg.norm(np.diff(root_positions, axis=0), axis=1)
    joint_step_angles = []
    for frame_index in range(1, len(corrected_globals_by_frame)):
        joint_step_angles.extend([
            angle_degrees(
                corrected_globals_by_frame[frame_index - 1][joint_index],
                corrected_globals_by_frame[frame_index][joint_index],
            )
            for joint_index in range(len(names))
        ])

    validation = {
        "shape_matches_sequence": list(vectors.shape) == [len(frames), 135],
        "root_position_roundtrip_max_error_cm": root_roundtrip_error,
        "continuity_limits": {"profile": "caller_supplied_per_sample", "root_step_cm": max_root_step_cm, "joint_step_degrees": max_joint_step_degrees},
        "all_values_finite": bool(np.all(np.isfinite(vectors))),
        "max_6d_to_corrected_global_angle_error_degrees": max(decoded_local_errors),
        "max_inverse_to_original_ue_global_angle_error_degrees": max(reconstructed_ue_errors),
        "max_rotation_determinant_error": float(
            np.max(np.abs(np.asarray(determinants) - 1.0))
        ),
        "max_root_step_cm": float(np.max(root_step_distances)),
        "max_joint_step_angle_degrees": max(joint_step_angles),
    }
    validation["passed"] = bool(
        validation["shape_matches_sequence"]
        and validation["all_values_finite"]
        and validation["max_6d_to_corrected_global_angle_error_degrees"] < 0.001
        and validation["max_inverse_to_original_ue_global_angle_error_degrees"] < 0.001
        and validation["max_rotation_determinant_error"] < 1e-9
        and validation["max_joint_step_angle_degrees"] < max_joint_step_degrees
        and validation["max_root_step_cm"] < max_root_step_cm
        and root_roundtrip_error < 1e-8
    )

    output = {
        "description": "Raw Manny animation states in the LAFAN1 model layout.",
        "source_animation": sequence["animation"],
        "frame_indices": sequence["frame_indices"],
        "shape": list(vectors.shape),
        "layout": "T x (22 joints * rotation6D + Hips XYZ)",
        "joint_order": names,
        "calibration_convention": calibration['convention'],
        "root_position_offset_lafan": root_offset.tolist(),
        "horizontal_center_reference_frame": sequence["frame_indices"][-1],
        "heading_alignment_applied": False,
        "normalization_applied": False,
        "root_positions_before_centering": root_positions.tolist(),
        "root_positions_after_centering": centered_positions.tolist(),
        "vectors_tx135": vectors.tolist(),
        "validation": validation,
        "warning": "Raw pre-normalization prototype. Rest-pose corrections still require team visual approval.",
    }
    return output


def main():
    parser = argparse.ArgumentParser(
        description="Convert an exported UE5 Manny sequence to raw LAFAN1 135D states."
    )
    parser.add_argument(
        "input",
        nargs="?",
        default=os.path.join(HERE, "manny_mm_idle_frames_0_9.json"),
        help="UE-exported Manny sequence JSON",
    )
    parser.add_argument(
        "-c",
        "--calibration",
        default=os.path.join(HERE, "rest_pose_corrections_22_prototype.json"),
        help="22-joint rest-pose calibration JSON",
    )
    parser.add_argument(
        "-o",
        "--output",
        default=os.path.join(HERE, "manny_sequence_135_raw.json"),
        help="output raw 135D JSON",
    )
    parser.add_argument("--max-root-step-cm", type=float, default=15.0)
    parser.add_argument("--max-joint-step-degrees", type=float, default=60.0)
    args = parser.parse_args()

    sequence = load_json(args.input)
    calibration = load_json(args.calibration)
    output = convert_sequence(
        sequence,
        calibration,
        max_root_step_cm=args.max_root_step_cm,
        max_joint_step_degrees=args.max_joint_step_degrees,
    )
    validation = output['validation']
    if not validation['passed']:
        raise ValueError('Sequence validation failed: ' + json.dumps(validation))
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(output, f, ensure_ascii=False, indent=2)
    print(json.dumps({
        "output": os.path.abspath(args.output),
        "shape": output['shape'],
        "validation": validation,
    }, indent=2))


if __name__ == "__main__":
    main()
