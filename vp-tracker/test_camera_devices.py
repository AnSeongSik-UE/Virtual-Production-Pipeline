"""Tests for stable, user-visible camera selection."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest

from camera_devices import (
    build_camera_devices,
    choose_startup_camera,
    load_selected_camera_id,
    save_selected_camera_id,
)


def _info(index, name, path="", backend=700, vid=None, pid=None):
    return SimpleNamespace(
        index=index,
        name=name,
        path=path,
        backend=backend,
        vid=vid,
        pid=pid,
    )


class CameraDeviceTests(unittest.TestCase):
    def test_builds_stable_ids_and_marks_virtual_camera(self):
        devices = build_camera_devices([
            _info(0, "USB Camera", r"\\?\usb#camera", vid=1, pid=2),
            _info(1, "OBS Virtual Camera"),
        ])

        self.assertEqual(len(devices), 2)
        self.assertEqual(len(devices[0].device_id), 24)
        self.assertFalse(devices[0].is_virtual)
        self.assertTrue(devices[1].is_virtual)
        self.assertEqual(
            devices[0].device_id,
            build_camera_devices([
                _info(5, "Renamed Camera", r"\\?\usb#camera", vid=1, pid=2),
            ])[0].device_id,
        )

    def test_duplicate_names_receive_distinct_labels(self):
        devices = build_camera_devices([
            _info(0, "USB Camera", "path-a"),
            _info(1, "USB Camera", "path-b"),
        ])
        self.assertEqual(
            [device.display_name for device in devices],
            ["USB Camera (1)", "USB Camera (2)"],
        )

    def test_saved_device_wins_and_missing_saved_device_falls_back_to_physical(self):
        devices = build_camera_devices([
            _info(0, "OBS Virtual Camera"),
            _info(1, "USB Camera", "physical-path"),
        ])

        selected, fallback = choose_startup_camera(devices, devices[0].device_id)
        self.assertEqual(selected, devices[0])
        self.assertFalse(fallback)

        selected, fallback = choose_startup_camera(devices, "missing")
        self.assertEqual(selected, devices[1])
        self.assertTrue(fallback)

    def test_selection_round_trip_and_invalid_file_recovery(self):
        with TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "camera.json"
            save_selected_camera_id("a1b2c3", path)
            self.assertEqual(load_selected_camera_id(path), "a1b2c3")
            self.assertEqual(json.loads(path.read_text())["version"], 1)

            path.write_text("not json", encoding="utf-8")
            self.assertEqual(load_selected_camera_id(path), "")


if __name__ == "__main__":
    unittest.main()
