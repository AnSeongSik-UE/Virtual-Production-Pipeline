"""Unit tests for the managed Unreal/tracker lifecycle contract."""

import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from io import StringIO
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
from obs_controller import OBSOutputActionResult, OBSOutputStatus
from supervisor import (
    PipelineSupervisor,
    RUNTIME_DISABLED_PLUGINS,
    STREAM_CONFIRM_TIMEOUT_SECONDS,
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


class FakeSupervisorOBS:
    def __init__(self):
        self.streaming = False
        self.recording = False
        self.start_stream_calls = 0
        self.start_record_calls = 0
        self.stop_stream_calls = 0
        self.stop_record_calls = 0
        self.disconnected = False

    def get_output_status(self):
        return OBSOutputStatus(
            stream_active=self.streaming,
            stream_duration_ms=3_000 if self.streaming else 0,
            stream_bytes=1_048_576 if self.streaming else 0,
            record_active=self.recording,
            record_paused=False,
            record_duration_ms=2_000 if self.recording else 0,
            record_bytes=524_288 if self.recording else 0,
            record_path="",
        )

    def start_streaming(self):
        self.start_stream_calls += 1
        self.streaming = True
        return OBSOutputActionResult("started")

    def stop_streaming(self):
        self.stop_stream_calls += 1
        self.streaming = False
        return OBSOutputActionResult("stopped")

    def start_recording(self):
        if self.recording:
            return OBSOutputActionResult("already_active")
        self.start_record_calls += 1
        self.recording = True
        return OBSOutputActionResult("started")

    def stop_recording(self):
        self.stop_record_calls += 1
        self.recording = False
        return OBSOutputActionResult("stopped", "C:/recordings/test.mkv")

    def disconnect(self):
        self.disconnected = True


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
    def test_streaming_requires_a_fresh_second_confirmation(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/UE/UnrealEditor.exe"),
            project_path=Path("C:/Project/VPPipeline.uproject"),
        )
        obs = FakeSupervisorOBS()
        supervisor.obs_ctrl = obs

        with redirect_stdout(StringIO()), patch(
            "supervisor.time.monotonic",
            return_value=100.0,
        ):
            supervisor._handle_command("stream")
        self.assertEqual(obs.start_stream_calls, 0)

        with redirect_stdout(StringIO()), patch(
            "supervisor.time.monotonic",
            return_value=100.0 + STREAM_CONFIRM_TIMEOUT_SECONDS,
        ):
            supervisor._handle_command("stream confirm")
        self.assertEqual(obs.start_stream_calls, 1)

    def test_expired_stream_confirmation_cannot_start_output(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/UE/UnrealEditor.exe"),
            project_path=Path("C:/Project/VPPipeline.uproject"),
        )
        obs = FakeSupervisorOBS()
        supervisor.obs_ctrl = obs

        with redirect_stdout(StringIO()), patch(
            "supervisor.time.monotonic",
            return_value=100.0,
        ):
            supervisor._handle_command("stream")
        output = StringIO()
        with redirect_stdout(output), patch(
            "supervisor.time.monotonic",
            return_value=100.001 + STREAM_CONFIRM_TIMEOUT_SECONDS,
        ):
            supervisor._handle_command("stream confirm")

        self.assertEqual(obs.start_stream_calls, 0)
        self.assertIn("expired", output.getvalue())

    def test_recording_command_starts_once_and_status_lists_both_outputs(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/UE/UnrealEditor.exe"),
            project_path=Path("C:/Project/VPPipeline.uproject"),
        )
        obs = FakeSupervisorOBS()
        supervisor.obs_ctrl = obs
        output = StringIO()

        with redirect_stdout(output):
            supervisor._handle_command("rec")
            supervisor._handle_command("rec")
            supervisor._handle_command("status")

        self.assertEqual(obs.start_record_calls, 1)
        self.assertIn("Streaming: inactive", output.getvalue())
        self.assertIn("[REC] Recording", output.getvalue())

    def test_shutdown_warns_but_does_not_stop_external_obs_outputs(self):
        supervisor = PipelineSupervisor(
            unreal_executable=Path("C:/UE/UnrealEditor.exe"),
            project_path=Path("C:/Project/VPPipeline.uproject"),
        )
        obs = FakeSupervisorOBS()
        obs.streaming = True
        obs.recording = True
        supervisor.obs_ctrl = obs
        output = StringIO()

        with redirect_stdout(output):
            supervisor._shutdown()

        self.assertEqual(obs.stop_stream_calls, 0)
        self.assertEqual(obs.stop_record_calls, 0)
        self.assertTrue(obs.disconnected)
        self.assertIn("Streaming remains active", output.getvalue())
        self.assertIn("Recording remains active", output.getvalue())

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
