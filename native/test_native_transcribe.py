"""Synthetic ASR bridge tests: no audio devices or real model are opened."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("native_transcribe", Path(__file__).with_name("native_transcribe.py"))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="lightrec-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.session = self.root / "轻量_合成"
        self.chunks = self.session / "chunks"
        self.chunks.mkdir(parents=True)
        self.control = self.root / "control.json"
        self.flags(enabled=True, paused=False, recording=False)

    def flags(self, **value):
        worker.atomic(self.control, value)

    def job(self, name="microphone_000", **overrides):
        audio = self.chunks / (name + ".wav")
        audio.write_bytes(b"synthetic-placeholder")
        value = dict(audio=str(audio), source="microphone", start=10, duration=10, status="pending")
        value.update(overrides)
        path = audio.with_suffix(".job")
        worker.atomic(path, value)
        return path, audio

    def run_worker(self, model=None, retry=False, sleep=None):
        argv = ["worker", "--root", str(self.root), "--session", str(self.session),
                "--model", str(self.root / "fake-model"), "--control", str(self.control)]
        if retry:
            argv.append("--retry")
        if model is None:
            model = SimpleNamespace(transcribe=lambda *a, **k: (
                [SimpleNamespace(start=1.5, end=2.5, text=" 合成中文 ")], None))
        module = SimpleNamespace(WhisperModel=lambda *a, **k: model)
        def stop(*_):
            self.flags(enabled=False, paused=True, recording=False)
        with patch.object(sys, "argv", argv), patch.dict(sys.modules, {"faster_whisper": module}), \
                patch.object(worker.time, "sleep", sleep or stop):
            return worker.main()

    def test_bad_control_defaults_to_disabled(self):
        for value in ("broken", "[]", '{"enabled":"true"}'):
            self.control.write_text(value, encoding="utf-8")
            self.assertFalse(worker.control(self.control)["enabled"])

    def test_atomic_write_retries_transient_windows_sharing_error(self):
        actual_replace = worker.os.replace
        attempts = []
        def replace(source, destination):
            attempts.append(True)
            if len(attempts) == 1:
                raise PermissionError("synthetic sharing conflict")
            actual_replace(source, destination)
        with patch.object(worker.os, "replace", replace), patch.object(worker.time, "sleep"):
            self.flags(enabled=False, paused=True)
        self.assertEqual(len(attempts), 2)
        self.assertFalse(worker.control(self.control)["enabled"])

    def test_audio_outside_session_is_ignored(self):
        outside = self.root / "private.wav"
        outside.write_bytes(b"x")
        self.job(audio=str(outside))
        self.assertEqual(list(worker.jobs(self.root)), [])
        self.assertTrue(outside.exists())

    def test_malformed_jobs_are_ignored(self):
        self.job(start=float("nan"))
        (self.chunks / "bad.job").write_text("[]", encoding="utf-8")
        self.assertEqual(list(worker.jobs(self.root)), [])

    def test_success_exports_then_deletes_only_chunk(self):
        path, audio = self.job()
        original = self.session / "microphone.wav"
        original.write_bytes(b"retained-original")
        self.assertEqual(self.run_worker(), 0)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "done")
        self.assertFalse(audio.exists())
        self.assertEqual(original.read_bytes(), b"retained-original")
        self.assertIn("[00:00:11] 麦克风：合成中文", (self.session / "转写.txt").read_text(encoding="utf-8-sig"))

    def test_model_failure_preserves_audio_and_marks_failed(self):
        path, audio = self.job()
        def fail(*a, **k):
            raise RuntimeError("synthetic failure")
        self.assertEqual(self.run_worker(SimpleNamespace(transcribe=fail)), 2)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "failed")
        self.assertTrue(audio.exists())
        self.assertFalse((self.session / "转写.txt").exists())

    def test_pause_finishes_current_segment_and_retains_next_audio(self):
        first, first_audio = self.job("a")
        second, second_audio = self.job("b")
        def recognize(*a, **k):
            self.flags(enabled=True, paused=True, recording=True)
            return [SimpleNamespace(start=0, end=1, text="当前分段")], None
        self.assertEqual(self.run_worker(SimpleNamespace(transcribe=recognize)), 0)
        self.assertEqual(json.loads(first.read_text(encoding="utf-8"))["status"], "done")
        self.assertFalse(first_audio.exists())
        self.assertEqual(json.loads(second.read_text(encoding="utf-8"))["status"], "pending")
        self.assertTrue(second_audio.exists())

    def test_retry_recovers_failed_and_interrupted_jobs(self):
        for status in ("failed", "running"):
            with self.subTest(status=status):
                path, audio = self.job(name=status, status=status)
                self.flags(enabled=True, paused=False, recording=False)
                self.assertEqual(self.run_worker(retry=True), 0)
                self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "done")
                self.assertFalse(audio.exists())

    def test_paused_does_not_load_model(self):
        path, audio = self.job()
        self.flags(enabled=True, paused=True, recording=True)
        with patch.object(worker, "jobs", side_effect=AssertionError("must not scan")):
            self.assertEqual(self.run_worker(), 0)
        self.assertTrue(audio.exists())
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "pending")

    def test_recording_keeps_idle_worker_alive_past_five_seconds(self):
        self.flags(enabled=True, paused=False, recording=True)
        calls = []
        def stop(_):
            calls.append(True)
            self.flags(enabled=False, paused=True)
        with patch.object(worker.time, "monotonic", side_effect=[0, 100]):
            self.assertEqual(self.run_worker(sleep=stop), 0)
        self.assertEqual(len(calls), 1)

    def test_session_switch_drains_old_pending_audio_before_release(self):
        path, audio = self.job()
        self.flags(enabled=True, paused=False, recording=True, session=str(self.root / "轻量_new"))
        self.assertEqual(self.run_worker(), 0)
        self.assertFalse(audio.exists())
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "done")

    def test_export_orders_both_sources_and_ignores_failed(self):
        self.job("b", source="system", start=2, status="done",
                 segments=[dict(start=0, text="电脑")])
        self.job("a", start=1, status="done", segments=[dict(start=0, text="麦克风")])
        self.job("c", start=0, status="failed", segments=[dict(start=0, text="未完成")])
        worker.export(self.session)
        text = (self.session / "转写.txt").read_text(encoding="utf-8-sig")
        self.assertLess(text.index("麦克风"), text.index("电脑"))
        self.assertNotIn("未完成", text)

    def test_outside_session_is_rejected(self):
        argv = ["worker", "--root", str(self.root), "--session", str(self.root.parent / "轻量_outside"),
                "--model", "fake", "--control", str(self.control)]
        with patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                worker.main()
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
