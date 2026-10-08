"""Exercise real child-process calls and the proposed inference file contract."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

PLUGIN = Path(__file__).resolve().parents[1]
PROJECT = Path(os.environ.get('MIB_TEST_PROJECT_ROOT', str(PLUGIN.parents[1])))
sys.path.insert(0, str(PLUGIN / 'Scripts'))
from prepare_manny_input import save_json
from run_manny_inference import run


class InferenceBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='manny inference 한글 ')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        fixture = json.loads((PLUGIN / 'Tests/Fixtures/context_model_output.json').read_text(encoding='utf-8'))
        calibration = json.loads((PROJECT / 'Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json').read_text(encoding='utf-8'))
        self.input = {'schema': 'motion_inbetweening.manny_input.v1', 'vectors_tx135': fixture['predictions_tx135'],
                      'alignment': {'position_offset_xz': fixture['heading_position_offset_xz'],
                                    'rotation_offset': fixture['heading_rotation_offset']},
                      'normalization_applied': False, 'sample_rate_hz': 30, 'sample_count': 31,
                      'context_len': 10, 'position_space': 'lafan_start_centered',
                      'joint_order': calibration['joint_order'], 'parents': calibration['parents']}
        for name in ('start', 'end'):
            save_json(self.directory / (name + '_input.json'), self.input)
        self.config = self.directory / 'model config.json'
        save_json(self.config, {})
        self.job = {'request_id': 'unit-test-request', 'transition_frames': 20,
                    'output_directory': str(self.directory), 'mapping_directory': str(PROJECT / 'Tools/MannyLafanMapping'),
                    'inference_python': sys.executable,
                    'inference_script': str(PLUGIN / 'Tests/Fixtures/inference_fixture_backend.py'),
                    'model_config_file': str(self.config), 'statistics_file': ''}

    def test_success_runs_backend_with_space_and_unicode_paths(self):
        original = (self.directory / 'start_input.json').read_bytes()
        result = run(self.job)
        self.assertTrue(result['success'])
        self.assertEqual(result['sample_count'], 31)
        request = json.loads((self.directory / 'request.json').read_text(encoding='utf-8'))
        self.assertEqual(request['start_context_start_index'], 21)
        self.assertEqual(request['end_target_index'], 0)
        self.assertEqual(request['transition_frames'], 20)
        self.assertEqual(original, (self.directory / 'start_input.json').read_bytes())

    def test_nonzero_exit_preserves_backend_error(self):
        save_json(self.config, {'test_mode': 'failure'})
        with self.assertRaisesRegex(ValueError, 'code 7'):
            run(self.job)
        self.assertIn('Intentional test failure', (self.directory / 'model_stderr.log').read_text(encoding='utf-8'))

    def test_success_without_output_is_rejected(self):
        save_json(self.config, {'test_mode': 'missing_output'})
        with self.assertRaisesRegex(ValueError, 'without writing'):
            run(self.job)

    def test_output_from_another_request_is_rejected(self):
        save_json(self.config, {'test_mode': 'wrong_request'})
        with self.assertRaisesRegex(ValueError, 'inference_request_id'):
            run(self.job)

    def test_wrong_transition_length_is_rejected(self):
        save_json(self.config, {'test_mode': 'wrong_count'})
        with self.assertRaisesRegex(ValueError, 'context \+ transition \+ target'):
            run(self.job)

    def test_coerced_numbers_are_rejected(self):
        save_json(self.config, {'test_mode': 'invalid_numbers'})
        with self.assertRaisesRegex(ValueError, 'JSON numbers'):
            run(self.job)

    def test_changed_heading_is_rejected(self):
        save_json(self.config, {'test_mode': 'wrong_alignment'})
        with self.assertRaisesRegex(ValueError, 'start input coordinate frame'):
            run(self.job)

    def test_existing_output_is_never_reused_or_overwritten(self):
        output = self.directory / 'model_output.json'
        output.write_text('prior result', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'fresh job'):
            run(self.job)
        self.assertEqual(output.read_text(encoding='utf-8'), 'prior result')

    def test_bad_input_fails_before_backend_starts(self):
        document = copy.deepcopy(self.input)
        document['vectors_tx135'][0][132] = '0'
        save_json(self.directory / 'end_input.json', document)
        with self.assertRaisesRegex(ValueError, 'JSON numbers'):
            run(self.job)
        self.assertFalse((self.directory / 'test_backend_started.json').exists())

    def test_normalized_input_requires_matching_statistics(self):
        document = copy.deepcopy(self.input)
        document['normalization_applied'] = True
        document['statistics'] = {'sha256': 'unmatched'}
        save_json(self.directory / 'start_input.json', document)
        with self.assertRaisesRegex(ValueError, 'Training Statistics'):
            run(self.job)
        self.assertFalse((self.directory / 'test_backend_started.json').exists())

    def test_invalid_transition_count_is_rejected(self):
        for value in (True, 0, 241, '20', 20.5):
            with self.subTest(value=value):
                self.job['transition_frames'] = value
                with self.assertRaisesRegex(ValueError, 'transition_frames'):
                    run(self.job)


if __name__ == '__main__':
    unittest.main()
