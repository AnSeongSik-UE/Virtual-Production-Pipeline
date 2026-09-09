import json
import socket
import threading
import time
import unittest
from unittest.mock import patch

from lifecycle import (
    LifecycleCommand,
    LifecycleLease,
    encode_lifecycle_command,
    parse_lifecycle_command,
)
from obs_control import (
    BackgroundCommand,
    FPSCommand,
    OBSControlListener,
    apply_background_command,
    apply_fps_command,
    parse_background_command,
    parse_fps_command,
)
from obs_controller import (
    OBSController,
    OBSOutputActionResult,
    OBSOutputStatus,
    OBSVideoFPSResult,
)


class FakeController:
    def __init__(self):
        self.command = None

    def sync_spout_output(self, **kwargs):
        self.command = kwargs

    def sync_video_fps(self, fps):
        self.command = fps
        return OBSVideoFPSResult("applied", fps, float(fps))


class FakeResponse:
    def __init__(self, filters):
        self.filters = filters


class FakeOBSClient:
    def __init__(self, filters):
        self.filters = filters
        self.calls = []

    def set_input_settings(self, *args):
        self.calls.append(("input", args))

    def get_source_filter_list(self, source):
        self.calls.append(("list", (source,)))
        return FakeResponse(self.filters)

    def set_source_filter_settings(self, *args):
        self.calls.append(("settings", args))

    def create_source_filter(self, *args):
        self.calls.append(("create", args))

    def set_source_filter_enabled(self, *args):
        self.calls.append(("enabled", args))

    def remove_source_filter(self, *args):
        self.calls.append(("remove", args))


class FakeVideoSettings:
    def __init__(self, numerator=60, denominator=1):
        self.fps_numerator = numerator
        self.fps_denominator = denominator


class FakeOutputStatus:
    def __init__(
        self,
        active=False,
        *,
        duration=0,
        byte_count=0,
        paused=False,
        output_path="",
    ):
        self.output_active = active
        self.output_duration = duration
        self.output_bytes = byte_count
        self.output_paused = paused
        self.output_path = output_path


class FakeOutputClient:
    def __init__(self, *, stream_active=False, record_active=False):
        self.stream_status = FakeOutputStatus(
            stream_active,
            duration=12_000,
            byte_count=2_097_152,
        )
        self.record_status = FakeOutputStatus(
            record_active,
            duration=5_000,
            byte_count=1_048_576,
            paused=record_active,
            output_path="C:/recordings/active.mkv" if record_active else "",
        )
        self.calls = []

    def get_stream_status(self):
        return self.stream_status

    def get_record_status(self):
        return self.record_status

    def start_stream(self):
        self.calls.append("start_stream")
        self.stream_status.output_active = True

    def stop_stream(self):
        self.calls.append("stop_stream")
        self.stream_status.output_active = False

    def start_record(self):
        self.calls.append("start_record")
        self.record_status.output_active = True

    def stop_record(self):
        self.calls.append("stop_record")
        self.record_status.output_active = False
        return FakeOutputStatus(output_path="C:/recordings/stopped.mkv")


class FakeFPSClient:
    def __init__(self, current_fps=60, active_output=None):
        self.current_fps = current_fps
        self.active_output = active_output
        self.calls = []

    def get_video_settings(self):
        self.calls.append(("get_video_settings", None))
        return FakeVideoSettings(self.current_fps)

    def _status(self, name):
        self.calls.append((name, None))
        return FakeOutputStatus(self.active_output == name)

    def get_stream_status(self):
        return self._status("stream")

    def get_record_status(self):
        return self._status("record")

    def get_virtual_cam_status(self):
        return self._status("virtual_cam")

    def get_replay_buffer_status(self):
        return self._status("replay_buffer")

    def send(self, request, payload):
        self.calls.append((request, payload))


class OBSControlTests(unittest.TestCase):
    def test_output_status_aggregates_stream_and_recording(self):
        controller = object.__new__(OBSController)
        controller.client = FakeOutputClient(stream_active=True, record_active=True)

        status = controller.get_output_status()

        self.assertEqual(
            status,
            OBSOutputStatus(
                stream_active=True,
                stream_duration_ms=12_000,
                stream_bytes=2_097_152,
                record_active=True,
                record_paused=True,
                record_duration_ms=5_000,
                record_bytes=1_048_576,
                record_path="C:/recordings/active.mkv",
            ),
        )

    def test_output_commands_are_idempotent_and_return_record_path(self):
        controller = object.__new__(OBSController)
        controller.client = FakeOutputClient(stream_active=True, record_active=True)

        self.assertEqual(
            controller.start_streaming(),
            OBSOutputActionResult("already_active"),
        )
        self.assertEqual(
            controller.start_recording(),
            OBSOutputActionResult("already_active"),
        )
        self.assertEqual(controller.client.calls, [])

        self.assertEqual(
            controller.stop_streaming(),
            OBSOutputActionResult("stopped"),
        )
        self.assertEqual(
            controller.stop_recording(),
            OBSOutputActionResult("stopped", "C:/recordings/stopped.mkv"),
        )
        self.assertEqual(controller.client.calls, ["stop_stream", "stop_record"])

    def test_slow_obs_connection_does_not_block_lifecycle_heartbeat(self):
        session_token = "0123456789abcdef0123456789abcdef"
        lease = LifecycleLease(
            session_token,
            startup_timeout=1.0,
            heartbeat_timeout=0.25,
        )
        factory_started = threading.Event()
        release_factory = threading.Event()

        def slow_factory():
            factory_started.set()
            release_factory.wait(timeout=2.0)
            return FakeController()

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_probe:
            port_probe.bind(("127.0.0.1", 0))
            port = port_probe.getsockname()[1]

        listener = OBSControlListener(
            port=port,
            controller_factory=slow_factory,
            lifecycle_lease=lease,
        )
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        background = json.dumps({
            "version": 1,
            "type": "broadcast_background",
            "mode": "background_removed",
            "sender": "Virtual Production Pipeline",
            "source": "Spout2 Capture",
            "filter": "Chroma Key",
        }).encode()

        try:
            with patch("obs_control.is_obs_websocket_available", return_value=True):
                listener.start()
                sender.sendto(
                    encode_lifecycle_command("hello", session_token),
                    ("127.0.0.1", port),
                )
                self.assertTrue(lease.wait_until_connected(timeout=1.0))
                sender.sendto(background, ("127.0.0.1", port))
                self.assertTrue(factory_started.wait(timeout=1.0))
                time.sleep(0.15)
                sender.sendto(
                    encode_lifecycle_command("heartbeat", session_token),
                    ("127.0.0.1", port),
                )
                time.sleep(0.15)
                self.assertFalse(lease.should_shutdown())
        finally:
            release_factory.set()
            sender.close()
            listener.stop()

    def test_listener_returns_fps_result_to_unreal_socket(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as port_probe:
            port_probe.bind(("127.0.0.1", 0))
            port = port_probe.getsockname()[1]

        listener = OBSControlListener(
            port=port,
            controller_factory=FakeController,
        )
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sender.bind(("127.0.0.1", 0))
        sender.settimeout(1.0)
        try:
            with patch("obs_control.is_obs_websocket_available", return_value=True):
                listener.start()
                sender.sendto(json.dumps({
                    "version": 1,
                    "type": "broadcast_fps",
                    "fps": 48,
                }).encode(), ("127.0.0.1", port))
                response, _ = sender.recvfrom(2048)
            payload = json.loads(response.decode())
            self.assertEqual(payload["type"], "broadcast_fps_result")
            self.assertEqual(payload["status"], "applied")
            self.assertEqual(payload["requested_fps"], 48)
            self.assertEqual(payload["obs_fps"], 48.0)
        finally:
            sender.close()
            listener.stop()

    def test_parse_lifecycle_command(self):
        payload = json.dumps({
            "version": 1,
            "type": "pipeline_lifecycle",
            "event": "shutdown",
            "token": "0123456789abcdef0123456789abcdef",
        }).encode()
        self.assertEqual(
            parse_lifecycle_command(payload),
            LifecycleCommand(
                event="shutdown",
                token="0123456789abcdef0123456789abcdef",
            ),
        )

    def test_parse_valid_background_command(self):
        payload = json.dumps({
            "version": 1,
            "type": "broadcast_background",
            "mode": "background_removed",
            "sender": "Virtual Production Pipeline",
            "source": "Spout2 Capture",
            "filter": "크로마 키",
        }).encode()
        command = parse_background_command(payload)
        self.assertEqual(command.mode, "background_removed")

    def test_parse_valid_fps_command(self):
        payload = json.dumps({
            "version": 1,
            "type": "broadcast_fps",
            "fps": 144,
        }).encode()
        self.assertEqual(parse_fps_command(payload), FPSCommand(fps=144))

    def test_rejects_fps_outside_supported_range(self):
        for value in (14, 145, True, 60.0):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_fps_command(json.dumps({
                    "version": 1,
                    "type": "broadcast_fps",
                    "fps": value,
                }).encode())

    def test_rejects_invalid_color_and_remote_shape(self):
        with self.assertRaises(ValueError):
            parse_background_command(b'{"version":1}')

    def test_rejects_removed_chroma_mode(self):
        payload = json.dumps({
            "version": 1,
            "type": "broadcast_background",
            "mode": "chroma_key",
            "sender": "Virtual Production Pipeline",
            "source": "Spout2 Capture",
            "filter": "크로마 키",
        }).encode()
        with self.assertRaises(ValueError):
            parse_background_command(payload)

    def test_apply_delegates_validated_fields(self):
        controller = FakeController()
        command = BackgroundCommand(
            mode="solid_color",
            sender="Sender",
            source="Source",
            filter_name="Filter",
        )
        apply_background_command(controller, command)
        self.assertEqual(controller.command["mode"], "solid_color")
        self.assertEqual(controller.command["sender_name"], "Sender")

    def test_apply_fps_delegates_validated_value(self):
        controller = FakeController()
        result = apply_fps_command(controller, FPSCommand(fps=30))
        self.assertEqual(controller.command, 30)
        self.assertEqual(result.status, "applied")

    def test_idle_obs_applies_only_fps_pair(self):
        controller = object.__new__(OBSController)
        controller.client = FakeFPSClient(current_fps=60)
        result = controller.sync_video_fps(30)
        self.assertEqual(result, OBSVideoFPSResult("applied", 30, 30.0))
        self.assertIn((
            "SetVideoSettings",
            {"fpsNumerator": 30, "fpsDenominator": 1},
        ), controller.client.calls)

    def test_active_obs_output_keeps_existing_fps(self):
        controller = object.__new__(OBSController)
        controller.client = FakeFPSClient(current_fps=60, active_output="record")
        result = controller.sync_video_fps(30)
        self.assertEqual(result, OBSVideoFPSResult("output_active", 30, 60.0))
        self.assertFalse(any(call[0] == "SetVideoSettings" for call in controller.client.calls))

    def test_matching_obs_fps_does_not_reset_video(self):
        controller = object.__new__(OBSController)
        controller.client = FakeFPSClient(current_fps=60)
        result = controller.sync_video_fps(60)
        self.assertEqual(result, OBSVideoFPSResult("unchanged", 60, 60.0))
        self.assertEqual(controller.client.calls, [("get_video_settings", None)])

    def test_background_removed_uses_premultiplied_alpha(self):
        controller = object.__new__(OBSController)
        controller.client = FakeOBSClient([])
        controller.sync_spout_output(
            source_name="Spout2 Capture",
            sender_name="Virtual Production Pipeline",
            filter_name="크로마 키",
            mode="background_removed",
        )
        self.assertIn(
            (
                "input",
                (
                    "Spout2 Capture",
                    {
                        "spoutsenders": "Virtual Production Pipeline",
                        "compositemode": 4,
                    },
                    True,
                ),
            ),
            controller.client.calls,
        )

    def test_modes_remove_existing_chroma_filter(self):
        controller = object.__new__(OBSController)
        controller.client = FakeOBSClient([{"filterName": "크로마 키"}])
        controller.sync_spout_output(
            source_name="Spout2 Capture",
            sender_name="Virtual Production Pipeline",
            filter_name="크로마 키",
            mode="solid_color",
        )
        self.assertIn(
            ("remove", ("Spout2 Capture", "크로마 키")),
            controller.client.calls,
        )
        self.assertIn(
            (
                "input",
                (
                    "Spout2 Capture",
                    {
                        "spoutsenders": "Virtual Production Pipeline",
                        "compositemode": 1,
                    },
                    True,
                ),
            ),
            controller.client.calls,
        )


if __name__ == "__main__":
    unittest.main()
