"""
VP Pipeline one-click launcher
Runs preflight checks, launches tracker, connects OBS, and monitors status.
UE editor must be opened and Play mode entered manually.
"""
import subprocess
import time
import sys
import os
import socket
import threading


class PipelineLauncher:
    def __init__(self):
        self.processes = {}
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        self.obs_ctrl = None

    def preflight_check(self) -> bool:
        """Pre-launch checks."""
        print("[Preflight]")
        checks = [
            ("Webcam", self._check_webcam()),
            ("UDP port 7000", self._check_port(7000)),
            ("OBS WebSocket", self._check_obs()),
            ("MediaPipe models", self._check_models()),
        ]

        all_ok = True
        for name, (ok, msg) in checks:
            status = "[OK]" if ok else "[FAIL]"
            print(f"  {status} {name}: {msg}")
            if not ok:
                all_ok = False

        return all_ok

    def launch(self):
        """Run the full pipeline."""
        print("=" * 50)
        print("  VP Pipeline Launcher")
        print("=" * 50)

        # 1. Preflight
        if not self.preflight_check():
            print("\n[FAIL] Fix issues above and retry.")
            return

        print("\n[OK] All checks passed!")

        # 2. Launch tracker + sender
        print("\n[TRACK] Launching tracker...")
        self.processes["tracker"] = subprocess.Popen(
            [sys.executable, "sender.py"],
            cwd=self.base_dir,
        )
        time.sleep(3)

        if self.processes["tracker"].poll() is not None:
            print("[FAIL] Tracker exited unexpectedly")
            return

        print("[TRACK] Tracker running (PID: %d)" % self.processes["tracker"].pid)

        # 3. Connect OBS
        print("\n[OBS] Connecting...")
        try:
            from obs_controller import OBSController
            self.obs_ctrl = OBSController()
            print(f"[OBS] Current scene: {self.obs_ctrl.get_current_scene()}")
        except Exception as e:
            print(f"[OBS] Connection failed: {e}")
            print("[OBS] Continuing without OBS control")
            self.obs_ctrl = None

        # 4. Ready
        print("\n" + "=" * 50)
        print("  Pipeline Ready!")
        print("=" * 50)
        print("  Commands:")
        print("    stream  - Start streaming")
        print("    stop    - Stop streaming")
        print("    rec     - Start recording")
        print("    stoprec - Stop recording")
        print("    status  - Show pipeline status")
        print("    quit    - Shutdown everything")
        print()

        # 5. Monitor loop
        try:
            while True:
                cmd = input("> ").strip().lower()
                if cmd == "stream":
                    if self.obs_ctrl:
                        self.obs_ctrl.start_streaming()
                    else:
                        print("[OBS] Not connected")
                elif cmd == "stop":
                    if self.obs_ctrl:
                        self.obs_ctrl.stop_streaming()
                elif cmd == "rec":
                    if self.obs_ctrl:
                        self.obs_ctrl.start_recording()
                elif cmd == "stoprec":
                    if self.obs_ctrl:
                        self.obs_ctrl.stop_recording()
                elif cmd == "status":
                    self._print_status()
                elif cmd in ("quit", "q", "exit"):
                    break
                elif cmd:
                    print("Unknown command. Try: stream, stop, rec, stoprec, status, quit")
        except (KeyboardInterrupt, EOFError):
            pass

        # 6. Shutdown
        self._shutdown()

    def _print_status(self):
        """Print current pipeline status."""
        print("\n--- Pipeline Status ---")

        # Tracker
        tracker = self.processes.get("tracker")
        if tracker and tracker.poll() is None:
            print(f"  [OK] Tracker: running (PID {tracker.pid})")
        else:
            print("  [FAIL] Tracker: not running")

        # OBS
        if self.obs_ctrl:
            try:
                st = self.obs_ctrl.get_stream_status()
                if st["active"]:
                    print(f"  [LIVE] Streaming: {st['duration']}ms, {st['bytes']} bytes")
                else:
                    print("  [OK] OBS: connected, not streaming")
            except Exception:
                print("  [FAIL] OBS: connection lost")
        else:
            print("  [--] OBS: not connected")

        print()

    def _shutdown(self):
        """Clean shutdown of all processes."""
        print("\n[*] Shutting down...")

        if self.obs_ctrl:
            try:
                st = self.obs_ctrl.get_stream_status()
                if st["active"]:
                    self.obs_ctrl.stop_streaming()
                self.obs_ctrl.disconnect()
            except Exception:
                pass

        for name, proc in self.processes.items():
            if proc.poll() is None:
                proc.terminate()
                print(f"  [{name}] terminated")

        print("[*] Pipeline stopped")

    def _check_webcam(self):
        try:
            import cv2
            cap = cv2.VideoCapture(0)
            ok = cap.isOpened()
            cap.release()
            return (ok, "Available" if ok else "Not found")
        except ImportError:
            return (False, "OpenCV not installed")

    def _check_port(self, port):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", port))
            s.close()
            return (True, "Available")
        except OSError:
            return (False, "In use")

    def _check_obs(self):
        try:
            from obs_controller import OBSController
            ctrl = OBSController()
            ctrl.disconnect()
            return (True, "Connected")
        except ConnectionRefusedError:
            return (False, "OBS not running or WebSocket disabled")
        except Exception as e:
            return (False, str(e))

    def _check_models(self):
        models_dir = os.path.join(self.base_dir, "models")
        required = ["face_landmarker.task", "pose_landmarker_heavy.task"]
        missing = [m for m in required if not os.path.exists(os.path.join(models_dir, m))]
        if not missing:
            return (True, "All models found")
        return (False, f"Missing: {', '.join(missing)}")


if __name__ == "__main__":
    PipelineLauncher().launch()
