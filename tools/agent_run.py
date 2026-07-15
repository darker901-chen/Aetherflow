#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform as host_platform_module
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any


def repo_root_default() -> Path:
    return Path(__file__).resolve().parents[1]


def timestamp_id() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def host_platform() -> str:
    system = host_platform_module.system().lower()
    if system == "windows":
        return "windows"
    if system == "darwin":
        return "macos"
    if system == "linux":
        return "linux"
    return system or "unknown"


def resolve_platform(requested: str) -> str:
    return host_platform() if requested == "auto" else requested


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def run_command(args: list[str | Path], cwd: Path, env: dict[str, str] | None = None) -> tuple[int, str]:
    proc = subprocess.run(
        [str(arg) for arg in args],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout


def git_output(repo_root: Path, args: list[str]) -> str:
    try:
        exit_code, output = run_command(["git", "-C", repo_root, *args], cwd=repo_root)
        if exit_code == 0:
            return output.strip()
    except OSError:
        pass
    return ""


def copy_templates(repo_root: Path, run_dir: Path) -> None:
    template_dir = repo_root / ".aetherflow" / "templates"
    # Markdown templates produce .md files; everything else is .json. The plan
    # template lets the Architecture Planning Gate materialize its plan on disk
    # so the Code Review Agent has a real intent artifact to audit against
    # (instead of pulling intent from chat history that may be lost).
    markdown_templates = {"handoff", "plan"}
    for name in ["task_card", "diagnosis", "verify_report", "handoff", "plan"]:
        ext = "md" if name in markdown_templates else "json"
        src = template_dir / f"{name}.template.{ext}"
        dst = run_dir / f"{name}.{ext}"
        if src.exists() and not dst.exists():
            shutil.copyfile(src, dst)


def publish_stable_output(repo_root: Path, produced_video: str | None) -> Path | None:
    """Copy the run's final mp4 to <repo_root>/output/demo.mp4.

    Gives one stable, easy-to-find path to eyeball every run, regardless of the
    per-run .aetherflow/runs/<id>/artifacts/ location. Copy (not move): the
    verifier still reads the run-dir artifact. Best-effort: a missing /
    non-existent / non-mp4 source is skipped silently (never fails the run).
    """
    if not produced_video:
        return None
    src = Path(produced_video)
    if not src.exists() or src.suffix.lower() != ".mp4":
        return None
    try:
        out_dir = repo_root / "output"
        out_dir.mkdir(parents=True, exist_ok=True)
        dst = out_dir / "demo.mp4"
        shutil.copyfile(src, dst)
        return dst
    except OSError:
        return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and run AetherFlow with agent-operable artifacts.")
    parser.add_argument("--repo-root", default=str(repo_root_default()), help="Repository root.")
    parser.add_argument("--build-dir", default="", help="CMake build directory.")
    parser.add_argument("--config", default="Release", help="CMake configuration.")
    parser.add_argument("--platform", choices=["auto", "windows", "macos"], default="auto", help="Target platform gate.")
    parser.add_argument("--run-id", default="", help="Run id under .aetherflow/runs.")
    parser.add_argument("--skip-build", action="store_true", help="Skip cmake build.")
    parser.add_argument("--skip-run", action="store_true", help="Skip AetherFlow.exe runtime execution.")
    # macOS-only passthrough flags. Windows ignores these.
    parser.add_argument("--target-fps", type=int, default=None, help="macOS: target FPS (default 30).")
    parser.add_argument("--duration-frames", type=int, default=None, help="macOS: duration in frames (default 900).")
    parser.add_argument("--cursor-radius", type=int, default=None, help="macOS: cursor ROI radius px (default 200).")
    parser.add_argument("program_args", nargs=argparse.REMAINDER, help="Arguments passed to AetherFlow.exe after --.")
    args = parser.parse_args()
    if args.program_args and args.program_args[0] == "--":
        args.program_args = args.program_args[1:]
    return args


def run_macos(
    *,
    args: argparse.Namespace,
    repo_root: Path,
    build_dir: Path,
    run_id: str,
    run_dir: Path,
    artifact_output_dir: Path,
    console_log: Path,
    manifest_path: Path,
    host: str,
) -> int:
    """Build + run the macOS ScreenCaptureKit/VideoToolbox shim.

    Phase 1 contract:
    - Output container goes to <run_dir>/artifacts/output/output.mp4
    - A byte-identical copy is also published to <repo_root>/output/demo.mp4
      every run (stable, easy-to-find path to eyeball; copy not move — the
      verifier still reads the run-dir artifact).
    - frame_trace.jsonl is written under <run_dir>
    - macos_smoke.json is written under <run_dir>
    - platform_status.json records build/run gate state.
    - run_manifest.json captures cmake_args, runtime_args, exit codes, mp4 path,
      and the stable_output path.
    """
    start_time = datetime.now().astimezone().isoformat()
    build_log = run_dir / "build.log"
    platform_status_path = run_dir / "platform_status.json"
    smoke_path = run_dir / "macos_smoke.json"
    trace_path = run_dir / "frame_trace.jsonl"

    # Honest non-passed token when both stages are skipped on macOS.
    if args.skip_build and args.skip_run:
        scaffolded = {
            "schema_version": 1,
            "platform": "macos",
            "host_platform": host,
            "status": "scaffolded",
            "failure_stage": None,
            "unsupported_reason": None,
            "build_supported": True,
            "smoke_supported": True,
            "note": "Both --skip-build and --skip-run were set; no real run performed.",
        }
        write_json(platform_status_path, scaffolded)
        manifest = {
            "schema_version": 1,
            "run_id": run_id,
            "platform": "macos",
            "start_time": start_time,
            "end_time": datetime.now().astimezone().isoformat(),
            "binary_path": None,
            "cmake_args": [],
            "runtime_args": [],
            "exit_code": 0,
            "summarizer_exit_code": None,
            "output_mp4": None,
            "skipped": ["build", "run"],
        }
        write_json(manifest_path, manifest)
        console_log.write_text("[agent_run] macOS dryrun: build+run skipped\n", encoding="utf-8")
        print(f"[agent_run] macOS scaffolded dryrun. RunDir: {run_dir}")
        return 0

    # ---------- Build ----------
    cmake_configure_args: list[str] = []
    cmake_build_args: list[str] = []
    if not args.skip_build:
        cmake_configure_args = [
            "cmake",
            "-S",
            str(repo_root),
            "-B",
            str(build_dir),
            f"-DCMAKE_BUILD_TYPE={args.config}",
        ]
        configure_exit, configure_output = run_command(cmake_configure_args, cwd=repo_root)
        # Multi-config Xcode generator ignores -DCMAKE_BUILD_TYPE; --config is passed
        # to the build step below. Single-config Make/Ninja honors it. Either way ok.
        cmake_build_args = [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "AetherFlow",
            "--config",
            args.config,
        ]
        build_exit, build_output = run_command(cmake_build_args, cwd=repo_root)
        combined = (
            f"$ {' '.join(cmake_configure_args)}\n"
            f"{configure_output}\n"
            f"$ {' '.join(cmake_build_args)}\n"
            f"{build_output}"
        )
        build_log.write_text(combined, encoding="utf-8")
        if configure_exit != 0 or build_exit != 0:
            failed_step = "cmake configure" if configure_exit != 0 else "cmake build"
            status = {
                "schema_version": 1,
                "platform": "macos",
                "host_platform": host,
                "status": "failed",
                "failure_stage": "build",
                "unsupported_reason": None,
                "build_supported": True,
                "smoke_supported": True,
                "failed_step": failed_step,
                "configure_exit": configure_exit,
                "build_exit": build_exit,
                "log": str(build_log),
            }
            write_json(platform_status_path, status)
            manifest = {
                "schema_version": 1,
                "run_id": run_id,
                "platform": "macos",
                "start_time": start_time,
                "end_time": datetime.now().astimezone().isoformat(),
                "binary_path": None,
                "cmake_args": cmake_configure_args + cmake_build_args,
                "runtime_args": [],
                "exit_code": configure_exit if configure_exit != 0 else build_exit,
                "summarizer_exit_code": None,
                "output_mp4": None,
            }
            write_json(manifest_path, manifest)
            print(f"[agent_run] macOS build failed at {failed_step}. RunDir: {run_dir}")
            return configure_exit if configure_exit != 0 else build_exit

    # ---------- Resolve binary ----------
    binary_candidates = [
        build_dir / "AetherFlow",
        build_dir / args.config / "AetherFlow",
        build_dir / "Release" / "AetherFlow",
        build_dir / "Debug" / "AetherFlow",
    ]
    binary_path: Path | None = None
    for candidate in binary_candidates:
        if candidate.exists() and candidate.is_file():
            binary_path = candidate
            break

    runtime_args: list[str] = []
    summarize_exit: int | None = None
    run_exit = 0

    if not args.skip_run:
        if binary_path is None:
            status = {
                "schema_version": 1,
                "platform": "macos",
                "host_platform": host,
                "status": "failed",
                "failure_stage": "binary-missing",
                "unsupported_reason": None,
                "build_supported": True,
                "smoke_supported": True,
                "searched": [str(p) for p in binary_candidates],
            }
            write_json(platform_status_path, status)
            manifest = {
                "schema_version": 1,
                "run_id": run_id,
                "platform": "macos",
                "start_time": start_time,
                "end_time": datetime.now().astimezone().isoformat(),
                "binary_path": None,
                "cmake_args": cmake_configure_args + cmake_build_args,
                "runtime_args": [],
                "exit_code": 1,
                "summarizer_exit_code": None,
                "output_mp4": None,
            }
            write_json(manifest_path, manifest)
            print(f"[agent_run] macOS binary not found. RunDir: {run_dir}")
            return 1

        # Build runtime arg list, matching MacosPlatformShim CLI flags exactly.
        runtime_args = [
            "--macos-output-dir",
            str(artifact_output_dir),
            "--macos-trace-path",
            str(trace_path),
            "--macos-smoke-path",
            str(smoke_path),
        ]
        if args.target_fps is not None:
            runtime_args.extend(["--target-fps", str(args.target_fps)])
        if args.duration_frames is not None:
            runtime_args.extend(["--duration-frames", str(args.duration_frames)])
        if args.cursor_radius is not None:
            runtime_args.extend(["--cursor-radius", str(args.cursor_radius)])
        # Pass through any user --program_args after these.
        runtime_args.extend(args.program_args)

        child_env = os.environ.copy()
        child_env["AETHERFLOW_OUTPUT_DIR"] = str(artifact_output_dir)
        run_exit, run_output = run_command(
            [str(binary_path), *runtime_args],
            cwd=repo_root,
            env=child_env,
        )
        console_log.write_text(run_output, encoding="utf-8")

        # Permission gate: nonzero exit + smoke says permission denied -> unsupported.
        smoke_data: dict[str, Any] = {}
        if smoke_path.exists():
            try:
                smoke_data = json.loads(smoke_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                smoke_data = {}
        permission = smoke_data.get("screen_capture_permission")

        if run_exit != 0 and permission and permission != "granted":
            status = {
                "schema_version": 1,
                "platform": "macos",
                "host_platform": host,
                "status": "unsupported",
                "failure_stage": None,
                "unsupported_reason": "screen recording permission not granted",
                "build_supported": True,
                "smoke_supported": True,
                "screen_capture_permission": permission,
            }
        elif run_exit != 0:
            status = {
                "schema_version": 1,
                "platform": "macos",
                "host_platform": host,
                "status": "failed",
                "failure_stage": "run",
                "unsupported_reason": None,
                "build_supported": True,
                "smoke_supported": True,
                "exit_code": run_exit,
                "screen_capture_permission": permission,
            }
        else:
            status = {
                "schema_version": 1,
                "platform": "macos",
                "host_platform": host,
                "status": "passed",
                "failure_stage": None,
                "unsupported_reason": None,
                "build_supported": True,
                "smoke_supported": True,
                "screen_capture_permission": permission,
            }
        write_json(platform_status_path, status)
    else:
        # Skip-run: scaffold a "passed-build" status so the verifier can be exercised
        # against pre-existing trace data.
        status = {
            "schema_version": 1,
            "platform": "macos",
            "host_platform": host,
            "status": "scaffolded",
            "failure_stage": None,
            "unsupported_reason": None,
            "build_supported": True,
            "smoke_supported": True,
            "note": "--skip-run: build performed, runtime not executed.",
        }
        write_json(platform_status_path, status)

    # ---------- Summarizer pass ----------
    summarizer = repo_root / "tools" / "agent_summarize.py"
    if summarizer.exists() and trace_path.exists():
        sx, so = run_command(
            [sys.executable, str(summarizer), "--run-dir", str(run_dir)],
            cwd=repo_root,
        )
        summarize_exit = sx
        if so:
            sys.stdout.write(so)

    # ---------- Manifest ----------
    output_mp4: str | None = None
    if smoke_path.exists():
        try:
            smoke_data2 = json.loads(smoke_path.read_text(encoding="utf-8"))
            op = smoke_data2.get("output_path")
            if isinstance(op, str) and op:
                output_mp4 = str(Path(op).resolve()) if Path(op).exists() else op
        except (OSError, json.JSONDecodeError):
            pass

    stable_output = publish_stable_output(repo_root, output_mp4)

    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "platform": "macos",
        "start_time": start_time,
        "end_time": datetime.now().astimezone().isoformat(),
        "binary_path": str(binary_path) if binary_path else None,
        "cmake_args": cmake_configure_args + cmake_build_args,
        "runtime_args": runtime_args,
        "exit_code": run_exit,
        "summarizer_exit_code": summarize_exit,
        "output_mp4": output_mp4,
        "stable_output": str(stable_output) if stable_output else None,
    }
    write_json(manifest_path, manifest)

    # Return the runtime exit code (0 on full success).
    if run_exit != 0:
        print(f"[agent_run] macOS runtime exit={run_exit}. RunDir: {run_dir}")
        return run_exit
    if stable_output:
        print(f"[agent_run] macOS stable output: {stable_output}")
    print(f"[agent_run] macOS RunDir: {run_dir}")
    return 0


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir).resolve() if args.build_dir else repo_root / "build"
    run_id = args.run_id or timestamp_id()
    host = host_platform()
    effective_platform = resolve_platform(args.platform)

    run_root = repo_root / ".aetherflow" / "runs"
    run_dir = run_root / run_id
    artifact_output_dir = run_dir / "artifacts" / "output"
    console_log = run_dir / "console.log"
    manifest_path = run_dir / "run_manifest.json"

    run_dir.mkdir(parents=True, exist_ok=True)
    artifact_output_dir.mkdir(parents=True, exist_ok=True)
    copy_templates(repo_root, run_dir)

    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "created_at": datetime.now().astimezone().isoformat(),
        "repo_root": str(repo_root),
        "build_dir": str(build_dir),
        "config": args.config,
        "git_sha": git_output(repo_root, ["rev-parse", "HEAD"]),
        "git_status_short": git_output(repo_root, ["status", "--short"]),
        "program_args": args.program_args,
        "output_dir": str(artifact_output_dir),
        "platform": {
            "requested": args.platform,
            "host": host,
            "effective": effective_platform,
        },
    }
    # Record the intended frame count ONLY when the AETHERFLOW_MAX_FRAMES env
    # override is set (run_full_test.sh --seconds / --minutes / --frames set
    # it) so the verifier's throughput gates (frame_throughput_ge_90pct /
    # fast_path_not_blocked) compare against what THIS run targeted instead of
    # the Config.h default. When unset, omit the field so the verifier keeps
    # its existing 900 default (unchanged behavior). Windows-only: the macOS
    # runtime uses options.durationFrames, not this env var, so recording an
    # env-derived value there would be wrong -- hence the env-presence guard.
    max_frames_env = os.environ.get("AETHERFLOW_MAX_FRAMES")
    if max_frames_env and effective_platform == "windows":
        try:
            manifest["max_frames"] = int(max_frames_env)
        except ValueError:
            pass
    write_json(manifest_path, manifest)

    if effective_platform == "macos":
        return run_macos(
            args=args,
            repo_root=repo_root,
            build_dir=build_dir,
            run_id=run_id,
            run_dir=run_dir,
            artifact_output_dir=artifact_output_dir,
            console_log=console_log,
            manifest_path=manifest_path,
            host=host,
        )

    if effective_platform != "windows":
        reason = f"Unsupported target platform for agent_run.py: {effective_platform}"
        platform_status = {
            "schema_version": 1,
            "platform": effective_platform,
            "host_platform": host,
            "status": "unsupported",
            "build_supported": False,
            "smoke_supported": False,
            "unsupported_reason": reason,
            "expected_future_artifacts": [],
        }
        write_json(run_dir / "platform_status.json", platform_status)
        console_log.write_text(reason + "\n", encoding="utf-8")
        print(f"[agent_run] {reason} RunDir: {run_dir}")
        return 0 if args.skip_build and args.skip_run else 20

    if not args.skip_build:
        build_log = run_dir / "build.log"
        build_exit, build_output = run_command(
            ["cmake", "--build", build_dir, "--config", args.config, "--target", "AetherFlow"],
            cwd=repo_root,
        )
        build_log.write_text(build_output, encoding="utf-8")
        if build_exit != 0:
            print(f"[agent_run] Build failed. RunDir: {run_dir}")
            return build_exit

    if not args.skip_run:
        exe_path = build_dir / args.config / "AetherFlow.exe"
        if not exe_path.exists():
            raise FileNotFoundError(f"AetherFlow.exe not found: {exe_path}")

        child_env = os.environ.copy()
        child_env["AETHERFLOW_OUTPUT_DIR"] = str(artifact_output_dir)
        if not child_env.get("AETHERFLOW_LIVE_LOG_INTERVAL"):
            child_env["AETHERFLOW_LIVE_LOG_INTERVAL"] = "60"

        run_exit, run_output = run_command([exe_path, *args.program_args], cwd=repo_root, env=child_env)
        console_log.write_text(run_output, encoding="utf-8")

        trace_src = artifact_output_dir / "traces" / "frame_trace.jsonl"
        if trace_src.exists():
            shutil.copyfile(trace_src, run_dir / "frame_trace.jsonl")

        summarizer = repo_root / "tools" / "agent_summarize.py"
        if summarizer.exists():
            summarize_exit, summarize_output = run_command(
                [sys.executable, summarizer, "--run-dir", run_dir],
                cwd=repo_root,
            )
            if summarize_exit != 0:
                if summarize_output:
                    sys.stdout.write(summarize_output)
                return summarize_exit

        if run_exit != 0:
            print(f"[agent_run] Runtime failed with exit code {run_exit}. RunDir: {run_dir}")
            return run_exit

    print(f"[agent_run] RunDir: {run_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
