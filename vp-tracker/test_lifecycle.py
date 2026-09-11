"""Unit tests for the managed Unreal/tracker lifecycle contract."""

import subprocess
import sys
import unittest
from unittest.mock import Mock, patch
import uuid
from pathlib import Path

from lifecycle import (
    DEFAULT_HEARTBEAT_TIMEOUT_SECONDS,
    LifecycleCommand,
    LifecycleLease,
    WindowsSingleInstance,
    WindowsJob,
    encode_lifecycle_command,
    parse_lifecycle_command,
    stop_process,
)
from supervisor import (
    PipelineSupervisor,
    RUNTIME_DISABLED_PLUGINS,
    _is_normal_unreal_exit,
)


TOKEN = "0123456789abcdef0123456789abcdef"


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakeProcess:
    def __init__(self, wait_results):
        self.returncode = None
        self.wait_results = list(wait_results)
        self.terminated = False
        self.killed = False

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        result = self.wait_results.pop(0)
        if isinstance(result, BaseException):
            raise result
        self.returncode = result
        return result

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


class LifecycleProtocolTests(unittest.TestCase):
    def test_default_heartbeat_timeout_allows_short_game_thread_stall(self):
        clock = FakeClock()
        lease = LifecycleLease(TOKEN, clock=clock)
        lease.accept(LifecycleCommand("hello", TOKEN))

        clock.now = DEFAULT_HEARTBEAT_TIMEOUT_SECONDS - 0.01
        self.assertFalse(lease.should_shutdown())
        clock.now = DEFAULT_HEARTBEAT_TIMEOUT_SECONDS
        self.assertTrue(lease.should_shutdown())
        self.assertEqual(DEFAULT_HEARTBEAT_TIMEOUT_SECONDS, 10.0)

    def test_round_trip(self):
        command = parse_lifecycle_command(encode_lifecycle_command("heartbeat", TOKEN))
        self.assertEqual(command, LifecycleCommand(event="heartbeat", token=TOKEN))

    def test_rejects_invalid_event_and_token(self):
        with self.assertRaises(ValueError):
            encode_lifecycle_command("restart", TOKEN)
        with self.assertRaises(ValueError):
            encode_lifecycle_command("hello", "not-a-token")

    def test_lease_times_out_before_and_after_connection(self):
        clock = FakeClock()
        lease = LifecycleLease(TOKEN, startup_timeout=5.0, heartbeat_timeout=3.0, clock=clock)
        self.assertFalse(lease.should_shutdown())
        clock.now = 5.0
        self.assertTrue(lease.should_shutdown())

        clock = FakeClock()
        lease = LifecycleLease(TOKEN, startup_timeout=5.0, heartbeat_timeout=3.0, clock=clock)
        self.assertTrue(lease.accept(LifecycleCommand("hello", TOKEN)))
        clock.now = 2.9
        self.assertFalse(lease.should_shutdown())
        clock.now = 3.0
        self.assertTrue(lease.should_shutdown())

    def test_wrong_session_is_ignored_and_shutdown_wakes_waiter(self):
        clock = FakeClock()
        lease = LifecycleLease(TOKEN, clock=clock)
        self.assertFalse(lease.accept(LifecycleCommand("hello", "f" * 32)))
        self.assertFalse(lease.connected)
        self.assertTrue(lease.accept(LifecycleCommand("shutdown", TOKEN)))
        self.assertTrue(lease.wait_until_connected(0.0))
        self.assertTrue(lease.should_shutdown())


class ProcessShutdownTests(unittest.TestCase):
    def test_managed_game_command_disables_editor_only_toolsets(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/UE/UnrealEditor.exe"),
            project_path=Path("C:/Project/VPPipeline.uproject"),
        )

        command = supervisor._build_unreal_command()

        self.assertIn("-game", command)
        self.assertIn("-windowed", command)
        self.assertIn("-ResX=1280", command)
        self.assertIn("-ResY=720", command)
        self.assertIn(
            f"-DisablePlugins={','.join(RUNTIME_DISABLED_PLUGINS)}",
            command,
        )

    def test_packaged_command_uses_runtime_without_editor_console(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/Release/Runtime/VPPipeline.exe"),
            packaged_mode=True,
            interactive_commands=False,
        )

        command = supervisor._build_unreal_command()

        self.assertEqual(command[0], str(Path("C:/Release/Runtime/VPPipeline.exe").resolve()))
        self.assertIn("/Game/Maps/Lvl_Empty", command)
        self.assertIn("-SaveToUserDir", command)
        self.assertNotIn("-game", command)
        self.assertNotIn("-log", command)
        self.assertTrue(callable(supervisor._check_webcam))
        self.assertTrue(callable(supervisor._check_models))
        self.assertTrue(callable(supervisor._check_pipeline_ports))

    def test_user_window_close_status_is_a_normal_unreal_exit(self):
        self.assertTrue(_is_normal_unreal_exit(0))
        self.assertTrue(_is_normal_unreal_exit(0xC000013A))
        self.assertTrue(_is_normal_unreal_exit(-1073741510))
        self.assertFalse(_is_normal_unreal_exit(1))

    def test_graceful_stop_does_not_terminate(self):
        process = FakeProcess([0])
        request = Mock()
        result = stop_process(process, "test", graceful_request=request)
        self.assertEqual(result, "graceful")
        request.assert_called_once()
        self.assertFalse(process.terminated)

    def test_escalates_from_graceful_to_terminate_to_kill(self):
        process = FakeProcess([
            subprocess.TimeoutExpired("test", 1),
            subprocess.TimeoutExpired("test", 1),
            -9,
        ])
        result = stop_process(process, "test", graceful_request=lambda: None)
        self.assertEqual(result, "killed")
        self.assertTrue(process.terminated)
        self.assertTrue(process.killed)

    def test_named_mutex_rejects_duplicate_supervisor(self):
        name = rf"Local\VPTest.{uuid.uuid4().hex}"
        first = WindowsSingleInstance(name)
        second = WindowsSingleInstance(name)
        try:
            self.assertTrue(first.acquire())
            self.assertFalse(second.acquire())
        finally:
            second.close()
            first.close()

    @unittest.skipUnless(sys.platform == "win32", "Windows Job Object test")
    def test_job_close_terminates_managed_child(self):
        job = WindowsJob()
        process = None
        try:
            self.assertTrue(job.open())
            process = subprocess.Popen([
                sys.executable,
                "-c",
                "import time; time.sleep(30)",
            ])
            job.assign(process)
            job.close()
            process.wait(timeout=3.0)
            self.assertIsNotNone(process.returncode)
        finally:
            job.close()
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=3.0)


if __name__ == "__main__":
    unittest.main()
