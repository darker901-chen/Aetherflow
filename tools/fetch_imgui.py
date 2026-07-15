#!/usr/bin/env python3
"""Vendor Dear ImGui for the AetherFlowStudio settings UI (Windows).

Downloads the pinned Dear ImGui release archive and extracts the subset the
Studio target compiles (core + Win32/D3D11 backends) into
third_party/imgui/ (gitignored; same model as third_party/ffmpeg — see
third_party/imgui/SOURCE.md). Run from the repo root:

  python tools/fetch_imgui.py

CMake auto-detects third_party/imgui/imgui.h and only then builds the
AetherFlowStudio target.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

PIN_TAG = "v1.92.8"
PIN_URL = f"https://github.com/ocornut/imgui/archive/refs/tags/{PIN_TAG}.zip"
# SHA256 of the release archive, recorded on first vendored download
# (2026-07-03). GitHub tag archives are content-stable for a given tag.
PIN_SHA256 = "27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870"

KEEP_ROOT = {
    "imgui.h", "imgui_internal.h", "imconfig.h",
    "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h",
    "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
    "imgui_demo.cpp", "LICENSE.txt",
}
KEEP_BACKENDS = {
    "imgui_impl_win32.h", "imgui_impl_win32.cpp",
    "imgui_impl_dx11.h", "imgui_impl_dx11.cpp",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--print-sha-only", action="store_true")
    args = parser.parse_args()

    out_dir = repo_root() / "third_party" / "imgui"
    marker = out_dir / "VERSION.json"
    if marker.exists() and not args.force and not args.print_sha_only:
        print(f"[fetch_imgui] already vendored ({PIN_TAG}); use --force to refresh.")
        return 0

    out_dir.mkdir(parents=True, exist_ok=True)
    tmp_zip = out_dir / "_download.zip"
    if not (tmp_zip.exists() and sha256_of(tmp_zip) == PIN_SHA256):
        print(f"[fetch_imgui] downloading {PIN_URL}")
        req = urllib.request.Request(PIN_URL, headers={"User-Agent": "aetherflow-fetch/1"})
        with urllib.request.urlopen(req, timeout=120) as resp, tmp_zip.open("wb") as out:
            shutil.copyfileobj(resp, out)
    digest = sha256_of(tmp_zip)
    print(f"[fetch_imgui] sha256 = {digest}")
    if args.print_sha_only:
        return 0
    if PIN_SHA256 and not PIN_SHA256.startswith("0000") and digest != PIN_SHA256:
        print(f"[fetch_imgui] ERROR: sha256 mismatch (expected {PIN_SHA256}); refusing to extract.")
        return 1

    for sub in ("backends",):
        target = out_dir / sub
        if target.exists():
            shutil.rmtree(target)
    with zipfile.ZipFile(tmp_zip) as zf:
        names = zf.namelist()
        root = names[0].split("/")[0] + "/"
        for name in names:
            if not name.startswith(root) or name.endswith("/"):
                continue
            rel = name[len(root):]
            keep = (
                ("/" not in rel and rel in KEEP_ROOT) or
                (rel.startswith("backends/") and Path(rel).name in KEEP_BACKENDS)
            )
            if not keep:
                continue
            target = out_dir / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(name) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)

    if not (out_dir / "imgui.h").exists() or not (out_dir / "backends" / "imgui_impl_dx11.cpp").exists():
        print("[fetch_imgui] ERROR: extraction incomplete.")
        return 1

    marker.write_text(json.dumps({
        "tag": PIN_TAG,
        "url": PIN_URL,
        "sha256": digest,
        "extracted_at": datetime.now(timezone.utc).isoformat(),
    }, indent=2) + "\n", encoding="utf-8")
    print(f"[fetch_imgui] vendored to {out_dir}. Reconfigure CMake to enable AetherFlowStudio.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
