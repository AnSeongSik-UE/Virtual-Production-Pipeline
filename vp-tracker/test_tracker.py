"""Unit tests for latest-only Face/Pose result fusion."""

import os
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

from tracker import (
    TrackingFrame,
    UnifiedTracker,
    _next_frame_timestamp_ms,
    selected_pose_model,
)


class TrackerConfigurationTests(unittest.TestCase):
    def test_full_pose_model_is_default(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(selected_pose_model(), "full")

    def test_invalid_pose_model_is_rejected(self):
        with patch.dict(os.environ, {"VP_POSE_MODEL": "unknown"}, clear=True):
            with self.assertRaises(ValueError):
                selected_pose_model()

    def test_frame_timestamp_uses_milliseconds_and_never_moves_backward(self):
        self.assertEqual(_next_frame_timestamp_ms(-1, 12.345678), 12345)
        self.assertEqual(_next_frame_timestamp_ms(12345, 12.345100), 12346)


class TrackerFusionTests(unittest.TestCase):
    def test_face_callback_extracts_three_by_three_rotation(self):
        tracker = UnifiedTracker()
        result = SimpleNamespace(
            face_blendshapes=[[
                SimpleNamespace(category_name="jawOpen", score=0.5),
            ]],
            facial_transformation_matrixes=[np.eye(4, dtype=np.float32)],
        )

        tracker._on_face_result(result, None, 123)

        self.assertTrue(tracker._latest_face_tracked)
        self.assertEqual(tracker._latest_face_timestamp, 123)
        self.assertEqual(
            tracker._latest_face_rotation_matrix,
            (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
        )

    def test_fuses_recent_face_and_pose_results(self):
        tracker = UnifiedTracker(result_stale_ms=250)
        tracker._latest_face_timestamp = 900
        tracker._latest_face_blendshapes = {"jawOpen": 0.5}
        tracker._latest_face_rotation_matrix = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
        tracker._latest_face_tracked = True
        tracker._latest_pose_timestamp = 1000
        tracker._latest_pose_landmarks = [(0.5, 0.5, 0.0, 0.9, 0.8)] * 33
        tracker._latest_pose_tracked = True

        tracker._publish_fused_locked(1000)
        frame = tracker.get_latest()

        self.assertIsNotNone(frame)
        self.assertTrue(frame.face_tracked)
        self.assertTrue(frame.pose_tracked)
        self.assertEqual(len(frame.blendshapes), 1)
        self.assertEqual(len(frame.face_rotation_matrix), 9)
        self.assertEqual(len(frame.pose_landmarks), 33)

    def test_drops_stale_modality_and_drains_to_latest(self):
        tracker = UnifiedTracker(result_stale_ms=100)
        tracker._latest_face_timestamp = 0
        tracker._latest_face_blendshapes = {"jawOpen": 0.5}
        tracker._latest_face_rotation_matrix = (1.0, 0.0, 0.0, 1.0)
        tracker._latest_face_tracked = True
        tracker._latest_pose_timestamp = 500
        tracker._latest_pose_landmarks = [(0.5, 0.5, 0.0, 0.9, 0.8)] * 33
        tracker._latest_pose_tracked = True
        tracker.data_queue.put_nowait(TrackingFrame(timestamp=1.0))

        tracker._publish_fused_locked(500)
        frame = tracker.get_latest()

        self.assertEqual(frame.timestamp, 500.0)
        self.assertFalse(frame.face_tracked)
        self.assertTrue(frame.pose_tracked)


if __name__ == "__main__":
    unittest.main()
