import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from infer import (CALIBRATION_FILE, DEFAULT_FRAME_REFINEMENT, DEFAULT_INPUT_Z_CLAMP, Runtime,
                   clamp_input_z, decode_rotations, file_hash, generate, in_frame, load_frame_refinement,
                   parse_input_z_clamp, place_target, refine_rotations, roll_matrix, run_request,
                   save_json, slerp_matrix, text_sha256_variants)
import infer
from official_context_preprocess import (pack_state, project_static_channels, normalize,
                                       reverse_start_center_root, yaw_matrix)


class GeometryTests(unittest.TestCase):
    def test_independently_aligned_clip_roundtrip(self):
        p = np.array([[120., 95., -330.], [124., 96., -327.]])
        r = np.broadcast_to(np.eye(3), (2, 22, 3, 3)).copy()
        r[:, 0] = yaw_matrix(-.8)
        alignment = dict(position_offset_xz=[88., -240.], rotation_offset=yaw_matrix(1.2).tolist())
        a, b = in_frame(p, r, alignment)
        p2, r2 = reverse_start_center_root(a, b, alignment['position_offset_xz'], alignment['rotation_offset'])
        np.testing.assert_allclose(p2, p, rtol=0, atol=1e-10)
        np.testing.assert_allclose(r2, r, rtol=0, atol=1e-12)

    def test_interleaved_rotation_and_degenerate_rejection(self):
        r = yaw_matrix(.7)[None, None]
        six = r[..., :2].reshape(1, 1, 6)
        np.testing.assert_allclose(decode_rotations(six), r, atol=1e-12)
        with self.assertRaisesRegex(ValueError, 'collinear'):
            decode_rotations(np.zeros((1, 1, 6)))

    def test_static_joint_slerp_endpoints_and_short_arc(self):
        a, b = yaw_matrix(np.deg2rad(170)), yaw_matrix(np.deg2rad(-170))
        np.testing.assert_allclose(slerp_matrix(a,b,0), a, atol=1e-12)
        np.testing.assert_allclose(slerp_matrix(a,b,1), b, atol=1e-12)
        np.testing.assert_allclose(slerp_matrix(a,b,.5), yaw_matrix(np.pi), atol=1e-12)

    def test_continue_context_uses_observed_velocity_and_preserves_target_step(self):
        p = np.zeros((10,3));p[:,0]=np.arange(10)*2;p[:,1]=95
        r = np.broadcast_to(np.eye(3),(10,22,3,3)).copy()
        # Root local +Y must have horizontal projection.
        r[:,0] = np.array([[0,1,0],[1,0,0],[0,0,-1.]])
        ep, er = p[:2].copy(), r[:2].copy()
        a,b=place_target(p,r,ep,er,20,'continue_context')
        np.testing.assert_allclose(a[:,0], [60,62], atol=1e-12)
        np.testing.assert_allclose(a[:,1], [95,95])
        c,d=place_target(p,r,ep,er,20,'preserve_exported')
        np.testing.assert_array_equal(c,ep);np.testing.assert_array_equal(d,er)


def random_rotations(rng, shape):
    q = rng.normal(size=shape + (4,))
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    x, y, z, w = np.moveaxis(q, -1, 0)
    return np.stack([
        np.stack([1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)], -1),
        np.stack([2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)], -1),
        np.stack([2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)], -1)], -2)


class FrameRefinementTests(unittest.TestCase):
    calibration = json.loads(CALIBRATION_FILE.read_text(encoding='utf-8'))

    def test_refinement_equals_reexport_with_refined_calibration(self):
        # Team mapper: local_j = (M_p B_p)^T (M_j B_j). With B' = B @ C it must equal refine_rotations.
        rng = np.random.default_rng(3)
        parents = self.calibration['parents']
        M, B, C = random_rotations(rng, (4, 22)), random_rotations(rng, (22,)), random_rotations(rng, (22,))
        C[0] = np.eye(3)
        def locals_from(globals_):
            return np.stack([globals_[:, j] if p < 0 else np.swapaxes(globals_[:, p], -1, -2) @ globals_[:, j]
                             for j, p in enumerate(parents)], axis=1)
        old = locals_from(M @ B)
        new = locals_from(M @ B @ C)
        np.testing.assert_allclose(refine_rotations(old, C, parents), new, atol=1e-12)
        np.testing.assert_allclose(refine_rotations(new, C, parents, inverse=True), old, atol=1e-12)

    def test_bundled_refinement_matches_committed_calibration(self):
        recorded = json.loads(DEFAULT_FRAME_REFINEMENT.read_text(encoding='utf-8'))['calibration_sha256']
        self.assertIn(recorded, text_sha256_variants(CALIBRATION_FILE))
        frames, info = load_frame_refinement(DEFAULT_FRAME_REFINEMENT, self.calibration)
        self.assertEqual(info['calibration_sha256'], recorded)
        self.assertTrue(set(info['roll_degrees']) <= set(self.calibration['joint_order'][1:]))
        np.testing.assert_allclose(frames[0], np.eye(3))
        for name, degrees in info['roll_degrees'].items():
            np.testing.assert_allclose(frames[self.calibration['joint_order'].index(name)], roll_matrix(degrees))

    def test_calibration_line_endings_do_not_matter(self):
        # core.autocrlf may check the calibration out with LF or CRLF; both must be accepted.
        content = CALIBRATION_FILE.read_bytes().replace(b'\r\n', b'\n')
        with tempfile.TemporaryDirectory(prefix='mib eol 한글 ') as folder:
            for label, data in (('lf', content), ('crlf', content.replace(b'\n', b'\r\n'))):
                with self.subTest(label=label):
                    copy_path = Path(folder) / f'calibration_{label}.json'
                    copy_path.write_bytes(data)
                    with mock.patch.object(infer, 'CALIBRATION_FILE', copy_path):
                        _, info = load_frame_refinement(DEFAULT_FRAME_REFINEMENT, self.calibration)
                    self.assertEqual(set(info['calibration_sha256_variants']), set(text_sha256_variants(copy_path)))

    def test_invalid_refinement_files_are_rejected(self):
        document = json.loads(DEFAULT_FRAME_REFINEMENT.read_text(encoding='utf-8'))
        cases = {
            'schema': dict(document, schema='other'),
            'joint order': dict(document, joint_order=list(reversed(document['joint_order']))),
            'different calibration': dict(document, calibration_sha256='0' * 64),
            'Invalid frame refinement joint': dict(document, roll_degrees={'Hips': 5.0}),
            'finite': dict(document, roll_degrees={'LeftArm': float('inf')}),
        }
        with tempfile.TemporaryDirectory(prefix='mib refine 한글 ') as folder:
            for message, payload in cases.items():
                with self.subTest(message=message):
                    path = Path(folder) / 'refinement.json'
                    path.write_text(json.dumps(payload), encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, message):
                        load_frame_refinement(path, self.calibration)

    def test_input_clamp_only_touches_selected_joints(self):
        names = self.calibration['joint_order']
        clamp = parse_input_z_clamp(DEFAULT_INPUT_Z_CLAMP, names)
        rng = np.random.default_rng(5)
        mean, std = rng.normal(size=135), rng.uniform(.01, .5, size=135)
        states = mean + rng.normal(scale=20, size=(12, 135)) * std
        clamped, before = clamp_input_z(states, mean, std, clamp)
        z = (clamped - mean) / std
        selected = np.zeros(135, dtype=bool); selected[clamp['columns']] = True
        self.assertLessEqual(np.abs(z[:, selected]).max(), clamp['limit'] + 1e-9)
        np.testing.assert_array_equal(clamped[:, ~selected], states[:, ~selected])
        np.testing.assert_allclose(before, np.abs((states - mean) / std)[:, selected])
        for bad in ({'limit': 0, 'joints': ['LeftArm']}, {'limit': 3, 'joints': ['Hips']},
                    {'limit': 3, 'joints': ['LeftArm', 'LeftArm']}, {'limit': 3}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_input_z_clamp(bad, names)
        self.assertIsNone(parse_input_z_clamp(None, names))


CONFIG = os.environ.get('MIB_MODEL_CONFIG')


@unittest.skipUnless(CONFIG, 'Set MIB_MODEL_CONFIG to run official-weight integration tests.')
class ModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime = Runtime(CONFIG)
        cls.start = HERE/'samples/start_input.json'
        cls.end = HERE/'samples/end_input.json'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mib model 한글 ')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def test_actual_model_transition_lengths_and_distinct_target(self):
        for n in (5,15,20,30,45):
            with self.subTest(n=n):
                result=generate(self.runtime,self.start,self.end,n)
                self.assertEqual(result['frame_count'],11+n)
                states=np.array(result['predictions_tx135'])
                self.assertTrue(np.isfinite(states).all())
                self.assertEqual(result['inference']['diagnostics']['context_max_abs_error'],0)
                self.assertEqual(result['inference']['diagnostics']['target_max_abs_error'],0)
                self.assertFalse(np.allclose(states[-1,:132],states[9,:132]))
                self.assertFalse(np.allclose(states[10:-1],states[9]))
                contacts=np.array(result['inference']['diagnostics']['generated_contacts'])
                self.assertEqual(contacts.shape,(n,4));self.assertTrue(((contacts>=0)&(contacts<=1)).all())

    def runtime_with(self, **overrides):
        config=copy.deepcopy(self.runtime.config)
        config.update({key:str(value) for key,value in self.runtime.files.items()})
        config.update(overrides)
        path=self.directory/'config.json';save_json(path,config)
        return Runtime(path)

    def check_baseline(self, runtime, fixture):
        if runtime.pipeline!='context_detail' or runtime.placement!='endpoint_velocity':
            self.skipTest('Baseline is specific to the default Context + Detail endpoint_velocity pipeline.')
        meta=json.loads((HERE/f'Tests/Fixtures/{fixture}.json').read_text(encoding='utf-8'))
        self.assertEqual(meta['model_files_sha256'],runtime.hashes)
        self.assertEqual(meta['input_sha256'],[file_hash(self.start),file_hash(self.end)])
        expected=np.load(HERE/f'Tests/Fixtures/{fixture}.npz',allow_pickle=False)['states']
        actual=generate(runtime,self.start,self.end,20)['predictions_tx135']
        np.testing.assert_allclose(actual,expected,rtol=0,atol=meta['atol'])

    def test_legacy_settings_reproduce_original_baseline(self):
        # frame_refinement/input_z_clamp = null must reproduce the pre-refinement adapter exactly.
        self.check_baseline(self.runtime_with(frame_refinement=None,input_z_clamp=None),'context_detail_baseline')

    def test_default_refined_numerical_baseline(self):
        if 'frame_refinement' in self.runtime.config or 'input_z_clamp' in self.runtime.config:
            self.skipTest('The refined baseline uses the built-in refinement and clamp defaults.')
        self.check_baseline(self.runtime,'refined_context_detail_baseline')

    def test_refinement_keeps_known_frames_and_reports_settings(self):
        result=generate(self.runtime,self.start,self.end,20)
        inference=result['inference']
        self.assertEqual(inference['diagnostics']['context_max_abs_error'],0)
        self.assertEqual(inference['diagnostics']['target_max_abs_error'],0)
        if self.runtime.refine is not None:
            self.assertEqual(inference['frame_refinement']['sha256'],self.runtime.refinement_info['sha256'])
        if self.runtime.clamp is not None:
            report=inference['diagnostics']['input_z_clamp']
            self.assertEqual(set(report['clamped_known_values']),set(self.runtime.clamp['joints']))

    def test_inputs_exported_with_either_line_ending_are_accepted(self):
        if self.runtime.refine is None:
            self.skipTest('Frame refinement disabled in this configuration.')
        document=json.loads(self.start.read_text(encoding='utf-8'))
        for recorded in text_sha256_variants(CALIBRATION_FILE):
            with self.subTest(recorded=recorded[:8]):
                document['mapping_source_sha256'][CALIBRATION_FILE.name]=recorded
                path=self.directory/f'start_{recorded[:8]}.json';save_json(path,document)
                self.assertEqual(generate(self.runtime,path,self.end,5)['frame_count'],16)

    def test_input_from_other_calibration_is_rejected_when_refined(self):
        if self.runtime.refine is None:
            self.skipTest('Frame refinement disabled in this configuration.')
        document=json.loads(self.start.read_text(encoding='utf-8'))
        document.setdefault('mapping_source_sha256',{})[CALIBRATION_FILE.name]='0'*64
        path=self.directory/'other_calibration.json';save_json(path,document)
        with self.assertRaisesRegex(ValueError,'different calibration'):
            generate(self.runtime,path,self.end,20)

    def normalized(self,path,dest):
        document=json.loads(path.read_text(encoding='utf-8'))
        states=np.array(document['vectors_tx135']);r=states[:,:132].reshape(-1,22,6)
        document['unprojected_static_rotations']=dict(joint_indices=self.runtime.static_joints.tolist(),rotations_txjx6=r[:,self.runtime.static_joints].tolist())
        projected,_=project_static_channels(states,self.runtime.mean,self.runtime.std)
        document['vectors_tx135']=normalize(projected,self.runtime.mean,self.runtime.std).tolist()
        document['normalization_applied']=True
        document['statistics']={'sha256':self.runtime.hashes['context_statistics']}
        save_json(dest,document)
        return document

    def test_normalized_and_physical_inputs_have_same_poses_including_toes(self):
        a,b=self.directory/'start.json',self.directory/'end.json'
        self.normalized(self.start,a);self.normalized(self.end,b)
        raw=generate(self.runtime,self.start,self.end,20)
        normalized=generate(self.runtime,a,b,20)
        np.testing.assert_allclose(normalized['predictions_tx135'],raw['predictions_tx135'],rtol=0,atol=2e-5)

    def test_legacy_normalized_input_requires_reexport(self):
        path=self.directory/'legacy.json';doc=self.normalized(self.start,path)
        doc.pop('unprojected_static_rotations');save_json(path,doc)
        with self.assertRaisesRegex(ValueError,'Re-export'):
            self.runtime.input(path)

    def test_wrong_stats_and_invalid_original_toe_values_are_rejected(self):
        path=self.directory/'bad.json';doc=self.normalized(self.start,path)
        doc['statistics']['sha256']='0'*64;save_json(path,doc)
        with self.assertRaisesRegex(ValueError,'hash'):
            self.runtime.input(path)
        doc=self.normalized(self.start,path)
        doc['unprojected_static_rotations']['rotations_txjx6'][0][0][0]=True;save_json(path,doc)
        with self.assertRaisesRegex(ValueError,'JSON numbers'):
            self.runtime.input(path)

    def test_model_configuration_hash_mismatch_is_rejected(self):
        config=copy.deepcopy(self.runtime.config)
        # The test relocates the config; retain the original resolved model paths.
        config.update({key:str(value) for key,value in self.runtime.files.items()})
        fake=self.directory/'not_statistics.pkl';fake.write_bytes(b'not a pickle')
        config['context_statistics']=str(fake);path=self.directory/'config.json';save_json(path,config)
        with self.assertRaisesRegex(ValueError,'SHA-256'):
            Runtime(path)

    def test_unsupported_length_and_missing_next_target_fail(self):
        for n in (True,4,46,54,240):
            with self.subTest(n=n),self.assertRaisesRegex(ValueError,'transition_frames'):
                generate(self.runtime,self.start,self.end,n)
        end=json.loads(self.end.read_text(encoding='utf-8'))
        with self.assertRaisesRegex(ValueError,'end_target_index'):
            generate(self.runtime,self.start,self.end,20,target_index=end['sample_count']-1)

    def test_swapping_start_and_end_changes_request_result(self):
        a=generate(self.runtime,self.start,self.end,20)
        b=generate(self.runtime,self.end,self.start,20)
        self.assertFalse(np.allclose(a['predictions_tx135'],b['predictions_tx135']))


if __name__=='__main__':
    unittest.main()
