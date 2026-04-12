"""Debug all blendshapes - show which ones are active during expressions"""
import time
from tracker import UnifiedTracker

def main():
    print("Full Blendshape Debug")
    print("Do these expressions, 3 seconds each:")
    print("  1. Neutral (rest)")
    print("  2. Big smile")
    print("  3. Eyebrows up (surprised)")
    print("  4. Eyebrows down (angry)")
    print("  5. Look left/right")
    print("=" * 60)

    tracker = UnifiedTracker(camera_id=0)
    tracker.start()
    print("[*] Loading models (10s)...")
    time.sleep(10)

    expressions = ["NEUTRAL", "SMILE", "SURPRISED", "ANGRY", "LOOK_AROUND"]

    for expr in expressions:
        print(f"\n>>> Do: {expr} (3 seconds)")
        time.sleep(1)  # give time to prepare

        samples = []
        start = time.time()
        while time.time() - start < 2:
            f = tracker.get_latest()
            if f and f.blendshapes:
                samples.append(dict(f.blendshapes))
            time.sleep(0.1)

        if not samples:
            print("  No data")
            continue

        # Average values
        avg = {}
        for s in samples:
            for k, v in s.items():
                if k == "_neutral":
                    continue
                avg[k] = avg.get(k, 0) + v / len(samples)

        # Show values > 0.1
        active = {k: v for k, v in sorted(avg.items(), key=lambda x: -x[1]) if v > 0.1}
        print(f"  Active blendshapes (>{0.1}):")
        for k, v in active.items():
            print(f"    {k}: {v:.2f}")

    tracker.stop()
    print("\n[OK] Done")

if __name__ == "__main__":
    main()
