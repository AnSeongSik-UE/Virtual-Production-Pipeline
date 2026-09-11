"""Tests for the local camera and lifecycle control channel."""

import json
import socket
import time
import unittest

from control_channel import (
    CameraCommand,
    ControlListener,
    parse_camera_command,
)
from lifecycle import LifecycleLease, encode_lifecycle_command


TOKEN = "0123456789abcdef0123456789abcdef"


def _unused_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class ControlChannelTests(unittest.TestCase):
    def test_parse_camera_list_and_selection_commands(self):
        self.assertEqual(
            parse_camera_command(json.dumps({
                "version": 1,
                "type": "camera_control",
                "action": "list",
            }).encode()),
            CameraCommand(action="list"),
        )
        self.assertEqual(
            parse_camera_command(json.dumps({
                "version": 1,
                "type": "camera_control",
                "action": "select",
                "device_id": "0123456789abcdef",
            }).encode()),
            CameraCommand(action="select", device_id="0123456789abcdef"),
        )

    def test_rejects_invalid_camera_selection(self):
        for device_id in ("", "camera/path", "한글", "x" * 65):
            with self.subTest(device_id=device_id), self.assertRaises(ValueError):
                parse_camera_command(json.dumps({
                    "version": 1,
                    "type": "camera_control",
                    "action": "select",
                    "device_id": device_id,
                }).encode())

    def test_listener_queues_request_and_returns_device_list(self):
        listener = ControlListener(port=_unused_udp_port())
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sender.bind(("127.0.0.1", 0))
        sender.settimeout(1.0)
        try:
            listener.start()
            sender.sendto(json.dumps({
                "version": 1,
                "type": "camera_control",
                "action": "list",
            }).encode(), ("127.0.0.1", listener.port))
            deadline = time.monotonic() + 1.0
            queued = None
            while queued is None and time.monotonic() < deadline:
                queued = listener.take_camera_command()
                time.sleep(0.01)
            self.assertIsNotNone(queued)
            self.assertEqual(queued.command, CameraCommand(action="list"))

            listener.send_camera_result(
                queued.reply_address,
                action="list",
                status="ok",
                active_device_id="abc123",
                devices=[{
                    "id": "abc123",
                    "name": "USB Camera",
                    "is_virtual": False,
                }],
            )
            response, _ = sender.recvfrom(8192)
            payload = json.loads(response.decode())
            self.assertEqual(payload["type"], "camera_control_result")
            self.assertEqual(payload["active_device_id"], "abc123")
            self.assertEqual(payload["devices"][0]["name"], "USB Camera")
        finally:
            sender.close()
            listener.stop()

    def test_lifecycle_heartbeat_is_processed(self):
        lease = LifecycleLease(TOKEN, startup_timeout=1.0, heartbeat_timeout=1.0)
        listener = ControlListener(port=_unused_udp_port(), lifecycle_lease=lease)
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            listener.start()
            sender.sendto(
                encode_lifecycle_command("hello", TOKEN),
                ("127.0.0.1", listener.port),
            )
            self.assertTrue(lease.wait_until_connected(timeout=1.0))
            self.assertFalse(lease.should_shutdown())
        finally:
            sender.close()
            listener.stop()

    def test_legacy_obs_command_is_ignored(self):
        listener = ControlListener(port=_unused_udp_port())
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            listener.start()
            sender.sendto(json.dumps({
                "version": 1,
                "type": "obs_output",
                "action": "start_stream",
            }).encode(), ("127.0.0.1", listener.port))
            time.sleep(0.05)
            self.assertIsNone(listener.take_camera_command())
        finally:
            sender.close()
            listener.stop()


if __name__ == "__main__":
    unittest.main()
