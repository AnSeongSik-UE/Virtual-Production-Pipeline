"""
VP Pipeline - UDP/OSC 네트워크 발신
트래킹 데이터를 언리얼 엔진으로 전송
"""
import os
import socket
import sys
import time
from pythonosc import udp_client
from lifecycle import (
    DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
    DEFAULT_STARTUP_TIMEOUT_SECONDS,
    LifecycleLease,
)
from obs_control import OBSControlListener
from tracker import UnifiedTracker, TrackingFrame
from protocol import encode_packet


class OSCSender:
    """python-osc를 활용한 OSC 프로토콜 전송"""

    def __init__(self, host: str = "127.0.0.1", port: int = 7000):
        self.client = udp_client.SimpleUDPClient(host, port)

    def send_tracking_frame(self, frame: TrackingFrame):
        # 블렌드쉐이프 전송 (52개 float)
        for name, value in frame.blendshapes.items():
            self.client.send_message(f"/face/{name}", value)

        # 포즈 랜드마크 전송 (33 x 3 = 99 floats)
        for i, (x, y, z, visibility, presence) in enumerate(frame.pose_landmarks):
            self.client.send_message(f"/pose/{i}", [x, y, z])


class RawUDPSender:
    """고성능 바이너리 UDP 전송 (OSC 오버헤드 없음)"""

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
    tracker = UnifiedTracker(camera_id=0)
    sender = RawUDPSender(host="127.0.0.1", port=7000)
    obs_control = OBSControlListener(lifecycle_lease=lifecycle_lease)
    # OSC가 필요하면: sender = OSCSender(host="127.0.0.1", port=7000)

    try:
        obs_control.start()
        if lifecycle_lease:
            print(f"[*] Waiting up to {startup_timeout:.0f}s for Unreal session...")
            if not obs_control.wait_for_lifecycle_connection(startup_timeout):
                print("[FAIL] Unreal did not start before the lifecycle timeout")
                return 3
            if obs_control.lifecycle_should_shutdown():
                print(f"[*] {obs_control.lifecycle_shutdown_reason()}")
                return 0
        tracker.start()
        print(
            "[*] Tracking started. Sending to UE at 127.0.0.1:7000 "
            f"(Pose model: {tracker.pose_model_name})"
        )
        print("    (Ctrl+C to stop)")
        last_status_time = time.monotonic()
        while not obs_control.lifecycle_should_shutdown():
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
            print(f"\n[*] {obs_control.lifecycle_shutdown_reason()}")
    except KeyboardInterrupt:
        print("\n[*] Stopping...")
    finally:
        tracker.stop()
        sender.close()
        obs_control.stop()
        print(f"[OK] Total {sender.send_count} packets sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
