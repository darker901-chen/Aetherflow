---
name: aetherflow-arch-sync
description: Detect drift between AetherFlow's architecture documentation and the actual source code. Use when the user asks to "sync docs with code", "update architecture", "check architecture drift", or "check if docs match the code", and after substantial code changes that may have left docs stale. ALSO use this skill at the closeout of any subagent task that modified runtime files — pass `--apply-mechanical` so docs stay synced with each patch. Compares docs/3-product/ARCHITECTURE.md (§12 status table, §6/§7 component lists) and protocol/COMPONENT_INDEX.md against the actual filesystem and class definitions. Two modes: audit (default, report only) and `--apply-mechanical` (auto-apply low-risk fixes; report complex ones).
---

# AetherFlow Architecture Sync

Compare what the AetherFlow docs **claim** about the project against what the **code actually contains**. Output a structured drift report.

This skill is the manual-trigger version of the "Architecture Sync Loop" described in `protocol/AGENT_PROTOCOL.md` §10.5. **Two modes**:

- **Audit mode** (default): drift report only, no doc edits. Use when the user manually triggers via slash command or natural language.
- **`--apply-mechanical` mode**: drift report PLUS auto-apply low-risk mechanical fixes. Use when a subagent invokes this skill at the end of a successful task closeout — this is what keeps docs synced with each patch automatically.

Determine mode from invocation:
- `args` contains `--apply-mechanical` OR the caller says "auto-apply" / "auto-sync" / "apply it" → **apply mode**
- Otherwise → **audit mode**

## Inputs to read

1. `docs/3-product/ARCHITECTURE.md` — focus on:
   - §6 (Scene Detection structures + concrete modules)
   - §6.3 (module list and registration order)
   - §7 (Encoder Backends)
   - §11 (Dev Agent Workflow file references)
   - §12 (status table — primary drift target)
   - §14 (file map)
2. `protocol/COMPONENT_INDEX.md` — full file
3. Filesystem inventory (use `rg --files` or an equivalent fast inventory;
   do NOT read every file):
   - `include/AetherFlow/**/*.h`
   - `src/**/*.cpp`
   - `tools/agent_*.py`, `tools/agent_*.ps1`, `tools/roi_benchmark.ps1`
   - `.github/agents/*.md`
   - `.github/copilot-instructions.md`
   - `.claude/agents/*.md`
   - `.codex/agents/*.toml`
   - `.claude/skills/*/SKILL.md`
   - `.agents/skills/*/SKILL.md`
   - `CMakeLists.txt`
4. Mutable docs that often drift after runtime work:
   - `docs/1-status/PROJECT_STATUS.md`
   - `docs/4-qa-debugging/VERIFICATION_HISTORY.md`
   - `README.md`
   - `docs/3-product/PRODUCT_ROADMAP.md`
   - `docs/3-product/CODE_COMPONENT_SUMMARY.md`
   - `docs/DOCUMENTATION_INDEX.md`
   - `docs/HIGHLIGHTS.md`

The audit is incomplete if it only checks `ARCHITECTURE.md` and
`COMPONENT_INDEX.md` after a runtime, encoder, trace, fixture, CLI, or env
change.

## Checks to run

### Check 1: §12 status table claims vs reality

For each row in §12 of `ARCHITECTURE.md`, verify the ✅/⚠️/❌ claim against code:

| Row | Verification |
|---|---|
| `BaselineSceneModule ✅` | grep `class BaselineSceneModule` in `include/AetherFlow/IAIFrameAnalyzer.h` |
| `CursorFocusModule ✅` | grep `class CursorFocusModule` in same |
| `FramePolicyEngine ✅` | grep `class FramePolicyEngine` |
| `frame_trace.jsonl writer ✅` | grep `WriteFrameTraceJson` in `src/main.cpp` |
| `NVENC backend ✅` | check `src/NvencH264Wrapper.cpp` exists and is in CMakeLists |
| `VPL backend ✅` | check `src/VplH264Wrapper.cpp` exists |
| `Privacy mask producer ❌` | grep IFrameDecisionModule subclasses outside Baseline+CursorFocus; check if any emits `FrameRegionPurpose::PrivacyMask` |
| `Pre-encode mask GPU stage ❌` | grep for blur/mosaic compositor classes or shaders |
| `ONNX async analyzer ❌` | grep IAIFrameAnalyzer subclasses other than NullAIFrameAnalyzer |
| `IAIFrameAnalyzer bridge ❌` | grep `AsyncAnalyzerBridge` |
| `Confidence-based merge ❌` | grep for the merge logic in FramePolicyEngine — if it uses confidence comparison instead of HasScene() early-out |

If reality contradicts the doc claim, that's a drift.

### Check 2: COMPONENT_INDEX.md entries vs filesystem

- For each file path mentioned in `COMPONENT_INDEX.md`, verify the file exists at that path. Flag any path that no longer exists.
- For each file under `include/AetherFlow/`, `src/`, `tools/agent_*`, check if it's mentioned in `COMPONENT_INDEX.md`. Flag files in repo but not indexed (especially recently added ones).

### Check 3: §6.3 module list vs main.cpp wiring

`ARCHITECTURE.md §6.3` lists which `IFrameDecisionModule` subclasses are documented. Check `src/main.cpp` for `framePolicy.AddModule(...)` calls and compare:
- Modules registered in code but not mentioned in §6.3 → doc needs update
- Modules mentioned in §6.3 but not registered → either docs lying or wiring missing

### Check 4: Encoder backend list vs reality

`§7` of `ARCHITECTURE.md` describes NVENC + VPL backends. Verify:
- Each described backend has a matching `XxxH264Wrapper.cpp` in `src/`
- No new backends in `src/` that aren't in docs (e.g. future `VideoToolboxH264Wrapper.cpp`)

### Check 5: Agent/skill file parity

Inventory `.github/agents/*.md` (GitHub side), `.claude/agents/*.md` (Claude
Code side), and `.codex/agents/*.toml` (Codex side):
- Each role should have parity files in all three directories
- Note any role that exists on only one or two sides

Also list `.claude/skills/*/SKILL.md` and `.agents/skills/*/SKILL.md`; check
that they are mentioned by `CLAUDE.md`, `AGENTS.md`, or the skill itself.

### Check 6 (optional): Recently changed files

If the user mentions specific recent changes ("after I added X module"), focus the audit on related sections of ARCHITECTURE.md — don't drift into unrelated areas.

### Check 7: User-facing CLI/env/fixture drift

When `src/main.cpp`, `CMakeLists.txt`, `fixtures/*`, or any producer/compositor
source file changed, inspect the changed source for:

- new `--flag` or `--flag=value` command-line options
- new `AETHERFLOW_*` environment variables
- new fixture files or fixture commands
- new mask modes, trace fields, output paths, backend behavior, or verifier
  gate semantics

Then check `README.md`, `docs/1-status/PROJECT_STATUS.md`, and
`docs/3-product/ARCHITECTURE.md` for matching user-facing commands, env
defaults, validation SOPs, and current state. Missing or contradictory docs are
drift.

When a run, benchmark, or review adds dated evidence, also check
`docs/4-qa-debugging/VERIFICATION_HISTORY.md`. Keep current capability/release
boundaries in `PROJECT_STATUS.md`; keep dated run IDs and measurements in
verification history.

### Check 8: Cross-doc stale-claim search

Search mutable docs for stale status phrases tied to recent code changes:

- old module names or examples that now refer to completed work
- `pending`, `not implemented`, `not built yet`, `blackout only`, `currently`,
  or similar status words near implemented module names or modes
- roadmap or guide claims that conflict with source files, `CMakeLists.txt`, or
  run artifacts

Report these as complex drifts unless the fix is a narrow mechanical update.

### Check 9: Role-shortcut parity in audience-facing docs

The source of truth for protocol roles is `protocol/AGENT_COMMANDS.md` (the
`agent:<role>` alias table). Role-name grep (`Architecture Agent`) and
shortcut grep (`agent:architecture`) match disjoint surfaces, so adding or
removing a role can leave audience-facing guides stale even when the role-name
mentions are correct.

For every public block that displays `agent:<role>` shortcuts:

- Confirm each shown shortcut exists in `protocol/AGENT_COMMANDS.md`.
- If the block claims to be complete, confirm it contains every canonical
  responsibility; if it is labeled as examples, partial coverage is allowed.
- Confirm every role-count phrase (for example, `seven roles`) matches the
  responsibilities defined in `AGENT_COMMANDS.md` and `AGENT_PROTOCOL.md`.
- Spot-check `docs/2-agent-system/AGENT_ARCHITECTURE.md`, `docs/HIGHLIGHTS.md`,
  and `README.md` when they expose shortcuts or role counts.

An invalid shortcut, a missing role in a claimed-complete block, or a stale
role-count phrase is a mechanical-fix drift.

## Output Format

Print to chat (no file written) using this exact structure:

```
## AetherFlow Architecture Sync — Drift Report
Run: <YYYY-MM-DD HH:MM>
Working tree clean: yes/no (mention if uncommitted doc changes exist)

### Summary
- Checks run: <N>
- Drifts found: <N>
- Suggestions: <N> doc edits, <N> code-side notes

### Drift 1 — <one-line title>
- Doc says (`docs/<file>.md` <section>): "<quoted text>"
- Reality (`<source file>`:<line>): "<quoted text or summary>"
- Suggested fix: edit `docs/<file>.md` to <specific change>

### Drift 2 — ...

### No-action observations
- M1-M5 known gaps (in §13.1) — intentional TODOs, not drift
- Files in third_party/, build*/, output/ — out of scope by design

### Mutable docs checked
- `docs/1-status/PROJECT_STATUS.md`: updated / no change / drift found
- `docs/4-qa-debugging/VERIFICATION_HISTORY.md`: updated / no change / drift found
- `docs/3-product/ARCHITECTURE.md`: updated / no change / drift found
- `protocol/COMPONENT_INDEX.md`: updated / no change / drift found
- `README.md`: updated / no change / drift found
- `docs/3-product/PRODUCT_ROADMAP.md`: updated / no change / drift found
- `docs/3-product/CODE_COMPONENT_SUMMARY.md`: updated / no change / drift found
- `docs/DOCUMENTATION_INDEX.md`: updated / no change / drift found
- `docs/HIGHLIGHTS.md`: updated / no change / drift found

### Suggested commit (if user accepts the doc edits)
docs: sync architecture status with code

- Update §12 row X
- Update COMPONENT_INDEX.md for new file Y
- ...
```

## Apply-mechanical mode rules

When invoked with `--apply-mechanical`, AFTER the drift report is produced, classify each drift:

### Mechanical (auto-apply with the available patch/edit tool)
These are low-risk, format-preserving edits where the "right answer" is unambiguous from the code:

1. **§12 status table symbol flip**: `❌` → `⚠️` or `⚠️` → `✅` based on observed implementation state.
2. **§12 status table description text** after the symbol, when the existing description directly contradicts code (e.g. doc says "no producer" but a producer class exists).
3. **COMPONENT_INDEX.md additions**: adding a single new file path under an existing section, using the existing format. Only when the file is clearly a new producer/backend that fits an existing category.
4. **§6.3 module list**: adding a single new module name to the list of registered `IFrameDecisionModule` subclasses, when the new module is found via `framePolicy.AddModule()` in `src/main.cpp`.

5. **Narrow stale examples**: replacing a completed module name in `README.md`,
   `AGENTS.md`, or `CLAUDE.md` examples with an obviously unfinished example,
   without changing the protocol meaning.

### Complex (report only, do NOT auto-apply)
- Adding new §12 rows (decisions about what to track go to human)
- New top-level sections in any doc
- COMPONENT_INDEX category changes / new sections
- Any multi-line prose rewrite
- `PROJECT_STATUS.md` current-state updates that require interpreting run
  artifacts or benchmark status
- `VERIFICATION_HISTORY.md` dated evidence updates that require interpreting
  run artifacts, benchmark results, or review findings
- README validation SOPs that require deciding shell/platform wording
- Roadmap or audience-guide wording that changes product scope or priority
- Anything that affects §13 (known gaps) — those are decisions, not drift
- Anything that affects §14 file map structurally

In apply mode, the report should clearly mark `[APPLIED]` vs `[REPORT-ONLY]` for each drift. Auto-applied edits go through the available patch/edit tool so the user sees them in `git diff`.

## What NOT to do

1. **Do NOT auto-apply complex edits**. Stay strict on the mechanical/complex split above.
2. **Do NOT mark P1/P2/M1-M5 known gaps as drift**. Those are explicit TODOs in §13.1, not stale claims.
3. **Do NOT scan** `third_party/`, `build*/`, `output/`, `.aetherflow/runs/`, `.claude/worktrees/`, `.codex/worktrees/`.
4. **Do NOT dump more than 10 drifts**. If more found, list top 10 by impact and report total count.
5. **Do NOT propose architectural changes**. This skill audits docs vs current code, not "what code should be".
6. **Do NOT touch §13** (known gaps & next steps). That section is human-curated.

## When to skip / ask first

- If `git diff HEAD -- docs/3-product/ARCHITECTURE.md protocol/COMPONENT_INDEX.md README.md docs/1-status/PROJECT_STATUS.md docs/4-qa-debugging/VERIFICATION_HISTORY.md docs/3-product/PRODUCT_ROADMAP.md docs/3-product/CODE_COMPONENT_SUMMARY.md docs/DOCUMENTATION_INDEX.md docs/HIGHLIGHTS.md` shows uncommitted changes, mention it in the report header. Use working-tree version unless user says otherwise.
- If the repo is mid-merge (`.git/MERGE_HEAD` exists), abort and ask the user to finish the merge first.

## Performance budget

- File inventory should be under 2 seconds total (prefer `rg --files`; do not use recursive `find`).
- Reading code files: only read files needed for grep verification, not entire source.
- Total skill execution should finish in under 30 seconds for a clean repo.

## Done criteria

- Drift report printed
- Each drift cites a doc location AND a code location (file:line preferred)
- Each drift has a specific suggested fix (not "fix this somehow")
- Known gaps explicitly excluded from drift count
- Mutable-docs matrix included for `PROJECT_STATUS.md`,
  `VERIFICATION_HISTORY.md`, `ARCHITECTURE.md`, `COMPONENT_INDEX.md`,
  `README.md`, `PRODUCT_ROADMAP.md`,
  `CODE_COMPONENT_SUMMARY.md`, `DOCUMENTATION_INDEX.md`, and
  `HIGHLIGHTS.md`
- User can copy the suggested commit message verbatim
