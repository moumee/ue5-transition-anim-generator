"""Run a supplied inference entry point; this adapter contains no model logic."""
import argparse
import json
from pathlib import Path
import subprocess
import traceback

from prepare_manny_input import file_hash, save_json
from prepare_manny_output import adapt_document, load_model_output


def read_input(path, calibration, statistics_file):
    document = load_model_output(path)
    if not isinstance(document, dict) or document.get('schema') != 'motion_inbetweening.manny_input.v1':
        raise ValueError('Select a model_input.json exported by the Manny input menu.')
    required = ('vectors_tx135', 'alignment', 'joint_order', 'parents', 'position_space', 'sample_count')
    if any(field not in document for field in required):
        raise ValueError('The model input is missing mapping metadata.')
    adapted = adapt_document(document, calibration)
    if type(document.get('context_len')) is not int or document['context_len'] != 10:
        raise ValueError('The inference bridge requires context_len=10.')
    if document['sample_rate_hz'] != 30 or len(document['vectors_tx135']) < 10:
        raise ValueError('The inference bridge requires at least 10 samples at 30 fps.')
    check_statistics(document, statistics_file)
    return document, adapted


def check_statistics(document, statistics_file):
    if document['normalization_applied']:
        statistics = document.get('statistics')
        if not isinstance(statistics, dict) or not statistics.get('sha256'):
            raise ValueError('Normalized data must identify its Training Statistics SHA-256.')
        if not statistics_file or not Path(statistics_file).is_file():
            raise ValueError('Normalized data requires Training Statistics in the plugin settings.')
        if statistics['sha256'] != file_hash(statistics_file):
            raise ValueError('Training Statistics hash does not match the normalized data.')


def prepare_request(job):
    directory = Path(job['output_directory']).resolve()
    output = directory / 'model_output.json'
    if output.exists():
        raise ValueError('Use a fresh job directory; model_output.json already exists.')
    transition = job['transition_frames']
    if type(transition) is not int or not 1 <= transition <= 240:
        raise ValueError('transition_frames must be an integer between 1 and 240.')
    config_path = Path(job['model_config_file']).resolve()
    if not isinstance(load_model_output(config_path), dict):
        raise ValueError('Model Config must be a JSON object understood by the inference entry point.')
    for field in ('inference_python', 'inference_script'):
        if not Path(job[field]).is_file():
            raise ValueError(f'{field} must point to an existing file.')
    calibration = load_model_output(Path(job['mapping_directory']) / 'rest_pose_corrections_22_prototype.json')
    start, start_adapted = read_input(directory / 'start_input.json', calibration, job.get('statistics_file'))
    end, _ = read_input(directory / 'end_input.json', calibration, job.get('statistics_file'))
    if start['normalization_applied'] != end['normalization_applied']:
        raise ValueError('Export both inputs with the same normalization setting and model statistics.')
    request = {
        'schema': 'motion_inbetweening.inference_request.v1',
        'request_id': job['request_id'],
        'start_input_file': str(directory / 'start_input.json'),
        'end_input_file': str(directory / 'end_input.json'),
        'output_file': str(output),
        'model_config_file': str(config_path),
        'sample_rate_hz': 30,
        'context_frames': 10,
        'start_context_start_index': len(start['vectors_tx135']) - 10,
        'end_target_index': 0,
        'transition_frames': transition,
        'expected_output_samples': 10 + transition + 1,
        'source_sha256': {name: file_hash(directory / (name + '_input.json')) for name in ('start', 'end')},
    }
    save_json(directory / 'request.json', request)
    return request, calibration, start_adapted


def run(job):
    directory = Path(job['output_directory']).resolve()
    request, calibration, start = prepare_request(job)
    command = [str(Path(job['inference_python']).resolve()), '-X', 'utf8', '-B',
               str(Path(job['inference_script']).resolve()), '--request', str(directory / 'request.json')]
    # UE owns the timeout and kills this process tree on timeout/cancel/shutdown.
    # File-backed logs cannot deadlock on a full pipe and retain backend errors.
    with (directory / 'model_stdout.log').open('w', encoding='utf-8') as stdout, \
            (directory / 'model_stderr.log').open('w', encoding='utf-8') as stderr:
        completed = subprocess.run(command, cwd=str(Path(job['inference_script']).resolve().parent),
                                   stdout=stdout, stderr=stderr, check=False)
    if completed.returncode != 0:
        raise ValueError(f'Inference exited with code {completed.returncode}. See model_stderr.log and model_stdout.log.')
    output = Path(request['output_file'])
    if not output.is_file():
        raise ValueError('Inference exited successfully without writing model_output.json.')
    document = adapt_document(load_model_output(output), calibration)
    if document.get('inference_request_id') != request['request_id']:
        raise ValueError('Model output inference_request_id does not match this request.')
    states = next(document[field] for field in ('predictions_tx135', 'output_tx135', 'vectors_tx135') if field in document)
    if len(states) != request['expected_output_samples'] or document['sample_rate_hz'] != 30:
        raise ValueError('Output must contain context + transition + target samples at 30 fps.')
    if document.get('frame_count') != request['expected_output_samples']:
        raise ValueError('Output must declare the expected frame_count.')
    if document.get('position_space') != 'lafan_start_centered' or 'root_position_offset_lafan' in document:
        raise ValueError('Output must use the start input coordinate frame, without a raw root offset.')
    for field in ('heading_position_offset_xz', 'heading_rotation_offset'):
        if document.get(field) != start[field]:
            raise ValueError('Output heading metadata must preserve the start input coordinate frame.')
    check_statistics(document, job.get('statistics_file'))
    # Keep the backend output intact. The existing output importer validates poses
    # and creates an asset only after this stage has succeeded.
    return {'success': True, 'request_id': request['request_id'], 'sample_count': len(states),
            'model_output_file': str(output), 'model_output_sha256': file_hash(output),
            'inference_script_sha256': file_hash(job['inference_script']),
            'model_config_sha256': file_hash(job['model_config_file'])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--job', required=True)
    args = parser.parse_args()
    job = load_model_output(args.job)
    directory = Path(job['output_directory'])
    try:
        result = run(job)
    except Exception as error:
        (directory / 'error.log').write_text(traceback.format_exc(), encoding='utf-8')
        save_json(directory / 'result.json', {'success': False, 'error': str(error)})
        return 1
    save_json(directory / 'result.json', result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
