#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


PATTERNS = [
    "ERROR",
    "FAILED",
    "Failed",
    "failed",
    "Warning",
    "WARNING",
    "DROP",
    "MFX_ERR",
    "NV_ENC_ERR",
    "Capture Failures",
    "No frames were successfully encoded",
    "Encoder failed",
]


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def to_int(value: Any) -> int:
    try:
        if value is None:
            return 0
        return int(value)
    except (TypeError, ValueError):
        return 0


def to_float(value: Any) -> float:
    try:
        if value is None:
            return 0.0
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _percentile(values: list[float], pct: float) -> float:
    """Linear-interpolated percentile over an unordered list.

    Used by the Bridge Hardening analyzer aggregates. Returns 0.0 when the
    sample is empty so bridge-inactive runs emit deterministic zero
    aggregates without leaking NaN into trace.summary.json.
    """
    if not values:
        return 0.0
    if pct <= 0.0:
        return float(min(values))
    if pct >= 100.0:
        return float(max(values))
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    frac = rank - lo
    return float(ordered[lo] + (ordered[hi] - ordered[lo]) * frac)


def detect_platform_and_roi(run_dir: Path) -> tuple[str, bool]:
    """Deterministic, file-based platform detection.

    Rules:
    - If <run_dir>/macos_smoke.json exists -> ("macos", False).
    - Else if run_manifest.json platform.effective is "macos" -> ("macos", False).
    - Else if platform_status.json platform == "macos" -> ("macos", False).
    - Else default to ("windows", True) for backward compatibility.
    """
    macos_smoke = run_dir / "macos_smoke.json"
    if macos_smoke.exists():
        return ("macos", False)

    manifest_path = run_dir / "run_manifest.json"
    if manifest_path.exists():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            plat = manifest.get("platform")
            if isinstance(plat, dict):
                effective = plat.get("effective")
                if isinstance(effective, str) and effective == "macos":
                    return ("macos", False)
            elif isinstance(plat, str) and plat == "macos":
                return ("macos", False)
        except (OSError, json.JSONDecodeError):
            pass

    status_path = run_dir / "platform_status.json"
    if status_path.exists():
        try:
            status = json.loads(status_path.read_text(encoding="utf-8"))
            if status.get("platform") == "macos":
                return ("macos", False)
        except (OSError, json.JSONDecodeError):
            pass

    return ("windows", True)


def summarize_run(run_dir: Path, max_evidence_lines: int = 80) -> None:
    if not run_dir.exists():
        raise FileNotFoundError(f"RunDir not found: {run_dir}")

    console_log = run_dir / "console.log"
    console_summary = run_dir / "console.summary.json"
    trace_path = run_dir / "frame_trace.jsonl"
    trace_summary = run_dir / "trace.summary.json"
    handoff_path = run_dir / "handoff.md"

    platform_name, roi_supported = detect_platform_and_roi(run_dir)

    pattern_lowers = [pattern.lower() for pattern in PATTERNS]
    evidence: list[dict[str, Any]] = []

    if console_log.exists():
        for line_no, line in enumerate(console_log.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line_lower = line.lower()
            if any(pattern in line_lower for pattern in pattern_lowers):
                if len(evidence) < max_evidence_lines:
                    evidence.append(
                        {
                            "source": "console.log",
                            "line": line_no,
                            "text": line,
                        }
                    )

    console_obj = {
        "schema_version": 1,
        "evidence_count": len(evidence),
        "evidence": evidence,
        "token_safety": {
            "max_evidence_lines": max_evidence_lines,
            "summary_is_navigation_only": True,
        },
    }
    write_json(console_summary, console_obj)

    frames = 0
    encoded = 0
    failed = 0
    quality_regions = 0
    privacy_masks = 0
    privacy_masks_applied = 0
    privacy_mask_fallback_frames = 0
    panic_mask_active_frames = 0
    mask_ms_total = 0.0
    mask_ms_max = 0.0
    decision_sources: dict[str, int] = {}
    scene_sources: dict[str, int] = {}
    privacy_mask_sources: dict[str, int] = {}
    privacy_mask_paths: dict[str, int] = {}
    parse_errors = 0

    # Bridge Hardening (schema v2): analyzer bridge aggregates.
    # The four analyzer* fields are emitted per-frame only when the async
    # analyzer bridge is active (strategy A: conditional emission). On a
    # no-mock / bridge-inactive run, the fields are absent on every line and
    # `analyzer_bridge_active` resolves to False with zeroed aggregates.
    analyzer_bridge_active = False
    analyzer_submitted_frames = 0
    analyzer_contributed_frames = 0
    analyzer_inference_ms_values: list[float] = []
    analyzer_staleness_frames_values: list[int] = []

    # Phase 4 P0.1 scene classifier + policy aggregates (schema v3).
    # The scene/policy per-frame fields (sceneClass, sceneClassConfidence,
    # policyMode, policyReason) are emitted per-frame only when the scene
    # classifier is active (strategy A: conditional emission, same convention
    # as the analyzer* bridge fields). On a classifier-inactive run the
    # fields are absent on every line and `scene_classifier_active` resolves
    # to False with zeroed aggregates -- mirroring the analyzer_bridge
    # precedent exactly so consumers have one consistent convention.
    #
    # NOTE: when the classifier IS active, these fields are present on EVERY
    # frame (the PolicyEngine runs every frame once the classifier is wired);
    # `sceneClass` carries the post-merge scene, which on deterministic-
    # producer frames (e.g. notification-producer, confidence=1.0) and
    # baseline frames (confidence=0.5) is NOT a classifier-owned verdict.
    # `scene_inference_count` therefore counts only frames whose scene was
    # contributed by the classifier (sceneSource == "scene-classifier-onnx"),
    # not every frame that merely carries a sceneClass field.
    scene_classifier_active = False
    scene_inference_count = 0
    scene_inference_failures = 0
    scene_class_distribution: dict[str, int] = {}
    policy_mode_switches = 0
    policy_fallback_frames = 0
    policy_low_confidence_frames = 0
    _prev_policy_mode: str | None = None
    # policyReason values that mean "the classifier verdict was NOT trusted
    # this frame; the deterministic/last-stable decision was used instead".
    _policy_fallback_reasons = {"low_confidence_fallback", "panic_override"}

    if trace_path.exists():
        for raw_line in trace_path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw_line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
                frames += 1
                if item.get("encodeOk") is True:
                    encoded += 1
                else:
                    failed += 1

                quality_regions += to_int(item.get("qualityRegionCount"))
                privacy_masks += to_int(item.get("privacyMaskCount"))
                privacy_masks_applied += to_int(item.get("privacyMaskAppliedCount"))
                if item.get("privacyMaskFallbackUsed") is True:
                    privacy_mask_fallback_frames += 1
                if item.get("panicMaskActive") is True:
                    panic_mask_active_frames += 1
                mask_ms = to_float(item.get("maskMs"))
                mask_ms_total += mask_ms
                mask_ms_max = max(mask_ms_max, mask_ms)

                decision_source = str(item.get("decisionSource") or "")
                decision_sources[decision_source] = decision_sources.get(decision_source, 0) + 1

                scene_source = str(item.get("sceneSource") or "")
                scene_sources[scene_source] = scene_sources.get(scene_source, 0) + 1

                privacy_mask_source = str(item.get("privacyMaskSource") or "")
                privacy_mask_sources[privacy_mask_source] = privacy_mask_sources.get(privacy_mask_source, 0) + 1

                privacy_mask_path = str(item.get("privacyMaskPath") or "")
                privacy_mask_paths[privacy_mask_path] = privacy_mask_paths.get(privacy_mask_path, 0) + 1

                # ----- analyzer bridge per-frame fields -----
                # Field-presence is the bridge-active signal. On strategy-A
                # bridge-inactive runs `analyzerSubmitted` is absent.
                if "analyzerSubmitted" in item:
                    analyzer_bridge_active = True
                    if item.get("analyzerSubmitted") is True:
                        analyzer_submitted_frames += 1
                    if item.get("analyzerContributed") is True:
                        analyzer_contributed_frames += 1
                        # Only frames that actually merged a result carry a
                        # meaningful inference latency + staleness; the
                        # producer leaves both at 0 on non-contributing
                        # frames (cold start, queue empty).
                        try:
                            analyzer_inference_ms_values.append(
                                to_float(item.get("analyzerInferenceMs"))
                            )
                            analyzer_staleness_frames_values.append(
                                to_int(item.get("analyzerStalenessFrames"))
                            )
                        except (TypeError, ValueError):
                            parse_errors += 1

                # ----- scene classifier + policy per-frame fields (v3) -----
                # Field-presence is the classifier-active signal, exactly
                # like `analyzerSubmitted` for the bridge. On strategy-A
                # classifier-inactive runs `sceneClass` is absent.
                if "sceneClass" in item:
                    scene_classifier_active = True

                    scene_class = str(item.get("sceneClass") or "")
                    if scene_class:
                        scene_class_distribution[scene_class] = (
                            scene_class_distribution.get(scene_class, 0) + 1
                        )

                    # A frame "the classifier contributed a scene" is one
                    # whose merged scene was owned by the classifier. We key
                    # on sceneSource == "scene-classifier-onnx" -- the same
                    # canonical stamp src/main.cpp uses to recover the
                    # scene fields (PHASE4_P0_PLAN.md D3). Deterministic
                    # producers (confidence=1.0) and baseline (0.5) frames
                    # still carry a sceneClass but are NOT classifier
                    # inferences, so they are excluded here.
                    if scene_source == "scene-classifier-onnx":
                        scene_inference_count += 1

                    confidence = to_float(item.get("sceneClassConfidence"))
                    if confidence < 0.6:
                        policy_low_confidence_frames += 1

                    policy_reason = str(item.get("policyReason") or "")
                    if policy_reason in _policy_fallback_reasons:
                        policy_fallback_frames += 1

                    policy_mode = str(item.get("policyMode") or "")
                    if (
                        _prev_policy_mode is not None
                        and policy_mode != _prev_policy_mode
                    ):
                        policy_mode_switches += 1
                    _prev_policy_mode = policy_mode
            except (json.JSONDecodeError, AttributeError):
                parse_errors += 1

    mask_ms_avg = (mask_ms_total / frames) if frames else 0.0

    analyzer_inference_ms_p50 = _percentile(analyzer_inference_ms_values, 50.0)
    analyzer_inference_ms_p95 = _percentile(analyzer_inference_ms_values, 95.0)
    analyzer_inference_ms_p99 = _percentile(analyzer_inference_ms_values, 99.0)
    analyzer_staleness_frames_p95 = _percentile(
        [float(v) for v in analyzer_staleness_frames_values], 95.0
    )

    trace_obj = {
        "schema_version": 3,
        "trace_file": str(trace_path),
        "frames": frames,
        "encoded_frames": encoded,
        "encode_failure_frames": failed,
        "quality_region_total": quality_regions,
        "privacy_mask_total": privacy_masks,
        "privacy_mask_applied_total": privacy_masks_applied,
        "privacy_mask_fallback_frames": privacy_mask_fallback_frames,
        "panic_mask_active_frames": panic_mask_active_frames,
        "mask_ms_avg": mask_ms_avg,
        "mask_ms_max": mask_ms_max,
        "scene_sources": scene_sources,
        "decision_sources": decision_sources,
        "privacy_mask_sources": privacy_mask_sources,
        "privacy_mask_paths": privacy_mask_paths,
        "parse_errors": parse_errors,
        "platform": platform_name,
        "roi_supported": roi_supported,
        # Bridge Hardening (schema v2). These keys are always present in
        # trace.summary.json regardless of whether the bridge ran, so
        # downstream readers can key on them unconditionally. The
        # "no new fields when bridge inactive" rule from the recorded plan
        # applies only to per-frame frame_trace.jsonl (strategy A); the
        # summary is an aggregate and intentionally surfaces zeros to make
        # bridge-inactive runs explicit.
        "analyzer_bridge_active": analyzer_bridge_active,
        "analyzer_submitted_frames": analyzer_submitted_frames,
        "analyzer_contributed_frames": analyzer_contributed_frames,
        "analyzer_inference_ms_p50": analyzer_inference_ms_p50,
        "analyzer_inference_ms_p95": analyzer_inference_ms_p95,
        "analyzer_inference_ms_p99": analyzer_inference_ms_p99,
        "analyzer_staleness_frames_p95": analyzer_staleness_frames_p95,
        # Phase 4 P0.1 scene classifier + policy aggregates (schema v3).
        # Like the analyzer_bridge aggregates above, these keys are ALWAYS
        # present in trace.summary.json regardless of whether the classifier
        # ran, so downstream readers can key on them unconditionally. The
        # "no new fields when inactive" rule from the recorded plan applies
        # only to per-frame frame_trace.jsonl (strategy A); the summary is an
        # aggregate and intentionally surfaces zeros to make
        # classifier-inactive runs explicit. This matches the analyzer_bridge
        # precedent exactly (one consistent convention for consumers).
        "scene_classifier_active": scene_classifier_active,
        # Frames whose merged scene was contributed by the classifier
        # (sceneSource == "scene-classifier-onnx"). Deterministic-producer
        # and baseline frames carry a sceneClass but are excluded.
        "scene_inference_count": scene_inference_count,
        # The classifier rides the async analyzer bridge, so the bridge
        # inference-latency p95 IS the classifier inference p95. This is a
        # deliberate reuse (do not recompute a parallel measurement path);
        # 0.0 when the classifier/bridge is inactive.
        "scene_inference_p95_ms": analyzer_inference_ms_p95,
        # No per-frame classifier failure/parse marker exists in
        # frame_trace.jsonl today (a failed inference falls back silently to
        # the deterministic baseline scene). JSON-level parse failures are
        # already surfaced separately via `parse_errors`. This is therefore
        # always 0 until the runtime emits an explicit classifier-failure
        # signal; documented here so the 0 is not mistaken for "no data".
        "scene_inference_failures": scene_inference_failures,
        "scene_class_distribution": scene_class_distribution,
        # Count of classifier-active frames whose policyMode differs from the
        # previous classifier-active frame's policyMode.
        "policy_mode_switches": policy_mode_switches,
        # Count of classifier-active frames whose policyReason indicates the
        # classifier verdict was not trusted (low_confidence_fallback /
        # panic_override).
        "policy_fallback_frames": policy_fallback_frames,
        # Count of classifier-active frames with sceneClassConfidence < 0.6.
        "policy_low_confidence_frames": policy_low_confidence_frames,
    }
    write_json(trace_summary, trace_obj)

    first_evidence = ""
    if evidence:
        first = evidence[0]
        first_evidence = f"- console.log line {first['line']}: {first['text']}"

    handoff = f"""# Handoff

## Goal
Review this run using source/log/trace evidence. Do not patch from this summary alone.

## Tried
Generated run artifacts and compact summaries.

## Result
Frames: {frames}
Encoded frames: {encoded}
Encode failure frames: {failed}

## Evidence
{first_evidence}

## Next Suspect
Use `console.summary.json` and `trace.summary.json` to choose source files from `protocol/COMPONENT_INDEX.md`.

## Must Read Before Patch
- protocol/COMPONENT_INDEX.md
- console.summary.json
- trace.summary.json
"""
    handoff_path.write_text(handoff, encoding="utf-8")

    print(f"[agent_summarize] Wrote summaries in {run_dir}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize AetherFlow agent run artifacts.")
    parser.add_argument("--run-dir", required=True, help="Run artifact directory to summarize.")
    parser.add_argument("--max-evidence-lines", type=int, default=80, help="Maximum console evidence lines to copy.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summarize_run(Path(args.run_dir), args.max_evidence_lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
