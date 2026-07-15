# AetherFlow Documentation Index

This file routes readers to the single owner for each kind of information.
Supporting documents link back to the owner instead of duplicating mutable
status, commands, or architecture claims.

## Documentation Groups

| Group | Directory | Purpose | Owner | Important companions |
|---|---|---|---|---|
| Status | `docs/1-status/` | Current release identity, verified capability, open gates, and next work | [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md) | [VERIFICATION_HISTORY.md](4-qa-debugging/VERIFICATION_HISTORY.md), [COMPONENT_INDEX.md](../protocol/COMPONENT_INDEX.md) |
| Agent system | `docs/2-agent-system/` | How development agents plan, implement, verify, review, repair, and hand off evidence | [AGENT_ARCHITECTURE.md](2-agent-system/AGENT_ARCHITECTURE.md) | [AGENT_EFFECTIVENESS_LOG.md](2-agent-system/AGENT_EFFECTIVENESS_LOG.md), [AGENT_PROTOCOL.md](../protocol/AGENT_PROTOCOL.md), [AGENT_COMMANDS.md](../protocol/AGENT_COMMANDS.md) |
| Product architecture | `docs/3-product/` | Runtime, encoder, scene, privacy-mask, platform, and product design | [ARCHITECTURE.md](3-product/ARCHITECTURE.md) | [CODE_COMPONENT_SUMMARY.md](3-product/CODE_COMPONENT_SUMMARY.md), [PRODUCT_ROADMAP.md](3-product/PRODUCT_ROADMAP.md), [MACOS_AGENT_VERIFICATION.md](3-product/MACOS_AGENT_VERIFICATION.md), [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md) |
| QA and debugging | `docs/4-qa-debugging/` | Current QA coverage, unverified paths, historical evidence, and root-caused investigations | [TROUBLESHOOTING_QA.md](4-qa-debugging/TROUBLESHOOTING_QA.md) | [VERIFICATION_HISTORY.md](4-qa-debugging/VERIFICATION_HISTORY.md), [JUDDER_INVESTIGATION.md](4-qa-debugging/JUDDER_INVESTIGATION.md) |
| Operations | repository root and `docs/` | Setup, flags, environment variables, outputs, and manual validation | [README.md](../README.md) | [BUILD_WINDOWS.md](BUILD_WINDOWS.md), [OPERATION_GUIDE.md](OPERATION_GUIDE.md), [HIGHLIGHTS.md](HIGHLIGHTS.md) |

## Recommended Reading Orders

### External evaluator

1. [README.md](../README.md)
2. [HIGHLIGHTS.md](HIGHLIGHTS.md)
3. [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md)
4. [ARCHITECTURE.md](3-product/ARCHITECTURE.md)
5. [AGENT_ARCHITECTURE.md](2-agent-system/AGENT_ARCHITECTURE.md)
6. [TROUBLESHOOTING_QA.md](4-qa-debugging/TROUBLESHOOTING_QA.md)

### New contributor or development agent

1. [AGENTS.md](../AGENTS.md) or [CLAUDE.md](../CLAUDE.md)
2. [AGENT_OPERABLE_ARCHITECTURE.md](../protocol/AGENT_OPERABLE_ARCHITECTURE.md)
3. [AGENT_COMMANDS.md](../protocol/AGENT_COMMANDS.md)
4. [AGENT_PROTOCOL.md](../protocol/AGENT_PROTOCOL.md)
5. [COMPONENT_INDEX.md](../protocol/COMPONENT_INDEX.md)
6. [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md)
7. [ARCHITECTURE.md](3-product/ARCHITECTURE.md) for runtime, encoder, scene,
   ROI/QP, privacy-mask, or platform work

### Operator

1. [README.md](../README.md)
2. [BUILD_WINDOWS.md](BUILD_WINDOWS.md) for a fresh Windows source build
3. [OPERATION_GUIDE.md](OPERATION_GUIDE.md)
4. [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md) for current limitations
5. [TROUBLESHOOTING_QA.md](4-qa-debugging/TROUBLESHOOTING_QA.md) when a known
   failure pattern appears

## Canonical Ownership

| Question | Canonical owner | Rule |
|---|---|---|
| What version is this, what works now, and what remains unverified? | [PROJECT_STATUS.md](1-status/PROJECT_STATUS.md) | Keep current truth concise; put dated detail in verification history. |
| What happened in earlier runs? | [VERIFICATION_HISTORY.md](4-qa-debugging/VERIFICATION_HISTORY.md) | Historical evidence must not silently become a current capability claim. |
| How is the product designed? | [ARCHITECTURE.md](3-product/ARCHITECTURE.md) | Keep long-term design and implementation state aligned with source. |
| Which file owns a component? | [COMPONENT_INDEX.md](../protocol/COMPONENT_INDEX.md) | Use it before scanning the repository. |
| What does every source/header file do? | [CODE_COMPONENT_SUMMARY.md](3-product/CODE_COMPONENT_SUMMARY.md) | This is the maintained contributor map; source still wins on behavior. |
| What should be built next? | [PRODUCT_ROADMAP.md](3-product/PRODUCT_ROADMAP.md) | Milestone labels are not release tags. |
| How do development agents cooperate? | [AGENT_ARCHITECTURE.md](2-agent-system/AGENT_ARCHITECTURE.md) | Protocol details remain in `protocol/`; adapters must not fork the rules. |
| Did the agent workflow catch anything real? | [AGENT_EFFECTIVENESS_LOG.md](2-agent-system/AGENT_EFFECTIVENESS_LOG.md) | Record only attributable catches, repairs, or measurements; omit routine green runs. |
| Which paths are tested or still unverified? | [TROUBLESHOOTING_QA.md](4-qa-debugging/TROUBLESHOOTING_QA.md) | A passed path must name its platform/backend/condition boundary. |
| How do I build it from a fresh Windows clone? | [BUILD_WINDOWS.md](BUILD_WINDOWS.md) | Keep the bootstrap, prerequisites, dependency policy, and success criteria aligned. |
| How do I run and operate it? | [README.md](../README.md), then [OPERATION_GUIDE.md](OPERATION_GUIDE.md) | Commands, paths, defaults, and shell syntax must come from source or verified artifacts. |

## Update Rules

- Runtime behavior changes: update source first, verify it, then synchronize
  `ARCHITECTURE.md`, `COMPONENT_INDEX.md`, current status, and operation docs as
  required.
- Encoder/backend changes: verify the affected hardware path when available;
  distinguish source/build coverage from hardware runtime proof.
- Trace/verifier changes: update the schema/behavior owner and any current
  status or QA claim that depends on the changed gate.
- Documentation-only changes: run link, language, configuration-parse,
  architecture-sync, and diff checks. Product build/benchmark is unnecessary
  unless behavior changed.
- Current status must remain a snapshot, not a chronological journal. Append
  dated detail to `VERIFICATION_HISTORY.md`.
- Run bundles under `.aetherflow/runs/` remain local because they may contain
  captured-screen-sensitive data. Public docs cite paths and summarized facts.
- Ignored local material must not be referenced by public documentation or
  agent contracts.

## Quick Router

- Current state or release readiness: `docs/1-status/PROJECT_STATUS.md`
- Fresh Windows build: `docs/BUILD_WINDOWS.md`
- Setup or runtime flags: `README.md`, then `docs/OPERATION_GUIDE.md`
- Product architecture: `docs/3-product/ARCHITECTURE.md`
- File ownership: `protocol/COMPONENT_INDEX.md`
- Agent workflow: `docs/2-agent-system/AGENT_ARCHITECTURE.md`
- Agent commands and aliases: `protocol/AGENT_COMMANDS.md`
- QA coverage and known failure patterns:
  `docs/4-qa-debugging/TROUBLESHOOTING_QA.md`
- Dated verification detail:
  `docs/4-qa-debugging/VERIFICATION_HISTORY.md`
- Resolved recorded-video judder investigation:
  `docs/4-qa-debugging/JUDDER_INVESTIGATION.md`
- macOS claim boundary:
  `docs/3-product/MACOS_AGENT_VERIFICATION.md`
