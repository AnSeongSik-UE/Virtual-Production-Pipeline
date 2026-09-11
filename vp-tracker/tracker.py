"""VP Pipeline asynchronous latest-only webcam tracker."""

import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from queue import Empty, Queue
from typing import Optional

import cv2
import mediapipe as mp

@dataclass
class TrackingFrame:
    """하나의 프레임에서 추출된 전체 트래킹 데이터"""
    timestamp: float = 0.0
    blendshapes: dict[str, float] = field(default_factory=dict)      # ARKit 52 블렌드쉐이프
    face_rotation_matrix: tuple[float, ...] = field(default_factory=tuple)
    pose_landmarks: list[tuple[float, float, float, float, float]] = field(default_factory=list)
    face_tracked: bool = False
    pose_tracked: bool = False


@dataclass
class _CameraSwitchRequest:
    camera_id: int
    camera_backend: int
    completed: threading.Event = field(default_factory=threading.Event)
    cancelled: bool = False
    success: bool = False
    error: str = ""

BaseOptions = mp.tasks.BaseOptions
FaceLandmarker = mp.tasks.vision.FaceLandmarker
FaceLandmarkerOptions = mp.tasks.vision.FaceLandmarkerOptions
PoseLandmarker = mp.tasks.vision.PoseLandmarker
PoseLandmarkerOptions = mp.tasks.vision.PoseLandmarkerOptions
VisionRunningMode = mp.tasks.vision.RunningMode

POSE_MODEL_FILES = {
    "full": "pose_landmarker_full.task",
    "heavy": "pose_landmarker_heavy.task",
}


def selected_pose_model() -> str:
    """Return the validated runtime pose quality profile."""
    model = os.getenv("VP_POSE_MODEL", "full").strip().lower()
    if model not in POSE_MODEL_FILES:
        supported = ", ".join(sorted(POSE_MODEL_FILES))
        raise ValueError(f"VP_POSE_MODEL must be one of: {supported}")
    return model


class UnifiedTracker:
    """Single-camera Face + Pose tracker with independent latest-only inference."""

    def __init__(
        self,
        camera_id: int = 0,
        camera_backend: int = cv2.CAP_ANY,
        capture_width: int = 1280,
        capture_height: int = 720,
        pose_width: int = 640,
        pose_height: int = 360,
        result_stale_ms: int = 250,
    ):
        self.camera_id = camera_id
        self.camera_backend = camera_backend
        self.capture_width = capture_width
        self.capture_height = capture_height
        self.pose_width = pose_width
        self.pose_height = pose_height
        self.result_stale_ms = result_stale_ms

        models_dir = Path(__file__).resolve().parent / "models"
        self.pose_model_name = selected_pose_model()
        self.face_model_path = models_dir / "face_landmarker.task"
        self.pose_model_path = models_dir / POSE_MODEL_FILES[self.pose_model_name]

        self.data_queue: Queue[TrackingFrame] = Queue(maxsize=2)
        self.running = False
        self._thread: Optional[threading.Thread] = None
        self._error: Optional[BaseException] = None
        self._result_lock = threading.Lock()
        self._capture_lock = threading.Lock()
        self._capture: Optional[cv2.VideoCapture] = None
        self._camera_switch_lock = threading.Lock()
        self._pending_camera_switch: _CameraSwitchRequest | None = None
        self._stop_event = threading.Event()
        self._accept_results_after_timestamp = -1

        self._latest_face_timestamp = -1
        self._latest_face_blendshapes: dict[str, float] = {}
        self._latest_face_rotation_matrix: tuple[float, ...] = ()
        self._latest_face_tracked = False
        self._latest_pose_timestamp = -1
        self._latest_pose_landmarks: list[tuple[float, float, float, float, float]] = []
        self._latest_pose_tracked = False

        self._camera_fps = 0.0
        self._face_fps = 0.0
        self._pose_fps = 0.0
        self._camera_count = 0
        self._face_count = 0
        self._pose_count = 0
        self._metrics_started = time.monotonic()

    @property
    def fps(self) -> float:
        """Effective Face+Pose callback rate retained for existing callers."""
        rates = [rate for rate in (self._face_fps, self._pose_fps) if rate > 0.0]
        return min(rates) if rates else 0.0

    @property
    def camera_fps(self) -> float:
        return self._camera_fps

    @property
    def face_fps(self) -> float:
        return self._face_fps

    @property
    def pose_fps(self) -> float:
        return self._pose_fps

    def start(self):
        """Start webcam capture and asynchronous inference."""
        if self.running:
            return
        self._validate_models()
        self._error = None
        self._stop_event.clear()
        self.running = True
        self._thread = threading.Thread(target=self._tracking_loop, daemon=True)
        self._thread.start()

    def stop(self) -> bool:
        """Stop inference, unblock capture if needed, and confirm thread exit."""
        self.running = False
        self._stop_event.set()
        thread = self._thread
        if not thread:
            return True
        thread.join(timeout=1.0)
        if thread.is_alive():
            with self._capture_lock:
                capture = self._capture
            if capture is not None:
                capture.release()
            thread.join(timeout=4.0)
        stopped = not thread.is_alive()
        if stopped:
            self._thread = None
        else:
            print("[ERROR] Webcam tracking thread did not stop within 5 seconds")
        return stopped

    def raise_if_failed(self):
        """Propagate worker/callback failures to the sender process."""
        if self._error is not None:
            raise RuntimeError("tracking worker failed") from self._error

    def switch_camera(
        self,
        camera_id: int,
        camera_backend: int,
        timeout: float = 5.0,
    ) -> tuple[bool, str]:
        """Replace only the capture device while retaining MediaPipe instances."""
        if camera_id < 0 or camera_backend < 0:
            return False, "invalid camera selection"
        if not self.running or not self._thread or not self._thread.is_alive():
            return False, "tracker is not running"
        if self.camera_id == camera_id and self.camera_backend == camera_backend:
            return True, "unchanged"

        request = _CameraSwitchRequest(camera_id, camera_backend)
        with self._camera_switch_lock:
            if self._pending_camera_switch is not None:
                return False, "another camera change is in progress"
            self._pending_camera_switch = request

        if not request.completed.wait(timeout=max(0.1, timeout)):
            request.cancelled = True
            return False, "camera change timed out"
        return request.success, request.error

    def get_latest(self) -> Optional[TrackingFrame]:
        """Drain queued results and return only the newest frame."""
        latest = None
        while True:
            try:
                latest = self.data_queue.get_nowait()
            except Empty:
                return latest

    def _validate_models(self):
        missing = [
            str(path)
            for path in (self.face_model_path, self.pose_model_path)
            if not path.is_file()
        ]
        if missing:
            raise FileNotFoundError(
                "Missing MediaPipe model(s): "
                + ", ".join(missing)
                + ". Run tools/mediapipe/Install-MediaPipeModels.ps1."
            )

    def _open_camera_capture(
        self,
        camera_id: int,
        camera_backend: int,
    ) -> cv2.VideoCapture:
        capture = cv2.VideoCapture(camera_id, camera_backend)
        capture.set(cv2.CAP_PROP_FRAME_WIDTH, self.capture_width)
        capture.set(cv2.CAP_PROP_FRAME_HEIGHT, self.capture_height)
        capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        return capture

    def _take_camera_switch_request(self) -> _CameraSwitchRequest | None:
        with self._camera_switch_lock:
            request = self._pending_camera_switch
            self._pending_camera_switch = None
            return request

    def _replace_capture_if_requested(
        self,
        current_capture: cv2.VideoCapture,
        last_timestamp_ms: int,
    ) -> tuple[cv2.VideoCapture, object | None]:
        request = self._take_camera_switch_request()
        if request is None:
            return current_capture, None

        replacement = None
        try:
            replacement = self._open_camera_capture(
                request.camera_id,
                request.camera_backend,
            )
            if not replacement.isOpened():
                raise RuntimeError("selected camera could not be opened")
            ok, first_frame = replacement.read()
            if not ok:
                raise RuntimeError("selected camera did not return a frame")
            if request.cancelled or self._stop_event.is_set():
                raise RuntimeError("camera change was cancelled")

            with self._capture_lock:
                self._capture = replacement
            current_capture.release()
            self.camera_id = request.camera_id
            self.camera_backend = request.camera_backend
            self._clear_tracking_results(
                max(last_timestamp_ms + 1, int(time.perf_counter() * 1000))
            )
            request.success = True
            request.error = "changed"
            print(f"[CAM] Input changed: index={self.camera_id} backend={self.camera_backend}")
            return replacement, first_frame
        except Exception as exc:
            if replacement is not None:
                replacement.release()
            request.error = str(exc)
            return current_capture, None
        finally:
            request.completed.set()

    def _clear_tracking_results(self, accept_after_timestamp: int) -> None:
        with self._result_lock:
            self._latest_face_timestamp = -1
            self._latest_face_blendshapes = {}
            self._latest_face_rotation_matrix = ()
            self._latest_face_tracked = False
            self._latest_pose_timestamp = -1
            self._latest_pose_landmarks = []
            self._latest_pose_tracked = False
            self._accept_results_after_timestamp = accept_after_timestamp
        while True:
            try:
                self.data_queue.get_nowait()
            except Empty:
                break

    def _tracking_loop(self):
        cap = None
        try:
            cap = self._open_camera_capture(self.camera_id, self.camera_backend)
            with self._capture_lock:
                self._capture = cap
            if not cap.isOpened():
                raise RuntimeError("cannot open webcam")

            actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            print(
                f"[CAM] Webcam opened: {actual_w}x{actual_h}; "
                f"Pose={self.pose_model_name} {self.pose_width}x{self.pose_height}"
            )

            face_options = FaceLandmarkerOptions(
                base_options=BaseOptions(model_asset_path=str(self.face_model_path)),
                running_mode=VisionRunningMode.LIVE_STREAM,
                output_face_blendshapes=True,
                output_facial_transformation_matrixes=True,
                num_faces=1,
                result_callback=self._on_face_result,
            )
            pose_options = PoseLandmarkerOptions(
                base_options=BaseOptions(model_asset_path=str(self.pose_model_path)),
                running_mode=VisionRunningMode.LIVE_STREAM,
                num_poses=1,
                result_callback=self._on_pose_result,
            )

            with FaceLandmarker.create_from_options(face_options) as face_lm, \
                    PoseLandmarker.create_from_options(pose_options) as pose_lm:
                last_timestamp_ms = -1
                consecutive_read_failures = 0
                while self.running and not self._stop_event.is_set() and cap.isOpened():
                    cap, switched_frame = self._replace_capture_if_requested(
                        cap,
                        last_timestamp_ms,
                    )
                    if switched_frame is None:
                        ret, frame = cap.read()
                    else:
                        ret, frame = True, switched_frame
                    if not ret:
                        if self._stop_event.is_set():
                            break
                        consecutive_read_failures += 1
                        if consecutive_read_failures >= 30:
                            raise RuntimeError("webcam stopped returning frames")
                        time.sleep(0.01)
                        continue
                    consecutive_read_failures = 0

                    # QueryPerformanceCounter-backed on Windows. Unreal's
                    # FPlatformTime uses the same monotonic clock, so the
                    # existing millisecond packet timestamp can be compared
                    # without changing the VPTP schema.
                    timestamp_ms = _next_frame_timestamp_ms(
                        last_timestamp_ms,
                        time.perf_counter(),
                    )
                    last_timestamp_ms = timestamp_ms
                    rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                    pose_rgb = cv2.resize(
                        rgb_frame,
                        (self.pose_width, self.pose_height),
                        interpolation=cv2.INTER_AREA,
                    )
                    face_lm.detect_async(
                        mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame),
                        timestamp_ms,
                    )
                    pose_lm.detect_async(
                        mp.Image(image_format=mp.ImageFormat.SRGB, data=pose_rgb),
                        timestamp_ms,
                    )
                    with self._result_lock:
                        self._camera_count += 1
                        self._update_metrics_locked()
        except BaseException as error:
            self._error = error
            self.running = False
            self._stop_event.set()
            print(f"[ERROR] Tracking worker failed: {error}")
        finally:
            pending_request = self._take_camera_switch_request()
            if pending_request is not None:
                pending_request.error = "tracker stopped before camera change"
                pending_request.completed.set()
            with self._capture_lock:
                if self._capture is cap:
                    self._capture = None
            if cap is not None:
                cap.release()
            print("[CAM] Webcam released")

    def _on_face_result(self, result, _output_image, timestamp_ms: int):
        try:
            if timestamp_ms < self._accept_results_after_timestamp:
                return
            blendshapes = {}
            face_rotation_matrix = ()
            if result.face_blendshapes:
                blendshapes = {
                    item.category_name: item.score
                    for item in result.face_blendshapes[0]
                }
            if result.facial_transformation_matrixes:
                matrix = result.facial_transformation_matrixes[0]
                if getattr(matrix, "shape", None) == (4, 4):
                    face_rotation_matrix = tuple(
                        float(matrix[row, column])
                        for row in range(3)
                        for column in range(3)
                    )
            with self._result_lock:
                self._latest_face_timestamp = timestamp_ms
                self._latest_face_blendshapes = blendshapes
                self._latest_face_rotation_matrix = face_rotation_matrix
                self._latest_face_tracked = bool(blendshapes) and len(face_rotation_matrix) == 9
                self._face_count += 1
                self._update_metrics_locked()
        except BaseException as error:
            self._error = error
            self.running = False
            self._stop_event.set()

    def _on_pose_result(self, result, _output_image, timestamp_ms: int):
        try:
            if timestamp_ms < self._accept_results_after_timestamp:
                return
            pose_landmarks = []
            if result.pose_landmarks:
                pose_landmarks = [
                    (
                        landmark.x,
                        landmark.y,
                        landmark.z,
                        float(getattr(landmark, "visibility", 0.0) or 0.0),
                        float(getattr(landmark, "presence", 0.0) or 0.0),
                    )
                    for landmark in result.pose_landmarks[0]
                ]
            with self._result_lock:
                self._latest_pose_timestamp = timestamp_ms
                self._latest_pose_landmarks = pose_landmarks
                self._latest_pose_tracked = bool(pose_landmarks)
                self._pose_count += 1
                self._update_metrics_locked()
                self._publish_fused_locked(timestamp_ms)
        except BaseException as error:
            self._error = error
            self.running = False
            self._stop_event.set()

    def _publish_fused_locked(self, timestamp_ms: int):
        face_fresh = (
            self._latest_face_timestamp >= 0
            and timestamp_ms - self._latest_face_timestamp <= self.result_stale_ms
        )
        pose_fresh = (
            self._latest_pose_timestamp >= 0
            and timestamp_ms - self._latest_pose_timestamp <= self.result_stale_ms
        )
        tracking_frame = TrackingFrame(
            timestamp=float(timestamp_ms),
            blendshapes=dict(self._latest_face_blendshapes) if face_fresh else {},
            face_rotation_matrix=(
                self._latest_face_rotation_matrix
                if face_fresh and self._latest_face_tracked
                else ()
            ),
            pose_landmarks=list(self._latest_pose_landmarks) if pose_fresh else [],
            face_tracked=face_fresh and self._latest_face_tracked,
            pose_tracked=pose_fresh and self._latest_pose_tracked,
        )
        if self.data_queue.full():
            try:
                self.data_queue.get_nowait()
            except Empty:
                pass
        self.data_queue.put_nowait(tracking_frame)

    def _update_metrics_locked(self):
        now = time.monotonic()
        elapsed = now - self._metrics_started
        if elapsed < 1.0:
            return
        self._camera_fps = self._camera_count / elapsed
        self._face_fps = self._face_count / elapsed
        self._pose_fps = self._pose_count / elapsed
        self._camera_count = 0
        self._face_count = 0
        self._pose_count = 0
        self._metrics_started = now


def _next_frame_timestamp_ms(last_timestamp_ms: int, now_seconds: float) -> int:
    """Return a strictly increasing millisecond timestamp for MediaPipe/VPTP."""
    return max(int(now_seconds * 1000), last_timestamp_ms + 1)


def main():
    """Run the tracker without Unreal and print live diagnostics."""
    tracker = UnifiedTracker(camera_id=0)
    tracker.start()
    print("[*] Tracking started (Ctrl+C to stop)")

    try:
        while True:
            tracker.raise_if_failed()
            frame = tracker.get_latest()
            if frame:
                bs_count = len(frame.blendshapes)
                pose_count = len(frame.pose_landmarks)

                print(
                    f"\r[Cam:{tracker.camera_fps:.1f} "
                    f"Face:{tracker.face_fps:.1f} Pose:{tracker.pose_fps:.1f}] "
                    f"BS:{bs_count} Pose:{pose_count}",
                    end="", flush=True
                )
            time.sleep(1/60)
    except KeyboardInterrupt:
        print("\n[*] Stopping...")
    finally:
        tracker.stop()
        print("[OK] Tracking stopped")


if __name__ == "__main__":
    main()
