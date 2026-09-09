"""Phase 1 verification script - webcam tracking + UDP test"""
import time
from tracker import UnifiedTracker

def test_tracking():
    print("=" * 50)
    print("Phase 1 Verification: Webcam Tracking Test")
    print("=" * 50)

    tracker = UnifiedTracker(camera_id=0)
    tracker.start()
    print("[*] Loading models + initializing webcam (8s)...")
    time.sleep(8)

    print("[*] Collecting data for 8 seconds...")
    print("    >> Make sure your FACE is visible to the webcam! <<")
    frames = 0
    face_detected = 0
    last = None
    start = time.time()

    while time.time() - start < 8:
        f = tracker.get_latest()
        if f:
            frames += 1
            last = f
            if f.blendshapes:
                face_detected += 1
        time.sleep(0.016)

    tracker.stop()

    print("\n--- Results ---")
    print(f"Frames received: {frames}")
    print(f"Face detected: {face_detected}/{frames}")
    print(f"Inference FPS: {tracker.fps:.1f}")

    if last:
        print(f"Blendshapes: {len(last.blendshapes)}")
        print(f"Face rotation matrix values: {len(last.face_rotation_matrix)}")
        print(f"Pose landmarks: {len(last.pose_landmarks)}")

        if last.blendshapes:
            print("\nSample blendshapes (top 5):")
            for k, v in list(last.blendshapes.items())[:5]:
                print(f"  {k}: {v:.4f}")

        if last.pose_landmarks:
            print("\nSample pose landmarks (first 3):")
            labels = ["nose", "left_eye_inner", "left_eye"]
            for i, (x, y, z, visibility, presence) in enumerate(last.pose_landmarks[:3]):
                label = labels[i] if i < len(labels) else f"landmark_{i}"
                print(
                    f"  {label}: ({x:.3f}, {y:.3f}, {z:.3f}) "
                    f"confidence=({visibility:.2f}, {presence:.2f})"
                )

        # Verification checklist
        print("\n--- Checklist ---")
        checks = [
            ("Webcam capture", frames > 0),
            ("ARKit 52 blendshapes", len(last.blendshapes) == 52),
            ("Face 3x3 rotation matrix", len(last.face_rotation_matrix) == 9),
            ("PoseLandmarker 33 landmarks", len(last.pose_landmarks) == 33),
            ("20fps+ inference", tracker.fps >= 20),
        ]
        all_pass = True
        for name, ok in checks:
            status = "[PASS]" if ok else "[FAIL]"
            print(f"  {status} {name}")
            if not ok:
                all_pass = False

        if all_pass:
            print("\n>>> Phase 1 tracking verification PASSED!")
        else:
            print("\n>>> Some checks FAILED - review above")
            raise SystemExit(1)
    else:
        print("[FAIL] No tracking data received")
        raise SystemExit(1)


if __name__ == "__main__":
    test_tracking()
