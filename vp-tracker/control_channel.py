"""Local-only camera and lifecycle control channel from Unreal."""

from __future__ import annotations

from dataclasses import dataclass
import json
import socket
import threading
from queue import Empty, Full, Queue

from lifecycle import LIFECYCLE_TYPE, LifecycleLease, parse_lifecycle_command


CONTROL_HOST = "127.0.0.1"
CONTROL_PORT = 7001
MAX_CONTROL_PACKET_BYTES = 8192


@dataclass(frozen=True)
class CameraCommand:
    action: str
    device_id: str = ""


@dataclass(frozen=True)
class QueuedCameraCommand:
    command: CameraCommand
    reply_address: tuple[str, int]


def parse_camera_command(data: bytes) -> CameraCommand:
    if not data or len(data) > MAX_CONTROL_PACKET_BYTES:
        raise ValueError("invalid packet length")
    try:
        payload = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid JSON command") from exc

    if not isinstance(payload, dict):
        raise ValueError("command must be a JSON object")
    if payload.get("version") != 1:
        raise ValueError("unsupported control version")
    if payload.get("type") != "camera_control":
        raise ValueError("unsupported control type")

    action = payload.get("action")
    if action not in {"list", "select"}:
        raise ValueError("invalid camera action")
    device_id = payload.get("device_id", "")
    if action == "select":
        if (
            not isinstance(device_id, str)
            or not 1 <= len(device_id) <= 64
            or not device_id.isascii()
            or not device_id.isalnum()
        ):
            raise ValueError("invalid camera device id")
    return CameraCommand(action=action, device_id=device_id)


class ControlListener:
    """Receives validated camera/lifecycle commands only from localhost."""

    def __init__(
        self,
        host: str = CONTROL_HOST,
        port: int = CONTROL_PORT,
        lifecycle_lease: LifecycleLease | None = None,
    ):
        self.host = host
        self.port = port
        self._socket: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._camera_command_queue: Queue[QueuedCameraCommand] = Queue(maxsize=4)
        self.lifecycle_lease = lifecycle_lease

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind((self.host, self.port))
        self._socket.settimeout(0.25)
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run,
            name="VPControlListener",
            daemon=True,
        )
        self._thread.start()
        print(f"[CTRL] Unreal control listening on {self.host}:{self.port}")

    def stop(self) -> None:
        self._stop_event.set()
        if self._socket:
            self._socket.close()
            self._socket = None
        if self._thread:
            self._thread.join(timeout=1.0)
            self._thread = None

    def wait_for_lifecycle_connection(self, timeout: float) -> bool:
        if self.lifecycle_lease is None:
            return True
        return self.lifecycle_lease.wait_until_connected(timeout)

    def lifecycle_should_shutdown(self) -> bool:
        return bool(self.lifecycle_lease and self.lifecycle_lease.should_shutdown())

    def lifecycle_shutdown_reason(self) -> str:
        return self.lifecycle_lease.shutdown_reason() if self.lifecycle_lease else ""

    def _run(self) -> None:
        assert self._socket is not None
        while not self._stop_event.is_set():
            try:
                data, address = self._socket.recvfrom(MAX_CONTROL_PACKET_BYTES + 1)
            except socket.timeout:
                continue
            except OSError:
                if not self._stop_event.is_set():
                    print("[CTRL] Control socket stopped unexpectedly")
                return

            if address[0] != CONTROL_HOST:
                continue
            try:
                payload = json.loads(data.decode("utf-8"))
                if isinstance(payload, dict) and payload.get("type") == LIFECYCLE_TYPE:
                    command = parse_lifecycle_command(data)
                    if self.lifecycle_lease:
                        self.lifecycle_lease.accept(command)
                    continue
                if not isinstance(payload, dict) or payload.get("type") != "camera_control":
                    raise ValueError("unsupported control type")
                self._enqueue_camera_command(parse_camera_command(data), address)
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
                print(f"[CTRL] Ignored invalid Unreal command: {exc}")

    def _enqueue_camera_command(
        self,
        command: CameraCommand,
        reply_address: tuple[str, int],
    ) -> None:
        queued = QueuedCameraCommand(command=command, reply_address=reply_address)
        if self._camera_command_queue.full():
            try:
                self._camera_command_queue.get_nowait()
            except Empty:
                pass
        try:
            self._camera_command_queue.put_nowait(queued)
        except Full:
            pass

    def take_camera_command(self) -> QueuedCameraCommand | None:
        try:
            return self._camera_command_queue.get_nowait()
        except Empty:
            return None

    def send_camera_result(
        self,
        reply_address: tuple[str, int],
        *,
        action: str,
        status: str,
        active_device_id: str,
        devices: list[dict[str, str | bool]],
        message: str = "",
    ) -> None:
        if self._socket is None:
            return
        payload = json.dumps(
            {
                "version": 1,
                "type": "camera_control_result",
                "action": action,
                "status": status,
                "active_device_id": active_device_id,
                "devices": devices,
                "message": message,
            },
            ensure_ascii=True,
            separators=(",", ":"),
        ).encode("utf-8")
        if len(payload) > MAX_CONTROL_PACKET_BYTES:
            print("[CAM] Camera list response exceeded the control packet limit")
            return
        try:
            self._socket.sendto(payload, reply_address)
        except OSError:
            pass
