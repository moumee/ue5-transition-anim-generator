import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from infer import (Runtime, decode_rotations, generate, in_frame, place_target,
                   run_request, save_json, slerp_matrix)
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

    def test_default_context_detail_numerical_baseline(self):
        if self.runtime.pipeline!='context_detail' or self.runtime.placement!='endpoint_velocity':
            self.skipTest('Baseline is specific to the default Context + Detail endpoint_velocity pipeline.')
        meta=json.loads((HERE/'Tests/Fixtures/context_detail_baseline.json').read_text(encoding='utf-8'))
        self.assertEqual(meta['model_files_sha256'],self.runtime.hashes)
        from infer import file_hash
        self.assertEqual(meta['input_sha256'],[file_hash(self.start),file_hash(self.end)])
        expected=np.load(HERE/'Tests/Fixtures/context_detail_baseline.npz',allow_pickle=False)['states']
        actual=generate(self.runtime,self.start,self.end,20)['predictions_tx135']
        np.testing.assert_allclose(actual,expected,rtol=0,atol=2e-5)

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
        config=copy.deepcopy(self.runtime.config);fake=self.directory/'not_statistics.pkl';fake.write_bytes(b'not a pickle')
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
