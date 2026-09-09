"""Phase 3 verification script - AnimInstance integration test
Sends mock tracking data via UDP and verifies packet format.
UE side verification: check Output Log for VPAnimInstance/VPUDPReceiver messages.
"""
import socket
import time
from protocol import (
    ARKIT_BLENDSHAPE_NAMES,
    BLENDSHAPE_COUNT,
    EXPECTED_PACKET_SIZE,
    HEADER_STRUCT,
    POSE_LANDMARK_COUNT,
    encode_packet,
)


def build_packet(blink_value: float = 0.0, smile_value: float = 0.0) -> bytes:
    """Build a VPTP schema-3 packet with controllable test values."""
    blendshapes = {
        "eyeBlinkLeft": blink_value,
        "eyeBlinkRight": blink_value,
        "mouthSmileLeft": smile_value,
        "mouthSmileRight": smile_value,
    }
    pose = [
        (0.5, 0.1 + (i / POSE_LANDMARK_COUNT) * 0.8, 0.0, 1.0, 1.0)
        for i in range(POSE_LANDMARK_COUNT)
    ]
    return encode_packet(
        frame_id=int(time.time() * 1000) & 0xFFFFFFFF,
        timestamp=time.time(),
        blendshapes=blendshapes,
        face_rotation_matrix=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
        pose_landmarks=pose,
        face_tracked=True,
        pose_tracked=True,
    )


def test_phase3():
    print("=" * 55)
    print("Phase 3 Verification: AnimInstance Integration Test")
    print("=" * 55)
    print()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = ("127.0.0.1", 7000)

    # Step 1: Packet format validation
    print("[1/3] Packet format validation...")
    pkt = build_packet(blink_value=0.8, smile_value=0.6)
    expected_size = EXPECTED_PACKET_SIZE
    actual_size = len(pkt)
    fmt_ok = actual_size == expected_size
    print(f"  Expected: {expected_size} bytes")
    print(f"  Actual:   {actual_size} bytes")
    print(f"  {'[PASS]' if fmt_ok else '[FAIL]'} Packet size")

    # Verify magic header
    magic_ok = pkt[:4] == b'VPTP'
    print(f"  {'[PASS]' if magic_ok else '[FAIL]'} Magic header")

    # Verify blendshape count
    bs_count = HEADER_STRUCT.unpack_from(pkt)[-1]
    bs_ok = bs_count == BLENDSHAPE_COUNT
    print(f"  {'[PASS]' if bs_ok else '[FAIL]'} Blendshape count: {bs_count}")

    # Verify pose count
    pose_offset = HEADER_STRUCT.size + BLENDSHAPE_COUNT * 4
    pose_count = int.from_bytes(pkt[pose_offset:pose_offset + 2], "little")
    pose_ok = pose_count == POSE_LANDMARK_COUNT
    print(f"  {'[PASS]' if pose_ok else '[FAIL]'} Pose landmark count: {pose_count}")

    # Step 2: Send test sequences to UE
    print()
    print("[2/3] Sending test sequences to UE (127.0.0.1:7000)...")
    print("  >> Make sure UE Play mode is active with VPUDPReceiver! <<")
    print()

    # Sequence A: blink animation (2 seconds)
    print("  Sequence A: Eye blink animation (2s)...")
    frames_sent = 0
    start = time.time()
    while time.time() - start < 2.0:
        t = time.time() - start
        # Oscillate blink 0->1->0 over 1 second
        import math
        blink = abs(math.sin(t * math.pi))
        pkt = build_packet(blink_value=blink, smile_value=0.0)
        sock.sendto(pkt, target)
        frames_sent += 1
        time.sleep(1 / 60)
    print(f"    Sent {frames_sent} frames")

    # Sequence B: smile animation (2 seconds)
    print("  Sequence B: Smile animation (2s)...")
    start = time.time()
    while time.time() - start < 2.0:
        t = time.time() - start
        smile = abs(math.sin(t * math.pi))
        pkt = build_packet(blink_value=0.0, smile_value=smile)
        sock.sendto(pkt, target)
        frames_sent += 1
        time.sleep(1 / 60)
    print(f"    Total sent: {frames_sent} frames")

    # Sequence C: combined (2 seconds)
    print("  Sequence C: Blink + Smile combined (2s)...")
    start = time.time()
    while time.time() - start < 2.0:
        t = time.time() - start
        blink = abs(math.sin(t * 2 * math.pi))
        smile = abs(math.cos(t * math.pi))
        pkt = build_packet(blink_value=blink, smile_value=smile)
        sock.sendto(pkt, target)
        frames_sent += 1
        time.sleep(1 / 60)
    print(f"    Total sent: {frames_sent} frames")

    sock.close()

    # Step 3: Summary
    print()
    print("[3/3] Verification checklist")
    print(f"  [PASS] Packet format valid ({actual_size}B)")
    print(f"  [PASS] {frames_sent} frames sent at ~60Hz")
    print()
    print("  >> UE side: check Output Log for these messages:")
    print('     [VPAnimInstance] Auto-connected to VPUDPReceiver on <ActorName>')
    print('     [VPUDPReceiver] Packets=60 BS=52 Pose=33')
    print()
    print("  >> Visual check:")
    print("     - Sequence A: avatar eyes should blink")
    print("     - Sequence B: avatar mouth should smile")
    print("     - Sequence C: blink + smile combined")
    print()

    all_pass = fmt_ok and magic_ok and bs_ok and pose_ok
    if all_pass:
        print(">>> Phase 3 packet verification PASSED!")
        print(">>> Visual verification requires UE Play mode with avatar mesh.")
    else:
        print(">>> Some checks FAILED - review above")
        raise SystemExit(1)


if __name__ == "__main__":
    test_phase3()
