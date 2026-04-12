"""OBS Studio auto-control via obsws-python (WebSocket v5)"""
import os
import obsws_python as obs


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
    def __init__(self, host="localhost", port=4455, password=None):
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

    def start_streaming(self):
        """Start streaming (CHZZK RTMP)."""
        status = self.client.get_stream_status()
        if status.output_active:
            print("[OBS] Already streaming")
            return
        self.client.start_stream()
        print("[OBS] Streaming started")

    def stop_streaming(self):
        """Stop streaming."""
        status = self.client.get_stream_status()
        if not status.output_active:
            print("[OBS] Not streaming")
            return
        self.client.stop_stream()
        print("[OBS] Streaming stopped")

    def get_stream_status(self) -> dict:
        """Get streaming status."""
        status = self.client.get_stream_status()
        return {
            "active": status.output_active,
            "duration": getattr(status, "output_duration", 0),
            "bytes": getattr(status, "output_bytes", 0),
        }

    def start_recording(self):
        """Start recording."""
        self.client.start_record()
        print("[OBS] Recording started")

    def stop_recording(self):
        """Stop recording."""
        self.client.stop_record()
        print("[OBS] Recording stopped")

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
