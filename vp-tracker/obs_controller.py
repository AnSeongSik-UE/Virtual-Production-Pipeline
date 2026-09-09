"""OBS Studio auto-control via obsws-python (WebSocket v5)"""
from dataclasses import dataclass
import os
import socket
import time
import obsws_python as obs
from obsws_python.error import OBSSDKRequestError


OBS_DEFAULT_HOST = "127.0.0.1"
OBS_DEFAULT_PORT = 4455


@dataclass(frozen=True)
class OBSVideoFPSResult:
    status: str
    requested_fps: int
    current_fps: float


@dataclass(frozen=True)
class OBSOutputStatus:
    stream_active: bool
    stream_duration_ms: int
    stream_bytes: int
    record_active: bool
    record_paused: bool
    record_duration_ms: int
    record_bytes: int
    record_path: str


@dataclass(frozen=True)
class OBSOutputActionResult:
    status: str
    output_path: str = ""


def is_obs_websocket_available(
    host: str = OBS_DEFAULT_HOST,
    port: int = OBS_DEFAULT_PORT,
    timeout: float = 0.25,
) -> bool:
    """Probe OBS without triggering the WebSocket client's noisy traceback."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _load_password() -> str:
    """Load OBS password from .env file or environment variable."""
    pw = os.environ.get("VP_OBS_PASSWORD", "")
    if not pw:
        env_path = os.path.join(os.path.dirname(__file__), ".env")
        if os.path.exists(env_path):
            with open(env_path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("VP_OBS_PASSWORD="):
                        pw = line.split("=", 1)[1]
                        break
    return pw


class OBSController:
    def __init__(self, host=OBS_DEFAULT_HOST, port=OBS_DEFAULT_PORT, password=None):
        if password is None:
            password = _load_password()
        self.client = obs.ReqClient(host=host, port=port, password=password, timeout=5)
        v = self.client.get_version()
        print(f"[OBS] Connected - OBS v{v.obs_version}, WebSocket v{v.obs_web_socket_version}")

    def get_scenes(self) -> list:
        """Get list of scene names."""
        scenes = self.client.get_scene_list()
        return [s["sceneName"] for s in scenes.scenes]

    def get_current_scene(self) -> str:
        """Get current program scene name."""
        scenes = self.client.get_scene_list()
        return scenes.current_program_scene_name

    def switch_scene(self, scene_name: str):
        """Switch to a scene by name."""
        self.client.set_current_program_scene(scene_name)
        print(f"[OBS] Switched to scene: {scene_name}")

    def get_sources(self) -> list:
        """Get list of input source names."""
        sources = self.client.get_input_list()
        return [s["inputName"] for s in sources.inputs]

    def sync_spout_output(
        self,
        *,
        source_name: str,
        sender_name: str,
        filter_name: str,
        mode: str,
    ) -> None:
        """Select the Spout sender, alpha mode, and remove the legacy filter."""
        composite_mode = 4 if mode == "background_removed" else 1
        self.client.set_input_settings(
            source_name,
            {
                "spoutsenders": sender_name,
                "compositemode": composite_mode,
            },
            True,
        )

        filters = self.client.get_source_filter_list(source_name).filters
        filter_exists = any(
            item.get("filterName") == filter_name for item in filters
        )

        if filter_exists:
            self.client.remove_source_filter(source_name, filter_name)

        mode_label = (
            "premultiplied alpha"
            if mode == "background_removed"
            else "opaque solid background"
        )
        print(f"[OBS] Spout '{sender_name}', {mode_label} synchronized")

    def sync_video_fps(self, requested_fps: int) -> OBSVideoFPSResult:
        """Set OBS' global video FPS only while every known output is idle."""
        if isinstance(requested_fps, bool) or not 15 <= requested_fps <= 144:
            raise ValueError("requested_fps must be an integer from 15 through 144")

        settings = self.client.get_video_settings()
        current_fps = settings.fps_numerator / settings.fps_denominator
        if abs(current_fps - requested_fps) < 0.001:
            return OBSVideoFPSResult("unchanged", requested_fps, current_fps)

        status_getters = (
            self.client.get_stream_status,
            self.client.get_record_status,
            self.client.get_virtual_cam_status,
            self.client.get_replay_buffer_status,
        )
        for get_status in status_getters:
            try:
                if get_status().output_active:
                    return OBSVideoFPSResult("output_active", requested_fps, current_fps)
            except OBSSDKRequestError:
                # Some optional outputs may not exist in a given OBS profile.
                continue

        try:
            self.client.send(
                "SetVideoSettings",
                {
                    "fpsNumerator": requested_fps,
                    "fpsDenominator": 1,
                },
            )
        except OBSSDKRequestError as exc:
            # obs-websocket uses OutputRunning (500) when any video output is active.
            if exc.code == 500:
                return OBSVideoFPSResult("output_active", requested_fps, current_fps)
            raise

        return OBSVideoFPSResult("applied", requested_fps, float(requested_fps))

    def start_streaming(self) -> OBSOutputActionResult:
        """Start the configured OBS stream without restarting an active output."""
        status = self.client.get_stream_status()
        if status.output_active:
            print("[OBS] Already streaming")
            return OBSOutputActionResult("already_active")
        self.client.start_stream()
        self._wait_for_output_state(self.client.get_stream_status, True)
        print("[OBS] Streaming started")
        return OBSOutputActionResult("started")

    def stop_streaming(self) -> OBSOutputActionResult:
        """Stop the configured OBS stream when it is active."""
        status = self.client.get_stream_status()
        if not status.output_active:
            print("[OBS] Not streaming")
            return OBSOutputActionResult("already_inactive")
        self.client.stop_stream()
        self._wait_for_output_state(self.client.get_stream_status, False)
        print("[OBS] Streaming stopped")
        return OBSOutputActionResult("stopped")

    def get_output_status(self) -> OBSOutputStatus:
        """Return stream and recording status as one operational snapshot."""
        stream = self.client.get_stream_status()
        record = self.client.get_record_status()
        return OBSOutputStatus(
            stream_active=bool(stream.output_active),
            stream_duration_ms=int(getattr(stream, "output_duration", 0)),
            stream_bytes=int(getattr(stream, "output_bytes", 0)),
            record_active=bool(record.output_active),
            record_paused=bool(getattr(record, "output_paused", False)),
            record_duration_ms=int(getattr(record, "output_duration", 0)),
            record_bytes=int(getattr(record, "output_bytes", 0)),
            record_path=str(getattr(record, "output_path", "") or ""),
        )

    def get_stream_status(self) -> dict:
        """Get streaming status."""
        status = self.client.get_stream_status()
        return {
            "active": status.output_active,
            "duration": getattr(status, "output_duration", 0),
            "bytes": getattr(status, "output_bytes", 0),
        }

    def start_recording(self) -> OBSOutputActionResult:
        """Start local recording without restarting an active recording."""
        status = self.client.get_record_status()
        if status.output_active:
            print("[OBS] Already recording")
            return OBSOutputActionResult("already_active")
        self.client.start_record()
        self._wait_for_output_state(self.client.get_record_status, True)
        print("[OBS] Recording started")
        return OBSOutputActionResult("started")

    def stop_recording(self) -> OBSOutputActionResult:
        """Stop local recording and return the saved output path when available."""
        status = self.client.get_record_status()
        if not status.output_active:
            print("[OBS] Not recording")
            return OBSOutputActionResult("already_inactive")
        response = self.client.stop_record()
        self._wait_for_output_state(self.client.get_record_status, False)
        output_path = str(getattr(response, "output_path", "") or "")
        print("[OBS] Recording stopped")
        if output_path:
            print(f"[OBS] Recording saved: {output_path}")
        return OBSOutputActionResult("stopped", output_path)

    @staticmethod
    def _wait_for_output_state(get_status, expected_active: bool) -> bool:
        """Briefly wait for OBS' asynchronous output state to catch up."""
        deadline = time.monotonic() + 1.0
        while True:
            if bool(get_status().output_active) == expected_active:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.05)

    def disconnect(self):
        """Disconnect from OBS."""
        self.client.disconnect()
        print("[OBS] Disconnected")

if __name__ == "__main__":
    ctrl = OBSController()
    print(f"Scenes: {ctrl.get_scenes()}")
    print(f"Current: {ctrl.get_current_scene()}")
    print(f"Sources: {ctrl.get_sources()}")
    print(f"Stream status: {ctrl.get_stream_status()}")
    ctrl.disconnect()
