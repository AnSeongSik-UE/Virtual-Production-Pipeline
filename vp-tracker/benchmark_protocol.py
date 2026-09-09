"""Repeatable dependency-free VPTP schema-3 encoding benchmark."""

import argparse
import json
import time

from protocol import POSE_LANDMARK_COUNT, encode_packet


def benchmark(iterations: int) -> dict:
    blendshapes = {
        "eyeBlinkLeft": 0.25,
        "eyeBlinkRight": 0.25,
        "jawOpen": 0.4,
        "mouthSmileLeft": 0.75,
        "mouthSmileRight": 0.75,
    }
    pose = [(0.5, 0.5, 0.0, 0.9, 0.8)] * POSE_LANDMARK_COUNT

    for frame_id in range(100):
        encode_packet(
            frame_id=frame_id,
            timestamp=1234.5,
            blendshapes=blendshapes,
            face_rotation_matrix=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
            pose_landmarks=pose,
            face_tracked=True,
            pose_tracked=True,
        )

    started = time.perf_counter()
    total_bytes = 0
    for frame_id in range(iterations):
        packet = encode_packet(
            frame_id=frame_id & 0xFFFFFFFF,
            timestamp=1234.5,
            blendshapes=blendshapes,
            face_rotation_matrix=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
            pose_landmarks=pose,
            face_tracked=True,
            pose_tracked=True,
        )
        total_bytes += len(packet)
    elapsed = time.perf_counter() - started

    return {
        "iterations": iterations,
        "elapsed_seconds": round(elapsed, 6),
        "packets_per_second": round(iterations / elapsed, 1),
        "microseconds_per_packet": round(elapsed * 1_000_000 / iterations, 2),
        "payload_megabytes_per_second": round(total_bytes / elapsed / 1_000_000, 2),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=10_000)
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    print(json.dumps(benchmark(args.iterations), indent=2))
