"""Optional local ASR process. The Win32 recorder works without this process."""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", PYTHONDONTWRITEBYTECODE="1",
                  OMP_NUM_THREADS="4", OPENBLAS_NUM_THREADS="1", MKL_NUM_THREADS="4")


def atomic(path: Path, value):
    temporary = path.with_name(path.name + f".{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    for attempt in range(6):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 5:
                raise
            time.sleep(.02)


def control(path):
    try:
        flags = json.loads(path.read_text(encoding="utf-8-sig"))
        if not isinstance(flags, dict) or not isinstance(flags.get("enabled"), bool):
            raise ValueError("invalid control")
        return flags
    except (OSError, ValueError):
        # A missing/damaged control file must not start private audio processing.
        return {"enabled": False, "paused": True}


def jobs(root, session=None):
    folders = [session] if session is not None else sorted(root.glob("轻量_*"))
    for folder in folders:
        for path in sorted((folder / "chunks").glob("*.job")):
            try:
                job = json.loads(path.read_text(encoding="utf-8-sig"))
                if not isinstance(job, dict) or not isinstance(job.get("audio"), str):
                    continue
                if job.get("source") not in ("microphone", "system"):
                    continue
                if any(not isinstance(job.get(key), (int, float)) or
                       not math.isfinite(job[key]) or job[key] < 0
                       for key in ("start", "duration")):
                    continue
                audio = Path(job["audio"])
                if audio.resolve().parent != (folder / "chunks").resolve():
                    continue
                yield folder, path, job
            except (OSError, ValueError, KeyError):
                continue


def stamp(seconds):
    seconds = max(0, int(seconds))
    return f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}"


def export(folder):
    rows = []
    for _, path, job in jobs(folder.parent, folder):
        try:
            if job.get("status") != "done":
                continue
            for segment in job.get("segments", []):
                rows.append((job["start"] + segment["start"], job["source"], segment["text"]))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    rows.sort(key=lambda row: (row[0], row[1]))
    text = "\n".join(
        f"[{stamp(start)}] {'麦克风' if source == 'microphone' else '电脑声音'}：{text}"
        for start, source, text in rows if text)
    temporary = folder / "转写.txt.tmp"
    temporary.write_text(text + "\n", encoding="utf-8-sig")
    os.replace(temporary, folder / "转写.txt")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--retry", action="store_true")
    parser.add_argument("--session", type=Path)
    args = parser.parse_args()
    if args.session is not None:
        if args.session.resolve().parent != args.root.resolve() or not args.session.name.startswith("轻量_"):
            parser.error("--session 必须是录音根目录内的轻量版会话")
    status = args.control.with_name("native-asr-status.json")
    initial = control(args.control)
    if not initial.get("enabled") or initial.get("paused"):
        return 0
    if args.retry:
        for _, path, job in jobs(args.root, args.session):
            if job.get("status") in ("running", "failed"):
                job.update(status="pending", error="")
                atomic(path, job)
    else:
        for _, path, job in jobs(args.root, args.session):
            if job.get("status") == "running":
                job["status"] = "pending"
                atomic(path, job)
    model = None
    last_work = time.monotonic()
    seen = set()
    while True:
        flags = control(args.control)
        if not flags.get("enabled") or flags.get("paused"):
            break
        item = next(((folder, path, job) for folder, path, job in jobs(args.root, args.session)
                     if path not in seen and job.get("status") == "pending"), None)
        if item is None:
            if args.session and flags.get("session") not in (None, str(args.session)):
                break
            if not flags.get("recording") and time.monotonic() - last_work >= 5:
                break
            time.sleep(.5)
            continue
        folder, path, job = item
        try:
            atomic(status, {"state": "working", "pid": os.getpid()})
            if model is None:
                from faster_whisper import WhisperModel
                model = WhisperModel(str(args.model), device="cpu", compute_type="int8",
                                     cpu_threads=4, num_workers=1, local_files_only=True)
            job["status"] = "running"
            atomic(path, job)
            segments, _ = model.transcribe(
                job["audio"], language="zh", beam_size=1, vad_filter=True,
                condition_on_previous_text=False, repetition_penalty=1.1,
                no_repeat_ngram_size=4)
            job.update(status="done", error="", segments=[
                {"start": s.start, "end": s.end, "text": s.text.strip()} for s in segments])
            atomic(path, job)
            export(folder)
            Path(job["audio"]).unlink(missing_ok=True)
            seen.add(path)
            last_work = time.monotonic()
            atomic(status, {"state": "waiting", "pid": os.getpid()})
        except Exception as exc:
            job.update(status="failed", error=f"{type(exc).__name__}: {exc}")
            atomic(path, job)
            atomic(status, {"state": "error", "error": job["error"], "pid": os.getpid()})
            return 2
    atomic(status, {"state": "released", "pid": 0})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
