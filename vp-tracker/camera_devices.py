"""Windows camera enumeration and persistent input-device selection."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
from typing import Iterable

import cv2
from cv2_enumerate_cameras import enumerate_cameras


SETTINGS_DIRECTORY_NAME = "VirtualProductionPipeline"
SETTINGS_FILE_NAME = "camera.json"


@dataclass(frozen=True)
class CameraDevice:
    device_id: str
    name: str
    display_name: str
    index: int
    backend: int
    path: str
    vid: int | None
    pid: int | None
    is_virtual: bool

    def to_control_payload(self) -> dict[str, str | bool]:
        return {
            "id": self.device_id,
            "name": self.display_name,
            "is_virtual": self.is_virtual,
        }


def camera_settings_path() -> Path:
    local_app_data = os.getenv("LOCALAPPDATA", "").strip()
    if local_app_data:
        return Path(local_app_data) / SETTINGS_DIRECTORY_NAME / SETTINGS_FILE_NAME
    return Path.home() / "AppData" / "Local" / SETTINGS_DIRECTORY_NAME / SETTINGS_FILE_NAME


def _device_identity(
    *,
    backend: int,
    path: str,
    name: str,
    vid: int | None,
    pid: int | None,
    index: int,
) -> str:
    stable_part = path.strip().casefold()
    if not stable_part:
        stable_part = f"{name.strip().casefold()}|{vid}|{pid}|{index}"
    digest = hashlib.sha256(f"{backend}|{stable_part}".encode("utf-8")).hexdigest()
    return digest[:24]


def _is_virtual_camera(name: str) -> bool:
    normalized = name.casefold()
    return "virtual" in normalized or "가상" in normalized


def build_camera_devices(camera_infos: Iterable[object]) -> list[CameraDevice]:
    raw_devices: list[CameraDevice] = []
    for info in camera_infos:
        name = str(getattr(info, "name", "")).strip() or "이름 없는 카메라"
        index = int(getattr(info, "index"))
        backend = int(getattr(info, "backend"))
        path = str(getattr(info, "path", "") or "")
        vid = getattr(info, "vid", None)
        pid = getattr(info, "pid", None)
        raw_devices.append(CameraDevice(
            device_id=_device_identity(
                backend=backend,
                path=path,
                name=name,
                vid=vid,
                pid=pid,
                index=index,
            ),
            name=name,
            display_name=name,
            index=index,
            backend=backend,
            path=path,
            vid=vid,
            pid=pid,
            is_virtual=_is_virtual_camera(name),
        ))

    name_totals: dict[str, int] = {}
    for device in raw_devices:
        key = device.name.casefold()
        name_totals[key] = name_totals.get(key, 0) + 1

    name_positions: dict[str, int] = {}
    devices: list[CameraDevice] = []
    for device in raw_devices:
        key = device.name.casefold()
        name_positions[key] = name_positions.get(key, 0) + 1
        display_name = device.name
        if name_totals[key] > 1:
            display_name = f"{device.name} ({name_positions[key]})"
        devices.append(CameraDevice(**{
            **asdict(device),
            "display_name": display_name,
        }))
    return devices


def enumerate_camera_devices() -> list[CameraDevice]:
    return build_camera_devices(enumerate_cameras(cv2.CAP_DSHOW))


def find_camera(
    devices: Iterable[CameraDevice],
    device_id: str,
) -> CameraDevice | None:
    return next((device for device in devices if device.device_id == device_id), None)


def choose_startup_camera(
    devices: list[CameraDevice],
    saved_device_id: str,
) -> tuple[CameraDevice | None, bool]:
    if not devices:
        return None, bool(saved_device_id)
    if saved_device_id:
        saved = find_camera(devices, saved_device_id)
        if saved is not None:
            return saved, False
    preferred = next((device for device in devices if not device.is_virtual), devices[0])
    return preferred, bool(saved_device_id)


def load_selected_camera_id(path: Path | None = None) -> str:
    settings_path = path or camera_settings_path()
    try:
        payload = json.loads(settings_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, UnicodeDecodeError, json.JSONDecodeError):
        return ""
    if not isinstance(payload, dict):
        return ""
    device_id = payload.get("device_id")
    return device_id if isinstance(device_id, str) and len(device_id) <= 64 else ""


def save_selected_camera_id(device_id: str, path: Path | None = None) -> None:
    if not device_id or len(device_id) > 64:
        raise ValueError("invalid camera device id")
    settings_path = path or camera_settings_path()
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = settings_path.with_suffix(settings_path.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps({"version": 1, "device_id": device_id}, separators=(",", ":")),
        encoding="utf-8",
    )
    temporary_path.replace(settings_path)
