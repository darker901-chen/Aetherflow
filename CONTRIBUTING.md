# Contributing to AetherFlow

Thanks for helping improve AetherFlow. The runtime handles captured screen
content, so correctness, privacy boundaries, and reproducible evidence matter
as much as the code change itself.

## Before you start

- Read `AGENTS.md` and use `protocol/COMPONENT_INDEX.md` to find the owning
  component without scanning generated or vendored directories.
- For runtime, encoder, scene, ROI, QP, privacy-mask, CLI/env, cross-platform,
  or architecture changes, follow the Architecture Planning Gate in
  `AGENTS.md` before patching.
- Do not commit files from `build*/`, `output/`, `.aetherflow/runs/`, local SDK
  directories, models, credentials, or captured-screen artifacts.
- Report vulnerabilities privately according to `SECURITY.md`.

Small documentation corrections that do not change behavior may go directly
through the lighter documentation checks described in `AGENTS.md`.

## Build and test

The clean-checkout CI path uses MSVC with Ninja. Open an x64 Native Tools
Command Prompt or Developer PowerShell so `cl.exe` is on `PATH`, then run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional NVENC, ONNX Runtime, FFmpeg, and Dear ImGui integrations auto-disable
when their local SDK files are absent. Their pinned setup instructions live in
the corresponding `third_party/*/SOURCE.md` files.

For implementation changes, use the canonical artifact workflow and keep its
raw output local:

```powershell
python tools/agent_run.py --run-id <run_id>
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>
```

Add `--run-benchmark` when runtime, encoder, ROI, QP, or privacy-mask behavior
changed. If a gate fails, follow the one-repair-attempt and evidence handoff
rules in `AGENTS.md`.

## Pull requests

Keep each pull request focused. In its description, include:

- the problem and intended behavior;
- the files/components changed;
- commands run and the verification result;
- benchmark results when required;
- documentation updated or intentionally unchanged;
- hardware, platform, or backend paths that were not exercised;
- any remaining privacy, compatibility, or performance risk.

Do not attach raw traces, recordings, screenshots, or run bundles until you
have confirmed that they contain no private screen content or machine-specific
secrets.
