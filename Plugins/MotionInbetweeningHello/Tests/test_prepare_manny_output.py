"""Regression checks for model-output declarations and ambiguous JSON."""
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
from prepare_manny_output import adapt_document, load_model_output


class OutputDocumentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = load_model_output(PLUGIN / 'Tests/Fixtures/context_model_output.json')
        cls.calibration = json.loads((PROJECT / 'Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json').read_text(encoding='utf-8'))

    def with_metadata(self):
        document = copy.deepcopy(self.fixture)
        document.update(joint_order=self.calibration['joint_order'].copy(), parents=self.calibration['parents'].copy(),
                        frame_count=31, sample_count=31, shape=[31, 135],
                        layout='T x (22 joints * rotation6D + Hips XYZ)', rotation_space='parent_local',
                        source_position_unit='cm', position_space='lafan_start_centered')
        return document

    def test_minimal_existing_fixture_remains_supported(self):
        adapted = adapt_document(self.fixture, self.calibration)
        self.assertEqual(adapted['predictions_tx135'], self.fixture['predictions_tx135'])

    def test_declared_plugin_metadata_and_nested_alignment_are_supported(self):
        document = self.with_metadata()
        document['alignment'] = {'position_offset_xz': document.pop('heading_position_offset_xz'),
                                 'rotation_offset': document.pop('heading_rotation_offset')}
        document['heading_alignment_applied'] = True
        adapted = adapt_document(document, self.calibration)
        self.assertEqual(adapted['heading_position_offset_xz'], self.fixture['heading_position_offset_xz'])
        self.assertEqual(adapted['heading_rotation_offset'], self.fixture['heading_rotation_offset'])

    def test_two_samples_and_consistent_counts_are_supported(self):
        document = self.with_metadata()
        document['predictions_tx135'] = document['predictions_tx135'][:2]
        document.update(frame_count=2, sample_count=2, shape=[2, 135])
        self.assertEqual(len(adapt_document(document, self.calibration)['predictions_tx135']), 2)

    def test_swapped_columns_and_joint_order_are_rejected(self):
        document = self.with_metadata()
        left, right = document['joint_order'].index('LeftArm'), document['joint_order'].index('RightArm')
        document['joint_order'][left], document['joint_order'][right] = document['joint_order'][right], document['joint_order'][left]
        for row in document['predictions_tx135']:
            row[left*6:left*6+6], row[right*6:right*6+6] = row[right*6:right*6+6], row[left*6:left*6+6]
        with self.assertRaisesRegex(ValueError, 'joint_order'):
            adapt_document(document, self.calibration)

    def test_unsupported_semantics_are_rejected(self):
        wrong_parents = self.calibration['parents'].copy(); wrong_parents[2] = 0
        bool_parents = self.calibration['parents'].copy(); bool_parents[1] = False
        duplicate_joints = self.calibration['joint_order'].copy(); duplicate_joints[1] = duplicate_joints[2]
        for field, value in [('source_position_unit', 'm'), ('rotation_space', 'component'),
                             ('position_space', 'ue_world'), ('layout', 'Hips XYZ + rotations'),
                             ('parents', wrong_parents), ('parents', bool_parents), ('joint_order', duplicate_joints)]:
            with self.subTest(field=field, value=value):
                document = self.with_metadata(); document[field] = value
                with self.assertRaisesRegex(ValueError, field):
                    adapt_document(document, self.calibration)

    def test_declared_truncated_sequence_is_rejected(self):
        document = self.with_metadata()
        document['predictions_tx135'].pop()
        with self.assertRaisesRegex(ValueError, 'frame_count'):
            adapt_document(document, self.calibration)

    def test_count_and_shape_declarations_are_consistent_integers(self):
        for field, value in [('frame_count', 30), ('sample_count', 32), ('sample_count', 31.0),
                             ('frame_count', True), ('shape', [31, 134]), ('shape', [31.0, 135]),
                             ('shape', [31, 135, 1]), ('shape', None)]:
            with self.subTest(field=field, value=value):
                document = self.with_metadata(); document[field] = value
                with self.assertRaisesRegex(ValueError, field):
                    adapt_document(document, self.calibration)

    def test_declared_centered_positions_require_restoration_offsets(self):
        document = self.with_metadata()
        document.pop('heading_position_offset_xz'); document.pop('heading_rotation_offset')
        with self.assertRaisesRegex(ValueError, 'requires heading'):
            adapt_document(document, self.calibration)

    def test_root_document_and_heading_flag_have_explicit_types(self):
        with self.assertRaisesRegex(ValueError, 'JSON object'):
            adapt_document(list(self.fixture.items()), self.calibration)
        document = copy.deepcopy(self.fixture); document['heading_alignment_applied'] = None
        with self.assertRaisesRegex(ValueError, 'heading_alignment_applied'):
            adapt_document(document, self.calibration)

    def test_duplicate_keys_are_rejected_before_values_are_discarded(self):
        cases = [(json.dumps(self.fixture)[:-1] + ', "sample_rate_hz":60}', 'sample_rate_hz'),
                 ('{"normalization_applied":true,"normalization_applied":false}', 'normalization_applied'),
                 ('{"statistics":{"sha256":"first","sha256":"second"}}', 'sha256'),
                 ('{"predictions_tx135":[],"predictions_tx135":[]}', 'predictions_tx135')]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'model_output.json'
            for text, field in cases:
                with self.subTest(field=field):
                    path.write_text(text, encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'Duplicate JSON key: ' + field):
                        load_model_output(path)

    def test_utf8_bom_remains_supported(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'model_output.json'
            path.write_text(json.dumps(self.fixture), encoding='utf-8-sig')
            self.assertEqual(load_model_output(path), self.fixture)


if __name__ == '__main__':
    unittest.main()
