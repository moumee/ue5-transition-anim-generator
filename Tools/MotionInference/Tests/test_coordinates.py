"""Independent world-space fixtures exercise the entire generate coordinate path."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import infer


def yaw(angle):
    c,s=np.cos(angle),np.sin(angle)
    return np.array([[c,0,s],[0,1,0],[-s,0,c]])


def roll(angle):
    c,s=np.cos(angle),np.sin(angle)
    return np.array([[c,-s,0],[s,c,0],[0,0,1]])


BASE=roll(-np.pi/2)  # local +Y points along world +X


class CapturingPredictor:
    pipeline='controlled_fixture'
    detail=None
    hashes={}
    provenance={}
    config={}
    torch=SimpleNamespace(__version__='not_loaded')
    static_joints=np.array([4,8])

    def __init__(self,policy,config_path):
        self.placement,self.config_path=policy,config_path
        self.clips={}
        for name,length,offset,angle,root_yaw in (
                ('start',15,[40.,0.,-20.],.3,.6),
                ('end',6,[-30.,0.,110.],-.8,-.5)):
            t=np.arange(length)
            wp=(np.column_stack([100+2*t,90+.1*t,-60+.4*t]) if name=='start'
                else np.column_stack([-80+3*t,92-.2*t,150+1.1*t]))
            wr=np.empty((length,22,3,3))
            wr[:,0]=yaw(root_yaw)@BASE
            for frame in range(length):
                for j in range(1,22):
                    a=.02*j+.01*frame if name=='start' else -.03*j+.015*frame
                    wr[frame,j]=roll(a)
            turn=yaw(angle)
            aligned_p=(turn@(wp-np.asarray(offset)).T).T
            aligned_r=wr.copy();aligned_r[:,0]=turn@wr[:,0]
            doc={'alignment':{'position_offset_xz':[offset[0],offset[2]],'rotation_offset':turn.tolist(),'reference_frame':9},
                 'joint_order':[f'joint{j}' for j in range(22)],'parents':[-1]+[0]*21}
            self.clips[name]=(doc,aligned_p,aligned_r,wp,wr)

    def input(self,path):return copy.deepcopy(self.clips[str(path)][:3])

    def predict_window(self,p,r,transition):
        self.captured=(p.copy(),r.copy())
        p,r=p.copy(),r.copy()
        for k in range(transition):
            p[10+k]=[2+1.5*k,88+.3*k,-4+.7*k]
            r[10+k,0]=yaw(.1+.04*k)@BASE
            for j in range(1,22):r[10+k,j]=roll(.02*j+.01*k)
        return p,r,{}


class GenerateCoordinateTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.config=Path(temp.name)/'config.json';self.config.write_text('{}',encoding='utf-8')

    def check_policy(self,policy):
        runtime=CapturingPredictor(policy,self.config)
        n=5;start_index=2;end_index=2
        result=infer.generate(runtime,'start','end',n,start_index=start_index,target_index=end_index)
        sp,sr=runtime.clips['start'][3][2:12],runtime.clips['start'][4][2:12]
        ep,er=runtime.clips['end'][3][2:4].copy(),runtime.clips['end'][4][2:4].copy()
        if policy!='preserve_exported':
            # Analytic fixtures: heading difference is 0.6 - (-0.5) = 1.1.
            step=yaw(1.1)@np.array([3.,-.2,1.1])
            velocity=np.array([2.,.4])
            if policy=='endpoint_velocity':velocity=(velocity+step[[0,2]])/2
            target=sp[-1].copy();target[[0,2]]+=velocity*6;target[1]=ep[0,1]
            ep=np.array([target,target+step]);er[:,0]=yaw(.6)@BASE
        world_window=np.concatenate([sp,np.repeat(sp[-1:],n,axis=0),ep])
        center=np.array([sp[-1,0],0.,sp[-1,2]])
        expected_p=(yaw(-.6)@(world_window-center).T).T
        expected_r=np.concatenate([sr,np.repeat(sr[-1:],n,axis=0),er])
        expected_r[:,0]=yaw(-.6)@expected_r[:,0]
        np.testing.assert_allclose(runtime.captured[0],expected_p,rtol=0,atol=1e-10)
        np.testing.assert_allclose(runtime.captured[1],expected_r,rtol=0,atol=1e-12)
        states=np.asarray(result['predictions_tx135'])
        output_p=states[:,-3:];output_r=infer.decode_rotations(states[:,:132].reshape(-1,22,6))
        # Predictor result -> literal world -> original start-export frame.
        prediction=np.array([[2+1.5*k,88+.3*k,-4+.7*k] for k in range(n)])
        gen_world=(yaw(.6)@prediction.T).T+center
        if policy=='endpoint_velocity':
            first=sp[-1,[0,2]]+np.array([2.,.4])-gen_world[0,[0,2]]
            last=ep[0,[0,2]]-(ep[1]-ep[0])[[0,2]]-gen_world[-1,[0,2]]
            gen_world[:,[0,2]]+=np.array([first*(1-k/4)+last*k/4 for k in range(n)])
        original_offset=np.array([40.,0.,-20.])
        expected_output=(yaw(.3)@(np.concatenate([sp,gen_world,ep[:1]])-original_offset).T).T
        np.testing.assert_allclose(output_p,expected_output,rtol=0,atol=1e-10)
        for k in range(n):
            np.testing.assert_allclose(output_r[10+k,0],yaw(.9)@yaw(.1+.04*k)@BASE,atol=1e-12)
            for j in range(1,22):
                if j in (4,8):
                    w=(k+1)/6;angle=(.02*j+.11)*(1-w)+(-.03*j+.03)*w
                else:angle=.02*j+.01*k
                np.testing.assert_allclose(output_r[10+k,j],roll(angle),atol=1e-12)
        self.assertEqual(result['alignment'],runtime.clips['start'][0]['alignment'])
        self.assertEqual(result['frame_count'],16)

    def test_preserve_exported_window_and_generated_inverse(self):self.check_policy('preserve_exported')
    def test_continue_context_window_and_generated_inverse(self):self.check_policy('continue_context')
    def test_endpoint_velocity_window_and_generated_inverse(self):self.check_policy('endpoint_velocity')


class BoundaryVelocityTests(unittest.TestCase):
    def test_matches_adjacent_steps_without_touching_known_frames_or_height(self):
        p=np.arange(17*3,dtype=float).reshape(17,3)
        sp=p[:10].copy();ep=p[15:].copy();sp[-2]=sp[-1]-[2,1,-3];ep[1]=ep[0]+[7,2,4]
        out,diagnostic=infer.match_horizontal_boundary_steps(p,sp,ep,5)
        np.testing.assert_allclose(out[10,[0,2]]-sp[-1,[0,2]],[2,-3])
        np.testing.assert_allclose(ep[0,[0,2]]-out[14,[0,2]],[7,4])
        np.testing.assert_array_equal(out[:10],p[:10]);np.testing.assert_array_equal(out[15:],p[15:])
        np.testing.assert_array_equal(out[:,1],p[:,1])
        np.testing.assert_array_equal(p,np.arange(17*3,dtype=float).reshape(17,3))
        self.assertEqual(diagnostic['policy'],'linear_endpoint_residual')


if __name__=='__main__':unittest.main()
