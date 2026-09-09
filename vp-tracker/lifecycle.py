"""Managed pipeline lifecycle protocol and Windows process ownership helpers."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import hmac
import json
import os
import socket
import subprocess
import sys
import threading
import time
from typing import Callable


LIFECYCLE_TYPE = "pipeline_lifecycle"
LIFECYCLE_EVENTS = frozenset({"hello", "heartbeat", "shutdown"})
DEFAULT_STARTUP_TIMEOUT_SECONDS = 45.0
DEFAULT_HEARTBEAT_TIMEOUT_SECONDS = 10.0


@dataclass(frozen=True)
class LifecycleCommand:
    event: str
    token: str


def encode_lifecycle_command(event: str, token: str) -> bytes:
    """Encode one authenticated localhost lifecycle command."""
    if event not in LIFECYCLE_EVENTS:
        raise ValueError("invalid lifecycle event")
    if not _valid_token(token):
        raise ValueError("invalid lifecycle token")
    return json.dumps(
        {
            "version": 1,
            "type": LIFECYCLE_TYPE,
            "event": event,
            "token": token,
        },
        separators=(",", ":"),
    ).encode("utf-8")


def parse_lifecycle_command(data: bytes) -> LifecycleCommand:
    """Validate and decode one lifecycle command."""
    try:
        payload = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid lifecycle JSON") from exc
    if not isinstance(payload, dict):
        raise ValueError("lifecycle command must be an object")
    if payload.get("version") != 1 or payload.get("type") != LIFECYCLE_TYPE:
        raise ValueError("unsupported lifecycle command")
    event = payload.get("event")
    token = payload.get("token")
    if event not in LIFECYCLE_EVENTS:
        raise ValueError("invalid lifecycle event")
    if not isinstance(token, str) or not _valid_token(token):
        raise ValueError("invalid lifecycle token")
    return LifecycleCommand(event=event, token=token)


def _valid_token(token: str) -> bool:
    return len(token) == 32 and all(character in "0123456789abcdef" for character in token)


class LifecycleLease:
    """Tracks Unreal liveness for one launcher-created session."""

    def __init__(
        self,
        token: str,
        *,
        startup_timeout: float = DEFAULT_STARTUP_TIMEOUT_SECONDS,
        heartbeat_timeout: float = DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
        clock: Callable[[], float] = time.monotonic,
    ):
        if not _valid_token(token):
            raise ValueError("invalid lifecycle token")
        if startup_timeout <= 0.0 or heartbeat_timeout <= 0.0:
            raise ValueError("lifecycle timeouts must be positive")
        self.token = token
        self.startup_timeout = startup_timeout
        self.heartbeat_timeout = heartbeat_timeout
        self._clock = clock
        self._started_at = clock()
        self._last_heartbeat: float | None = None
        self._connected = threading.Event()
        self._shutdown = threading.Event()
        self._lock = threading.Lock()

    def accept(self, command: LifecycleCommand) -> bool:
        """Accept a matching command; silently reject another session's token."""
        if not hmac.compare_digest(command.token, self.token):
            return False
        now = self._clock()
        with self._lock:
            if command.event in {"hello", "heartbeat"}:
                self._last_heartbeat = now
                self._connected.set()
            elif command.event == "shutdown":
                self._shutdown.set()
                self._connected.set()
        return True

    def wait_until_connected(self, timeout: float | None = None) -> bool:
        return self._connected.wait(timeout)

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    def should_shutdown(self) -> bool:
        if self._shutdown.is_set():
            return True
        now = self._clock()
        with self._lock:
            if self._last_heartbeat is None:
                return now - self._started_at >= self.startup_timeout
            return now - self._last_heartbeat >= self.heartbeat_timeout

    def shutdown_reason(self) -> str:
        if self._shutdown.is_set():
            return "Unreal requested shutdown"
        return "Unreal heartbeat timed out"


def send_lifecycle_command(
    token: str,
    event: str,
    *,
    host: str = "127.0.0.1",
    port: int = 7001,
    repeats: int = 3,
) -> None:
    """Best-effort local UDP notification used by the supervisor fallback path."""
    payload = encode_lifecycle_command(event, token)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control_socket:
        for _ in range(max(1, repeats)):
            control_socket.sendto(payload, (host, port))


def stop_process(
    process: subprocess.Popen,
    name: str,
    *,
    graceful_request: Callable[[], None] | None = None,
    graceful_timeout: float = 3.0,
    terminate_timeout: float = 2.0,
) -> str:
    """Cooperative stop, then terminate, then kill with bounded waits."""
    if process.poll() is not None:
        return "already_exited"
    if graceful_request is not None:
        try:
            graceful_request()
        except OSError:
            pass
        try:
            process.wait(timeout=graceful_timeout)
            return "graceful"
        except subprocess.TimeoutExpired:
            pass

    process.terminate()
    try:
        process.wait(timeout=terminate_timeout)
        return "terminated"
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=terminate_timeout)
        return "killed"


class WindowsSingleInstance:
    """Named mutex held for the lifetime of one supervisor instance."""

    ERROR_ALREADY_EXISTS = 183

    def __init__(self, name: str):
        self.name = name
        self._handle = None

    def acquire(self) -> bool:
        if sys.platform != "win32":
            return True
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
        kernel32.CreateMutexW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateMutexW(None, False, self.name)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        if ctypes.get_last_error() == self.ERROR_ALREADY_EXISTS:
            kernel32.CloseHandle(handle)
            return False
        self._handle = handle
        return True

    def close(self) -> None:
        if self._handle and sys.platform == "win32":
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            kernel32.CloseHandle(self._handle)
            self._handle = None


if sys.platform == "win32":
    class _JobBasicLimitInformation(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_longlong),
            ("PerJobUserTimeLimit", ctypes.c_longlong),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]


    class _IoCounters(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_ulonglong),
            ("WriteOperationCount", ctypes.c_ulonglong),
            ("OtherOperationCount", ctypes.c_ulonglong),
            ("ReadTransferCount", ctypes.c_ulonglong),
            ("WriteTransferCount", ctypes.c_ulonglong),
            ("OtherTransferCount", ctypes.c_ulonglong),
        ]


    class _JobExtendedLimitInformation(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _JobBasicLimitInformation),
            ("IoInfo", _IoCounters),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]


class WindowsJob:
    """Kills assigned child processes if the supervisor disappears."""

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9

    def __init__(self):
        self._handle = None

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.CreateJobObjectW(None, None)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        info = _JobExtendedLimitInformation()
        info.BasicLimitInformation.LimitFlags = self.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        ok = kernel32.SetInformationJobObject(
            handle,
            self.JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            ctypes.byref(info),
            ctypes.sizeof(info),
        )
        if not ok:
            kernel32.CloseHandle(handle)
            raise ctypes.WinError(ctypes.get_last_error())
        self._handle = handle
        return True

    def assign(self, process: subprocess.Popen) -> None:
        if not self._handle or sys.platform != "win32":
            return
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        if not kernel32.AssignProcessToJobObject(self._handle, wintypes.HANDLE(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        if self._handle and sys.platform == "win32":
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            kernel32.CloseHandle(self._handle)
            self._handle = None


def request_windows_close(process_id: int) -> bool:
    """Post WM_CLOSE to every visible top-level window owned by a process."""
    if sys.platform != "win32":
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    posted = False
    enum_callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [enum_callback_type, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL

    @enum_callback_type
    def callback(window, _lparam):
        nonlocal posted
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == process_id and user32.IsWindowVisible(window):
            user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE
            posted = True
        return True

    user32.EnumWindows(callback, 0)
    return posted
