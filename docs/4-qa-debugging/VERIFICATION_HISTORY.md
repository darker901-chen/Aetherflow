# AetherFlow Verification History

This file preserves dated verification and implementation evidence. It is historical context, not the current release snapshot. For current capability and risk boundaries, read [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md).

## Dated Evidence Log

> **2026-07-14 — English public-documentation and status-surface cleanup.**
> All tracked first-party documentation and agent configuration is English.
> The public status snapshot was separated from dated verification history;
> the resolved judder handoff was renamed as an investigation record, while
> task-specific handoff remains an ignored run artifact. Static gates passed:
> 56 tracked public Markdown/config files contained zero CJK characters; all
> checked relative Markdown links and heading anchors across 48 first-party
> Markdown files resolved; 7 TOML, 4 JSON, and 1 YAML files parsed;
> architecture sync found
> 59/59 source/header/tool paths indexed, nine runtime registrations aligned,
> and GitHub/Claude/Codex role parity at 7/7/7. Ignored local material remained
> untracked. No runtime, encoder, trace, CLI, environment, or package behavior
> changed, so no product build, smoke, or benchmark was required.

> **2026-07-14 — public-release hardening VERIFIED (run
> `public_release_hardening_20260714`).** Source-backed documentation now uses
> the real privacy env names, one scene-demo mapping, the complete module
> registration order, seven agent roles, portable ONNX setup paths, and the
> actual cross-platform mask-failure semantics. Public repo hygiene added
> `SECURITY.md`, `CONTRIBUTING.md`, least-privilege workflow permissions,
> checked-in Codex role/skill adapters, and removed machine-specific Claude
> permission paths. First-party MSVC targets now compile with `/utf-8` (app,
> Studio, and four tests; no C4819 in the build log). Evidence:
> `.aetherflow/runs/public_release_hardening_20260714/verify_report.json`
> **passed**, 120/120 encoded, 0 encode failures, 0 parse errors; CTest **4/4**
> passed and `AetherFlowStudio` built. The initial 900-frame smoke was stopped
> after more than seven minutes on a nearly static WGC desktop; the final
> manifest records `max_frames=120`. The resulting 1007/120 capture-failure
> ratio was 8.392; the warning remains warn-only and is reported, not hidden.
> Benchmark was not
> requested because runtime/encoder/mask behavior did not change. Independent
> code review found 1 blocker + 2 risks; one scoped repair fixed all three, and
> delta re-review finished at **0 blockers / 0 risks / 0 nits → proceed**.
> Policy still requiring maintainer choice: ROI benchmark pass/fail currently
> follows process exit even when the report flags adverse quality direction;
> raw run bundles remain local because they can contain captured-screen data.

> **2026-07-03 (third pass, same day) — Studio AI scene toggle + indicator
> LANDED + verified (run `studio_ai_toggle_v1`).** The Studio window can now
> drive the CLIP scene classifier on this machine: an "AI scene detection
> (advisory)" checkbox that unlocks only when `scene_classifier_v1.onnx` is
> auto-detected at launch (env `AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL` is
> authoritative when set → `<exe>/models/` → dev-tree `models/`; no model =
> grayed out with a hint, saved preference kept), plus a Status-panel
> per-class colored-dot indicator (`AI scene: <class> (NN%) via
> DirectML|CPU`; gray "no confident verdict yet" during baseline/low-conf
> fallback; `[deterministic override]` when a 1.0 producer out-ranks the AI;
> orange failure line when the model fails to load — session continues).
> Advisory boundary intact: the indicator is Studio-window-only — NO
> mask/encoder/stream feedback; the full-screen demo action stays CLI-only.
> Plumbing: 4 new `PipelineStatus` atomics + shared `kSceneClassNames` table
> (PipelineRunner.h ↔ SceneClassifierOnnx.h cross-referenced), consumer-side
> stamping in main.cpp (CLI path untouched), persisted `ai_scene_detection`
> INI key (default OFF; AppConfigIni test extended). `--ui-selftest` gained a
> 150-frame AI leg asserting classifier init + ≥1 scene-merge win (skipped
> when no model on disk, so portable/CI machines stay green). Evidence
> `.aetherflow/runs/studio_ai_toggle_v1/`: verify **passed** incl. benchmark
> (default path 900/900, `scene_sources={baseline:900}` — AI remains strictly
> opt-in), ctest **4/4**, `--ui-smoke` 0, `--ui-selftest` 0 pre- and
> post-repair (AI-leg trace: **119/150 frames the classifier won the merge**,
> `ui_selftest_ai_leg_trace*.jsonl`). Code review (fresh subagent + delta
> re-review verifying the repair hunks): initial **0 blockers / 2 risks /
> 3 nits → proceed**; risk 1 (baseline warm-up mislabeled as deterministic
> override) + 2 nits repaired in-task; delta verdict **proceed — 0 blockers /
> 1 accepted-residual risk / 2 nits** (a new display-text nit from the delta
> pass was also fixed; the grayed-toggle hint now distinguishes a bad env
> override from a missing exe-adjacent model). The remaining limitation is
> recorded in the current status capability matrix: classifier quality is not
> yet measured on a labeled real-screen corpus.

> **2026-07-03 (later the same day) — Studio settings UI (spec Delta B) +
> portable packaging (spec Delta C) LANDED + verified; executor: Claude
> Fable 5.** The Windows pipeline body was extracted in place as
> `AetherFlow::RunPipelineOnce(PipelineOptions, PipelineStatus*)` and
> `AetherFlowStudio.exe` (Dear ImGui, vendored pinned v1.92.8) drives the SAME
> translation units (`AETHERFLOW_STUDIO_BUILD` guards out `main()` — divergence
> is a link error, not a latent fork; see DESIGN_DECISIONS DD-9). UI covers the
> full spec table: monitor dropdown (new deterministic multi-monitor
> `ScreenCapture::EnumerateMonitors` + `CreateForMonitor`), encoder
> Auto/NVENC/oneVPL, resolution Native/1080p/720p, fps 15/30/60, bitrate slider
> (new `IH264Encoder::SetTargetBitrateKbps`, honored by both backends),
> deterministic-mask toggles + style, SRT port/latency/passphrase (invalid
> length disables Start — same 10-79 rule the CLI enforces with exit -4),
> Start/Stop (worker thread, clean closeout), Panic latch button (+ existing
> global Right-Ctrl), live status (frames/fps/masking-source/SRT
> viewer/bytes/Recording-to path), viewer URLs with copy. Settings persist to
> `aetherflow_studio.ini` (unit-tested `AppConfigIni`). A-fixes landed first:
> **Ctrl+C/console-close graceful stop** (full flush/report/`[SRT] summary`
> closeout; evidence `srt_output_v1/srt_graceful_stop_test.log` 7/7) and
> **`AETHERFLOW_MAX_FRAMES=0` = run-until-stopped** (stats retention capped at
> 100k with uncapped `deliveredFrames` report denominators). Delta C:
> `tools/package_portable.py` → `output/AetherFlow-portable-<date>.zip`
> (44.8 MiB: both exes, oneVPL/ORT/FFmpeg DLLs, app-local VC++ CRT, five
> license notices, plain-language README; the staged folder is SELF-TESTED
> before zipping — a 300-frame `--srt-output` run that a vendored-ffmpeg client
> actually decoded 30 frames from, plus `--ui-smoke`). Evidence
> `.aetherflow/runs/srt_ui_v1/`: post-refactor AND post-repair
> `verify_report.json` **passed** (900/900; benchmark passed on the refactor
> pass), `srt_probe.log` 90 live frames decoded, ctest **4/4**, `--ui-smoke`
> exit 0, and the new **`--ui-selftest`** deterministic gate (programmatic
> Start → 60-frame session → clean stop; run twice green — it also proves the
> UI's 6000 kbps reaches NVENC). Code review (fresh subagent, two rounds):
> initial **1 blocker / 7 risks / 10 nits → repair-then-recheck**; the blocker
> (a `JoinIfFinished` join-vs-thread-start race that could freeze the UI
> forever) plus all 7 risks and 7 of 10 nits were repaired in-task — incl. a
> repair-of-the-repair caught by evidence (`SetEnvironmentVariableW` did not
> reach the CRT `getenv` snapshot; `_wputenv_s` did, proven by the selftest
> trace path). Delta re-review verified every fix against the actual hunks:
> **0 open blockers / 0 open risks / 3 standing nits → `proceed`**
> (`srt_ui_v1/code_review.md`, amended in place). The still-open Studio
> interaction cases are tracked in the current status capability matrix.
> The first 10:43 package accidentally contained pre-toggle executables; the
> user caught this outside the gate. The 15:59 rebuild SHA256-matched staged
> `AetherFlowStudio.exe` to `build/Release`; evidence is
> `.aetherflow/runs/studio_ai_toggle_v1/repackage_log.txt`. This is historical
> package evidence, not a current-HEAD release artifact.

> **2026-07-03 — SRT live output (spec Delta A) LANDED + verified.** New
> opt-in (default OFF) streaming stage after the encoder: H.264 access units →
> MPEG-TS → `srt://0.0.0.0:<port>` listener, so any LAN device can watch the
> **already-masked** stream live in VLC/ffplay (`--srt-output`, port 8888 /
> latency 120 ms / optional passphrase; `AETHERFLOW_SRT_*` env equivalents).
> Architecture: `IEncodedFrameSink` seam on `IH264Encoder` (default no-op) →
> NVENC `DrainSlot` / VPL `WriteBitstreamSample` push copies into a bounded
> drop-oldest queue → dedicated worker owns all libavformat/SRT state
> (mid-stream join = wait-for-keyframe + enqueue-side-cached SPS/PPS prepend;
> viewer-disconnect → re-listen). Capture/mask/decision layers untouched; the
> CLIP/PolicyEngine advisory slow path is NOT wired into streaming. Evidence
> `.aetherflow/runs/srt_output_v1/`: verify **passed** incl. benchmark
> (900/900, 0 parse errors), `srt_probe.log` = vendored ffmpeg SRT caller
> connected on try 1 and **live-decoded 90 frames** (mid-GOP join exercised:
> 30 AUs dropped awaiting keyframe → SPS/PPS prepend → clean decode);
> `srt_off_regression` = default path passed with **0** `[SRT]` console lines.
> In-task autonomous repair: first probe received AUs but decoded none —
> root-caused to the SPS/PPS cache being filled only in the serve loop (the
> connect-time queue clear discarded frame 0's IDR, the only AU with in-band
> parameter sets; numerically confirmed by `sent == enqueued − 60` on two
> independent runs) — fixed by harvesting SPS/PPS on the enqueue side
> (`diagnosis.json`, `patch_report.md`). Code review (fresh subagent):
> **0 blockers / 4 risks / 7 nits → proceed**; three risks fixed in-task (VPL
> pts now prefers `bs.TimeStamp`; passphrase length 10–79 validated up front;
> compiled-out no-FFmpeg build evidenced green in
> `srt_compiled_out_build.log`), residual risk = VPL/Intel tap still needs an
> Intel-hardware artifact. Build dep: vendored **LGPL** FFmpeg shared SDK
> (gitignored; pinned BtbN autobuild + SHA256; `tools/fetch_ffmpeg.py`,
> `third_party/ffmpeg/SOURCE.md`); CMake auto-disables the stage when absent
> (CI unaffected). New ctest `AnnexBScan` (3/3 suite green). Environment note
> for agent-shell runs: WGC emits no frames on a fully static desktop — keep a
> self-repainting window open during smoke runs (with one, capture_failures
> was 0/900 vs the historical 2980/900).

> **2026-06-11 — Repo hygiene (no product / runtime behavior change).** Vendored
> Intel oneVPL trimmed 659 → 99 tracked files (its examples / tests / docs / CI
> that AetherFlow does not build were removed; the out-of-box build is preserved).
> Build re-verified: `VPL → libvpl.dll`, `AetherFlow.exe` built, `agent_verify`
> passed (run `onevpl_trim`: 900/900 encoded, 0 failures, 0 parse errors). Two
> dead one-off tool scripts retired. No scene / encoder / mask / trace behavior
> changed, so the runtime capability state was unaffected.

> **2026-05-26 — Claim-boundary decision: do not lead with
> "AI-driven content-aware encoding".** Strategic review (two independent
> assessments) concluded the "ML scene classifier drives NVENC / oneVPL"
> headline cannot carry the product claim on its own,
> because the differentiation space has been closed from three sides: NVENC
> built-in Spatial AQ + Temporal AQ + Emphasis Level Map API; the NVIDIA +
> Beamr CABR commercial integration (10–30 % bitrate savings on existing
> NVENC, live 4K p60 at Mile High Video 2025); AV1 / HEVC Screen Content
> Coding tools + W3C MediaStreamTrack `contentHint` (browser/codec auto-
> enables text-mode encoding). No public benchmark shows ML scene
> classification driving QP beating NVENC built-in AQ by >5 % BD-rate in
> real-time, so the headline has no defensible third-party baseline. The
> public technical summary therefore leads with the load-bearing evidence: (i) a
> self-built 7-role agent development protocol
> (`protocol/AGENT_PROTOCOL.md`, `protocol/AGENT_COMMANDS.md`,
> `protocol/COMPONENT_INDEX.md`, `.claude/agents/`); (ii) cross-platform
> native GPU media pipeline craft (macOS ScreenCaptureKit + VideoToolbox,
> Windows WGC + D3D11 + NVENC, Intel oneVPL); (iii) deterministic scene-
> first runtime with frame-level trace schema v3, verifier-gated handoff,
> and longitudinal audit ledger (`.aetherflow/audit/`). This is a
> claim-boundary decision; it did not stop further work on the repository.

> **2026-05-21 — Verifier gate repair and cross-run audit evidence.** Run
> `fix_broken_gates_20260521` repaired silent-failure handling in the agent
> evidence path after an independent reviewer found that the collector could
> miss its own append failure. The repair added catch-all event collection and
> explicit skipped-state handling. Capture-failure ratio warnings,
> `ledger.jsonl`, and `evidence_log.md` were backfilled across six canonical
> runs; ratios from 1.20 to 3.25 were surfaced without changing any of the six
> top-level results because the gate was intentionally warn-only. Delta review
> ended at **0 blockers / 0 risks / 0 nits — proceed**. Evidence:
> `.aetherflow/runs/fix_broken_gates_20260521/code_review.md`.

> **2026-05-18 — macOS privacy-mask off-thread parity CLOSED.** The Windows
> judder fix (below) had no macOS counterpart: `MacosNotificationProducerModule`
> still ran its `CGWindowListCopyWindowInfo` scan synchronously inside
> `Evaluate()` on the single capture→evaluate→encode thread (frame-gated,
> every N frames). It moved to the same off-thread model as Windows
> `NotificationProducerModule`: a dedicated background poll thread with a
> time-based interval, while `Evaluate()` only copies a mutex-protected
> snapshot. The `MacosSecureTextFieldPrivacyMaskModule` phase-1 stub now carries
> an explicit contract that its future AXUIElement scan must stay off-thread.
> Evidence: `.aetherflow/runs/mac_notif_offthread/verify_report.json` passed;
> `notification-producer` still applied 9/9 masks with zero fallback and zero
> parse errors. Latency under real capture remains interactive evidence by
> design; the structural result is that the window-server scan no longer sits
> on the `Evaluate()` path.

> **2026-05-17 — Recorded-video judder RESOLVED (three compounding bugs, no
> AI).** (1) NVENC drain busy-spin + bitstream write under the pipeline
> mutex → moved to a dedicated writer thread + drain backoff. (2) Dominant
> residual: `PasswordFieldPrivacyMaskModule` UIA scan +
> `NotificationProducerModule` `EnumWindows` ran synchronously on the
> producer/capture thread (every 5 frames, ~27ms each) → moved to per-module
> background poll threads; `Evaluate()` now copies a cached snapshot. (3)
> Fake fixed-30fps mux → opt-in real-PTS sidecar + PTS-honoring demo mux
> (PD2a/PD3, `AETHERFLOW_TIMED_RECORDING`; canonical path byte-stable).
> The earlier password-field fixture's first UIA query recorded
> `decisionMs=36.178` ms before the off-thread repair. Evidence: interactive
> run 2026-05-17 20:08 `scene_test_out/` —
> `decisionMs` p99 0.17ms / 0% ≥10ms with both masks ON (was 19.1%
> every-5-frame ~27ms); `totalMs` 1.6% over budget (was ~20%); `demo.mp4`
> duration 38.31s ≈ real capture span. Residual `captureDeltaMs` gaps are
> genuine WGC capture-side variance (PD4), not a pipeline defect. Full trail:
> [docs/4-qa-debugging/JUDDER_INVESTIGATION.md](../4-qa-debugging/JUDDER_INVESTIGATION.md) §0.
> Interactive-run evidence by design (headless WGC starves and is
> unrepresentative for judder); see the investigation record §7.

> **2026-05-15 — Windows ONNX scene classifier and bounded visual demo.**
> `scene_classifier_onnx_smoke` passed with
> `scene_inference_p95_ms=15.254`,
> `scene_sources={baseline:115, scene-classifier-onnx:420,
> notification-producer:365}`, and
> `scene_class_distribution={mixed_ui:266, sensitive_surface:634}`. This proves
> real-pixel plumbing and non-degenerate output, not real-screen accuracy.
> `scene_classifier_inactive_regression` was `not_applicable` and kept the
> classifier-off trace output byte-equivalent. The opt-in, non-product demo was bounded by
> `demo_action_on_smoke` (`privacyMaskSource={scene-demo-action:900}`),
> `demo_action_off_regression`, `demo_action_password_suppression`, and
> `demo_action_classifier_on_no_demo`; only two of five classes were observed,
> so a complete visual sweep remains open. macOS CoreML classification was not
> implemented; its deterministic notification/compositor path was separate.

> **2026-05-12 — Async analyzer bridge hardening.**
> `analyzer_bridge_no_mock` proved the inactive bridge stayed
> `not_applicable` while the run passed and preserved inactive fast-path/trace
> behavior. `analyzer_bridge_async_mock` used the
> real-thread mock at N=1: 887/900 frames received a contributed result and
> inference p95 was 262.299 ms without blocking the fast path.
> `analyzer_bridge_async_mock_sub5` submitted exactly 180/900 frames and reused
> cached results on 880/900 frames. The three artifacts closed bridge
> reachability, sub-sampling, drop/staleness, and fast-path regression gates;
> they did not prove model accuracy.

> **2026-05-11/12 — macOS phase-2 chat-window masking.**
> `mac_chat_window_mosaic` passed the no-mask regression at 500/500 frames with
> `privacy_mask_paths={"none":500}` and decision p99 7.005 ms.
> `mac_chat_window_mosaic_masked` exercised visible LINE and Microsoft Teams:
> 5,500/5,500 masks were applied across 500 frames, with zero fallback frames,
> decision p99 7.515 ms, and `coreimage-bgra-mosaic` output. The first mask
> render cost 234.094 ms while CoreImage/Metal initialized; frames 1 onward
> measured mask mean 5.40 ms and p99 7.14 ms at 11 rectangles per frame. Both
> `verify_report.json` files passed. This is historical hardware evidence; the
> current Windows host did not rerun macOS, AXUIElement secure-text remains a
> stub, and phase-3 ROI/QP is unsupported.

> **2026-05-10 — macOS phase-1 capture/encode baseline.** Run
> `macos_capture_encode_phase1` passed build, smoke, and trace with 900/900
> captured and encoded, zero encode failures, and a 3,019,295-byte MP4.
> ScreenCaptureKit capture pacing measured mean 34.05 ms / p95 37.77 ms;
> effective 28.47 fps was about 5% below the 30 fps target and 85.86% of frames
> exceeded the 33.33 ms budget. Average MP4 bitrate was 764 kbps versus the
> configured 3 Mbps on a near-static desktop, which did not prove dynamic
> content was under-driven. Frame-0 `encodeSubmitMs=33.22` ms was attributed to
> VideoToolbox cold start. These are historical observations, not current-tree
> macOS proof.

> **2026-05-10 — Capture-retry evidence remained warn-only.** Earlier default
> and panic runs recorded 90/900 and 42/900 capture retries. Run
> `project_status_audit_20260510` recorded 2,980 retries for 900 delivered
> frames while encoding still completed 900/900 and verification passed. This
> motivated the visible capture-retry warning; the current open gate is still
> to define a justified hard threshold from live moving-content evidence.
