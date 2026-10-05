"""Adapt plugin metadata, then call the unchanged team inverse mapper."""
import argparse
import json
from pathlib import Path
import sys
import traceback

from prepare_manny_input import file_hash, save_json


def adapt_document(document):
    import numpy as np

    document = dict(document)
    if type(document.get('normalization_applied')) is not bool:
        raise ValueError('Model output must explicitly declare normalization_applied: true or false.')
    keys = [key for key in ('predictions_tx135', 'output_tx135', 'vectors_tx135') if key in document]
    if len(keys) != 1:
        raise ValueError('Provide exactly one of predictions_tx135, output_tx135 or vectors_tx135.')
    states = np.asarray(document[keys[0]], dtype=np.float64)
    if states.ndim != 2 or states.shape[1] != 135 or not 2 <= len(states) <= 10000 or not np.isfinite(states).all():
        raise ValueError('Animation output must contain 2-10000 finite rows of 135 values.')
    rate = document.get('sample_rate_hz')
    if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not np.isfinite(rate) or not 0 < rate <= 240:
        raise ValueError('Declare sample_rate_hz between 0 (exclusive) and 240.')
    indices = document.get('frame_indices', list(range(len(states))))
    if len(indices) != len(states) or any(type(i) is not int for i in indices) or np.any(np.diff(indices) != 1):
        raise ValueError('frame_indices must be consecutive integers for a uniformly sampled animation.')
    if 'sample_times_seconds' in document:
        times = np.asarray(document['sample_times_seconds'], dtype=float)
        if times.shape != (len(states),) or not np.isfinite(times).all() or not np.allclose(np.diff(times), 1 / rate, atol=1e-6, rtol=0):
            raise ValueError('sample_times_seconds do not match the output frame count and rate.')

    alignment = document.get('alignment')
    flat_keys = ('heading_position_offset_xz', 'heading_rotation_offset')
    if alignment is not None:
        if not isinstance(alignment, dict) or 'position_offset_xz' not in alignment or 'rotation_offset' not in alignment:
            raise ValueError('alignment requires position_offset_xz and rotation_offset from the original input.')
        for flat, nested in zip(flat_keys, ('position_offset_xz', 'rotation_offset')):
            if flat in document and not np.array_equal(np.asarray(document[flat]), np.asarray(alignment[nested])):
                raise ValueError('Conflicting nested and flat heading metadata.')
            document[flat] = alignment[nested]
        # Plugin input already used original root positions before heading alignment.
        # Applying the raw mapper offset a second time would shift the result twice.
        if 'root_position_offset_lafan' in document:
            raise ValueError('Plugin alignment must not also include root_position_offset_lafan.')
    has_heading = [key in document for key in flat_keys]
    if any(has_heading) != all(has_heading):
        raise ValueError('Heading restoration requires both position and rotation offsets.')
    declared_heading = document.get('heading_alignment_applied')
    if declared_heading is not None and (type(declared_heading) is not bool or declared_heading != all(has_heading)):
        raise ValueError('heading_alignment_applied conflicts with the supplied heading offsets.')
    if all(has_heading):
        position = np.asarray(document[flat_keys[0]], dtype=float)
        rotation = np.asarray(document[flat_keys[1]], dtype=float)
        if position.shape != (2,) or rotation.shape != (3, 3) or not np.isfinite(position).all() or not np.isfinite(rotation).all():
            raise ValueError('Invalid heading offset dimensions or non-finite values.')
        if not np.allclose(rotation.T @ rotation, np.eye(3), atol=1e-6, rtol=0) or not np.isclose(np.linalg.det(rotation), 1, atol=1e-6, rtol=0):
            raise ValueError('heading_rotation_offset must be a proper rotation matrix.')
    return document


def prepare(job):
    directory = Path(job['output_directory'])
    mapping = Path(job['mapping_directory'])
    sys.path.insert(0, str(mapping))
    from convert_lafan_output_to_manny import convert_document

    source = directory / 'source_model_output.json'
    document = adapt_document(json.loads(source.read_text(encoding='utf-8-sig')))
    stats = job.get('statistics_file') if document['normalization_applied'] else None
    if document['normalization_applied'] and (not stats or not Path(stats).is_file()):
        raise ValueError('Normalized model output requires its Training Statistics in the plugin settings.')
    statistics = {'sha256': file_hash(stats), 'file': str(stats)} if stats else None
    declared_stats = document.get('statistics') or {}
    if stats and declared_stats.get('sha256') and declared_stats['sha256'] != statistics['sha256']:
        raise ValueError('Training Statistics hash does not match the statistics declared in the model output.')
    calibration = json.loads((mapping / 'rest_pose_corrections_22_prototype.json').read_text(encoding='utf-8'))
    skeleton = json.loads((directory / 'skeleton.json').read_text(encoding='utf-8'))
    converted = convert_document(document, calibration, stats_path=stats, skeleton=skeleton)
    if not converted['validation']['passed']:
        raise ValueError('The team inverse mapper rejected the result: ' + json.dumps(converted['validation']))
    converted['adapter_provenance'] = {
        'source_file': job['source_file'], 'source_sha256': file_hash(source),
        'statistics': statistics, 'skeleton_sha256': file_hash(directory / 'skeleton.json'),
        'heading_restored': 'heading_rotation_offset' in document,
        'mapping_source_sha256': {name: file_hash(mapping / name) for name in (
            'convert_lafan_output_to_manny.py', 'manny_lafan_transform.py',
            'official_context_preprocess.py', 'rest_pose_corrections_22_prototype.json')},
    }
    save_json(directory / 'manny_pose.json', converted)
    return {'success': True, 'sample_count': converted['frame_count'],
            'source_was_normalized': document['normalization_applied'],
            'heading_restored': 'heading_rotation_offset' in document}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--job', required=True)
    args = parser.parse_args()
    job = json.loads(Path(args.job).read_text(encoding='utf-8-sig'))
    directory = Path(job['output_directory'])
    try:
        result = prepare(job)
    except Exception as error:
        (directory / 'error.log').write_text(traceback.format_exc(), encoding='utf-8')
        save_json(directory / 'result.json', {'success': False, 'error': str(error)})
        return 1
    save_json(directory / 'result.json', result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
