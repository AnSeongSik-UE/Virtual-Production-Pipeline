"""VP Pipeline VPTP UDP sender."""
import os
import socket
import sys
import time
from camera_devices import (
    CameraDevice,
    choose_startup_camera,
    enumerate_camera_devices,
    find_camera,
    load_selected_camera_id,
    save_selected_camera_id,
)
from lifecycle import (
    DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
    DEFAULT_STARTUP_TIMEOUT_SECONDS,
    LifecycleLease,
)
from control_channel import ControlListener, QueuedCameraCommand
from tracker import UnifiedTracker, TrackingFrame
from protocol import encode_packet


class RawUDPSender:
    """VPTP 바이너리 UDP 전송."""

    def __init__(self, host: str = "127.0.0.1", port: int = 7000):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.target = (host, port)
        self._send_count = 0

    @property
    def send_count(self) -> int:
        return self._send_count

    def send_tracking_frame(self, frame: TrackingFrame):
        data = encode_packet(
            frame_id=self._send_count & 0xFFFFFFFF,
            timestamp=frame.timestamp,
            blendshapes=frame.blendshapes,
            face_rotation_matrix=frame.face_rotation_matrix,
            pose_landmarks=frame.pose_landmarks,
            face_tracked=frame.face_tracked,
            pose_tracked=frame.pose_tracked,
        )
        self.sock.sendto(data, self.target)
        self._send_count += 1

    def close(self):
        self.sock.close()


def _positive_timeout_from_env(name: str, default: float) -> float:
    raw = os.getenv(name, "").strip()
    if not raw:
        return default
    value = float(raw)
    if value <= 0.0:
        raise ValueError(f"{name} must be positive")
    return value


def _camera_payload(devices: list[CameraDevice]) -> list[dict[str, str | bool]]:
    return [device.to_control_payload() for device in devices]


def _handle_camera_command(
    listener: ControlListener,
    queued: QueuedCameraCommand,
    tracker: UnifiedTracker,
    active_device: CameraDevice,
    startup_notice: str,
) -> tuple[CameraDevice, str]:
    try:
        devices = enumerate_camera_devices()
    except Exception as exc:
        listener.send_camera_result(
            queued.reply_address,
            action=queued.command.action,
            status="failed",
            active_device_id=active_device.device_id,
            devices=[],
            message=f"카메라 목록을 읽지 못했습니다: {type(exc).__name__}",
        )
        return active_device, startup_notice

    status = "ok"
    message = startup_notice if queued.command.action == "list" else ""
    if queued.command.action == "select":
        selected = find_camera(devices, queued.command.device_id)
        if selected is None:
            status = "failed"
            message = "선택한 카메라가 연결되어 있지 않습니다. 목록을 새로고침하세요."
        elif selected.device_id == active_device.device_id:
            status = "unchanged"
            message = f"이미 {selected.display_name} 카메라를 사용 중입니다."
        else:
            success, detail = tracker.switch_camera(selected.index, selected.backend)
            if success:
                active_device = selected
                status = "applied"
                message = f"입력 카메라를 {selected.display_name}(으)로 변경했습니다."
                try:
                    save_selected_camera_id(selected.device_id)
                except OSError:
                    message += " 선택 저장에는 실패했습니다."
            else:
                status = "failed"
                message = (
                    f"{selected.display_name} 카메라를 열지 못했습니다. "
                    f"기존 카메라를 계속 사용합니다. ({detail})"
                )

    listener.send_camera_result(
        queued.reply_address,
        action=queued.command.action,
        status=status,
        active_device_id=active_device.device_id,
        devices=_camera_payload(devices),
        message=message,
    )
    return active_device, ""


def main() -> int:
    """트래킹 + 네트워크 발신 통합 실행"""
    session_token = os.getenv("VP_SESSION_TOKEN", "").strip()
    startup_timeout = _positive_timeout_from_env(
        "VP_LIFECYCLE_STARTUP_TIMEOUT",
        DEFAULT_STARTUP_TIMEOUT_SECONDS,
    )
    heartbeat_timeout = _positive_timeout_from_env(
        "VP_LIFECYCLE_HEARTBEAT_TIMEOUT",
        DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
    )
    lifecycle_lease = LifecycleLease(
        session_token,
        startup_timeout=startup_timeout,
        heartbeat_timeout=heartbeat_timeout,
    ) if session_token else None
    devices = enumerate_camera_devices()
    active_device, used_fallback = choose_startup_camera(
        devices,
        load_selected_camera_id(),
    )
    if active_device is None:
        raise RuntimeError("사용 가능한 카메라가 없습니다")
    startup_notice = ""
    if used_fallback:
        startup_notice = (
            "저장된 카메라를 찾지 못해 "
            f"{active_device.display_name}(으)로 전환했습니다."
        )
    try:
        save_selected_camera_id(active_device.device_id)
    except OSError:
        print("[CAM] Could not persist the selected camera")
    tracker = UnifiedTracker(
        camera_id=active_device.index,
        camera_backend=active_device.backend,
    )
    sender = RawUDPSender(host="127.0.0.1", port=7000)
    control_listener = ControlListener(lifecycle_lease=lifecycle_lease)
    try:
        control_listener.start()
        if lifecycle_lease:
            print(f"[*] Waiting up to {startup_timeout:.0f}s for Unreal session...")
            if not control_listener.wait_for_lifecycle_connection(startup_timeout):
                print("[FAIL] Unreal did not start before the lifecycle timeout")
                return 3
            if control_listener.lifecycle_should_shutdown():
                print(f"[*] {control_listener.lifecycle_shutdown_reason()}")
                return 0
        tracker.start()
        print(
            f"[*] Camera: {active_device.display_name}\n"
            "[*] Tracking started. Sending to UE at 127.0.0.1:7000 "
            f"(Pose model: {tracker.pose_model_name})"
        )
        print("    (Ctrl+C to stop)")
        last_status_time = time.monotonic()
        while not control_listener.lifecycle_should_shutdown():
            camera_command = control_listener.take_camera_command()
            if camera_command is not None:
                active_device, startup_notice = _handle_camera_command(
                    control_listener,
                    camera_command,
                    tracker,
                    active_device,
                    startup_notice,
                )
            tracker.raise_if_failed()
            frame = tracker.get_latest()
            if frame:
                sender.send_tracking_frame(frame)

                now = time.monotonic()
                if now - last_status_time >= 1.0:
                    bs_count = len(frame.blendshapes)
                    pose_count = len(frame.pose_landmarks)
                    print(
                        f"\r[Cam:{tracker.camera_fps:.1f} "
                        f"Face:{tracker.face_fps:.1f} Pose:{tracker.pose_fps:.1f}] "
                        f"Sent:{sender.send_count} "
                        f"BS:{bs_count} Pose:{pose_count}",
                        end="", flush=True
                    )
                    last_status_time = now
            time.sleep(1/60)  # ~60Hz
        if lifecycle_lease:
            print(f"\n[*] {control_listener.lifecycle_shutdown_reason()}")
    except KeyboardInterrupt:
        print("\n[*] Stopping...")
    finally:
        tracker.stop()
        sender.close()
        control_listener.stop()
        print(f"[OK] Total {sender.send_count} packets sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
