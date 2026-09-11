"""Windowless entry point for the self-contained Windows distribution."""

from __future__ import annotations

import ctypes
from datetime import datetime
import os
from pathlib import Path
import sys
import traceback


APP_NAME = "Virtual Production Pipeline"


def _log_path() -> Path:
    configured = os.environ.get("VP_LOG_FILE", "").strip()
    if configured:
        return Path(configured)
    local_app_data = Path(
        os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))
    )
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return local_app_data / "VirtualProductionPipeline" / "Logs" / f"session-{timestamp}.log"


def _redirect_output(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = path.open("a", encoding="utf-8", buffering=1)
    sys.stdout = stream
    sys.stderr = stream
    os.environ["VP_LOG_FILE"] = str(path)


def _message_box(message: str, *, error: bool) -> None:
    if sys.platform != "win32":
        return
    icon = 0x10 if error else 0x40
    ctypes.windll.user32.MessageBoxW(None, message, APP_NAME, icon | 0x0)


def main() -> int:
    tracker_child = "--tracker-child" in sys.argv[1:]
    log_path = _log_path()
    _redirect_output(log_path)
    print(f"[{datetime.now().isoformat(timespec='seconds')}] {APP_NAME}")
    print(f"Log: {log_path}")

    try:
        if tracker_child:
            from sender import main as sender_main

            return sender_main()

        from supervisor import main as supervisor_main

        result = supervisor_main(packaged_mode=True, interactive_commands=False)
        if result == 2:
            _message_box(
                "실행 준비 검사를 통과하지 못했습니다.\n"
                "웹캠 연결과 배포 파일을 확인하세요.\n\n"
                f"로그: {log_path}",
                error=True,
            )
        elif result == 4:
            _message_box("Virtual Production Pipeline이 이미 실행 중입니다.", error=False)
        elif result != 0:
            _message_box(
                "응용프로그램이 비정상 종료되었습니다.\n\n"
                f"로그: {log_path}",
                error=True,
            )
        return result
    except Exception:
        traceback.print_exc()
        if not tracker_child:
            _message_box(
                "응용프로그램을 시작하지 못했습니다.\n\n"
                f"로그: {log_path}",
                error=True,
            )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
