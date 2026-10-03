import unittest

import numpy as np

from official_context_preprocess import (
    denormalize,
    normalize,
    reverse_start_center_root,
    start_center_root,
    yaw_matrix,
)


class ContextPreprocessTests(unittest.TestCase):
    def test_headings_align_and_reverse(self):
        context_len = 10
        for source_yaw in (0.0, np.pi / 2, -np.pi / 2, np.pi):
            positions = np.arange(36, dtype=float).reshape(12, 3)
            rotations = np.tile(np.eye(3), (12, 22, 1, 1))
            base = np.array(
                [[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, -1.0, 0.0]]
            )
            rotations[:, 0] = yaw_matrix(source_yaw) @ base

            centered_p, centered_r, pos_offset, rot_offset, _ = start_center_root(
                positions, rotations, context_len
            )
            heading = centered_r[context_len - 1, 0] @ [0.0, 1.0, 0.0]
            np.testing.assert_allclose(heading[[0, 2]], [1.0, 0.0], atol=1e-10)

            restored_p, restored_r = reverse_start_center_root(
                centered_p, centered_r, pos_offset, rot_offset
            )
            np.testing.assert_allclose(restored_p, positions, atol=1e-10)
            np.testing.assert_allclose(restored_r, rotations, atol=1e-10)

    def test_normalization_round_trip(self):
        values = np.linspace(-3.0, 3.0, 270).reshape(2, 135)
        mean = np.linspace(-0.5, 0.5, 135)
        std = np.linspace(0.5, 1.5, 135)
        restored = denormalize(normalize(values, mean, std), mean, std)
        np.testing.assert_allclose(restored, values, atol=1e-12)

    def test_invalid_zero_standard_deviation_is_rejected(self):
        values = np.zeros((1, 135))
        mean = np.zeros(135)
        std = np.ones(135)
        std[24] = 0.0
        with self.assertRaises(ValueError):
            normalize(values, mean, std)


if __name__ == "__main__":
    unittest.main()
