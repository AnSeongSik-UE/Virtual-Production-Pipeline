"""Managed launcher for the Unreal application and Python tracker."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from queue import Empty, Queue
import secrets
import socket
import subprocess
import sys
import threading
import time

from lifecycle import (
    WindowsJob,
    WindowsSingleInstance,
    request_windows_close,
    send_lifecycle_command,
    stop_process,
)


DEFAULT_UNREAL_EDITOR = Path(
    r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
)
INSTANCE_MUTEX_NAME = r"Local\VirtualProductionPipeline.Supervisor"
WINDOWS_STATUS_CONTROL_C_EXIT = 0xC000013A
STREAM_CONFIRM_TIMEOUT_SECONDS = 10.0
RUNTIME_DISABLED_PLUGINS = (
    "ModelContextProtocol",
    "EditorToolset",
    "AnimationAssistantToolset",
    "AutomationTestToolset",
    "ToolsetRegistry",
)


def _is_normal_unreal_exit(return_code: int) -> bool:
    """Treat an ordinary close-window status as a successful user shutdown."""
    return return_code == 0 or return_code & 0xFFFFFFFF == WINDOWS_STATUS_CONTROL_C_EXIT


class PipelineSupervisor:
    def __init__(
        self,
        *,
        unreal_executable: Path | None = None,
        project_path: Path | None = None,
        map_path: str = "/Game/Maps/Lvl_Empty",
    ):
        self.base_dir = Path(__file__).resolve().parent
        self.project_path = (
            project_path
            or self.base_dir.parent / "VPPipeline" / "VPPipeline.uproject"
        ).resolve()
        configured_editor = os.getenv("VP_UNREAL_EDITOR", "").strip()
        self.unreal_executable = (
            unreal_executable
            or (Path(configured_editor) if configured_editor else DEFAULT_UNREAL_EDITOR)
        ).resolve()
        self.map_path = map_path
        self.processes: dict[str, subprocess.Popen] = {}
        self.obs_ctrl = None
        self.session_token = secrets.token_hex(16)
        self.job = WindowsJob()
        self._command_queue: Queue[str | None] = Queue()
        self._shutdown_started = False
        self._stream_confirmation_deadline: float | None = None

    def preflight_check(self) -> bool:
        """Check resources without retaining the webcam or OBS connection."""
        print("[Preflight]")
        checks = [
            ("Webcam", self._check_webcam(), True),
            ("MediaPipe models", self._check_models(), True),
            ("Unreal Editor", self._check_file(self.unreal_executable), True),
            ("Unreal project", self._check_file(self.project_path), True),
            ("Tracking ports", self._check_pipeline_ports(), True),
            ("OBS WebSocket", self._check_obs(), False),
        ]

        all_ok = True
        for name, (ok, message), required in checks:
            status = "[OK]" if ok else ("[FAIL]" if required else "[WARN]")
            print(f"  {status} {name}: {message}")
            if required and not ok:
                all_ok = False
        return all_ok

    def launch(self) -> int:
        """Run and supervise one complete pipeline session."""
        print("=" * 58)
        print("  Virtual Production Pipeline Supervisor")
        print("=" * 58)
        try:
            if not self.preflight_check():
                print("\n[FAIL] Fix the required checks and retry.")
                return 2

            self.job.open()
            self._launch_unreal()
            self._launch_tracker()
            self._connect_obs()
            self._start_command_reader()
            self._print_ready()
            return self._monitor_loop()
        except KeyboardInterrupt:
            print("\n[*] Shutdown requested")
            return 0
        except Exception as exc:
            print(f"\n[FAIL] Supervisor error: {type(exc).__name__}: {exc}")
            return 1
        finally:
            self._shutdown()

    def _launch_unreal(self) -> None:
        command = self._build_unreal_command()
        print("\n[UE] Launching managed Unreal session...")
        process = subprocess.Popen(
            command,
            cwd=self.project_path.parent,
            creationflags=_new_process_group_flags(),
        )
        self.processes["unreal"] = process
        self.job.assign(process)
        print(f"[UE] Unreal running (PID: {process.pid})")

    def _build_unreal_command(self) -> list[str]:
        return [
            str(self.unreal_executable),
            str(self.project_path),
            self.map_path,
            "-game",
            "-log",
            "-windowed",
            "-ResX=1280",
            "-ResY=720",
            f"-DisablePlugins={','.join(RUNTIME_DISABLED_PLUGINS)}",
            f"-VPSessionToken={self.session_token}",
        ]

    def _launch_tracker(self) -> None:
        environment = os.environ.copy()
        environment["VP_SESSION_TOKEN"] = self.session_token
        print("\n[TRACK] Launching managed tracker...")
        process = subprocess.Popen(
            [sys.executable, "sender.py"],
            cwd=self.base_dir,
            env=environment,
            creationflags=_new_process_group_flags(),
        )
        self.processes["tracker"] = process
        self.job.assign(process)
        print(f"[TRACK] Tracker waiting for Unreal heartbeat (PID: {process.pid})")

    def _connect_obs(self) -> None:
        print("\n[OBS] Connecting...")
        try:
            from obs_controller import OBSController, is_obs_websocket_available

            if not is_obs_websocket_available():
                print("[OBS] Connection unavailable: OBS is not running")
                print("[OBS] Continuing without OBS control")
                return
            self.obs_ctrl = OBSController()
            print(f"[OBS] Current scene: {self.obs_ctrl.get_current_scene()}")
        except Exception as exc:
            print(f"[OBS] Connection unavailable: {exc}")
            print("[OBS] Continuing without OBS control")
            self.obs_ctrl = None

    def _start_command_reader(self) -> None:
        def read_commands() -> None:
            try:
                while True:
                    line = sys.stdin.readline()
                    if not line:
                        self._command_queue.put(None)
                        return
                    self._command_queue.put(line.strip().lower())
            except Exception:
                self._command_queue.put(None)

        threading.Thread(
            target=read_commands,
            name="VPLauncherInput",
            daemon=True,
        ).start()

    def _monitor_loop(self) -> int:
        while True:
            unreal = self.processes.get("unreal")
            tracker = self.processes.get("tracker")
            if unreal and unreal.poll() is not None:
                print(f"\n[UE] Unreal exited (code={unreal.returncode})")
                return 0 if _is_normal_unreal_exit(unreal.returncode) else 1
            if tracker and tracker.poll() is not None:
                if tracker.returncode == 0 and unreal:
                    try:
                        unreal.wait(timeout=5.0)
                        print(f"\n[UE] Unreal exited (code={unreal.returncode})")
                        return 0 if _is_normal_unreal_exit(unreal.returncode) else 1
                    except subprocess.TimeoutExpired:
                        pass
                print(f"\n[FAIL] Tracker exited while Unreal remained active "
                      f"(code={tracker.returncode})")
                return 1

            try:
                command = self._command_queue.get(timeout=0.25)
            except Empty:
                continue
            if command is None or command in {"quit", "q", "exit"}:
                return 0
            if not command:
                continue
            try:
                self._handle_command(command)
            except Exception as exc:
                print(f"[WARN] Command failed: {type(exc).__name__}: {exc}")

    def _handle_command(self, command: str) -> None:
        if command == "stream":
            if not self.obs_ctrl:
                print("[OBS] Not connected")
                return
            status = self.obs_ctrl.get_output_status()
            if status.stream_active:
                self._stream_confirmation_deadline = None
                print("[OBS] Streaming is already active")
                self._print_obs_status(status)
                return
            self._stream_confirmation_deadline = (
                time.monotonic() + STREAM_CONFIRM_TIMEOUT_SECONDS
            )
            print(
                "[CONFIRM] Live streaming is armed. Type 'stream confirm' "
                f"within {STREAM_CONFIRM_TIMEOUT_SECONDS:.0f} seconds to start."
            )
        elif command == "stream confirm":
            if not self.obs_ctrl:
                print("[OBS] Not connected")
                return
            deadline = self._stream_confirmation_deadline
            self._stream_confirmation_deadline = None
            if deadline is None:
                print("[OBS] Streaming was not armed. Type 'stream' first.")
                return
            if time.monotonic() > deadline:
                print("[OBS] Streaming confirmation expired. Type 'stream' again.")
                return
            self.obs_ctrl.start_streaming()
            self._print_obs_status()
        elif command == "stop":
            self._stream_confirmation_deadline = None
            if self.obs_ctrl:
                self.obs_ctrl.stop_streaming()
                self._print_obs_status()
            else:
                print("[OBS] Not connected")
        elif command == "rec":
            if self.obs_ctrl:
                self.obs_ctrl.start_recording()
                self._print_obs_status()
            else:
                print("[OBS] Not connected")
        elif command == "stoprec":
            if self.obs_ctrl:
                self.obs_ctrl.stop_recording()
                self._print_obs_status()
            else:
                print("[OBS] Not connected")
        elif command == "status":
            self._print_status()
        else:
            print(
                "Unknown command. Try: stream, stream confirm, stop, rec, "
                "stoprec, status, quit"
            )

    def _print_ready(self) -> None:
        print("\n" + "=" * 58)
        print("  Pipeline managed session started")
        print("=" * 58)
        print("  Closing Unreal also stops tracking and releases the webcam.")
        print("  Commands: stream, stream confirm, stop, rec, stoprec, status, quit")
        print("  Live streaming requires 'stream' followed by confirmation within 10s.")
        print()

    def _print_obs_status(self, status=None) -> None:
        if not self.obs_ctrl:
            print("  [--] OBS: not connected")
            return
        status = status or self.obs_ctrl.get_output_status()
        print("  [OK] OBS: connected")
        if status.stream_active:
            print(
                "  [LIVE] Streaming: "
                f"{_format_duration(status.stream_duration_ms)}, "
                f"{_format_bytes(status.stream_bytes)}"
            )
        else:
            print("  [--] Streaming: inactive")
        if status.record_active:
            paused = " (paused)" if status.record_paused else ""
            print(
                f"  [REC] Recording{paused}: "
                f"{_format_duration(status.record_duration_ms)}, "
                f"{_format_bytes(status.record_bytes)}"
            )
            if status.record_path:
                print(f"        Path: {status.record_path}")
        else:
            print("  [--] Recording: inactive")

    def _print_status(self) -> None:
        print("\n--- Pipeline Status ---")
        for name in ("unreal", "tracker"):
            process = self.processes.get(name)
            if process and process.poll() is None:
                print(f"  [OK] {name}: running (PID {process.pid})")
            else:
                code = process.returncode if process else "not started"
                print(f"  [FAIL] {name}: not running (exit={code})")

        if self.obs_ctrl:
            try:
                self._print_obs_status()
            except Exception:
                print("  [FAIL] OBS: connection lost")
        else:
            print("  [--] OBS: not connected")
        print()

    def _shutdown(self) -> None:
        if self._shutdown_started:
            return
        self._shutdown_started = True
        print("\n[*] Shutting down managed session...")

        try:
            tracker = self.processes.get("tracker")
            if tracker:
                try:
                    result = stop_process(
                        tracker,
                        "tracker",
                        graceful_request=lambda: send_lifecycle_command(
                            self.session_token,
                            "shutdown",
                        ),
                        graceful_timeout=5.0,
                    )
                    print(f"  [tracker] {result}")
                except Exception as exc:
                    print(f"  [tracker] cleanup error: {exc}")

            unreal = self.processes.get("unreal")
            if unreal:
                try:
                    result = stop_process(
                        unreal,
                        "unreal",
                        graceful_request=lambda: request_windows_close(unreal.pid),
                        graceful_timeout=12.0,
                        terminate_timeout=3.0,
                    )
                    print(f"  [unreal] {result}")
                except Exception as exc:
                    print(f"  [unreal] cleanup error: {exc}")

            if self.obs_ctrl:
                try:
                    status = self.obs_ctrl.get_output_status()
                    if status.stream_active:
                        print("  [OBS] Streaming remains active; OBS is externally owned")
                    if status.record_active:
                        print("  [OBS] Recording remains active; OBS is externally owned")
                except Exception:
                    pass
                try:
                    self.obs_ctrl.disconnect()
                except Exception:
                    pass
                self.obs_ctrl = None
        finally:
            self.job.close()
        print("[*] Pipeline stopped; managed webcam ownership released")

    @staticmethod
    def _check_file(path: Path) -> tuple[bool, str]:
        return (path.is_file(), str(path))

    def _check_webcam(self) -> tuple[bool, str]:
        capture = None
        try:
            import cv2

            capture = cv2.VideoCapture(0)
            ok = capture.isOpened()
            return (ok, "Available" if ok else "Not found or already in use")
        except ImportError:
            return (False, "OpenCV not installed")
        except Exception as exc:
            return (False, f"Webcam check failed: {type(exc).__name__}")
        finally:
            if capture is not None:
                capture.release()

    def _check_pipeline_ports(self) -> tuple[bool, str]:
        occupied = []
        for port in (7000, 7001):
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.bind(("127.0.0.1", port))
            except OSError:
                occupied.append(str(port))
        if occupied:
            return (False, f"Already in use: {', '.join(occupied)}")
        return (True, "7000 and 7001 available")

    def _check_obs(self) -> tuple[bool, str]:
        try:
            from obs_controller import OBSController, is_obs_websocket_available

            if not is_obs_websocket_available():
                return (False, "OBS not running or WebSocket disabled")
            controller = OBSController()
            controller.disconnect()
            return (True, "Connected")
        except ConnectionRefusedError:
            return (False, "OBS not running or WebSocket disabled")
        except Exception as exc:
            return (False, str(exc))

    def _check_models(self) -> tuple[bool, str]:
        from tracker import POSE_MODEL_FILES, selected_pose_model

        pose_model = selected_pose_model()
        models_dir = self.base_dir / "models"
        required = ["face_landmarker.task", POSE_MODEL_FILES[pose_model]]
        missing = [name for name in required if not (models_dir / name).is_file()]
        if not missing:
            return (True, f"All models found (Pose={pose_model})")
        return (False, f"Missing: {', '.join(missing)}")


def _format_duration(milliseconds: int) -> str:
    total_seconds = max(0, int(milliseconds)) // 1000
    hours, remainder = divmod(total_seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def _format_bytes(byte_count: int) -> str:
    return f"{max(0, int(byte_count)) / (1024 * 1024):.1f} MiB"


def _new_process_group_flags() -> int:
    return subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unreal-exe", type=Path, help="UnrealEditor executable")
    parser.add_argument("--project", type=Path, help="VPPipeline.uproject path")
    parser.add_argument("--map", default="/Game/Maps/Lvl_Empty", help="Startup map")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    instance = WindowsSingleInstance(INSTANCE_MUTEX_NAME)
    if not instance.acquire():
        print("[FAIL] Virtual Production Pipeline is already running")
        return 4
    try:
        return PipelineSupervisor(
            unreal_executable=arguments.unreal_exe,
            project_path=arguments.project,
            map_path=arguments.map,
        ).launch()
    finally:
        instance.close()


if __name__ == "__main__":
    sys.exit(main())
