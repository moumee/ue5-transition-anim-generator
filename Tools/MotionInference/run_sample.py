"""Make a complete request for the bundled Walk/Run inputs, then call infer.py."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import uuid

HERE=Path(__file__).resolve().parent
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',required=True)
    parser.add_argument('--output-dir',required=True)
    parser.add_argument('--transition',type=int,default=20)
    args=parser.parse_args()
    directory=Path(args.output_dir).resolve();directory.mkdir(parents=True,exist_ok=False)
    start,end=HERE/'samples/start_input.json',HERE/'samples/end_input.json'
    count=json.loads(start.read_text(encoding='utf-8'))['sample_count']
    request=dict(schema='motion_inbetweening.inference_request.v1',request_id=str(uuid.uuid4()),start_input_file=str(start),end_input_file=str(end),output_file=str(directory/'model_output.json'),model_config_file=str(Path(args.config).resolve()),sample_rate_hz=30,context_frames=10,start_context_start_index=count-10,end_target_index=0,transition_frames=args.transition,expected_output_samples=11+args.transition,source_sha256=dict(start=sha(start),end=sha(end)))
    path=directory/'request.json';path.write_text(json.dumps(request,ensure_ascii=False,indent=2),encoding='utf-8')
    raise SystemExit(subprocess.call([sys.executable,'-X','utf8','-B',str(HERE/'infer.py'),'--request',str(path)]))
