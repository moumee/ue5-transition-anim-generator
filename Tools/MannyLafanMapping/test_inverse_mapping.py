"""Regression tests for LAFAN1 135D output -> UE5 Manny mapping."""

import json
from pathlib import Path
import unittest

import numpy as np

from convert_lafan_output_to_manny import (
    build_full_local_pose_frames,
    convert_document,
    extract_states,
)
from convert_manny_sequence_135 import convert_sequence
from manny_lafan_transform import matrix_to_quat_xyzw, quat_to_matrix


HERE = Path(__file__).resolve().parent


def rotation_from_xyzw(value):
    return quat_to_matrix(dict(zip(("x", "y", "z", "w"), value)))


def rotation_angle_degrees(a, b):
    relative = a.T @ b
    cosine = np.clip((np.trace(relative) - 1.0) * 0.5, -1.0, 1.0)
    return float(np.degrees(np.arccos(cosine)))


def synthetic_manny_skeleton():
    """Small real-hierarchy analogue including skipped spine/neck bones."""
    hierarchy = [
        ("root", None),
        ("pelvis", "root"),
        ("spine_01", "pelvis"),
        ("thigh_r", "pelvis"),
        ("thigh_l", "pelvis"),
        ("spine_02", "spine_01"),
        ("calf_r", "thigh_r"),
        ("calf_l", "thigh_l"),
        ("spine_03", "spine_02"),
        ("foot_r", "calf_r"),
        ("foot_l", "calf_l"),
        ("spine_04", "spine_03"),
        ("ball_r", "foot_r"),
        ("ball_l", "foot_l"),
        ("spine_05", "spine_04"),
        ("neck_01", "spine_05"),
        ("clavicle_l", "spine_05"),
        ("clavicle_r", "spine_05"),
        ("neck_02", "neck_01"),
        ("upperarm_l", "clavicle_l"),
        ("upperarm_r", "clavicle_r"),
        ("head", "neck_02"),
        ("lowerarm_l", "upperarm_l"),
        ("lowerarm_r", "upperarm_r"),
        ("hand_l", "lowerarm_l"),
        ("hand_r", "lowerarm_r"),
    ]
    identity = {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}
    return {
        "skeletal_mesh": "/Game/Test/SKM_Manny_Simple",
        "skeleton": "/Game/Test/SK_Mannequin",
        "bones": [
            {
                "index": index,
                "name": name,
                "parent": parent,
                "reference_local_transform": {
                    "translation_cm": {"x": 0.0, "y": 0.0, "z": 0.0},
                    "rotation_quaternion": identity,
                    "scale": {"x": 1.0, "y": 1.0, "z": 1.0},
                },
            }
            for index, (name, parent) in enumerate(hierarchy)
        ],
    }


class InverseMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calibration = json.loads(
            (HERE / "rest_pose_corrections_22_prototype.json").read_text(encoding="utf-8")
        )
        cls.sequence = json.loads(
            (HERE / "manny_mm_idle_frames_0_9.json").read_text(encoding="utf-8")
        )
        cls.raw = convert_sequence(cls.sequence, cls.calibration)
        cls.raw["sample_rate_hz"] = 30.0

    def test_forward_then_inverse_recovers_source_component_pose(self):
        result = convert_document(self.raw, self.calibration)
        self.assertTrue(result["validation"]["passed"])
        self.assertEqual(result["frame_count"], len(self.sequence["frames"]))

        max_rotation_error = 0.0
        max_position_error = 0.0
        for source_frame, output_frame in zip(self.sequence["frames"], result["frames"]):
            source = {bone["name"]: bone for bone in source_frame["bones"]}
            output = {bone["name"]: bone for bone in output_frame["bones"]}
            max_position_error = max(
                max_position_error,
                float(
                    np.max(
                        np.abs(
                            np.asarray(output_frame["pelvis_component_translation_cm"])
                            - np.asarray(source["pelvis"]["world"]["translation"])
                        )
                    )
                ),
            )
            for name in self.calibration["manny_bone_order"]:
                source_rotation = rotation_from_xyzw(source[name]["world"]["rotation_xyzw"])
                output_rotation = rotation_from_xyzw(output[name]["component_rotation_xyzw"])
                max_rotation_error = max(
                    max_rotation_error,
                    rotation_angle_degrees(source_rotation, output_rotation),
                )
        self.assertLess(max_position_error, 1e-8)
        self.assertLess(max_rotation_error, 0.001)
        for joint, name in enumerate(self.calibration["manny_bone_order"]):
            quaternions = np.asarray(
                [frame["bones"][joint]["component_rotation_xyzw"] for frame in result["frames"]]
            )
            self.assertTrue(np.all(np.sum(quaternions[:-1] * quaternions[1:], axis=1) >= 0.0), name)

    def test_full_pose_preserves_mapped_component_rotations(self):
        compact = convert_document(self.raw, self.calibration)["frames"]
        frames, validation = build_full_local_pose_frames(
            compact, synthetic_manny_skeleton(), self.calibration
        )
        self.assertTrue(validation["passed"])
        self.assertEqual(validation["mapped_bone_count"], 22)
        self.assertEqual(len(frames[0]["bones"]), 26)

    def test_normalization_contract_is_explicit(self):
        document = {
            "predictions_tx135": np.zeros((2, 135)).tolist(),
            "normalization_applied": True,
        }
        with self.assertRaises(ValueError):
            extract_states(document)
        document["normalization_applied"] = False
        states, key, normalized = extract_states(document)
        self.assertEqual(states.shape, (2, 135))
        self.assertEqual(key, "predictions_tx135")
        self.assertFalse(normalized)

    def test_matrix_quaternion_roundtrip(self):
        for quaternion in (
            [0.0, 0.0, 0.0, 1.0],
            [np.sin(np.pi / 4), 0.0, 0.0, np.cos(np.pi / 4)],
            [0.0, 0.0, 1.0, 0.0],
        ):
            matrix = rotation_from_xyzw(quaternion)
            restored = rotation_from_xyzw(matrix_to_quat_xyzw(matrix))
            np.testing.assert_allclose(restored, matrix, atol=1e-10, rtol=0)

    def test_bad_shape_and_missing_skeleton_bone_rejected(self):
        with self.assertRaises(ValueError):
            extract_states({"vectors_tx135": np.zeros((2, 134)).tolist()})
        compact = convert_document(self.raw, self.calibration)["frames"]
        skeleton = synthetic_manny_skeleton()
        skeleton["bones"] = [bone for bone in skeleton["bones"] if bone["name"] != "hand_r"]
        with self.assertRaises(ValueError):
            build_full_local_pose_frames(compact, skeleton, self.calibration)


if __name__ == "__main__":
    unittest.main(verbosity=2)
