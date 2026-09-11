"""VP Pipeline 전용 MCP 서버 - AI 어시스턴트가 파이프라인 상태를 진단/제어"""
import socket
import os
from mcp.server.fastmcp import FastMCP
from protocol import (
    BLENDSHAPE_COUNT,
    EXPECTED_PACKET_SIZE,
    MAGIC,
    POSE_LANDMARK_COUNT,
    SCHEMA_VERSION,
)

mcp = FastMCP("VP Pipeline Tools")

@mcp.tool()
def check_udp_listener(port: int = 7000) -> dict:
    """UDP 포트 바인딩 여부를 확인한다. 프로세스 소유자는 판별할 수 없다."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", port))
        return {
            "status": "warning",
            "port": port,
            "bound": False,
            "message": "No UDP listener detected; start Unreal PIE before tracking",
        }
    except OSError:
        return {
            "status": "ok",
            "port": port,
            "bound": True,
            "message": "UDP port is bound; owner is not verifiable from this probe",
        }

@mcp.tool()
def check_webcam() -> dict:
    """노트북 웹캠 접근 가능 여부 확인"""
    cap = None
    try:
        import cv2
        cap = cv2.VideoCapture(0)
        if cap.isOpened():
            ret, frame = cap.read()
            h, w = frame.shape[:2] if ret else (0, 0)
            return {"status": "ok", "width": w, "height": h}
        return {"status": "error", "message": "Webcam not accessible"}
    except ImportError:
        return {"status": "error", "message": "OpenCV not installed"}
    except Exception as e:
        return {"status": "error", "message": f"Webcam error: {type(e).__name__}"}
    finally:
        if cap is not None:
            cap.release()

@mcp.tool()
def check_mediapipe_models() -> dict:
    """MediaPipe Task 모델 파일 존재 여부 확인"""
    from tracker import POSE_MODEL_FILES, selected_pose_model

    selected = selected_pose_model()
    models = {
        "face_landmarker.task": "FaceLandmarker",
        POSE_MODEL_FILES[selected]: f"PoseLandmarker ({selected})",
    }
    results = {}
    for filename, name in models.items():
        path = os.path.join(os.path.dirname(__file__), "models", filename)
        if os.path.exists(path):
            size_mb = os.path.getsize(path) / (1024*1024)
            results[name] = {"status": "ok", "path": path, "size_mb": round(size_mb, 1)}
        else:
            results[name] = {"status": "error", "path": path, "message": "not found"}
    return results

@mcp.tool()
def get_tracking_protocol() -> dict:
    """Python/Unreal이 공유하는 VPTP 스키마 계약 정보를 반환한다."""
    return {
        "magic": MAGIC.decode("ascii"),
        "name": "Virtual Production Tracking Packet",
        "schema_version": SCHEMA_VERSION,
        "packet_size_bytes": EXPECTED_PACKET_SIZE,
        "blendshape_count": BLENDSHAPE_COUNT,
        "pose_landmark_count": POSE_LANDMARK_COUNT,
        "pose_fields": ["x", "y", "z", "visibility", "presence"],
    }

@mcp.tool()
def run_pipeline_diagnostics() -> dict:
    """전체 파이프라인 사전 진단 (원클릭)"""
    return {
        "webcam": check_webcam(),
        "unreal_udp": check_udp_listener(7000),
        "models": check_mediapipe_models(),
        "protocol": get_tracking_protocol(),
    }

if __name__ == "__main__":
    mcp.run()
