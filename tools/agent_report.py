#!/usr/bin/env python3
"""agent_report.py -- render the AetherFlow multi-agent workflow's *effectiveness*
for one run, in plain language, and optionally append a curated entry to
docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md.

It reads ONLY existing artifacts (never the runtime). For a run dir it extracts,
per agent role, the concrete VALUE the workflow delivered -- a finding caught, a
failure root-caused, a regression measured/prevented -- with honest severity
labels (risk/footgun vs bug vs measurement vs diagnosis). Routine "all green,
nothing caught" runs are reported as ROUTINE and, with --append, are NOT written
to the log, so the log stays a high-signal engineering evidence artifact.

Honesty contract: a run is logged only when an agent actually CAUGHT or
DIAGNOSED something (a code-review blocker/risk, a verifier failure/warning, a
debug root-cause). A clean benchmark measurement rides along as supporting
detail on an already-loggable run but does not by itself make a run loggable.

Usage:
  python tools/agent_report.py --run-dir .aetherflow/runs/<id>
  python tools/agent_report.py --run-dir .aetherflow/runs/<id> --append
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG = REPO_ROOT / "docs" / "2-agent-system" / "AGENT_EFFECTIVENESS_LOG.md"


def _read_text(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8")
    except Exception:
        return ""


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def _clip(s: str, n: int) -> str:
    """Truncate at a word boundary with an ellipsis (no mid-word cuts)."""
    s = s.strip()
    if len(s) <= n:
        return s
    return s[:n].rsplit(" ", 1)[0].rstrip(" ,;:-") + "…"


def _numbered(titles: list[str]) -> str:
    if not titles:
        return "see code_review.md"
    if len(titles) == 1:
        return titles[0]
    return "; ".join(f"({i + 1}) {t}" for i, t in enumerate(titles))


def parse_plan(run_dir: Path) -> dict:
    txt = _read_text(run_dir / "plan.md")
    out = {"goal": "", "role": "", "approval": ""}
    if not txt:
        return out

    def section(name: str) -> str:
        m = re.search(
            r"^##\s+" + re.escape(name) + r"\s*\n(.*?)(?=^##\s|\Z)",
            txt, re.S | re.M,
        )
        return m.group(1).strip() if m else ""

    out["goal"] = " ".join(section("Goal").split())[:400]
    out["role"] = " ".join(section("Owning Role").split())[:160]
    m = re.search(r"Approval wording quoted from user:\s*(.+)", txt)
    if m:
        out["approval"] = m.group(1).strip().strip('"')
    return out


def parse_code_review(run_dir: Path):
    txt = _read_text(run_dir / "code_review.md")
    if not txt:
        return None

    def count(label: str) -> int:
        m = re.search(r"(?im)^\s*[-*]?\s*" + label + r":\s*(\d+)", txt)
        if m:
            return int(m.group(1))
        m = re.search(r"(?i)###\s+" + label + r"\s*\((\d+)\)", txt)
        return int(m.group(1)) if m else 0

    def titles(sev: str) -> list[str]:
        m = re.search(
            r"(?is)###\s+" + sev + r"\s*\(\d+\)\s*(.*?)(?=^###\s|^##\s|\Z)",
            txt, re.M,
        )
        if not m:
            return []
        out: list[str] = []
        for line in m.group(1).splitlines():
            if not line.startswith("- "):  # top-level bullets only (skip Evidence/etc. sub-bullets)
                continue
            t = line[2:].strip()
            for sep in (" — ", " -- "):  # the description follows the em-dash / double-hyphen
                if sep in t:
                    t = t.split(sep, 1)[1]
                    break
            t = re.sub(r"\s+", " ", t).strip(" -—:")
            if t:
                out.append(_clip(t, 120))
        return out[:2]

    # Recommendation: prefer the Summary-section verdict; fall back to the last
    # match anywhere (avoids grabbing "repair-then-recheck" prose mid-body).
    summ = re.search(r"(?is)^##\s+Summary\b(.*)$", txt, re.M)
    recs = re.findall(r"(?i)Recommendation:\s*`?([A-Za-z][\w-]*)",
                      summ.group(1) if summ else txt)
    rec = recs[-1] if recs else ""
    return {
        "blockers": count("Blockers"),
        "risks": count("Risks"),
        "nits": count("Nits"),
        "rec": rec,
        "risk_titles": titles("Risks"),
        "blocker_titles": titles("Blockers"),
    }


def parse_verify(run_dir: Path):
    d = _read_json(run_dir / "verify_report.json")
    if not d:
        return None
    bench = d.get("benchmark") or {}
    sc = d.get("scene_classifier") or {}
    warnings = d.get("warnings") or []
    return {
        "status": d.get("status"),
        "warnings": [
            (w.get("summary") if isinstance(w, dict) else str(w)) for w in warnings
        ],
        "benchmark_passed": bench.get("passed"),
        "benchmark_summary": (bench.get("summary", "") or "").split("; output=")[0],
        "scene_status": sc.get("status"),
    }


def parse_diagnosis(run_dir: Path):
    d = _read_json(run_dir / "diagnosis.json")
    if not isinstance(d, dict):
        return None
    summary = d.get("summary") or d.get("classification") or d.get("root_cause") or ""
    component = d.get("component") or d.get("owning_component") or ""
    if not (summary or component):
        return None
    return {"summary": str(summary)[:200], "component": str(component)[:80]}


def trace_facts(run_dir: Path) -> dict:
    d = _read_json(run_dir / "trace.summary.json") or {}
    keys = ("frames", "encoded_frames", "quality_region_total", "privacy_mask_total")
    return {k: d.get(k) for k in keys if d.get(k) is not None}


def detect_events(cr, vr, dg) -> list[dict]:
    """Each event: {role, label, detail}. Loggable iff a CATCH/DIAGNOSIS exists."""
    events: list[dict] = []
    if cr:
        if cr["blockers"] > 0:
            events.append({"role": "code-reviewer", "label": "BUG/BLOCKER caught",
                           "detail": f"{cr['blockers']} caught: {_numbered(cr['blocker_titles'])}",
                           "loggable": True})
        if cr["risks"] > 0:
            events.append({"role": "code-reviewer", "label": "RISK (latent footgun) caught",
                           "detail": f"{cr['risks']} flagged: {_numbered(cr['risk_titles'])}",
                           "loggable": True})
        if cr["blockers"] == 0 and cr["risks"] == 0 and cr["nits"] > 0:
            events.append({"role": "code-reviewer", "label": "nits only",
                           "detail": f"{cr['nits']} nit(s); no blocker/risk",
                           "loggable": False})
    if vr:
        if vr["status"] == "failed":
            events.append({"role": "trace-verifier", "label": "FAILURE caught",
                           "detail": "verify status=failed -- a break/regression was caught before shipping",
                           "loggable": True})
        if vr["warnings"]:
            wsum = "; ".join(w[:120] for w in vr["warnings"][:3] if w)
            events.append({"role": "trace-verifier", "label": "WARNING surfaced",
                           "detail": f"{len(vr['warnings'])} warning(s): {wsum}",
                           "loggable": True})
        if vr["benchmark_passed"] is False:
            events.append({"role": "benchmark-reporter", "label": "REGRESSION caught",
                           "detail": f"benchmark gate failed: {vr['benchmark_summary']}",
                           "loggable": True})
        elif vr["benchmark_passed"] is True:
            events.append({"role": "benchmark-reporter", "label": "measurement",
                           "detail": f"benchmark passed: {vr['benchmark_summary']}",
                           "loggable": False})
    if dg:
        events.append({"role": "debug-verifier", "label": "ROOT-CAUSE",
                       "detail": f"{dg['component']}: {dg['summary']}".strip(": "),
                       "loggable": True})
    return events


def run_date(run_dir: Path) -> str:
    m = re.search(r"(20\d{6})", run_dir.name)
    if m:
        s = m.group(1)
        return f"{s[0:4]}-{s[4:6]}-{s[6:8]}"
    return date.today().isoformat()


def _humanize_run_id(name: str) -> str:
    return re.sub(r"[_-]20\d{6}.*$", "", name).replace("_", " ").strip() or name


def short_title(plan: dict, run_dir: Path) -> str:
    g = plan.get("goal", "")
    if g:
        first = re.split(r"[.。\n]", g)[0].strip().replace("**", "").replace("`", "")
        # Use the goal sentence only when it is mostly ASCII (the log is English)
        # AND short enough to read as a title; otherwise humanize the run-id so
        # the title stays portable and never truncates mid-word.
        if first and sum(1 for c in first if ord(c) > 127) <= len(first) * 0.15 and len(first) <= 70:
            return first
    return _humanize_run_id(run_dir.name)


def render_report(run_dir, plan, cr, vr, dg, events) -> str:
    L = []
    L.append(f"AetherFlow agent-system report -- {run_dir.name}")
    L.append("=" * 60)
    if plan.get("goal"):
        L.append(f"Task: {short_title(plan, run_dir)}")
    if plan.get("role"):
        L.append(f"Owning role: {plan['role']}")
    facts = trace_facts(run_dir)
    if facts:
        L.append("Run facts: " + ", ".join(f"{k}={v}" for k, v in facts.items()))
    L.append("")
    loggable = [e for e in events if e["loggable"]]
    if loggable:
        L.append("VALUE delivered by the agent system:")
        for e in loggable:
            L.append(f"  [{e['role']}] {e['label']} -- {e['detail']}")
    else:
        if vr and vr.get("status") == "passed":
            L.append("ROUTINE: all gates passed and no agent caught/diagnosed anything")
        elif vr:
            L.append(f"ROUTINE: verify status={vr.get('status')}; no loggable agent catch")
        else:
            L.append("ROUTINE: no verification artifacts found in this run dir")
        L.append("(this run would NOT be appended to the effectiveness log).")
    other = [e for e in events if not e["loggable"]]
    if other:
        L.append("")
        L.append("Supporting / non-logged:")
        for e in other:
            L.append(f"  [{e['role']}] {e['label']} -- {e['detail']}")
    if cr and cr["rec"]:
        L.append("")
        L.append(f"Code-review recommendation: {cr['rec']}")
    L.append("")
    L.append(f"Loggable: {'YES' if loggable else 'no (routine)'}")
    return "\n".join(L)


def render_log_entry(run_dir, plan, cr, vr, events) -> str:
    loggable = [e for e in events if e["loggable"]]
    L = []
    L.append(f"## {run_date(run_dir)} — {short_title(plan, run_dir)}")
    role = re.split(r"[(.,]", plan.get("role", ""))[0].strip()
    L.append(f"Run: `.aetherflow/runs/{run_dir.name}/`" + (f"  ·  Role(s): {role}" if role else ""))
    L.append("")
    for e in loggable:
        L.append(f"- **{e['role']}** — {e['label']}: {e['detail']}")
    # benchmark measurement rides along as supporting detail
    for e in events:
        if not e["loggable"] and e["role"] == "benchmark-reporter" and e["label"] == "measurement":
            L.append(f"- **benchmark-reporter** — {e['detail']}")
    if cr and cr["rec"]:
        L.append(f"- Outcome: code-review **{cr['rec']}** "
                 f"(blockers={cr['blockers']}, risks={cr['risks']}, nits={cr['nits']}).")
    L.append("- Attribution: _UNVERIFIED — set at closeout "
             "(agent-autonomous / human-led→agent-executed / agent-flagged→human-decided)_")
    evidence = ["plan.md"]
    if cr:
        evidence.append("code_review.md")
    if vr:
        evidence.append("verify_report.json")
    ev = ", ".join(f"`{e}`" for e in evidence)
    L.append(f"- Evidence: in `.aetherflow/runs/{run_dir.name}/` — {ev}")
    L.append("")
    return "\n".join(L)


def append_to_log(entry: str, run_id: str, log_path: Path) -> str:
    existing = _read_text(log_path)
    if not existing:
        print(f"[agent_report] log not found at {log_path}; create it (with header) first.",
              file=sys.stderr)
        return "no-log"
    if f"runs/{run_id}/" in existing:
        return "already-logged"
    # insert after the first '---' divider that ends the header, else append
    marker = "\n---\n"
    idx = existing.find(marker)
    if idx >= 0:
        head = existing[: idx + len(marker)]
        tail = existing[idx + len(marker):]
        new = head + "\n" + entry + tail
    else:
        new = existing.rstrip() + "\n\n" + entry
    log_path.write_text(new, encoding="utf-8")
    return "appended"


def main() -> int:
    try:  # keep non-ASCII (Chinese plan goals/approvals) readable on a cp950 console
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="Report AetherFlow agent-system effectiveness for a run.")
    ap.add_argument("--run-dir", required=True, help="Path to .aetherflow/runs/<id>")
    ap.add_argument("--append", action="store_true",
                    help="Append a curated entry to the effectiveness log (loggable runs only).")
    ap.add_argument("--log-path", default=str(DEFAULT_LOG))
    args = ap.parse_args()

    run_dir = Path(args.run_dir).resolve()
    if not run_dir.is_dir():
        print(f"[agent_report] run dir not found: {run_dir}", file=sys.stderr)
        return 2

    plan = parse_plan(run_dir)
    cr = parse_code_review(run_dir)
    vr = parse_verify(run_dir)
    dg = parse_diagnosis(run_dir)
    events = detect_events(cr, vr, dg)

    print(render_report(run_dir, plan, cr, vr, dg, events))

    loggable = [e for e in events if e["loggable"]]
    if args.append:
        if not loggable:
            print("\n[agent_report] routine run -- nothing appended (no catch/diagnosis).")
            return 0
        entry = render_log_entry(run_dir, plan, cr, vr, events)
        status = append_to_log(entry, run_dir.name, Path(args.log_path).resolve())
        print(f"\n[agent_report] append: {status} -> {args.log_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
