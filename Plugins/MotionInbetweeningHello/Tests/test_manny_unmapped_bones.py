"""Unit tests for Scripts/manny_unmapped_bones.py (no Unreal, no model weights)."""
import copy
from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Scripts"))
import manny_unmapped_bones as unmapped  # noqa: E402
from manny_root_motion import quat_to_matrix  # noqa: E402

# root -> pelvis -> spine (mapped) -> gap (unmapped) -> arm (mapped) -> finger (unmapped); root -> ik (unmapped)
PARENTS = {"root": None, "pelvis": "root", "spine": "pelvis", "gap": "spine", "arm": "gap", "finger": "arm", "ik": "root"}
NAMES = list(PARENTS)
COLUMN = {name: index for index, name in enumerate(NAMES)}
CONTEXT, TRANSITION = 10, 11
FRAMES = CONTEXT + TRANSITION + 1
SKELETON = {"bones": [{"name": name, "parent": parent} for name, parent in PARENTS.items()]}


def quat(axis, degrees):
    half = np.radians(degrees) / 2.0
    return [float(v) for v in np.append(np.sin(half) * np.asarray(axis, dtype=float), np.cos(half))]


def component_rotation(frame, name):
    bones = {bone["name"]: quat_to_matrix(bone["local_rotation_xyzw"]) for bone in frame["bones"]}
    rotation = bones[name]
    while PARENTS[name] is not None:
        name = PARENTS[name]
        rotation = bones[name] @ rotation
    return rotation


def clip(samples, gap, finger, gap_step=1.0, finger_step=2.0, **extra):
    rows = []
    for index in range(samples):
        rotations = {name: quat([0, 0, 1], 0) for name in NAMES}
        rotations["gap"] = quat([1, 0, 0], gap + gap_step * index)
        rotations["finger"] = quat([0, 1, 0], finger + finger_step * index)
        rows.append([[1.0, 0.0, 0.0] + rotations[name] for name in NAMES])
    return {"source_full_pose": {"bones": NAMES, "local": rows, "sample_rate_hz": 30.0, **extra}}


def converted_pose():
    frames = []
    for index in range(FRAMES):
        rotations = {"root": quat([0, 0, 1], 0), "pelvis": quat([0, 0, 1], 3 * index), "spine": quat([1, 0, 0], 2 * index),
                     "gap": quat([0, 0, 1], 0), "arm": quat([0, 1, 0], 5 * index), "finger": quat([0, 0, 1], 0), "ik": quat([0, 0, 1], 0)}
        frames.append({"bones": [{"name": name, "local_translation_cm": [1.0, 0.0, 0.0], "local_rotation_xyzw": rotations[name],
                                  "local_scale": [1.0, 1.0, 1.0]} for name in NAMES]})
    return {"manny_bone_order": ["pelvis", "spine", "arm"], "full_local_pose_frames": frames}


def rotation(frames, frame, name):
    return frames[frame]["bones"][COLUMN[name]]["local_rotation_xyzw"]


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.start, self.end = clip(40, 10.0, 20.0), clip(40, 40.0, -30.0, gap_step=-0.5)
        self.reference = unmapped.build_reference(self.start, self.end, 5, 30, TRANSITION)

    def test_inputs_without_the_full_pose_give_no_reference(self):
        self.assertIsNone(unmapped.build_reference({}, self.end, 5, 30, TRANSITION))
        self.assertIsNone(unmapped.build_reference(self.start, {"source_full_pose": None}, 5, 30, TRANSITION))
        document = converted_pose()
        before = copy.deepcopy(document["full_local_pose_frames"])
        self.assertEqual(unmapped.apply(document, SKELETON, None)["source"], "reference_pose")
        self.assertEqual(document["full_local_pose_frames"], before)

    def test_pinned_frames_copy_the_clips_and_mapped_orientation_is_kept(self):
        document = converted_pose()
        before = copy.deepcopy(document["full_local_pose_frames"])
        report = unmapped.apply(document, SKELETON, self.reference)
        after = document["full_local_pose_frames"]
        self.assertEqual((report["source"], report["filled_bones"]), ("source_clips", 2))
        self.assertEqual(report["pinned_frames"], list(range(CONTEXT)) + [FRAMES - 1])
        for frame in range(FRAMES):
            # bones outside the pelvis and the mapped bones' component-space orientation stay as they were
            self.assertEqual(after[frame]["bones"][COLUMN["ik"]], before[frame]["bones"][COLUMN["ik"]])
            for name in ("pelvis", "spine", "arm"):
                np.testing.assert_allclose(component_rotation(after[frame], name), component_rotation(before[frame], name), atol=1e-12)
        for name in ("gap", "finger"):
            for frame in range(CONTEXT):
                np.testing.assert_allclose(rotation(after, frame, name), self.start["source_full_pose"]["local"][5 + frame][COLUMN[name]][3:], atol=1e-12)
            np.testing.assert_allclose(rotation(after, FRAMES - 1, name), self.end["source_full_pose"]["local"][30][COLUMN[name]][3:], atol=1e-12)

    def test_speed_is_continuous_at_both_joins(self):
        document = converted_pose()
        unmapped.apply(document, SKELETON, self.reference)
        after = document["full_local_pose_frames"]
        steps = [unmapped.angle_degrees(rotation(after, frame, "gap"), rotation(after, frame + 1, "gap")) for frame in range(FRAMES - 1)]
        # the start clip turns this bone 1 degree per frame, the destination clip 0.5 degrees per frame
        self.assertAlmostEqual(steps[CONTEXT - 2], 1.0, places=9)
        self.assertAlmostEqual(steps[CONTEXT - 1], 1.0, delta=0.25)
        self.assertAlmostEqual(steps[-1], 0.5, delta=0.25)

    def test_looping_clips_wrap_and_others_hold_their_ends(self):
        looping = clip(21, 0.0, 0.0, gap_step=360.0 / 20, finger_step=0.0, play_length_seconds=20 / 30)["source_full_pose"]
        wrapped = unmapped.Clip(looping, NAMES, "start")
        self.assertTrue(wrapped.cyclic)
        np.testing.assert_allclose(wrapped.sample(23), wrapped.sample(3))
        np.testing.assert_allclose(wrapped.sample(-2), wrapped.sample(18))
        one_shot = unmapped.Clip(clip(21, 0.0, 0.0, gap_step=3.0, play_length_seconds=20 / 30)["source_full_pose"], NAMES, "start")
        self.assertFalse(one_shot.cyclic)
        np.testing.assert_allclose(one_shot.sample(99), one_shot.sample(20))
        np.testing.assert_allclose(one_shot.sample(-5), one_shot.sample(0))

    def test_rejects_missing_samples_and_mismatched_references(self):
        with self.assertRaises(ValueError):
            unmapped.build_reference(self.start, self.end, 31, 0, TRANSITION)
        with self.assertRaises(ValueError):
            unmapped.build_reference(self.start, self.end, 5, 40, TRANSITION)
        with self.assertRaises(ValueError):           # reference made for another transition length
            unmapped.apply(converted_pose(), SKELETON, unmapped.build_reference(self.start, self.end, 5, 30, TRANSITION + 1))
        without_finger = copy.deepcopy(self.reference)
        without_finger["bones"] = [name for name in NAMES if name != "finger"]
        without_finger["local"] = [[row[COLUMN[name]] for name in without_finger["bones"]] for row in self.reference["local"]]
        with self.assertRaises(ValueError):
            unmapped.apply(converted_pose(), SKELETON, without_finger)


if __name__ == "__main__":
    unittest.main()
