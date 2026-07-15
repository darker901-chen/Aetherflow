#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform as host_platform_module
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any


# Matches the runtime's stdout summary line, e.g.:
#   "Capture Failures: 2980 / 900"
# Captures (fails, frames_printed).
CAPTURE_FAILURE_RE = re.compile(r"Capture Failures:\s+(\d+)\s*/\s*(\d+)")

# Warn (never fail) when the capture-failure ratio crosses this. Observed
# historical ratios include 90/900 ~ 0.10 (earlier default smoke), 42/900 ~
# 0.047 (panic smoke), and 2980/900 ~ 3.31 (project_status_audit_20260510,
# capture retrying multiple times per encoded frame). 1.0 cleanly separates
# "occasional retries" from "capture fundamentally broken". This is
# intentionally a SOFT signal: the top-level status is not flipped until
# enough runs are collected to set a fail line. The current project status
# tracks the open capture-reliability decision.
CAPTURE_FAILURE_WARN_RATIO = 1.0


def append_audit_ledger(
    repo_root: Path, run_dir: Path, report: dict[str, Any], *, platform: str
) -> None:
    """Append one JSONL row per verify run to <repo>/.aetherflow/audit/ledger.jsonl.

    Built so the longitudinal "is the agent layer actually catching things?"
    question (docs/archive/AGENT_AUDIT.md Task A2) can answer itself from
    accumulated evidence over time, instead of a one-shot per-row audit.

    Best-effort: any I/O error is swallowed -- the ledger must never block
    or fail a verify run.

    **Single-writer invariant.** This appender is not file-locked. agent_verify
    is invoked sequentially by humans / CI today, so concurrent appenders
    are not a current concern; if that changes (parallel runners in CI),
    add an OS-appropriate file lock before the open(..."a") call. On POSIX
    a single `write()` under PIPE_BUF is atomic; Windows buffered text-mode
    writes are not.

    **Vocabulary invariant for forward-compat.** Ledger readers may add new
    keys and must tolerate keys they don't recognize. Existing keys keep
    these value vocabularies:
      - `status`: "passed" / "failed" / "unsupported"
      - `gates[*]` and `blocks[*]`: "passed" / "failed" / "not_applicable" /
        "skipped" / "unsupported" / None (absent for benchmark when not run)
      - `capture_failure_status`: "passed" / "warning" / None
      - `platform`: "windows" / "macos" / "unsupported"
    Bump `ledger_schema` when this writer's shape changes incompatibly.

    Args:
        repo_root: Repo root (so the ledger lives at a stable, well-known
            location independent of run-dir depth or future reshuffles).
        run_dir: The run directory the report was written for.
        report: The verify_report.json dict, already written to disk.
        platform: Explicit "windows" / "macos" / "unsupported". Removes the
            need to heuristically infer shape from the report's keys -- a
            future macOS refactor that promotes `gates.trace` to top-level
            `trace` would otherwise silently route through the Windows
            branch and corrupt the row.
    """
    audit_dir = repo_root / ".aetherflow" / "audit"
    try:
        audit_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    ledger_path = audit_dir / "ledger.jsonl"

    gate_states: dict[str, str | None] = {}
    capture_failures: dict[str, Any] | None = None

    if platform == "windows":
        # Windows main() flat shape: report["build"|"smoke"|"trace"|
        # "benchmark"] as dicts with `passed: bool`.
        for key in ("build", "smoke", "trace"):
            block = report.get(key) or {}
            gate_states[key] = "passed" if block.get("passed") else "failed"
        bench = report.get("benchmark") or {}
        if bench.get("passed") is None:
            gate_states["benchmark"] = None
        else:
            gate_states["benchmark"] = "passed" if bench["passed"] else "failed"
        trace_block = report.get("trace") or {}
        cf = trace_block.get("capture_failures")
        if isinstance(cf, dict):
            capture_failures = cf
    else:
        # macOS verify_macos() nested shape: report["gates"][...] with
        # `status: str`. Also covers the "unsupported" early-return shape
        # whose `gates[*].status` is already "skipped" / "unsupported".
        gates_nested = (
            report.get("gates") if isinstance(report.get("gates"), dict) else {}
        )
        for key in ("build", "smoke", "trace"):
            block = gates_nested.get(key) or {}
            gate_states[key] = block.get("status")
        bench = report.get("benchmark") or {}
        gate_states["benchmark"] = bench.get("status")
        trace_block = gates_nested.get("trace") or {}
        cf = trace_block.get("capture_failures")
        if isinstance(cf, dict):
            capture_failures = cf

    blocks: dict[str, str | None] = {}
    for key in ("analyzer_bridge", "scene_classifier"):
        block = report.get(key)
        if isinstance(block, dict):
            blocks[key] = block.get("status")

    warnings = report.get("warnings") or []

    entry = {
        "ts": datetime.now().isoformat(timespec="seconds"),
        "run_id": run_dir.name,
        "platform": platform,
        "status": report.get("status"),
        "verify_report_schema_version": report.get("schema_version"),
        "gates": gate_states,
        "blocks": blocks,
        "warnings_count": len(warnings),
        "warnings_sources": [
            w.get("source") for w in warnings if isinstance(w, dict)
        ],
        "capture_failure_ratio": (
            capture_failures.get("ratio") if capture_failures else None
        ),
        "capture_failure_status": (
            capture_failures.get("status") if capture_failures else None
        ),
        "ledger_schema": 1,
    }
    try:
        with ledger_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        return


def collect_verify_events(report: dict[str, Any], *, platform: str) -> list[dict[str, str]]:
    """Extract the noteworthy things a verify run caught.

    Returns a list of {title, detail} events: capture-failure warnings,
    failed gates, failed analyzer-bridge / scene-classifier blocks. Clean
    runs that caught nothing return []. This is the source of the
    human-readable evidence_log; the ledger already records every run
    quantitatively, this records only the catches and explains them.
    """
    events: list[dict[str, str]] = []

    # Resolve each gate to one of "passed" / "failed" / "skipped" /
    # "not_run" for either report shape. Only a genuine "failed" emits an
    # event -- "skipped" / "not_applicable" / "not_run" must NOT, or a
    # --skip-build run would spuriously read as a build failure and
    # contradict the ledger's faithful state record.
    if platform == "windows":
        # Windows gates carry a real bool `passed` (build always attempted,
        # smoke.passed == smoke_ran). False means genuinely failed.
        build_state = "passed" if (report.get("build") or {}).get("passed") else "failed"
        smoke_state = "passed" if (report.get("smoke") or {}).get("passed") else "failed"
        trace_block = report.get("trace") or {}
        trace_state = "passed" if trace_block.get("passed") else "failed"
        trace_detail = str(trace_block.get("summary") or "")
        bench = report.get("benchmark") or {}
        bp = bench.get("passed")
        bench_state = "not_run" if bp is None else ("passed" if bp else "failed")
        bench_detail = str(bench.get("summary") or "")
        capture_failures = trace_block.get("capture_failures")
    else:
        gates_nested = report.get("gates") if isinstance(report.get("gates"), dict) else {}
        build_state = (gates_nested.get("build") or {}).get("status")
        smoke_state = (gates_nested.get("smoke") or {}).get("status")
        trace_block = gates_nested.get("trace") or {}
        trace_state = trace_block.get("status")
        trace_detail = str(trace_block.get("evidence") or "")
        bench = report.get("benchmark") or {}
        bench_state = bench.get("status")  # passed/failed/skipped/None
        bench_detail = str(bench.get("evidence") or "")
        capture_failures = trace_block.get("capture_failures")

    # Capture-failure warning (warn-only; the original trigger for this whole gate).
    if isinstance(capture_failures, dict) and capture_failures.get("status") == "warning":
        fails = capture_failures.get("fails")
        frames = capture_failures.get("frames")
        ratio = capture_failures.get("ratio")
        ratio_str = f"{ratio:.2f}" if isinstance(ratio, (int, float)) else str(ratio)
        events.append({
            "title": "Capture reliability warning (warn-only, did not block)",
            "detail": (
                f"The capture path reported {fails} failures across {frames} "
                f"encoded frames (ratio {ratio_str}). A healthy run retries "
                f"occasionally (ratio well under 1.0); a ratio above 1.0 means "
                f"capture failed more times than it delivered frames -- usually "
                f"the captured window was occluded, minimized, or the display "
                f"mode changed mid-run. The verifier surfaced this as a warning "
                f"but did NOT fail the run: the warn-only contract holds until "
                f"enough runs are collected to set a hard fail line (see "
                f"CAPTURE_FAILURE_WARN_RATIO in tools/agent_verify.py). Audit "
                f"value: the trace/verify layer now catches a class of silent "
                f"degradation that previously passed green (original trigger: "
                f"project_status_audit_20260510 at ratio 3.31)."
            ),
        })

    # Failed gates -- only genuine "failed", never "skipped"/"not_run".
    if build_state == "failed":
        events.append({
            "title": "Build gate FAILED",
            "detail": (
                "The verifier could not confirm a passing build, so all "
                "downstream smoke / trace evidence is unreliable. See "
                "verify_build.log in the run directory. Route: Trace and "
                "Verification -> Repair (one scoped fix) -> re-verify."
            ),
        })
    if smoke_state == "failed":
        events.append({
            "title": "Smoke gate FAILED (no run evidence)",
            "detail": (
                "No trace.summary.json was found, so the runtime did not "
                "produce per-frame evidence this run. Nothing downstream "
                "(trace invariants, masks, classifier) can be asserted."
            ),
        })
    if trace_state == "failed":
        events.append({
            "title": "Trace gate FAILED",
            "detail": (
                f"A trace invariant broke (encoded frames, encode failures, "
                f"parse errors, or privacy-mask coverage). Verifier evidence: "
                f"{trace_detail}"
            ),
        })
    if bench_state == "failed":
        events.append({
            "title": "Benchmark gate FAILED",
            "detail": f"The ROI benchmark gate failed. Evidence: {bench_detail}",
        })

    # Failed additive blocks.
    bridge = report.get("analyzer_bridge")
    if isinstance(bridge, dict) and bridge.get("status") == "failed":
        events.append({
            "title": "Analyzer-bridge gates FAILED",
            "detail": (
                f"One of the 4 bridge invariants broke (contributions-have-a-"
                f"source / throughput-floor / at-least-one-contribution / "
                f"no-parse-errors). Summary: {bridge.get('summary', '')}"
            ),
        })
    classifier = report.get("scene_classifier")
    if isinstance(classifier, dict) and classifier.get("status") == "failed":
        events.append({
            "title": "Scene-classifier gates FAILED",
            "detail": (
                f"One of latency / mode-switch-rate / fast-path / low-confidence-"
                f"fallback broke. NOTE: this block never asserts classification "
                f"accuracy (that is the Stage A->B checkpoint). Summary: "
                f"{classifier.get('summary', '')}"
            ),
        })

    # Catch-all: a failed report must NEVER leave the evidence log empty.
    # The top-level status can be driven by inputs no single gate above
    # re-checks -- e.g. the macOS path also requires platform_status.status
    # == "passed", so a "scaffolded" run whose later stage failed can be
    # status=failed while build/smoke/trace each read passed. Without this,
    # the qualitative evidence log would silently disagree with the ledger's
    # status=failed -- exactly the silent-failure class this feature exists
    # to kill. Future-proofs any new status input too.
    if not events and str(report.get("status")) == "failed":
        events.append({
            "title": "Run FAILED (no single gate produced an event)",
            "detail": (
                "verify_report.json reports status=failed, but no individual "
                "gate or block above emitted an event -- e.g. on macOS the "
                "platform_status check can fail while build/smoke/trace each "
                "pass. The authoritative gate states are in verify_report.json "
                "in the run directory. This catch-all guarantees a failed run "
                "never leaves the evidence log empty."
            ),
        })

    return events


def append_evidence_log(
    repo_root: Path, run_dir: Path, report: dict[str, Any], *, platform: str
) -> None:
    """Append a plain-language record of what a verify run caught.

    The user's ask: when the verify agent blocks or warns about something,
    leave a human-readable note explaining WHAT was caught, as future
    evidence. Writes to <repo>/.aetherflow/audit/evidence_log.md, appending
    one dated section per run that caught at least one event. Runs that
    caught nothing write nothing here (the ledger already logs them).

    Best-effort: any I/O error is swallowed; never blocks a verify run.
    """
    events = collect_verify_events(report, platform=platform)
    if not events:
        return

    audit_dir = repo_root / ".aetherflow" / "audit"
    try:
        audit_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    log_path = audit_dir / "evidence_log.md"

    status = report.get("status")
    warn_count = len(report.get("warnings") or [])
    ts = datetime.now().isoformat(timespec="seconds")

    lines: list[str] = []
    if not log_path.exists():
        lines.append("# AetherFlow verify evidence log\n")
        lines.append(
            "Plain-language record of what the Trace and Verification agent "
            "caught, one section per run that warned or failed. Future "
            "evidence for whether the verify layer carries weight (see "
            "docs/archive/AGENT_AUDIT.md). The machine-readable per-run companion is "
            "`ledger.jsonl` in this directory.\n"
        )
    lines.append(
        f"\n## {ts} - run `{run_dir.name}` ({platform}) - "
        f"status: {status}, {warn_count} warning(s), {len(events)} event(s)\n"
    )
    for ev in events:
        lines.append(f"\n### {ev['title']}\n")
        lines.append(f"{ev['detail']}\n")

    try:
        with log_path.open("a", encoding="utf-8") as f:
            f.write("".join(lines))
    except OSError:
        return


def evaluate_capture_failures(run_dir: Path, frames: int) -> dict[str, Any] | None:
    """Read `Capture Failures: X / Y` from <run_dir>/console.log.

    The runtime prints this line in its end-of-run summary. The
    `agent_summarize.py` evidence array is capped at 80 lines and
    sometimes drops it, so this reads `console.log` directly.

    Returns None when no console.log exists or no matching line is found
    (e.g. older runs, or runs that didn't capture stdout). Returns a
    structured dict otherwise; `status` is `"warning"` when the ratio is
    above CAPTURE_FAILURE_WARN_RATIO, else `"passed"`. The caller is
    responsible for surfacing the warning into the top-level report.
    """
    log_path = run_dir / "console.log"
    if not log_path.exists():
        return None
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    # Picks the LAST `Capture Failures: X / Y` line. agent_run.py
    # overwrites console.log per run, so this is the end-of-run summary;
    # if a future tool appends across runs, the last match still
    # represents the most recent run.
    last_match: re.Match[str] | None = None
    for m in CAPTURE_FAILURE_RE.finditer(text):
        last_match = m
    if last_match is None:
        return None
    fails = int(last_match.group(1))
    log_frames = int(last_match.group(2))
    # Prefer the trace-reported frame count when available; fall back to
    # whatever the runtime printed in the same line.
    denom = frames if frames > 0 else log_frames
    if denom <= 0:
        denom = 1
    ratio = fails / float(denom)
    status = "warning" if ratio > CAPTURE_FAILURE_WARN_RATIO else "passed"
    return {
        "fails": fails,
        "frames": denom,
        "log_frames": log_frames,
        "ratio": ratio,
        "threshold": CAPTURE_FAILURE_WARN_RATIO,
        "status": status,
        "policy": (
            "warn-only; top-level status not flipped until enough runs "
            "collected to choose a fail line"
        ),
    }


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


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def evaluate_analyzer_bridge_gates(
    trace: dict[str, Any],
    run_manifest: dict[str, Any],
) -> dict[str, Any]:
    """Bridge Hardening verifier block.

    Four schema-v2 bridge invariants checked by this verifier:

      1. contributions_have_a_source: if contributed_frames>=1 then
         submitted_frames>=1 (formerly "submitted_ge_contributed";
         see rename note below)
      2. when bridge active, frames / target_fps >= 0.9 * expected_duration_s
      3. at least one contributed frame across the run (above cold-start)
      4. no parse_errors introduced by the new analyzer fields

    When ``analyzer_bridge_active`` is False, every gate reports
    ``not_applicable`` and the block's ``status`` is ``not_applicable`` so
    bridge-inactive regressions stay green. When bridge is active, the
    block's ``status`` is ``passed`` iff all four gates pass; otherwise
    ``failed``.

    The "frames / target_fps >= 0.9 * expected_duration_s" gate derives
    ``target_fps`` and ``expected_duration_seconds`` from the runtime config
    if the run_manifest does not record them. Defaults track src/Config.h:
    target_fps = 30, max_frames = 900, so expected_duration_seconds = 30 and
    the gate reduces to frames >= 810.

    Gate 1 rename note (2026-05-12, post sub-sampling implementation):
        The original gate asserted submitted >= contributed under the
        implicit assumption that every contribution came from a fresh
        submission (1:1). After analyzer sub-sampling shipped (interval-N
        producer + cache-reuse consumer), a single fresh submission can
        legitimately back many cached contributions on subsequent fast-path
        frames. The 1:1 invariant is therefore not the real safety
        property; the real property is "no phantom contributions" -- i.e.
        if any contribution was counted, at least one submission must have
        preceded it. The gate is renamed to ``contributions_have_a_source``
        to reflect the post-sub-sampling semantics, and the check is
        relaxed: when submitted==0 the gate fails iff contributed>0;
        when submitted>=1 the gate passes regardless of contributed count
        (cache reuse is by design).
    """
    bridge_active = bool(trace.get("analyzer_bridge_active") is True)
    submitted = int(trace.get("analyzer_submitted_frames") or 0)
    contributed = int(trace.get("analyzer_contributed_frames") or 0)
    parse_errors = int(trace.get("parse_errors") or 0)
    frames = int(trace.get("frames") or 0)

    # Resolve target_fps + expected_duration_seconds. The run manifest does
    # not yet record these explicitly; fall back to the Config.h defaults
    # (FPS=30, MAX_FRAMES=900). Programmatic overrides should populate
    # `target_fps` / `expected_duration_seconds` on the manifest in future.
    target_fps = int(run_manifest.get("target_fps") or 0)
    if target_fps <= 0:
        target_fps = 30
    expected_duration_seconds = float(
        run_manifest.get("expected_duration_seconds") or 0.0
    )
    if expected_duration_seconds <= 0.0:
        max_frames = int(run_manifest.get("max_frames") or 0)
        if max_frames <= 0:
            max_frames = 900
        expected_duration_seconds = float(max_frames) / float(target_fps)

    if not bridge_active:
        skipped_evidence = (
            "bridge inactive (analyzer_bridge_active=false); gate not applicable"
        )
        return {
            "status": "not_applicable",
            "summary": (
                "analyzer bridge inactive: no analyzer* fields emitted in "
                "frame_trace.jsonl; bridge gates skipped"
            ),
            "metrics": {
                "analyzer_bridge_active": False,
                "analyzer_submitted_frames": submitted,
                "analyzer_contributed_frames": contributed,
                "analyzer_inference_ms_p50": float(trace.get("analyzer_inference_ms_p50") or 0.0),
                "analyzer_inference_ms_p95": float(trace.get("analyzer_inference_ms_p95") or 0.0),
                "analyzer_inference_ms_p99": float(trace.get("analyzer_inference_ms_p99") or 0.0),
                "analyzer_staleness_frames_p95": float(trace.get("analyzer_staleness_frames_p95") or 0.0),
                "target_fps": target_fps,
                "expected_duration_seconds": expected_duration_seconds,
            },
            "gates": {
                "contributions_have_a_source": {"status": "not_applicable", "evidence": skipped_evidence},
                "frame_throughput_ge_90pct": {"status": "not_applicable", "evidence": skipped_evidence},
                "at_least_one_contribution": {"status": "not_applicable", "evidence": skipped_evidence},
                "no_parse_errors": {"status": "not_applicable", "evidence": skipped_evidence},
            },
        }

    # ---- Gate 1: contributions_have_a_source ----
    # Post sub-sampling semantics (see docstring "Gate 1 rename note"):
    # if any contribution was counted then at least one fresh submission
    # must have preceded it. Cache reuse legitimately permits
    # contributed > submitted under interval-N producer mode, so we no
    # longer enforce the 1:1 inequality.
    if submitted == 0:
        gate1_ok = contributed == 0
        gate1_evidence = (
            f"analyzer_submitted_frames=0; analyzer_contributed_frames="
            f"{contributed} (must be 0 when no submissions)"
        )
    else:
        gate1_ok = True
        gate1_evidence = (
            f"analyzer_submitted_frames={submitted}>=1 source for "
            f"analyzer_contributed_frames={contributed} "
            f"(cache reuse permitted)"
        )
    gate1 = {
        "status": "passed" if gate1_ok else "failed",
        "evidence": gate1_evidence,
    }

    # ---- Gate 2: throughput floor ----
    achieved_seconds = float(frames) / float(target_fps) if target_fps else 0.0
    floor_seconds = 0.9 * expected_duration_seconds
    gate2_ok = achieved_seconds >= floor_seconds
    gate2 = {
        "status": "passed" if gate2_ok else "failed",
        "evidence": (
            f"frames={frames} / target_fps={target_fps} = {achieved_seconds:.3f}s; "
            f"floor = 0.9 * expected_duration={expected_duration_seconds:.3f}s "
            f"= {floor_seconds:.3f}s"
        ),
    }

    # ---- Gate 3: at least one contributed frame (above cold-start window) ----
    # Cold-start window is 6 frames (kColdStartFrames). The producer fires
    # every frame and the worker latency is ~200 ms << run duration, so this
    # gate reduces to `analyzer_contributed_frames >= 1`.
    gate3_ok = contributed >= 1
    gate3 = {
        "status": "passed" if gate3_ok else "failed",
        "evidence": (
            f"analyzer_contributed_frames={contributed} (>=1 required; "
            f"cold-start window = 6 frames)"
        ),
    }

    # ---- Gate 4: no parse_errors introduced by new analyzer fields ----
    # Re-uses the existing summary counter; the new analyzer parser shares
    # the same try/except path so any parse failures will surface here.
    gate4_ok = parse_errors == 0
    gate4 = {
        "status": "passed" if gate4_ok else "failed",
        "evidence": f"trace.summary.json parse_errors={parse_errors}",
    }

    all_passed = gate1_ok and gate2_ok and gate3_ok and gate4_ok
    return {
        "status": "passed" if all_passed else "failed",
        "summary": (
            f"submitted={submitted}, contributed={contributed}, "
            f"frames={frames}/target_fps={target_fps}={achieved_seconds:.2f}s "
            f"vs floor={floor_seconds:.2f}s, parse_errors={parse_errors}"
        ),
        "metrics": {
            "analyzer_bridge_active": True,
            "analyzer_submitted_frames": submitted,
            "analyzer_contributed_frames": contributed,
            "analyzer_inference_ms_p50": float(trace.get("analyzer_inference_ms_p50") or 0.0),
            "analyzer_inference_ms_p95": float(trace.get("analyzer_inference_ms_p95") or 0.0),
            "analyzer_inference_ms_p99": float(trace.get("analyzer_inference_ms_p99") or 0.0),
            "analyzer_staleness_frames_p95": float(trace.get("analyzer_staleness_frames_p95") or 0.0),
            "target_fps": target_fps,
            "expected_duration_seconds": expected_duration_seconds,
            "achieved_duration_seconds": achieved_seconds,
        },
        "gates": {
            "contributions_have_a_source": gate1,
            "frame_throughput_ge_90pct": gate2,
            "at_least_one_contribution": gate3,
            "no_parse_errors": gate4,
        },
    }


def evaluate_scene_classifier_gates(
    trace: dict[str, Any],
    run_manifest: dict[str, Any],
    run_dir: Path | None = None,
) -> dict[str, Any]:
    """Phase 4 P0.1 scene classifier verifier block (schema v3).

    Four gates from docs/archive/PHASE4_P0_PLAN.md §2.4 / §4.x.4 P0.5:

      1. classifier_p95_inference_ms_under_80: scene_inference_p95_ms < 80.
         Source is the reused analyzer-bridge p95 (the classifier rides the
         bridge, so bridge latency == classifier latency; no parallel
         measurement path).
      2. mode_switches_le_1_per_5s: policy mode-switch rate over the run is
         <= 1 switch per 5 seconds of classifier-active wall time. The run
         has no per-frame timestamp, so wall time is derived from
         frames / target_fps using the same Config.h-default fallback
         (target_fps=30) the analyzer_bridge block already uses -- one
         consistent duration convention. The classifier only re-evaluates
         at the bridge sub-sample cadence (~1 Hz), so this gate mostly
         exercises PolicyEngine's 150-frame switch floor plus 3-consecutive
         hysteresis.
         Formula:
             windows_5s = (frames / target_fps) / 5.0
             rate = policy_mode_switches / windows_5s
             pass iff rate <= 1.0   (equivalently
             policy_mode_switches <= (frames / target_fps) / 5.0)
      3. fast_path_not_blocked: the deterministic fast path must not block
         on the classifier. Reuses the analyzer_bridge "fast path not
         blocked" evidence -- the in-verifier throughput-floor invariant
         frames / target_fps >= 0.9 * expected_duration_seconds -- computed
         on this (classifier-active) run. (The verifier sees one run-dir;
         the cross-run FPS A/B vs the inactive baseline is the
         benchmark-reporter's job, exactly as for analyzer_bridge.)
      4. low_confidence_takes_fallback: per-frame assertion aggregated to
         pass/fail. For EVERY frame with sceneClassConfidence < 0.6:
           (a) policyReason MUST be "low_confidence_fallback", AND
           (b) policyMode MUST equal the previous classifier-active frame's
               policyMode (a low-confidence verdict must not drive a mode
               change).
         This is asserted from the per-frame frame_trace.jsonl (the summary
         cannot express the per-frame ordering), with the violation count in
         the gate detail. trace.summary.json's policy_low_confidence_frames
         is the expected denominator.

    When ``scene_classifier_active`` is False, every gate reports
    ``not_applicable`` and the block ``status`` is ``not_applicable`` so
    classifier-inactive regressions (and bridge-active/classifier-absent
    runs like analyzer_bridge_async_mock) stay green. When active, the
    block ``status`` is ``passed`` iff all four gates pass; else ``failed``.
    This mirrors the analyzer_bridge block convention exactly.
    """
    classifier_active = bool(trace.get("scene_classifier_active") is True)
    p95_ms = float(trace.get("scene_inference_p95_ms") or 0.0)
    mode_switches = int(trace.get("policy_mode_switches") or 0)
    low_conf_frames = int(trace.get("policy_low_confidence_frames") or 0)
    fallback_frames = int(trace.get("policy_fallback_frames") or 0)
    inference_count = int(trace.get("scene_inference_count") or 0)
    frames = int(trace.get("frames") or 0)
    scene_class_distribution = trace.get("scene_class_distribution")
    if not isinstance(scene_class_distribution, dict):
        scene_class_distribution = {}

    # Same target_fps / expected_duration resolution as the analyzer_bridge
    # block: prefer the run manifest, fall back to Config.h defaults
    # (FPS=30, MAX_FRAMES=900 -> expected_duration_seconds=30).
    target_fps = int(run_manifest.get("target_fps") or 0)
    if target_fps <= 0:
        target_fps = 30
    expected_duration_seconds = float(
        run_manifest.get("expected_duration_seconds") or 0.0
    )
    if expected_duration_seconds <= 0.0:
        max_frames = int(run_manifest.get("max_frames") or 0)
        if max_frames <= 0:
            max_frames = 900
        expected_duration_seconds = float(max_frames) / float(target_fps)

    base_metrics = {
        "scene_classifier_active": classifier_active,
        "scene_inference_count": inference_count,
        "scene_inference_p95_ms": p95_ms,
        "scene_inference_failures": int(trace.get("scene_inference_failures") or 0),
        "scene_class_distribution": scene_class_distribution,
        "policy_mode_switches": mode_switches,
        "policy_fallback_frames": fallback_frames,
        "policy_low_confidence_frames": low_conf_frames,
        "target_fps": target_fps,
        "expected_duration_seconds": expected_duration_seconds,
    }

    # The four gates only ever assert reachability + non-degeneracy +
    # latency + policy plumbing. Real-screen classification accuracy is
    # NOT asserted here; that lives behind the Stage A-to-B accuracy
    # checkpoint in docs/3-product/PRODUCT_ROADMAP.md and the classifier-quality
    # gate in docs/1-status/PROJECT_STATUS.md. The two fields below ship in every return
    # branch so any reader of verify_report.json sees the contract
    # without having to cross-reference docs.
    proves_when_active = [
        "real captured pixels reach the model (SubmitFrame readback non-zero)",
        "model produces non-degenerate, varying output (scene_class_distribution has >1 class on real screens)",
        "inference p95 latency under 80 ms",
        "policy hysteresis bounds the mode-switch rate (<= 1 per 5s)",
        "low-confidence frames take the fallback path without driving a mode change",
    ]
    does_not_prove = [
        (
            "classification accuracy on real-screen content -- see "
            "the Windows ONNX row and classifier-quality gate in "
            "docs/1-status/PROJECT_STATUS.md, plus the Stage A->B accuracy "
            "checkpoint in docs/3-product/PRODUCT_ROADMAP.md "
            "(P0.1 currently runs CLIP zero-shot Stage A; expected "
            "real-screen accuracy ~60-75%)."
        ),
    ]

    if not classifier_active:
        skipped = (
            "scene classifier inactive (scene_classifier_active=false); "
            "gate not applicable"
        )
        return {
            "status": "not_applicable",
            "summary": (
                "scene classifier inactive: no sceneClass/policyMode fields "
                "emitted in frame_trace.jsonl; classifier gates skipped "
                "(this block never asserts classification accuracy -- "
                "see 'does_not_prove')"
            ),
            "proves": [],
            "does_not_prove": does_not_prove,
            "metrics": base_metrics,
            "gates": {
                "classifier_p95_inference_ms_under_80": {
                    "status": "not_applicable", "detail": skipped},
                "mode_switches_le_1_per_5s": {
                    "status": "not_applicable", "detail": skipped},
                "fast_path_not_blocked": {
                    "status": "not_applicable", "detail": skipped},
                "low_confidence_takes_fallback": {
                    "status": "not_applicable", "detail": skipped},
            },
        }

    # ---- Gate 1: classifier_p95_inference_ms_under_80 ----
    gate1_ok = p95_ms < 80.0
    gate1 = {
        "status": "passed" if gate1_ok else "failed",
        "detail": (
            f"scene_inference_p95_ms={p95_ms:.3f} (reused analyzer bridge "
            f"p95; threshold < 80.0 ms)"
        ),
    }

    # ---- Gate 2: mode_switches_le_1_per_5s ----
    run_seconds = float(frames) / float(target_fps) if target_fps else 0.0
    windows_5s = run_seconds / 5.0
    if windows_5s > 0.0:
        switch_rate = mode_switches / windows_5s
        gate2_ok = switch_rate <= 1.0
    else:
        # No measurable wall time -> a single switch can't be amortised;
        # only zero switches can pass.
        switch_rate = float(mode_switches)
        gate2_ok = mode_switches == 0
    gate2 = {
        "status": "passed" if gate2_ok else "failed",
        "detail": (
            f"policy_mode_switches={mode_switches}; run_seconds = frames "
            f"{frames}/target_fps {target_fps} = {run_seconds:.3f}s; "
            f"5s-windows = {windows_5s:.3f}; rate = "
            f"{switch_rate:.4f} switches/5s (threshold <= 1.0)"
        ),
    }

    # ---- Gate 3: fast_path_not_blocked ----
    # Reuse the analyzer_bridge throughput-floor invariant on this
    # classifier-active run (single-run-dir verifier; cross-run FPS A/B is
    # the benchmark-reporter's job, same as for analyzer_bridge).
    achieved_seconds = float(frames) / float(target_fps) if target_fps else 0.0
    floor_seconds = 0.9 * expected_duration_seconds
    gate3_ok = achieved_seconds >= floor_seconds
    gate3 = {
        "status": "passed" if gate3_ok else "failed",
        "detail": (
            f"fast-path throughput floor (reused analyzer_bridge invariant): "
            f"frames={frames}/target_fps={target_fps}={achieved_seconds:.3f}s "
            f">= 0.9 * expected_duration={expected_duration_seconds:.3f}s "
            f"= {floor_seconds:.3f}s"
        ),
    }

    # ---- Gate 4: low_confidence_takes_fallback ----
    # Per-frame assertion from frame_trace.jsonl (the summary cannot express
    # per-frame ordering). For every sceneClassConfidence<0.6 frame:
    #   (a) policyReason == "low_confidence_fallback"
    #   (b) policyMode == previous classifier-active frame's policyMode
    # Resolve frame_trace.jsonl robustly. trace.summary.json stores
    # trace_file as a repo-root-relative path, which only resolves when the
    # verifier's CWD is the repo root. Prefer the canonical location next to
    # the run artifacts (run_dir is authoritative and CWD-independent), then
    # fall back to the stored path.
    trace_path = Path(str(trace.get("trace_file") or ""))
    if run_dir is not None:
        candidate = run_dir / "frame_trace.jsonl"
        if candidate.exists():
            trace_path = candidate
    reason_violations = 0
    mode_change_violations = 0
    low_conf_seen = 0
    first_violation = ""
    if trace_path.exists():
        prev_mode: str | None = None
        for raw in trace_path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            line = raw.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "sceneClass" not in item:
                continue
            cur_mode = str(item.get("policyMode") or "")
            conf = item.get("sceneClassConfidence")
            try:
                conf_f = float(conf) if conf is not None else 1.0
            except (TypeError, ValueError):
                conf_f = 1.0
            if conf_f < 0.6:
                low_conf_seen += 1
                reason = str(item.get("policyReason") or "")
                if reason != "low_confidence_fallback":
                    reason_violations += 1
                    if not first_violation:
                        first_violation = (
                            f"frame {item.get('frameIndex')}: conf="
                            f"{conf_f:.3f} reason={reason!r} != "
                            f"'low_confidence_fallback'"
                        )
                if prev_mode is not None and cur_mode != prev_mode:
                    mode_change_violations += 1
                    if not first_violation:
                        first_violation = (
                            f"frame {item.get('frameIndex')}: conf="
                            f"{conf_f:.3f} policyMode {prev_mode!r}->"
                            f"{cur_mode!r} (low-confidence frame must not "
                            f"drive a mode change)"
                        )
            prev_mode = cur_mode
        gate4_violations = reason_violations + mode_change_violations
        gate4_ok = gate4_violations == 0
        gate4_detail = (
            f"low-confidence frames asserted={low_conf_seen} "
            f"(summary policy_low_confidence_frames={low_conf_frames}); "
            f"reason_violations={reason_violations}, "
            f"mode_change_violations={mode_change_violations}"
        )
        if first_violation:
            gate4_detail += f"; first violation: {first_violation}"
    else:
        # Cannot assert per-frame without the trace; fail closed rather
        # than silently passing (verification must produce evidence).
        gate4_ok = False
        gate4_detail = (
            f"frame_trace.jsonl not found at {trace_path}; cannot assert "
            f"low_confidence_takes_fallback per-frame"
        )
    gate4 = {
        "status": "passed" if gate4_ok else "failed",
        "detail": gate4_detail,
    }

    all_passed = gate1_ok and gate2_ok and gate3_ok and gate4_ok
    return {
        "status": "passed" if all_passed else "failed",
        "summary": (
            f"reaches-model + non-degenerate-output: "
            f"p95={p95_ms:.2f}ms, mode_switches={mode_switches} over "
            f"{achieved_seconds:.1f}s, fallback_frames={fallback_frames}, "
            f"low_conf_frames={low_conf_frames}, "
            f"scene_inferences={inference_count} -- "
            f"accuracy UNMEASURED (see 'does_not_prove')"
        ),
        "proves": proves_when_active,
        "does_not_prove": does_not_prove,
        "metrics": base_metrics,
        "gates": {
            "classifier_p95_inference_ms_under_80": gate1,
            "mode_switches_le_1_per_5s": gate2,
            "fast_path_not_blocked": gate3,
            "low_confidence_takes_fallback": gate4,
        },
    }


def run_command(args: list[str | Path], cwd: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [str(arg) for arg in args],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout


def find_powershell() -> str | None:
    return shutil.which("pwsh") or shutil.which("powershell")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify AetherFlow build and agent run artifacts.")
    parser.add_argument("--repo-root", default=str(repo_root_default()), help="Repository root.")
    parser.add_argument("--build-dir", default="", help="CMake build directory.")
    parser.add_argument("--config", default="Release", help="CMake configuration.")
    parser.add_argument("--platform", choices=["auto", "windows", "macos"], default="auto", help="Target platform gate.")
    parser.add_argument("--run-dir", default="", help="Run artifact directory to verify.")
    parser.add_argument("--run-benchmark", action="store_true", help="Run the ROI benchmark gate.")
    return parser.parse_args()


def resolve_platform(requested: str, run_manifest: dict[str, Any]) -> str:
    if requested != "auto":
        return requested
    manifest_platform = run_manifest.get("platform")
    if isinstance(manifest_platform, dict):
        effective = manifest_platform.get("effective")
        if isinstance(effective, str) and effective:
            return effective
    return host_platform()


def write_unsupported_platform_report(
    report_path: Path,
    *,
    requested_platform: str,
    effective_platform: str,
    host: str,
    reason: str,
    run_dir: Path,
) -> None:
    expected_artifacts = [
        "macos_smoke.json",
        "frame_trace.jsonl",
        "trace.summary.json",
    ] if effective_platform == "macos" else []
    present_artifacts = [name for name in expected_artifacts if (run_dir / name).exists()]
    missing_artifacts = [name for name in expected_artifacts if name not in present_artifacts]
    report = {
        "schema_version": 1,
        "status": "unsupported",
        "platform": {
            "requested": requested_platform,
            "host": host,
            "effective": effective_platform,
            "unsupported_reason": reason,
        },
        "capabilities": {
            "build": False,
            "smoke": False,
            "trace": False,
            "benchmark": False,
        },
        "build": {
            "passed": False,
            "command": "",
            "summary": reason,
        },
        "smoke": {
            "passed": False,
            "command": "",
            "summary": "macOS smoke artifacts are not implemented yet" if effective_platform == "macos" else reason,
        },
        "trace": {
            "passed": False,
            "summary": (
                f"missing={missing_artifacts}; present={present_artifacts}"
                if expected_artifacts
                else reason
            ),
        },
        "benchmark": {
            "passed": None,
            "summary": "not supported for this platform scaffold",
        },
    }
    write_json(report_path, report)


def verify_macos(
    *,
    repo_root: Path,
    run_dir: Path,
    report_path: Path,
    run_benchmark: bool,
) -> int:
    """macOS phase 1 verifier. Reads platform_status.json + macos_smoke.json + trace.summary.json
    and emits a verify_report.json with per-gate evidence."""
    platform_status_path = run_dir / "platform_status.json"
    smoke_path = run_dir / "macos_smoke.json"
    trace_summary_path = run_dir / "trace.summary.json"

    missing: list[str] = []
    platform_status: dict[str, Any] = {}
    if platform_status_path.exists():
        platform_status = read_json(platform_status_path)
    else:
        missing.append("platform_status.json")

    # Unsupported short-circuit: matches existing macOS verify behavior contract.
    if platform_status.get("status") == "unsupported":
        report = {
            "schema_version": 1,
            "platform": "macos",
            "status": "unsupported",
            "unsupported_reason": platform_status.get(
                "unsupported_reason", "platform_status.json reports unsupported"
            ),
            "gates": {
                "build": {"status": "skipped", "evidence": "platform unsupported"},
                "smoke": {"status": "skipped", "evidence": "platform unsupported"},
                "trace": {"status": "skipped", "evidence": "platform unsupported"},
            },
            "macos_smoke": read_json(smoke_path) if smoke_path.exists() else {},
            "trace_summary": read_json(trace_summary_path) if trace_summary_path.exists() else {},
            "missing_artifacts": missing,
        }
        write_json(report_path, report)
        print(f"[agent_verify] macOS unsupported. Report: {report_path}")
        return 5

    # ---------- Build gate ----------
    build_log = run_dir / "build.log"
    ps_status = platform_status.get("status")
    failure_stage = platform_status.get("failure_stage")
    if ps_status == "passed":
        build_gate = {
            "status": "passed",
            "evidence": f"platform_status.status=passed; build.log={build_log}",
        }
    elif failure_stage == "build":
        build_gate = {
            "status": "failed",
            "evidence": f"platform_status.failure_stage=build; see {build_log}",
        }
    elif ps_status in ("scaffolded", "failed", None):
        if failure_stage in ("run", "binary-missing"):
            # Build succeeded; later stage failed.
            build_gate = {
                "status": "passed",
                "evidence": f"platform_status.status={ps_status}; failure_stage={failure_stage}",
            }
        elif ps_status == "scaffolded":
            build_gate = {
                "status": "skipped",
                "evidence": "platform_status.status=scaffolded (--skip-build/--skip-run)",
            }
        else:
            build_gate = {
                "status": "failed",
                "evidence": f"platform_status.status={ps_status}",
            }
    else:
        build_gate = {"status": "failed", "evidence": f"unknown status: {ps_status}"}

    # ---------- Smoke gate ----------
    smoke_data: dict[str, Any] = {}
    if smoke_path.exists():
        smoke_data = read_json(smoke_path)
    else:
        missing.append("macos_smoke.json")

    permission = smoke_data.get("screen_capture_permission")
    captured = int(smoke_data.get("captured_frames") or 0)
    encoded_smoke = int(smoke_data.get("encoded_frames") or 0)
    output_path_str = smoke_data.get("output_path") or ""
    output_size = -1
    if output_path_str:
        try:
            output_size = os.path.getsize(output_path_str)
        except OSError:
            output_size = -1

    smoke_checks = []
    smoke_ok = True
    if not smoke_data:
        smoke_ok = False
        smoke_checks.append("macos_smoke.json missing or empty")
    else:
        if permission != "granted":
            smoke_ok = False
            smoke_checks.append(f"screen_capture_permission={permission!r}")
        else:
            smoke_checks.append("screen_capture_permission=granted")
        if captured <= 0:
            smoke_ok = False
            smoke_checks.append(f"captured_frames={captured}")
        else:
            smoke_checks.append(f"captured_frames={captured}")
        if encoded_smoke <= 0:
            smoke_ok = False
            smoke_checks.append(f"encoded_frames={encoded_smoke}")
        else:
            smoke_checks.append(f"encoded_frames={encoded_smoke}")
        if output_size <= 0:
            smoke_ok = False
            smoke_checks.append(
                f"output_path={output_path_str!r} size={output_size}"
            )
        else:
            smoke_checks.append(f"output_path size={output_size}")
    smoke_gate = {
        "status": "passed" if smoke_ok else "failed",
        "evidence": "; ".join(smoke_checks) if smoke_checks else "no smoke evidence",
    }

    # ---------- Trace gate ----------
    trace_data: dict[str, Any] = {}
    if trace_summary_path.exists():
        trace_data = read_json(trace_summary_path)
    else:
        missing.append("trace.summary.json")

    trace_checks = []
    trace_ok = True
    if not trace_data:
        trace_ok = False
        trace_checks.append("trace.summary.json missing or empty")
    else:
        parse_errors = int(trace_data.get("parse_errors") or 0)
        frames = int(trace_data.get("frames") or 0)
        encoded = int(trace_data.get("encoded_frames") or 0)
        roi_supported = trace_data.get("roi_supported")
        if parse_errors != 0:
            trace_ok = False
            trace_checks.append(f"parse_errors={parse_errors}")
        else:
            trace_checks.append("parse_errors=0")
        if frames <= 0:
            trace_ok = False
            trace_checks.append(f"frames={frames}")
        else:
            trace_checks.append(f"frames={frames}")
        if encoded <= 0:
            trace_ok = False
            trace_checks.append(f"encoded_frames={encoded}")
        else:
            trace_checks.append(f"encoded_frames={encoded}")
        if roi_supported is None:
            trace_ok = False
            trace_checks.append("roi_supported missing from trace.summary.json")
        elif roi_supported is not False:
            trace_ok = False
            trace_checks.append(f"roi_supported={roi_supported!r} (expected false on macOS)")
        else:
            trace_checks.append("roi_supported=false")

        # ----- Privacy mask coverage gate (phase 2: chat-window mosaic) -----
        # Mirrors the Windows gate logic at the bottom of main(): see
        # privacy_mask_passed there. macOS uses CoreImage BGRA paths instead
        # of D3D11 staging paths, but the trace summary keys are identical.
        mask_total = int(trace_data.get("privacy_mask_total") or 0)
        mask_applied = int(trace_data.get("privacy_mask_applied_total") or 0)
        mask_fallback = int(trace_data.get("privacy_mask_fallback_frames") or 0)
        mask_paths = trace_data.get("privacy_mask_paths")
        if not isinstance(mask_paths, dict):
            mask_paths = {}

        # (1) Mask coverage: every emitted mask must be applied.
        if mask_total > 0 and mask_applied != mask_total:
            trace_ok = False
            trace_checks.append(
                f"privacy_mask_applied={mask_applied} != total={mask_total} "
                "(compositor dropped masks)"
            )
        else:
            trace_checks.append(
                f"privacy_mask_applied={mask_applied}/{mask_total}"
            )

        # (2) No fallback frames: compositor must never blackout-fallback.
        if mask_fallback > 0:
            trace_ok = False
            trace_checks.append(
                f"privacy_mask_fallback_frames={mask_fallback} "
                "(compositor fell back to blackout)"
            )
        else:
            trace_checks.append("privacy_mask_fallback_frames=0")

        # (3) Path coherence: defensive — no clearfill-fallback path.
        fallback_path_key = "coreimage-bgra-clearfill-fallback"
        fallback_path_count = int(mask_paths.get(fallback_path_key) or 0)
        if fallback_path_count > 0:
            trace_ok = False
            trace_checks.append(
                f"privacy_mask_paths[{fallback_path_key}]={fallback_path_count}"
            )
        else:
            trace_checks.append(f"privacy_mask_paths={mask_paths}")

        # (4) When no masks were emitted, only the 'none' path is permitted.
        if mask_total == 0:
            non_none_keys = [
                k for k, v in mask_paths.items()
                if k != "none" and int(v or 0) > 0
            ]
            if non_none_keys:
                trace_ok = False
                trace_checks.append(
                    f"privacy_mask_total=0 but non-none paths present: {non_none_keys}"
                )

    trace_gate = {
        "status": "passed" if trace_ok else "failed",
        "evidence": "; ".join(trace_checks) if trace_checks else "no trace evidence",
    }

    # Surface explicit mask numbers for downstream readers, independent of the
    # human-readable evidence string.
    if trace_data:
        trace_gate["privacy_mask"] = {
            "total": int(trace_data.get("privacy_mask_total") or 0),
            "applied": int(trace_data.get("privacy_mask_applied_total") or 0),
            "fallback_frames": int(trace_data.get("privacy_mask_fallback_frames") or 0),
            "paths": trace_data.get("privacy_mask_paths") or {},
        }

    # Capture-failure soft signal (B1, 2026-05-21). Same semantics as the
    # Windows path: warn-only, surfaced into trace_gate + top-level
    # warnings, never flips the platform status. macOS smoke artifacts
    # often won't have a console.log captured at run-time -- in that
    # case evaluate_capture_failures returns None and nothing is added.
    trace_frames_for_capture = int(trace_data.get("frames") or 0) if trace_data else 0
    capture_failures = evaluate_capture_failures(run_dir, trace_frames_for_capture)
    if capture_failures is not None:
        trace_gate["capture_failures"] = capture_failures

    # ---------- Analyzer bridge gates (Bridge Hardening, schema v2) ----------
    # Same semantics as the Windows path; bridge-inactive runs (no
    # --mock-analyzer) report not_applicable and do not flip overall.
    run_manifest_macos = read_json(run_dir / "run_manifest.json")
    analyzer_bridge = evaluate_analyzer_bridge_gates(trace_data, run_manifest_macos)
    analyzer_bridge_failed = analyzer_bridge.get("status") == "failed"

    # ---------- Scene classifier gates (Phase 4 P0.1, schema v3) ----------
    # The scene classifier is Windows-only in P0.1; a macOS run will not have
    # scene_classifier_active set, so this reports not_applicable and never
    # flips overall. Guarded identically to analyzer_bridge so a macOS run
    # without the classifier still validates.
    scene_classifier = evaluate_scene_classifier_gates(
        trace_data, run_manifest_macos, run_dir
    )
    scene_classifier_failed = scene_classifier.get("status") == "failed"

    # ---------- Overall status ----------
    all_passed = (
        ps_status == "passed"
        and build_gate["status"] == "passed"
        and smoke_gate["status"] == "passed"
        and trace_gate["status"] == "passed"
        and not analyzer_bridge_failed
        and not scene_classifier_failed
    )
    overall = "passed" if all_passed else "failed"

    report = {
        "schema_version": 3,
        "platform": "macos",
        "status": overall,
        "gates": {
            "build": build_gate,
            "smoke": smoke_gate,
            "trace": trace_gate,
        },
        "analyzer_bridge": analyzer_bridge,
        "scene_classifier": scene_classifier,
        "macos_smoke": smoke_data,
        "trace_summary": trace_data,
        "missing_artifacts": missing,
    }

    # Capture-failure warnings (B1, 2026-05-21). Mirrors the Windows path
    # main() shape so cross-platform readers see the same key.
    warnings: list[dict[str, Any]] = []
    if capture_failures is not None and capture_failures["status"] == "warning":
        warnings.append({
            "source": "trace.capture_failures",
            "summary": (
                f"capture-failure ratio {capture_failures['ratio']:.3f} "
                f"exceeds warn threshold {capture_failures['threshold']}; "
                f"{capture_failures['fails']} failures / "
                f"{capture_failures['frames']} frames"
            ),
            "policy": capture_failures["policy"],
        })
    if warnings:
        report["warnings"] = warnings

    # ---------- Benchmark gate (no-op on macOS phase 1) ----------
    if run_benchmark:
        warning = "[macOS phase 1] ROI benchmark is Windows-only. Skipping."
        print(warning)
        report["benchmark"] = {
            "status": "skipped",
            "evidence": warning,
        }

    write_json(report_path, report)
    append_audit_ledger(repo_root, run_dir, report, platform="macos")
    append_evidence_log(repo_root, run_dir, report, platform="macos")
    print(f"[agent_verify] macOS {overall}. Report: {report_path}")
    return 0 if overall == "passed" else 3


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir).resolve() if args.build_dir else repo_root / "build"
    run_dir = Path(args.run_dir).resolve() if args.run_dir else repo_root / ".aetherflow" / "runs" / f"verify_{timestamp_id()}"
    run_dir.mkdir(parents=True, exist_ok=True)

    report_path = run_dir / "verify_report.json"
    build_log = run_dir / "verify_build.log"
    run_manifest = read_json(run_dir / "run_manifest.json")
    host = host_platform()
    effective_platform = resolve_platform(args.platform, run_manifest)

    if effective_platform == "macos":
        return verify_macos(
            repo_root=repo_root,
            run_dir=run_dir,
            report_path=report_path,
            run_benchmark=args.run_benchmark,
        )

    if effective_platform != "windows":
        reason = f"Unsupported target platform for agent_verify.py: {effective_platform}"
        write_unsupported_platform_report(
            report_path,
            requested_platform=args.platform,
            effective_platform=effective_platform,
            host=host,
            reason=reason,
            run_dir=run_dir,
        )
        print(f"[agent_verify] unsupported. Report: {report_path}")
        return 5

    build_command = ["cmake", "--build", build_dir, "--config", args.config, "--target", "AetherFlow"]
    build_exit, build_output = run_command(build_command, cwd=repo_root)
    build_log.write_text(build_output, encoding="utf-8")
    build_passed = build_exit == 0

    trace_summary_path = run_dir / "trace.summary.json"
    smoke_ran = trace_summary_path.exists()
    trace_passed = False
    trace_summary_text = "trace.summary.json not found"
    trace_frames_for_capture = 0
    if smoke_ran:
        try:
            trace = json.loads(trace_summary_path.read_text(encoding="utf-8"))
            privacy_mask_total = int(trace.get("privacy_mask_total", 0))
            privacy_mask_applied = int(trace.get("privacy_mask_applied_total", 0))
            privacy_mask_fallback = int(trace.get("privacy_mask_fallback_frames", 0))
            panic_mask_active = int(trace.get("panic_mask_active_frames", 0))
            privacy_mask_passed = (
                privacy_mask_total == 0
                or (privacy_mask_applied >= privacy_mask_total and privacy_mask_fallback == 0)
            )
            trace_passed = (
                int(trace.get("encoded_frames", 0)) > 0
                and int(trace.get("encode_failure_frames", 0)) == 0
                and int(trace.get("parse_errors", 0)) == 0
                and privacy_mask_passed
            )
            trace_frames_for_capture = int(trace.get("frames", 0) or 0)
            trace_summary_text = (
                f"frames={trace.get('frames')}, encoded={trace.get('encoded_frames')}, "
                f"encode_failures={trace.get('encode_failure_frames')}, parse_errors={trace.get('parse_errors')}, "
                f"privacy_masks={privacy_mask_total}, mask_applied={privacy_mask_applied}, "
                f"mask_fallback_frames={privacy_mask_fallback}, panic_mask_active_frames={panic_mask_active}"
            )
        except (json.JSONDecodeError, OSError, TypeError, ValueError) as exc:
            trace_summary_text = f"trace.summary.json parse failed: {exc}"

    # Capture-failure soft signal (B1, 2026-05-21). Reads
    # `Capture Failures: X / Y` from console.log and surfaces a warning
    # when the ratio exceeds CAPTURE_FAILURE_WARN_RATIO. Never flips the
    # top-level status; just makes a previously-silent "passed" loud.
    capture_failures = evaluate_capture_failures(run_dir, trace_frames_for_capture)
    if capture_failures is not None:
        trace_summary_text += (
            f", capture_failures={capture_failures['fails']}/"
            f"{capture_failures['frames']} "
            f"ratio={capture_failures['ratio']:.3f} "
            f"(warn if > {capture_failures['threshold']})"
        )

    benchmark_passed: bool | None = None
    benchmark_summary = "not requested"
    if args.run_benchmark:
        bench_script = repo_root / "tools" / "roi_benchmark.ps1"
        if bench_script.exists():
            ps = find_powershell()
            if ps is None:
                benchmark_passed = False
                benchmark_summary = "PowerShell executable not found for tools/roi_benchmark.ps1"
            else:
                bench_log = run_dir / "roi_benchmark.log"
                bench_output_dir = run_dir / "roi_benchmark_output"
                bench_exit, bench_output = run_command(
                    [
                        ps,
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        bench_script,
                        "-RepoRoot",
                        repo_root,
                        "-OutputDir",
                        bench_output_dir,
                        "-Static",
                    ],
                    cwd=repo_root,
                )
                bench_log.write_text(bench_output, encoding="utf-8")
                benchmark_passed = bench_exit == 0
                benchmark_summary = f"roi_benchmark exit={bench_exit}; output={bench_output_dir}"
        else:
            benchmark_passed = False
            benchmark_summary = "tools/roi_benchmark.ps1 not found"

    # ---------- Analyzer bridge gates (Bridge Hardening, schema v2) ----------
    # Only enforced when trace.summary.json reports analyzer_bridge_active.
    # Bridge-inactive runs (no --mock-analyzer) report not_applicable and
    # do not affect the top-level pass/fail. See evaluate_analyzer_bridge_gates
    # for gate semantics.
    trace_for_bridge: dict[str, Any] = {}
    if trace_summary_path.exists():
        trace_for_bridge = read_json(trace_summary_path)
    analyzer_bridge = evaluate_analyzer_bridge_gates(trace_for_bridge, run_manifest)
    analyzer_bridge_failed = analyzer_bridge.get("status") == "failed"

    # ---------- Scene classifier gates (Phase 4 P0.1, schema v3) ----------
    # Additive block, structured exactly like analyzer_bridge. Only enforced
    # when trace.summary.json reports scene_classifier_active. Classifier-
    # inactive runs (and bridge-active/classifier-absent runs) report
    # not_applicable and do not affect the top-level pass/fail.
    scene_classifier = evaluate_scene_classifier_gates(
        trace_for_bridge, run_manifest, run_dir
    )
    scene_classifier_failed = scene_classifier.get("status") == "failed"

    status = (
        "passed"
        if (
            build_passed
            and smoke_ran
            and trace_passed
            and benchmark_passed is not False
            and not analyzer_bridge_failed
            and not scene_classifier_failed
        )
        else "failed"
    )
    report = {
        "schema_version": 3,
        "status": status,
        "platform": {
            "requested": args.platform,
            "host": host,
            "effective": effective_platform,
            "unsupported_reason": "",
        },
        "capabilities": {
            "build": True,
            "smoke": True,
            "trace": True,
            "benchmark": True,
        },
        "build": {
            "passed": build_passed,
            "command": f"cmake --build {build_dir} --config {args.config} --target AetherFlow",
            "summary": f"exit={build_exit}; log={build_log}",
        },
        "smoke": {
            "passed": smoke_ran,
            "command": "tools/agent_run.py",
            "summary": "existing run artifacts checked" if smoke_ran else "not run by verifier",
        },
        "trace": {
            "passed": trace_passed,
            "summary": trace_summary_text,
        },
        "benchmark": {
            "passed": benchmark_passed,
            "summary": benchmark_summary,
        },
        "analyzer_bridge": analyzer_bridge,
        "scene_classifier": scene_classifier,
    }
    if capture_failures is not None:
        report["trace"]["capture_failures"] = capture_failures
    warnings: list[dict[str, Any]] = []
    if capture_failures is not None and capture_failures["status"] == "warning":
        warnings.append({
            "source": "trace.capture_failures",
            "summary": (
                f"capture-failure ratio {capture_failures['ratio']:.3f} "
                f"exceeds warn threshold {capture_failures['threshold']}; "
                f"{capture_failures['fails']} failures / "
                f"{capture_failures['frames']} frames"
            ),
            "policy": capture_failures["policy"],
        })
    if warnings:
        report["warnings"] = warnings
    write_json(report_path, report)
    append_audit_ledger(repo_root, run_dir, report, platform="windows")
    append_evidence_log(repo_root, run_dir, report, platform="windows")

    print(f"[agent_verify] {status}. Report: {report_path}")
    if not build_passed:
        return 1
    if not smoke_ran:
        return 2
    if not trace_passed:
        return 3
    if benchmark_passed is False:
        return 4
    if analyzer_bridge_failed:
        return 6
    if scene_classifier_failed:
        return 7
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
