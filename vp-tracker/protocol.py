"""Virtual Production Tracking Packet (VPTP), schema version 3.

The module is dependency-free so the wire contract can be tested without
loading MediaPipe or opening a webcam.
"""

from __future__ import annotations

import math
import struct
from collections.abc import Mapping, Sequence


MAGIC = b"VPTP"
SCHEMA_VERSION = 3
FACE_TRACKED = 1 << 0
POSE_TRACKED = 1 << 1
KNOWN_FLAGS = FACE_TRACKED | POSE_TRACKED
POSE_LANDMARK_COUNT = 33

ARKIT_BLENDSHAPE_NAMES = (
    "_neutral",
    "browDownLeft", "browDownRight",
    "browInnerUp",
    "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff",
    "cheekSquintLeft", "cheekSquintRight",
    "eyeBlinkLeft", "eyeBlinkRight",
    "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight",
    "eyeLookOutLeft", "eyeLookOutRight",
    "eyeLookUpLeft", "eyeLookUpRight",
    "eyeSquintLeft", "eyeSquintRight",
    "eyeWideLeft", "eyeWideRight",
    "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose",
    "mouthDimpleLeft", "mouthDimpleRight",
    "mouthFrownLeft", "mouthFrownRight",
    "mouthFunnel",
    "mouthLeft",
    "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthPressLeft", "mouthPressRight",
    "mouthPucker",
    "mouthRight",
    "mouthRollLower", "mouthRollUpper",
    "mouthShrugLower", "mouthShrugUpper",
    "mouthSmileLeft", "mouthSmileRight",
    "mouthStretchLeft", "mouthStretchRight",
    "mouthUpperUpLeft", "mouthUpperUpRight",
    "noseSneerLeft", "noseSneerRight",
)

BLENDSHAPE_COUNT = len(ARKIT_BLENDSHAPE_NAMES)
HEADER_STRUCT = struct.Struct("<4sBBHIdH")
POSE_COUNT_STRUCT = struct.Struct("<H")
FACE_ROTATION_STRUCT = struct.Struct("<9f")
POSE_STRUCT = struct.Struct("<fffff")
EXPECTED_PACKET_SIZE = (
    HEADER_STRUCT.size
    + BLENDSHAPE_COUNT * 4
    + FACE_ROTATION_STRUCT.size
    + POSE_COUNT_STRUCT.size
    + POSE_LANDMARK_COUNT * POSE_STRUCT.size
)


def encode_packet(
    *,
    frame_id: int,
    timestamp: float,
    blendshapes: Mapping[str, float],
    face_rotation_matrix: Sequence[float],
    pose_landmarks: Sequence[Sequence[float]],
    face_tracked: bool,
    pose_tracked: bool,
) -> bytes:
    """Encode one strict, fixed-size VPTP schema-3 tracking packet."""
    if not 0 <= frame_id <= 0xFFFFFFFF:
        raise ValueError("frame_id must fit uint32")
    if not math.isfinite(timestamp) or timestamp < 0.0:
        raise ValueError("timestamp must be finite and non-negative")
    if pose_tracked and len(pose_landmarks) != POSE_LANDMARK_COUNT:
        raise ValueError(f"tracked pose must contain {POSE_LANDMARK_COUNT} landmarks")
    if not pose_tracked and pose_landmarks:
        raise ValueError("untracked pose must not contain landmarks")
    if face_tracked and len(face_rotation_matrix) != 9:
        raise ValueError("tracked face must contain a 3x3 rotation matrix")
    if not face_tracked and face_rotation_matrix:
        raise ValueError("untracked face must not contain a rotation matrix")

    flags = (FACE_TRACKED if face_tracked else 0) | (POSE_TRACKED if pose_tracked else 0)
    data = bytearray(HEADER_STRUCT.pack(
        MAGIC, SCHEMA_VERSION, flags, 0, frame_id, timestamp, BLENDSHAPE_COUNT
    ))

    for name in ARKIT_BLENDSHAPE_NAMES:
        value = float(blendshapes.get(name, 0.0)) if face_tracked else 0.0
        if not math.isfinite(value) or not 0.0 <= value <= 1.0:
            raise ValueError(f"invalid blendshape value for {name}: {value}")
        data += struct.pack("<f", value)

    rotation_values = (
        tuple(float(value) for value in face_rotation_matrix)
        if face_tracked
        else (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    )
    if not all(math.isfinite(value) for value in rotation_values):
        raise ValueError("face rotation matrix contains NaN or Inf")
    data += FACE_ROTATION_STRUCT.pack(*rotation_values)

    data += POSE_COUNT_STRUCT.pack(POSE_LANDMARK_COUNT)
    landmarks = pose_landmarks if pose_tracked else [(0.0, 0.0, 0.0, 0.0, 0.0)] * POSE_LANDMARK_COUNT
    for index, landmark in enumerate(landmarks):
        if len(landmark) != 5:
            raise ValueError(f"pose landmark {index} must contain x, y, z, visibility, presence")
        values = tuple(float(value) for value in landmark)
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f"pose landmark {index} contains NaN or Inf")
        if not 0.0 <= values[3] <= 1.0 or not 0.0 <= values[4] <= 1.0:
            raise ValueError(f"pose landmark {index} has invalid confidence")
        data += POSE_STRUCT.pack(*values)

    if len(data) != EXPECTED_PACKET_SIZE:
        raise AssertionError(f"unexpected packet size: {len(data)}")
    return bytes(data)
