"""Mouth blendshape debug - show realtime values for mouth-related ARKit blendshapes"""
import time
from tracker import UnifiedTracker

MOUTH_KEYS = [
    "jawOpen", "jawForward", "jawLeft", "jawRight",
    "mouthClose", "mouthFunnel", "mouthPucker",
    "mouthSmileLeft", "mouthSmileRight",
    "mouthFrownLeft", "mouthFrownRight",
    "mouthLeft", "mouthRight",
    "mouthUpperUpLeft", "mouthUpperUpRight",
    "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthRollLower", "mouthRollUpper",
    "mouthShrugLower", "mouthShrugUpper",
    "mouthStretchLeft", "mouthStretchRight",
    "mouthDimpleLeft", "mouthDimpleRight",
    "mouthPressLeft", "mouthPressRight",
]

def main():
    print("Mouth Blendshape Debug (close your mouth normally)")
    print("=" * 60)
    tracker = UnifiedTracker(camera_id=0)
    tracker.start()
    print("[*] Loading models (5s)...")
    time.sleep(5)

    print("[*] Showing values > 0.05 (press Ctrl+C to stop)")
    print()

    try:
        while True:
            f = tracker.get_latest()
            if f and f.blendshapes:
                parts = []
                for k in MOUTH_KEYS:
                    v = f.blendshapes.get(k, 0.0)
                    if v > 0.05:
                        parts.append(f"{k}={v:.2f}")
                if parts:
                    print("\r" + " | ".join(parts) + "          ", end="", flush=True)
            time.sleep(0.1)
    except KeyboardInterrupt:
        tracker.stop()
        print("\n[OK] Done")

if __name__ == "__main__":
    main()
