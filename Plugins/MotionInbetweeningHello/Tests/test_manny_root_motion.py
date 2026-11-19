"""Root motion extraction: the root bone carries the travel and poses stay where they were."""
import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import numpy as np

PLUGIN = Path(__file__).resolve().parents[1]
PROJECT = Path(os.environ.get('MIB_TEST_PROJECT_ROOT', str(PLUGIN.parents[1])))
sys.path.insert(0, str(PLUGIN / 'Scripts'))
import manny_root_motion as rm
from prepare_manny_output import prepare

PELVIS_REFERENCE = ([0.0, 1.0, 95.0], [0.0, 0.0, 0.0, 1.0])
SKELETON = {'bones': [
    {'name': 'root', 'parent': None, 'reference_local_transform': {
        'translation_cm': [0, 0, 0], 'rotation_quaternion': [0, 0, 0, 1], 'scale': [1, 1, 1]}},
    {'name': 'pelvis', 'parent': 'root', 'reference_local_transform': {
        'translation_cm': PELVIS_REFERENCE[0], 'rotation_quaternion': PELVIS_REFERENCE[1], 'scale': [1, 1, 1]}},
    {'name': 'spine_01', 'parent': 'pelvis', 'reference_local_transform': {
        'translation_cm': {'x': 0, 'y': 0, 'z': 10}, 'rotation_quaternion': {'x': 0, 'y': 0, 'z': 0, 'w': 1},
        'scale': {'x': 1, 'y': 1, 'z': 1}}},
]}


def tilt(degrees):
    a = np.radians(degrees)
    return np.array([[1, 0, 0], [0, np.cos(a), -np.sin(a)], [0, np.sin(a), np.cos(a)]])


def clip(count, speed, turn_per_frame=0.0, facing_offset=0.0, phase=0.0):
    """A root-motion clip: the root walks along its own +Y and turns; the pelvis sways over it."""
    root_r, root_t, local_r, local_t = [], [], [], []
    position, yaw = np.zeros(3), 0.0
    for frame in range(count):
        root_r.append(rm.yaw_matrix(yaw))
        root_t.append(position.copy())
        sway = 2.0 * np.sin(phase + frame * 0.4)
        local_r.append(rm.yaw_matrix(facing_offset + 0.1 * np.sin(phase + frame * 0.4)) @ tilt(5.0))
        local_t.append(np.array([sway, 1.0, 95.0 + 0.5 * np.cos(phase + frame * 0.8)]))
        position = position + rm.yaw_matrix(yaw) @ np.array([0.0, speed, 0.0])
        yaw += turn_per_frame
    return [np.array(v) for v in (root_r, root_t, local_r, local_t)]


def exported(count, **settings):
    """The clip as Scripts/export_manny_input.py writes it, and its model-input document."""
    root_r, root_t, local_r, local_t = clip(count, **settings)
    frames = []
    for i in range(count):
        frames.append({'bones': [{'name': 'pelvis',
            'local': {'translation': local_t[i].tolist(), 'rotation_xyzw': rm.matrix_to_quat(local_r[i]).tolist()},
            'world': {'translation': (root_t[i] + root_r[i] @ local_t[i]).tolist(),
                      'rotation_xyzw': rm.matrix_to_quat(root_r[i] @ local_r[i]).tolist()}}]})
    document = {'vectors_tx135': [[0.0] * 135] * count, 'source_root_motion': rm.source_root_tracks({'frames': frames})}
    return document, (root_r, root_t, local_r, local_t)


def pose_document(pelvis_r, pelvis_t):
    """What the inverse mapper returns: root at reference, the whole motion on the pelvis."""
    frames = []
    for rotation, translation in zip(pelvis_r, pelvis_t):
        frames.append({'bones': [
            {'name': 'root', 'local_translation_cm': [0.0, 0.0, 0.0], 'local_rotation_xyzw': [0.0, 0.0, 0.0, 1.0], 'local_scale': [1, 1, 1]},
            {'name': 'pelvis', 'local_translation_cm': translation.tolist(),
             'local_rotation_xyzw': rm.matrix_to_quat(rotation).tolist(), 'local_scale': [1, 1, 1]},
            {'name': 'spine_01', 'local_translation_cm': [0.0, 0.0, 10.0], 'local_rotation_xyzw': [0.0, 0.0, 0.0, 1.0], 'local_scale': [1, 1, 1]}]})
    return {'full_local_pose_frames': frames}


def saved_tracks(document):
    frames = document['full_local_pose_frames']
    return [np.array([rm.quat_to_matrix(f['bones'][b]['local_rotation_xyzw']) if key == 'r' else f['bones'][b]['local_translation_cm']
                      for f in frames]) for b in (0, 1) for key in ('r', 't')]


class SmootherTests(unittest.TestCase):
    def test_fixed_rows_are_kept_and_a_straight_line_is_unchanged(self):
        line = np.linspace(3.0, 40.0, 20)
        fixed = np.zeros(20, dtype=bool); fixed[[0, 1, 19]] = True
        np.testing.assert_allclose(rm.smooth(line, fixed, 500.0), line, atol=1e-9)
        noisy = line + np.where(fixed, 0.0, np.sin(np.arange(20) * 2.0))
        result = rm.smooth(noisy, fixed, 500.0)
        np.testing.assert_array_equal(result[fixed], noisy[fixed])
        self.assertLess(np.abs(np.diff(result, 2)).max(), np.abs(np.diff(noisy, 2)).max() * 0.2)

    def test_interpolation_ignores_free_values_and_leaves_fixed_rows_without_a_kink(self):
        values = np.r_[np.arange(5) * 2.0, np.full(10, 999.0), 30.0, 31.0]   # speed 2 in, speed 1 out
        fixed = values != 999.0
        result = rm.smooth(values, fixed, None)
        np.testing.assert_array_equal(result[fixed], values[fixed])
        self.assertTrue(np.all(np.diff(result) > 0))
        self.assertLess(np.abs(np.diff(result, 2)).max(), 0.3)
        two_dimensional = rm.smooth(np.stack([values, -values], axis=1), fixed, None)
        np.testing.assert_allclose(two_dimensional[:, 1], -result, atol=1e-9)


class RootMotionTests(unittest.TestCase):
    transition = 12

    def transition_case(self, start_settings, end_settings):
        """Context = 10 start samples, target = an end sample placed rigidly somewhere ahead."""
        n, start_index, target_index = self.transition, 4, 3
        start, (sr, st, slr, slt) = exported(20, **start_settings)
        end, (er, et, elr, elt) = exported(12, **end_settings)
        reference = rm.build_reference(start, end, start_index, target_index, n)
        count = 11 + n
        pelvis_r, pelvis_t = np.zeros((count, 3, 3)), np.zeros((count, 3))
        for f in range(10):
            pelvis_r[f] = sr[start_index + f] @ slr[start_index + f]
            pelvis_t[f] = st[start_index + f] + sr[start_index + f] @ slt[start_index + f]
        # Rigid placement of the destination clip in the output frame (yaw + ground translation).
        place_r, place_t = rm.yaw_matrix(0.6), np.array([-12.0, 140.0, 0.0])
        target_root_r, target_root_t = place_r @ er[target_index], place_r @ et[target_index] + place_t
        pelvis_r[-1] = target_root_r @ elr[target_index]
        pelvis_t[-1] = target_root_t + target_root_r @ elt[target_index]
        # In-between pelvis: leaves and arrives at the clips' speeds (cubic), with a 1.5 cm sway.
        leave = (pelvis_t[9] - pelvis_t[8]) * (n + 1)
        arrive = (place_r @ (et[target_index + 1] + er[target_index + 1] @ elt[target_index + 1]
                             - et[target_index] - er[target_index] @ elt[target_index])) * (n + 1)
        for f in range(10, count - 1):
            w = (f - 9) / (n + 1)
            pelvis_t[f] = ((2 * w**3 - 3 * w**2 + 1) * pelvis_t[9] + (w**3 - 2 * w**2 + w) * leave
                           + (3 * w**2 - 2 * w**3) * pelvis_t[-1] + (w**3 - w**2) * arrive
                           + [1.5 * np.sin(f) * np.sin(np.pi * w), 0.0, 0.0])
            pelvis_r[f] = rm.yaw_matrix(0.6 * w) @ pelvis_r[9]
        next_root = (place_r @ et[target_index + 1] + place_t, place_r @ er[target_index + 1])
        return dict(reference=reference, pelvis_r=pelvis_r, pelvis_t=pelvis_t, start_index=start_index, target_index=target_index,
                    start=(sr, st, slr, slt), end=(er, et, elr, elt), target_root=(target_root_r, target_root_t), next_root=next_root)

    def test_root_is_pinned_to_the_source_roots_and_poses_are_unchanged(self):
        case = self.transition_case(dict(speed=5.0, turn_per_frame=0.02, facing_offset=0.4), dict(speed=12.0, phase=1.0))
        document = pose_document(case['pelvis_r'], case['pelvis_t'])
        report = rm.apply_root_motion(document, SKELETON, case['reference'])
        root_r, root_t, local_r, local_t = saved_tracks(document)
        self.assertEqual(report['source'], 'source_clip_roots')
        self.assertEqual(report['reference_frames'], list(range(10)) + [10 + self.transition])
        self.assertTrue(report['exit_velocity_matched'])
        np.testing.assert_allclose(root_r[0], np.eye(3), atol=1e-9)
        np.testing.assert_allclose(root_t[0], 0.0, atol=1e-9)
        np.testing.assert_allclose(root_t[:, 2], 0.0, atol=1e-9)
        # The saved clip is the original one moved rigidly so its first root is the origin.
        sr, st, slr, slt = case['start']
        index = case['start_index']
        for f in range(len(root_t)):
            np.testing.assert_allclose(sr[index] @ (root_r[f] @ local_r[f]), case['pelvis_r'][f], atol=1e-9)
            np.testing.assert_allclose(sr[index] @ (root_t[f] + root_r[f] @ local_t[f]) + st[index], case['pelvis_t'][f], atol=1e-8)
        # Known frames: root-relative pelvis and root steps are the source clips' own.
        for f in range(10):
            np.testing.assert_allclose(local_t[f], slt[index + f], atol=1e-8)
            np.testing.assert_allclose(local_r[f], slr[index + f], atol=1e-9)
            np.testing.assert_allclose(root_t[f], sr[index].T @ (st[index + f] - st[index]), atol=1e-8)
        er, et, elr, elt = case['end']
        np.testing.assert_allclose(local_t[-1], elt[case['target_index']], atol=1e-8)
        np.testing.assert_allclose(local_r[-1], elr[case['target_index']], atol=1e-9)
        target_r, target_t = case['target_root']
        np.testing.assert_allclose(sr[index] @ root_r[-1], target_r, atol=1e-9)
        np.testing.assert_allclose(sr[index] @ root_t[-1] + st[index], target_t, atol=1e-8)

    def test_root_enters_and_leaves_the_generated_frames_at_the_source_root_speed(self):
        # Straight clips at 5 and 12 cm per frame; the in-between pelvis sways 1.5 cm sideways.
        case = self.transition_case(dict(speed=5.0), dict(speed=12.0))
        document = pose_document(case['pelvis_r'], case['pelvis_t'])
        rm.apply_root_motion(document, SKELETON, case['reference'])
        root_r, root_t, _, local_t = saved_tracks(document)
        steps = np.linalg.norm(np.diff(root_t, axis=0), axis=1)
        np.testing.assert_allclose(steps[:9], 5.0, atol=1e-8)
        self.assertLess(abs(steps[9] - 5.0), 1.5)                  # first generated step continues the context
        self.assertLess(abs(steps[-1] - 12.0), 1.5)                # last step runs into the destination speed
        self.assertLess(np.abs(np.diff(steps)).max(), 1.5)         # no jump in between
        yaw = np.degrees(np.arctan2(root_r[:, 1, 0], root_r[:, 0, 0]))
        self.assertTrue(np.all(np.diff(yaw) > -1e-9))              # 0 -> 34.4 degrees without wobble
        self.assertAlmostEqual(yaw[-1], np.degrees(0.6), places=6)
        self.assertLess(np.abs(local_t[:, :2] - PELVIS_REFERENCE[0][:2]).max(), 8.0)   # pelvis stays over the root

    def test_without_reference_the_root_follows_the_pelvis_and_does_not_turn(self):
        case = self.transition_case(dict(speed=5.0), dict(speed=12.0))
        document = pose_document(case['pelvis_r'], case['pelvis_t'])
        before = copy.deepcopy(document)
        report = rm.apply_root_motion(document, SKELETON)
        root_r, root_t, local_r, local_t = saved_tracks(document)
        self.assertEqual(report['source'], 'pelvis_projection')
        self.assertEqual(report['reference_frames'], [])
        np.testing.assert_allclose(root_r, np.broadcast_to(np.eye(3), root_r.shape), atol=1e-12)
        np.testing.assert_allclose(root_t[0], 0.0, atol=1e-9)
        # The root travels exactly as far as the pelvis, which starts and ends at its reference spot.
        np.testing.assert_allclose(root_t[-1, :2], case['pelvis_t'][-1, :2] - case['pelvis_t'][0, :2], atol=1e-8)
        np.testing.assert_allclose(local_t[[0, -1], :2], [PELVIS_REFERENCE[0][:2]] * 2, atol=1e-8)
        self.assertLess(np.abs(local_t[:, :2] - PELVIS_REFERENCE[0][:2]).max(), 6.0)
        self.assertLess(np.abs(np.diff(root_t[:, 0], 2)).max(), np.abs(np.diff(case['pelvis_t'][:, 0], 2)).max() * 0.5)
        origin = np.array(report['first_frame_rebased_from']['translation_cm'])
        for f in range(len(root_t)):
            np.testing.assert_allclose(root_t[f] + root_r[f] @ local_t[f] + origin, case['pelvis_t'][f], atol=1e-8)
            np.testing.assert_allclose(root_r[f] @ local_r[f], case['pelvis_r'][f], atol=1e-9)
        # Only the root and pelvis tracks change.
        for new, old in zip(document['full_local_pose_frames'], before['full_local_pose_frames']):
            self.assertEqual(new['bones'][2], old['bones'][2])

    def test_reference_needs_both_inputs_and_is_validated(self):
        start, _ = exported(20, speed=5.0)
        end, _ = exported(12, speed=12.0)
        legacy = {'vectors_tx135': start['vectors_tx135']}
        self.assertIsNone(rm.build_reference(legacy, end, 4, 3, 12))
        self.assertIsNone(rm.build_reference(start, {'vectors_tx135': end['vectors_tx135']}, 4, 3, 12))
        last = rm.build_reference(start, end, 4, 11, 12)           # no sample after the target
        self.assertIsNone(last['next_root_delta'])
        reference = rm.build_reference(start, end, 4, 3, 12)
        self.assertEqual(rm.parse_reference(json.loads(json.dumps(reference)), 23)['frames'], reference['frames'])
        cases = {
            'schema': dict(reference, schema='other'),
            'frames': dict(reference, frames=reference['frames'][1:]),
            'first and the last': dict(reference, frames=reference['frames'][:-1] + [21]),
            'translation_cm': dict(reference, pelvis_in_root=dict(reference['pelvis_in_root'], translation_cm=[[0, 0, '1']] * 11)),
            'normalized': dict(reference, pelvis_in_root=dict(reference['pelvis_in_root'], rotation_xyzw=[[0, 0, 0, 2]] * 11)),
            'next_root_delta': dict(reference, next_root_delta=[1, 2, 3]),
        }
        for message, bad in cases.items():
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                rm.parse_reference(bad, 23)
        broken = copy.deepcopy(start)
        broken['source_root_motion']['root']['translation_cm'].pop()
        with self.assertRaisesRegex(ValueError, 'start source_root_motion.root.translation_cm'):
            rm.build_reference(broken, end, 4, 3, 12)

    def test_unsupported_skeletons_are_rejected(self):
        case = self.transition_case(dict(speed=5.0), dict(speed=12.0))
        skeleton = copy.deepcopy(SKELETON)
        skeleton['bones'][1]['parent'] = 'spine_01'
        with self.assertRaisesRegex(ValueError, 'pelvis as its direct child'):
            rm.apply_root_motion(pose_document(case['pelvis_r'], case['pelvis_t']), skeleton)


class OutputAdapterTests(unittest.TestCase):
    def test_fixture_output_is_saved_as_root_motion(self):
        mapping = PROJECT / 'Tools/MannyLafanMapping'
        calibration = json.loads((mapping / 'rest_pose_corrections_22_prototype.json').read_text(encoding='utf-8'))
        # Stand-in skeleton: root plus the 22 mapped bones in the calibration's hierarchy.
        mapped = calibration['manny_bone_order']
        identity = {'translation_cm': [0, 0, 0], 'rotation_quaternion': [0, 0, 0, 1], 'scale': [1, 1, 1]}
        bones = [{'index': 0, 'name': 'root', 'parent': None, 'reference_local_transform': identity}]
        for index, (name, parent) in enumerate(zip(mapped, calibration['parents']), start=1):
            bones.append({'index': index, 'name': name, 'parent': 'root' if parent < 0 else mapped[parent],
                          'reference_local_transform': dict(identity, translation_cm=[0, 0, 10])})
        with tempfile.TemporaryDirectory(prefix='mib root 한글 ') as folder:
            folder = Path(folder)
            shutil.copy(PLUGIN / 'Tests/Fixtures/context_model_output.json', folder / 'source_model_output.json')
            (folder / 'skeleton.json').write_text(json.dumps({'bones': bones}), encoding='utf-8')
            result = prepare({'output_directory': str(folder), 'mapping_directory': str(mapping),
                              'source_file': str(folder / 'source_model_output.json'), 'statistics_file': ''})
            self.assertEqual(result['root_motion_source'], 'pelvis_projection')
            pose = json.loads((folder / 'manny_pose.json').read_text(encoding='utf-8'))
        self.assertTrue(pose['root_motion']['enabled'])
        self.assertTrue(pose['validation']['passed'])
        frames = pose['full_local_pose_frames']
        names = [bone['name'] for bone in frames[0]['bones']]
        root = np.array([frame['bones'][names.index('root')]['local_translation_cm'] for frame in frames])
        pelvis = np.array([frame['bones'][names.index('pelvis')]['local_translation_cm'] for frame in frames])
        compact = np.array([frame['pelvis_component_translation_cm'] for frame in pose['frames']])
        np.testing.assert_allclose(root[0], 0.0, atol=1e-9)
        self.assertGreater(np.linalg.norm(root[-1]), 50.0)
        self.assertLess(np.ptp(pelvis[:, :2], axis=0).max(), 15.0)
        # Root + pelvis is still the mapper's pelvis path, measured from the first root.
        origin = np.array(pose['root_motion']['first_frame_rebased_from']['translation_cm'])
        np.testing.assert_allclose(root + pelvis + origin, compact, atol=1e-6)


if __name__ == '__main__':
    unittest.main()
