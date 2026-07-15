# AetherFlow — Agent System Effectiveness Log

A curated, accumulating record of where AetherFlow's **multi-agent development
workflow** delivered *concrete* value: a finding caught before merge, a failure
root-caused, a regression measured or prevented, documentation drift detected.

**Honest by design.** Entries are only recorded when an agent actually **caught**
or **diagnosed** something. Routine "all gates green, nothing caught" runs are
*not* logged. Findings are labeled by kind so a reader can tell a real save from
noise:

- **BUG/BLOCKER caught** — a defect that would have shipped, stopped before merge.
- **RISK (latent footgun) caught** — not broken today, but a design hole that
  would bite a future change; hardened proactively.
- **ROOT-CAUSE** — a failure traced to its real cause (debug-verifier).
- **measurement** — a number proving behavior / no-regression (rides along as
  supporting detail on an already-loggable run).

**Attribution — who drove each win (so the autonomous ones stay credible).**
Honesty is what makes the *autonomous* claims believable: if you claim every win
is autonomous, one human-led case gets found and then all claims are distrusted.
So each entry is also labeled:

- **`agent-autonomous`** — the system detected, repaired, and re-verified on its
  own within the task, including catching the implementer agent's own mistakes.
  This is the headline capability (see
  [AGENT_ARCHITECTURE.md](AGENT_ARCHITECTURE.md) §2.2 autonomous repair loop).
- **`human-led → agent-executed`** — a human caught the direction/cause; the
  agent measured, fixed, and verified.
- **`agent-flagged → human-decided`** — an agent surfaced findings; the human
  chose the fix.

Every claim cites the run-dir artifacts so it is **independently verifiable** —
open the cited `code_review.md` / `verify_report.json` / handoff yourself.

Go-forward entries are appended by `tools/agent_report.py --append` (which reads
the run's artifacts and excludes routine runs). The seven protocol
responsibilities are Architecture, Runtime Scene, Encoder, Trace and
Verification, Benchmark, Code Review, and Repair. Adapter filenames use
`scene-runtime`, `encoder`, `trace-verifier`, `benchmark-reporter`,
`code-reviewer`, and `debug-verifier`; architecture sync is a Claude/Codex
skill, not another protocol role.

---

## 2026-07-14 — Public-release hardening caught a broken Windows setup SOP and adapter portability risks
Run: `.aetherflow/runs/public_release_hardening_20260714/`  ·  Role(s): Architecture + code-reviewer + one scoped repair  ·  Attribution: `agent-autonomous`

- **code-reviewer — BUG/BLOCKER caught before publication:** the revised
  PowerShell ONNX Runtime instructions downloaded a valid NuGet ZIP payload as
  `_ort_dml.nupkg` and passed it directly to `Expand-Archive`. Windows
  PowerShell rejects that extension before reading the ZIP content, so the
  public clean-setup SOP would fail exactly where a new contributor needed it.
  Repair: save the payload as `_ort_dml.zip` in both the operation guide and the
  vendored dependency SOP; syntax/source checks then passed.
- **code-reviewer — two portability risks caught:** (1) replacing a
  machine-specific Claude permission with `Read/Edit/Write(./**)` still made
  access depend on the launch directory and was broader than necessary; the
  repair removed those rules and let normal project permissions govern file
  access. (2) Codex-facing skill/adapter instructions still named
  Claude-specific `Glob`, `Edit`, and `Agent(subagent_type=...)` calls; the
  repair changed them to capability-based inventory/patch language and
  platform-neutral collaboration wording.
- **measurement:** public hardening then passed the Windows default smoke
  (120/120 encoded, 0 encode failures, 0 parse errors), CTest 4/4, Studio build,
  JSON/TOML/YAML parsing, Markdown link checks, secret/path scans, and MSVC
  `/utf-8` coverage. The 1007/120 WGC capture-retry ratio remained visible as a
  warn-only limitation rather than being hidden.
- Outcome: initial review **1 blocker / 2 risks / 0 nits →
  `repair-then-recheck`**; one scoped repair; delta re-review **0 blockers / 0
  risks / 0 nits → `proceed`**.
- Attribution: **`agent-autonomous`** — the user asked for public hardening; the
  reviewer found these concrete defects, the workflow repaired them, and the
  reviewer rechecked the actual delta without the user identifying the bugs.
- Evidence: `.aetherflow/runs/public_release_hardening_20260714/plan.md`,
  `code_review.md`, `verify_report.json`, and `trace.summary.json`.

## 2026-07-03 — Stale portable package caught after the AI-toggle closeout — attribution: `human-led → agent-executed`
Run: `.aetherflow/runs/studio_ai_toggle_v1/` (`repackage_log.txt`)  ·  Role(s): user review + packaging re-run

- **The USER caught what every gate missed**: after the Studio AI toggle
  closeout, `output/AetherFlow-portable-20260703.zip` still contained the
  MORNING exes (staged 10:26/10:30, zipped 10:43 — pre-AI-toggle), while the
  build tree held the new 15:37/15:47 binaries. Packaging was declared
  out-of-scope in plan.md — correctly, nothing in the packaging *tool*
  changed — but the closeout report failed to FLAG that the on-disk zip was
  now stale relative to the just-landed feature. No gate covers "artifacts
  produced by earlier runs are now behind HEAD-of-tree"; a human eyeballing
  file dates did.
- Agent-executed repair: re-ran `tools/package_portable.py` (staged
  self-test green — 300-frame `--srt-output` run, real ffmpeg client decoded
  30 frames, `--ui-smoke` exit 0), verified by SHA256 that the staged
  `AetherFlowStudio.exe` is byte-identical to `build/Release`, zip rewritten
  15:59 (44.8 MiB, engine-only — no model, so the new grayed-out-toggle
  degrade is exactly what a clean machine shows). Recorded here per the
  attribution rules: claiming this one as autonomous would poison the entries
  that actually are.

## 2026-07-03 — Studio AI scene toggle + indicator — executor: Claude (user-selected model claude-fable-5)
Run: `.aetherflow/runs/studio_ai_toggle_v1/`  ·  Role(s): scene-runtime route + code-reviewer delta loop  ·  Attribution: `agent-autonomous`

- **code-reviewer** (fresh subagent) — RISK caught in the feature's core UX
  *before any human saw it*: the new AI indicator stamped `sceneSourceKind=2`
  ("deterministic override") for **any** non-classifier scene source, so for
  the first ~1 s of every AI session (baseline warm-up, before the first
  inference) the UI would claim "AI scene: mixed_ui (50%) via DirectML
  [deterministic override]" — a fabricated AI verdict on a row whose whole
  point is honest attribution. The reviewer proved it from the task's own
  trace artifact (31 warm-up baseline frames in `ui_selftest_ai_leg_trace.
  jsonl`) and from the header comment contradicting the code's predicate.
  Repair: a third source-kind (baseline/low-confidence fallback) rendered as
  a neutral gray "no confident verdict yet..." — the AI is only ever credited
  with classes it actually produced. Delta re-review verified the fix against
  the hunks, incl. an exhaustive enumeration that `"baseline"` is the ONLY
  fallback source string, so the new predicate cannot mislabel a real
  deterministic producer.
- **code-reviewer** — RISK (latent footgun) caught: the Studio's env-var
  model override silently fell through to a *different* model file when the
  env path was bad (CLI fails loudly on the same mistake). Repaired: the env
  override is now authoritative — use it or gray out. The delta pass then
  caught the follow-on doc-in-UI bug (the grayed-toggle hint still gave
  dead-end "put it next to the exe" advice when the env was the problem) —
  fixed the same hour.
- **measurement** — the new `--ui-selftest` AI leg turned "the toggle works"
  into a numeric gate: 150-frame session, classifier initialized on DirectML,
  **119/150 frames won the confidence merge** (31 warm-up), asserted
  end-to-end pixels→ONNX→merge→status; default-path regression stayed exact
  (`scene_sources={baseline:900}`, verify+benchmark passed).

## 2026-07-03 — Studio UI + portable packaging (spec Delta B+C) — executor: Claude Fable 5
Run: `.aetherflow/runs/srt_ui_v1/` (+ `srt_packaging_v1/`)  ·  Role(s): `encoder`-route cross-component change + code-reviewer + debug-verifier loop

- **code-reviewer** (fresh subagent) — BUG/BLOCKER caught **in brand-new UI
  code no gate had exercised**: `StudioSession::JoinIfFinished()` joined on
  `joinable && !running`, but `running` only turns true once the worker
  thread reaches `RunPipelineOnce` — a delayed thread start lets the next UI
  frame `join()` an unlimited session ⇒ permanent UI freeze with Stop
  unclickable. The reviewer explicitly noted "no recorded gate ever exercised
  Start" and suggested the `--ui-selftest` fixture; both the fix (join on a
  worker-set completion flag) and the fixture were implemented, and the
  fixture now gates exactly the path the blocker lived on (green twice,
  60/60 frames, clean stop).
- **code-reviewer** — RISK (latent footgun) caught: 7 flagged, ALL repaired
  in-task. Theme called out by the reviewer: "the product face is held to a
  weaker standard than the CLI on the same pipeline" — e.g. (1) invalid SRT
  passphrase silently stripped on Start → stream goes out UNENCRYPTED where
  the CLI refuses with exit -4 (now: inline red text + Start disabled);
  (2) unlimited sessions record ~2.7 GB/h into a directory outside the
  extracted portable folder with zero indication (now: `<exe>/output` +
  "Recording to:" in the status panel + README note); also capped-counter
  report skew on >100k-frame runs, silent wrong-monitor DXGI fallback,
  per-frame getaddrinfo on the UI thread, two bundled DLLs shipping without
  their MIT notices, and a packaging gate clause that had not actually been
  executed (staged SRT probe — now a real leg that decoded 30 frames).
- **debug-verifier loop** — repair-of-the-repair caught by evidence, not by
  eyes: the risk-2 fix first used `SetEnvironmentVariableW`, which the
  pipeline's CRT `getenv` snapshot never sees; the re-run `--ui-selftest`
  trace path exposed it still writing to the old location, fixed with
  `_wputenv_s` and re-proven by the same gate.
- **trace-verifier** — WARNING surfaced (governance working as designed):
  capture-failure ratio 3.063 (2757/900) on the post-repair rerun — the
  desktop had gone static; the run still delivered 900/900 and the warn-only
  policy held. Logged in `ledger.jsonl` + `evidence_log.md`.
- Outcome: initial code-review **repair-then-recheck** (blockers=1, risks=7,
  nits=10) → blocker + 7 risks + 7 nits repaired in one scoped round → delta
  re-review verified each fix against the actual hunks: **0 open blockers /
  0 open risks / 3 standing accepted nits → `proceed`**.
- Attribution: **`agent-autonomous`** — the human assigned Delta B/C ("after
  resolving the current issue, continue with B/C") and separately eyeballed the window; the find → fix →
  re-verify cycles (including the reviewer catching the implementer's fresh
  blocker, and the selftest catching the broken first repair) ran without
  human intervention.
- Evidence: in `.aetherflow/runs/srt_ui_v1/` — `plan.md`, `patch_report.md`,
  `code_review.md` (amended), `verify_report.json` (passed post-refactor AND
  post-repair), `srt_probe.log`; `.aetherflow/runs/srt_packaging_v1/` —
  `plan.md`, `package_manifest.txt`; `output/AetherFlow-portable-20260703.zip`

## 2026-07-03 — SRT live output (spec Delta A) — executor: Claude Fable 5
Run: `.aetherflow/runs/srt_output_v1/`  ·  Role(s): `encoder` (+ debug-verifier loop, code-reviewer)

- **debug-verifier loop** — BUG caught + ROOT-CAUSE + repaired **within the
  task, no human input**: the first live probe showed a viewer receiving AUs
  but decoding zero frames. Root-caused via a control experiment
  (ffmpeg→ffmpeg over SRT proved the client side healthy) plus numeric
  confirmation on two independent runs (`sent == enqueued − 60`, i.e. serving
  started exactly at the second IDR): the connect-time queue clear discarded
  frame 0's IDR — the only AU carrying in-band SPS/PPS — and the serve-loop
  cache never populated, so every mid-stream join got an undecodable stream.
  One scoped repair (harvest SPS/PPS at enqueue) → re-verified: caller decoded
  90 live frames on first try. Without this catch, Delta A would have "passed"
  transport while every real VLC viewer saw nothing.
- **code-reviewer** (fresh subagent) — 0 blockers / 4 risks / 7 nits →
  `proceed`; also caught a **plan-vs-implementation deviation** (VPL pts
  synthesized from frame index while plan.md declared `bs.TimeStamp`). Three
  risks fixed in-task and delta re-reviewed (post-amendment 0/2/8, `proceed`);
  one reviewer observation additionally narrowed the passphrase validation so
  a stray env var cannot fail non-SRT runs.
- **trace-verifier / benchmark-reporter** — measurement: verify+benchmark
  passed (900/900, 0 parse errors); default-path regression run passed with
  0 `[SRT]` console lines; compiled-out (no-FFmpeg) build evidenced green.
- Outcome: code-review **proceed** (final: blockers=0, risks=2 open —
  Intel-hardware validation + the since-evidenced compiled-out build, nits=8).
- Attribution: **`agent-autonomous`** — the human assigned Delta A; the
  detect → control-experiment → root-cause → scoped-repair → re-verify cycle
  and the review-flag → fix → delta-re-review cycle both ran without human
  intervention. First run of this protocol with Fable 5 as executor; the
  harness held (gates fired in order, one scoped repair, honest artifacts).
- Evidence: in `.aetherflow/runs/srt_output_v1/` — `plan.md`,
  `diagnosis.json`, `patch_report.md`, `code_review.md` (amended),
  `verify_report.json`, `srt_probe.log`, `srt_compiled_out_build.log`

## 2026-05-23 — agent effectiveness log
Run: `.aetherflow/runs/agent_effectiveness_log_20260523/`  ·  Role(s): trace-verifier

- **code-reviewer** — RISK (latent footgun) caught: 4 flagged: (1) "BUG/BLOCKER caught: N fixed before merge" asserts the blocker was *fixed*, but the tool only sees a count > 0 in a…; (2) the ledger is never read.
- Outcome: code-review **proceed** (blockers=0, risks=4, nits=3).
- Attribution: **`agent-flagged → human-decided`** — the fresh code-reviewer flagged 4 risks autonomously; the human chose which to act on.
- Evidence: in `.aetherflow/runs/agent_effectiveness_log_20260523/` — `plan.md`, `code_review.md`

## 2026-05-23 — cursor roi default off
Run: `.aetherflow/runs/cursor_roi_default_off_20260523/`  ·  Role(s): encoder (primary — the QP delta map is the actually-visible ROI) + scene-runtime (CursorFocusModule registration)

- **code-reviewer** — RISK (latent footgun) caught: 2 flagged: (1) Inconsistent default member state across backends (NVENC `m_roiEnabled=true`, VPL `m_roiEnabled` historically `false`…; (2) Plan declared a VPL `.cpp` edge (NumROI=0) that was not made; correct, but the residual is that the two backends now…
- **benchmark-reporter** — benchmark passed: roi_benchmark exit=0
- Outcome: code-review **proceed** (blockers=0, risks=2, nits=2).
- Attribution: **`agent-flagged → human-decided`** — code-review flagged 2 risks; the human applied the NVENC default-state fix.
- Evidence: in `.aetherflow/runs/cursor_roi_default_off_20260523/` — `plan.md`, `code_review.md`, `verify_report.json`

## 2026-05-21 — Verifier honesty fixes + cross-run audit ledger
Run: `.aetherflow/runs/fix_broken_gates_20260521/`  ·  Role(s): trace-verifier

- **code-reviewer** — BUG/BLOCKER caught: the new evidence-log feature could
  itself fail silently. A `status: failed` macOS run (gates pass but
  `platform_status` fails) would leave the evidence log empty — so the
  anti-silent-failure feature had its own silent-failure hole. Fixed with a
  catch-all event collector + skipped-state handling, then re-reviewed
  **0 blockers / 0 risks / 0 nits**.
- **trace-verifier** — value shipped: capture-failure ratio warnings +
  `ledger.jsonl` (one quantitative row per run) + `evidence_log.md`
  (plain-language entry on warn/fail), backfilled across 6 canonical runs
  (ratios 1.20–9.25 surfaced; **0/6 status flipped** — warn-only preserved).
- Outcome: code-review **proceed** after one repair pass.
- Attribution: **`agent-autonomous`** — the code-reviewer caught a blocker the implementer agent introduced, the workflow repaired it and re-reviewed to 0, **with no human pointing at the bug**. The headline autonomous-repair case (see AGENT_ARCHITECTURE §2.2).
- Evidence: `.aetherflow/runs/fix_broken_gates_20260521/code_review.md`,
  [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md)
  (2026-05-21 entry).

## 2026-05-17 — Recorded-video judder root-caused (3 compounding bugs)
Run: interactive capture 2026-05-17 20:08 (`scene_test_out/`)  ·  Role(s): debug-verifier

- **debug-verifier** — ROOT-CAUSE: severe recorded-video judder traced to three
  independent bugs, none AI-related — (1) NVENC drain busy-spin + bitstream
  `ofstream::write` held under the pipeline mutex (~240 ms freezes); (2) the
  dominant one — `PasswordFieldPrivacyMaskModule` UIA tree-walk +
  `NotificationProducerModule` `EnumWindows` ran synchronously on the
  producer/capture thread (~27 ms every 5 frames) → WGC frame-pool overflow →
  silently dropped captured frames; (3) a fake fixed-30 fps mux.
- **fix + measurement**: moved the cross-process scans to per-module background
  poll threads (`Evaluate()` now copies a cached snapshot), the bitstream write
  to a dedicated writer thread, and added an opt-in real-PTS sidecar. Result:
  `decisionMs` p99 ~27 ms → **0.17 ms**, frames ≥10 ms **19.1% → 0%**;
  `demo.mp4` duration ≈ real capture span.
- Attribution: **`human-led → agent-executed`** — the human flagged the realtime-hot-path root cause as an avoidable basic mistake; the agent measured, localized, fixed, and verified.
- Evidence: `docs/4-qa-debugging/JUDDER_INVESTIGATION.md` §0,
  [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md)
  (2026-05-17 entry).
