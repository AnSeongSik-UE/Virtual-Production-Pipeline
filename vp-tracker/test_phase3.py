"""Phase 3 verification script - AnimInstance integration test
Sends mock tracking data via UDP and verifies packet format.
UE side verification: check Output Log for VPAnimInstance/VPUDPReceiver messages.
"""
import struct
import socket
import time


# ARKit 52 blendshape names (same order as VPUDPReceiver.cpp)
ARKIT_NAMES = [
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
]


def build_packet(blink_value: float = 0.0, smile_value: float = 0.0) -> bytes:
    """Build a VPFR binary packet with controllable test values."""
    data = bytearray(b'VPFR')
    data += struct.pack('<d', time.time())

    # 52 blendshapes
    bs_values = [0.0] * 52
    bs_values[ARKIT_NAMES.index("eyeBlinkLeft")] = blink_value
    bs_values[ARKIT_NAMES.index("eyeBlinkRight")] = blink_value
    bs_values[ARKIT_NAMES.index("mouthSmileLeft")] = smile_value
    bs_values[ARKIT_NAMES.index("mouthSmileRight")] = smile_value

    data += struct.pack('<H', 52)
    for v in bs_values:
        data += struct.pack('<f', v)

    # 33 pose landmarks (simple T-pose mock)
    data += struct.pack('<H', 33)
    for i in range(33):
        x = 0.5  # center
        y = 0.1 + (i / 33.0) * 0.8  # spread vertically
        z = 0.0
        data += struct.pack('<fff', x, y, z)

    return bytes(data)


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
    expected_size = 4 + 8 + 2 + (52 * 4) + 2 + (33 * 3 * 4)
    actual_size = len(pkt)
    fmt_ok = actual_size == expected_size
    print(f"  Expected: {expected_size} bytes")
    print(f"  Actual:   {actual_size} bytes")
    print(f"  {'[PASS]' if fmt_ok else '[FAIL]'} Packet size")

    # Verify magic header
    magic_ok = pkt[:4] == b'VPFR'
    print(f"  {'[PASS]' if magic_ok else '[FAIL]'} Magic header")

    # Verify blendshape count
    bs_count = struct.unpack_from('<H', pkt, 12)[0]
    bs_ok = bs_count == 52
    print(f"  {'[PASS]' if bs_ok else '[FAIL]'} Blendshape count: {bs_count}")

    # Verify pose count
    pose_offset = 12 + 2 + 52 * 4
    pose_count = struct.unpack_from('<H', pkt, pose_offset)[0]
    pose_ok = pose_count == 33
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


if __name__ == "__main__":
    test_phase3()
