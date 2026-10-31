"""TEST ONLY: repeat known poses to exercise the bridge; does not run a model."""
import argparse
import json
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--request', required=True)
    args = parser.parse_args()
    request = json.loads(Path(args.request).read_text(encoding='utf-8-sig'))
    config = json.loads(Path(request['model_config_file']).read_text(encoding='utf-8-sig'))
    directory = Path(request['output_file']).parent
    (directory / 'test_backend_started.json').write_text(json.dumps({'pid': os.getpid()}), encoding='utf-8')
    mode = config.get('test_mode', 'success')
    if mode == 'sleep':
        time.sleep(60)
    if mode == 'failure':
        print('Intentional test failure', file=__import__('sys').stderr)
        return 7
    if mode == 'missing_output':
        return 0
    source = json.loads(Path(request['start_input_file']).read_text(encoding='utf-8-sig'))
    count = request['expected_output_samples']
    states = source['vectors_tx135']
    output = {'inference_request_id': request['request_id'], 'sample_rate_hz': 30,
              'frame_count': count, 'normalization_applied': source['normalization_applied'],
              'statistics': source.get('statistics'), 'position_space': 'lafan_start_centered',
              'alignment': source['alignment'], 'joint_order': source['joint_order'], 'parents': source['parents'],
              'predictions_tx135': [states[i % len(states)] for i in range(count)],
              'test_only': 'Fixture playback, not generated motion.'}
    if mode == 'wrong_request':
        output['inference_request_id'] = 'stale-request'
    elif mode == 'wrong_count':
        output['predictions_tx135'].pop()
        output['frame_count'] -= 1
    elif mode == 'invalid_numbers':
        output['predictions_tx135'][0][132] = True
    elif mode == 'wrong_alignment':
        output['alignment']['position_offset_xz'][0] += 1
    Path(request['output_file']).write_text(json.dumps(output, allow_nan=False), encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
