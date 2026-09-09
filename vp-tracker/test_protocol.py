"""Dependency-free tests for the VPTP schema-3 wire contract."""

import math
import struct
import unittest

from protocol import (
    ARKIT_BLENDSHAPE_NAMES,
    BLENDSHAPE_COUNT,
    EXPECTED_PACKET_SIZE,
    FACE_ROTATION_STRUCT,
    HEADER_STRUCT,
    MAGIC,
    POSE_LANDMARK_COUNT,
    SCHEMA_VERSION,
    encode_packet,
)


def valid_pose():
    return [(0.5, 0.5, 0.0, 0.9, 0.8)] * POSE_LANDMARK_COUNT


def identity_face_rotation():
    return (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


class ProtocolTests(unittest.TestCase):
    def test_fixed_size_and_header(self):
        packet = encode_packet(
            frame_id=42,
            timestamp=1234.5,
            blendshapes={"mouthSmileLeft": 0.75},
            face_rotation_matrix=identity_face_rotation(),
            pose_landmarks=valid_pose(),
            face_tracked=True,
            pose_tracked=True,
        )
        self.assertEqual(len(packet), EXPECTED_PACKET_SIZE)
        magic, version, flags, reserved, frame_id, timestamp, count = HEADER_STRUCT.unpack_from(packet)
        self.assertEqual(magic, MAGIC)
        self.assertEqual(version, SCHEMA_VERSION)
        self.assertEqual(flags, 3)
        self.assertEqual(reserved, 0)
        self.assertEqual(frame_id, 42)
        self.assertEqual(timestamp, 1234.5)
        self.assertEqual(count, BLENDSHAPE_COUNT)

    def test_blendshape_order_is_name_based(self):
        packet = encode_packet(
            frame_id=0,
            timestamp=0.0,
            blendshapes={"mouthSmileLeft": 0.75, "eyeBlinkLeft": 0.25},
            face_rotation_matrix=identity_face_rotation(),
            pose_landmarks=[],
            face_tracked=True,
            pose_tracked=False,
        )
        values = struct.unpack_from(f"<{BLENDSHAPE_COUNT}f", packet, HEADER_STRUCT.size)
        self.assertAlmostEqual(values[ARKIT_BLENDSHAPE_NAMES.index("eyeBlinkLeft")], 0.25)
        self.assertAlmostEqual(values[ARKIT_BLENDSHAPE_NAMES.index("mouthSmileLeft")], 0.75)

    def test_missing_detection_uses_zero_payload_and_flags(self):
        packet = encode_packet(
            frame_id=0,
            timestamp=0.0,
            blendshapes={},
            face_rotation_matrix=(),
            pose_landmarks=[],
            face_tracked=False,
            pose_tracked=False,
        )
        self.assertEqual(HEADER_STRUCT.unpack_from(packet)[2], 0)
        values = struct.unpack_from(f"<{BLENDSHAPE_COUNT}f", packet, HEADER_STRUCT.size)
        self.assertTrue(all(value == 0.0 for value in values))
        rotation_offset = HEADER_STRUCT.size + BLENDSHAPE_COUNT * 4
        self.assertEqual(
            FACE_ROTATION_STRUCT.unpack_from(packet, rotation_offset),
            identity_face_rotation(),
        )

    def test_rejects_invalid_values_and_counts(self):
        cases = (
            {"blendshapes": {"jawOpen": math.nan}, "face_rotation_matrix": identity_face_rotation(), "pose_landmarks": valid_pose()},
            {"blendshapes": {}, "face_rotation_matrix": identity_face_rotation(), "pose_landmarks": [(0.0, 0.0, 0.0, 1.0, 1.0)]},
            {"blendshapes": {}, "face_rotation_matrix": identity_face_rotation(), "pose_landmarks": [(0.0, 0.0, 0.0, 2.0, 1.0)] * POSE_LANDMARK_COUNT},
            {"blendshapes": {}, "face_rotation_matrix": (math.nan,) * 9, "pose_landmarks": valid_pose()},
            {"blendshapes": {}, "face_rotation_matrix": (), "pose_landmarks": valid_pose()},
        )
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                encode_packet(
                    frame_id=0,
                    timestamp=0.0,
                    face_tracked=True,
                    pose_tracked=True,
                    **case,
                )


if __name__ == "__main__":
    unittest.main()
