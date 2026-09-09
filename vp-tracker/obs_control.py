"""Local-only control channel from Unreal to OBS WebSocket."""

from __future__ import annotations

from dataclasses import dataclass
import json
import socket
import threading
from queue import Empty, Full, Queue
from typing import Callable

from lifecycle import (
    LIFECYCLE_TYPE,
    LifecycleLease,
    parse_lifecycle_command,
)
from obs_controller import OBSController, OBSVideoFPSResult, is_obs_websocket_available


CONTROL_HOST = "127.0.0.1"
CONTROL_PORT = 7001
MAX_CONTROL_PACKET_BYTES = 2048


@dataclass(frozen=True)
class BackgroundCommand:
    mode: str
    sender: str
    source: str
    filter_name: str


@dataclass(frozen=True)
class FPSCommand:
    fps: int


@dataclass(frozen=True)
class QueuedOBSCommand:
    command: BackgroundCommand | FPSCommand
    reply_address: tuple[str, int]


def _required_name(payload: dict, key: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not value.strip() or len(value) > 128:
        raise ValueError(f"invalid {key}")
    return value.strip()


def parse_background_command(data: bytes) -> BackgroundCommand:
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
    if payload.get("type") != "broadcast_background":
        raise ValueError("unsupported control type")

    mode = payload.get("mode")
    if mode not in {"background_removed", "solid_color"}:
        raise ValueError("invalid background mode")

    return BackgroundCommand(
        mode=mode,
        sender=_required_name(payload, "sender"),
        source=_required_name(payload, "source"),
        filter_name=_required_name(payload, "filter"),
    )


def parse_fps_command(data: bytes) -> FPSCommand:
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
    if payload.get("type") != "broadcast_fps":
        raise ValueError("unsupported control type")

    fps = payload.get("fps")
    if isinstance(fps, bool) or not isinstance(fps, int) or not 15 <= fps <= 144:
        raise ValueError("invalid fps")
    return FPSCommand(fps=fps)


def apply_background_command(
    controller: OBSController,
    command: BackgroundCommand,
) -> None:
    controller.sync_spout_output(
        source_name=command.source,
        sender_name=command.sender,
        filter_name=command.filter_name,
        mode=command.mode,
    )


def apply_fps_command(
    controller: OBSController,
    command: FPSCommand,
) -> OBSVideoFPSResult:
    return controller.sync_video_fps(command.fps)


class OBSControlListener:
    """Receives validated commands only from localhost on a worker thread."""

    def __init__(
        self,
        host: str = CONTROL_HOST,
        port: int = CONTROL_PORT,
        controller_factory: Callable[[], OBSController] = OBSController,
        lifecycle_lease: LifecycleLease | None = None,
    ):
        self.host = host
        self.port = port
        self.controller_factory = controller_factory
        self._socket: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._obs_thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._controller: OBSController | None = None
        self._command_queue: Queue[QueuedOBSCommand] = Queue(maxsize=8)
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
        self._obs_thread = threading.Thread(
            target=self._run_obs_commands,
            name="OBSControlWorker",
            daemon=True,
        )
        self._thread.start()
        self._obs_thread.start()
        print(f"[OBS] Unreal control listening on {self.host}:{self.port}")

    def stop(self) -> None:
        self._stop_event.set()
        if self._socket:
            self._socket.close()
            self._socket = None
        if self._thread:
            self._thread.join(timeout=1.0)
            self._thread = None
        if self._obs_thread:
            self._obs_thread.join(timeout=6.0)
            self._obs_thread = None
        if self._controller:
            try:
                self._controller.disconnect()
            except Exception:
                pass
            self._controller = None

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
                    print("[OBS] Control socket stopped unexpectedly")
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
                command_type = payload.get("type") if isinstance(payload, dict) else None
                if command_type == "broadcast_background":
                    command = parse_background_command(data)
                elif command_type == "broadcast_fps":
                    command = parse_fps_command(data)
                else:
                    raise ValueError("unsupported control type")
                self._enqueue_obs_command(command, address)
            except ValueError as exc:
                print(f"[OBS] Ignored invalid Unreal command: {exc}")

    def _enqueue_obs_command(
        self,
        command: BackgroundCommand | FPSCommand,
        reply_address: tuple[str, int],
    ) -> None:
        queued = QueuedOBSCommand(command=command, reply_address=reply_address)
        if self._command_queue.full():
            try:
                self._command_queue.get_nowait()
            except Empty:
                pass
        try:
            self._command_queue.put_nowait(queued)
        except Full:
            pass

    def _send_fps_result(
        self,
        reply_address: tuple[str, int],
        result: OBSVideoFPSResult,
    ) -> None:
        if self._socket is None:
            return
        payload = json.dumps(
            {
                "version": 1,
                "type": "broadcast_fps_result",
                "status": result.status,
                "requested_fps": result.requested_fps,
                "obs_fps": round(result.current_fps, 3),
            },
            ensure_ascii=True,
            separators=(",", ":"),
        ).encode("utf-8")
        try:
            self._socket.sendto(payload, reply_address)
        except OSError:
            pass

    def _run_obs_commands(self) -> None:
        while not self._stop_event.is_set():
            try:
                queued = self._command_queue.get(timeout=0.25)
            except Empty:
                continue
            command = queued.command
            try:
                if self._controller is None:
                    if not is_obs_websocket_available():
                        print("[OBS] Sync unavailable: OBS is not running")
                        if isinstance(command, FPSCommand):
                            self._send_fps_result(
                                queued.reply_address,
                                OBSVideoFPSResult("unavailable", command.fps, 0.0),
                            )
                        continue
                    self._controller = self.controller_factory()
                if isinstance(command, BackgroundCommand):
                    apply_background_command(self._controller, command)
                else:
                    result = apply_fps_command(self._controller, command)
                    self._send_fps_result(queued.reply_address, result)
            except Exception as exc:
                print(f"[OBS] Sync unavailable: {exc}")
                if isinstance(command, FPSCommand):
                    self._send_fps_result(
                        queued.reply_address,
                        OBSVideoFPSResult("failed", command.fps, 0.0),
                    )
                if self._controller:
                    try:
                        self._controller.disconnect()
                    except Exception:
                        pass
                self._controller = None
