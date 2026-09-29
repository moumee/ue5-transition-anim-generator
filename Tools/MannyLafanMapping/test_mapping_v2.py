"""Regression checks using real exported poses; no asset/output mutations."""
import copy
import json
from pathlib import Path
import unittest
import numpy as np
import manny_lafan_transform as t
from convert_manny_sequence_135 import convert_sequence

HERE = Path(__file__).resolve().parent

def qmul(a,b):
    av, bv = np.array(a[:3]), np.array(b[:3])
    return np.r_[a[3]*bv+b[3]*av+np.cross(av,bv), a[3]*b[3]-av@bv].tolist()

class MappingChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cal = json.loads((HERE/'rest_pose_corrections_22_prototype.json').read_text(encoding='utf-8'))
        cls.seq = json.loads((HERE/'manny_mm_idle_frames_0_9.json').read_text(encoding='utf-8'))

    def test_baseline_and_positions(self):
        result = convert_sequence(self.seq,self.cal)
        self.assertTrue(result['validation']['passed'])
        self.assertLess(result['validation']['root_position_roundtrip_max_error_cm'],1e-8)

    def test_rigid_rotations_preserve_relative_pose(self):
        baseline = np.asarray(convert_sequence(self.seq,self.cal)['vectors_tx135'])
        for axis in range(3):
            for degrees in (30,90,180):
                data = copy.deepcopy(self.seq)
                q = np.zeros(4)
                q[axis] = np.sin(np.radians(degrees)/2)
                q[3] = np.cos(np.radians(degrees)/2)
                r = t.quat_to_matrix(dict(zip(('x','y','z','w'),q)))
                for frame in data['frames']:
                    for bone in frame['bones']:
                        w=bone['world']
                        w['rotation_xyzw']=qmul(q,w['rotation_xyzw'])
                        w['translation']=(r@w['translation']).tolist()
                result = np.asarray(convert_sequence(data,self.cal)['vectors_tx135'])
                np.testing.assert_allclose(result[:,6:132],baseline[:,6:132],atol=1e-9,rtol=0)

    def test_joint_motion_tracks_source_world_delta(self):
        data=copy.deepcopy(self.seq)
        q=[0,0,np.sin(np.pi/12),np.cos(np.pi/12)]
        d=t.ue_rotation_to_lafan(t.quat_to_matrix(dict(zip(('x','y','z','w'),q))))
        for frame in data['frames']:
            for bone in frame['bones']:
                if bone['name'] in ('lowerarm_l','hand_l'):
                    bone['world']['rotation_xyzw']=qmul(q,bone['world']['rotation_xyzw'])
        def globals_for(seq):
            vectors=np.asarray(convert_sequence(seq,self.cal)['vectors_tx135'])
            return t.locals_to_globals(t.matrix6d_to_9d(vectors[:,:132].reshape(-1,22,6)),self.cal['parents'])
        old,new=globals_for(self.seq),globals_for(data)
        for j,name in enumerate(self.cal['manny_bone_order']):
            expected=d@old[:,j] if name in ('lowerarm_l','hand_l') else old[:,j]
            np.testing.assert_allclose(new[:,j],expected,atol=1e-9,rtol=0)

    def test_100m_jump_fails(self):
        data=copy.deepcopy(self.seq)
        for bone in data['frames'][5]['bones']:
            bone['world']['translation'][0]+=10000
        self.assertFalse(convert_sequence(data,self.cal)['validation']['passed'])

    def test_bad_schema_and_old_calibration_rejected(self):
        for mutation in ('empty','duplicate','missing','indices','nan','scale'):
            data=copy.deepcopy(self.seq)
            if mutation=='empty': data['frames']=[]
            if mutation=='duplicate': data['frames'][0]['bones'].append(data['frames'][0]['bones'][0])
            if mutation=='missing': data['frames'][0]['bones'].pop()
            if mutation=='indices': data['frames'][1]['frame_index']=0
            if mutation=='nan': data['frames'][0]['bones'][0]['world']['translation'][0]=float('nan')
            if mutation=='scale': data['frames'][0]['bones'][0]['world']['scale']=[2,1,1]
            with self.assertRaises(ValueError): convert_sequence(data,self.cal)
        cal=copy.deepcopy(self.cal)
        cal.pop('convention')
        with self.assertRaises(ValueError): convert_sequence(self.seq,cal)

    def test_decoder_inputs(self):
        np.testing.assert_allclose(t.matrix6d_to_9d([1,0,0,1,0,0]),np.eye(3))
        for bad in (np.zeros(6),[1,2,0,0,0,0],[1,2],np.full(6,np.nan)):
            with self.assertRaises(ValueError): t.matrix6d_to_9d(bad)
        with self.assertRaises(ValueError): t.quat_to_matrix(dict(x=0,y=0,z=0,w=0))

    def test_source_syntax(self):
        for path in HERE.glob('*.py'):
            compile(path.read_bytes(),str(path),'exec')

if __name__=='__main__':
    unittest.main(verbosity=2)
