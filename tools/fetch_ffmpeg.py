#!/usr/bin/env python3
"""Vendor the FFmpeg shared SDK for the SRT output stage (Windows).

Downloads a pinned BtbN FFmpeg win64 *shared* build and extracts the subset
AetherFlow links against into third_party/ffmpeg/ (gitignored, same model as
third_party/onnxruntime — see third_party/ffmpeg/SOURCE.md):

  third_party/ffmpeg/
    include/                 libavformat / libavcodec / libavutil / libsw* headers
    lib/                     avformat.lib avcodec.lib avutil.lib swresample.lib
    bin/                     matching DLLs + ffmpeg.exe/ffprobe.exe (pinned test caller)
    VERSION.json             provenance (tag, asset, sha256, extract date)

CMake auto-detects this directory and enables AETHERFLOW_ENABLE_SRT_OUTPUT.
Run from the repo root (any shell):

  python tools/fetch_ffmpeg.py

The pin (tag + asset + sha256) is deliberate: BtbN's `latest` release tag is
rolling, so we point at a dated autobuild whose assets never change. The LGPL
variant is used; it enables the `srt` protocol (verified at extract time) and
keeps dynamic linking license-clean.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

PIN_TAG = "autobuild-2026-07-01-13-54"
PIN_ASSET = "ffmpeg-n8.1.2-21-gce3c09c101-win64-lgpl-shared-8.1.zip"
PIN_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/"
    f"{PIN_TAG}/{PIN_ASSET}"
)
# SHA256 of PIN_ASSET. Verified on first vendored download (2026-07-02); a
# mismatch means the download is not the pinned artifact -- refuse to extract.
# The LGPL variant is pinned deliberately: it DOES enable the `srt` protocol
# (verified 2026-07-02 via `ffmpeg -protocols` on this exact asset), and
# dynamic linking against LGPL libavformat keeps AetherFlow's own license
# unaffected. (The GPL variant also has srt; not needed.)
PIN_SHA256 = "c45ddc95e9c8c7df9a4a79dced901de7eac503e446334937252988c8e5d5e99b"

# Import libs: only what AetherFlow links. Runtime bin/: keep ALL DLLs --
# AetherFlow.exe itself only loads avformat/avcodec/avutil/swresample, but the
# bundled ffmpeg.exe/ffprobe.exe (pinned SRT test caller) additionally load
# avfilter/avdevice/swscale/postproc and fail to start without them.
LIB_KEEP = {"avformat.lib", "avcodec.lib", "avutil.lib", "swresample.lib"}
EXE_KEEP = {"ffmpeg.exe", "ffprobe.exe"}
INCLUDE_KEEP_TOP = ("libavformat/", "libavcodec/", "libavutil/", "libswresample/")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path) -> None:
    print(f"[fetch_ffmpeg] downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "aetherflow-fetch/1"})
    with urllib.request.urlopen(req, timeout=120) as resp, dest.open("wb") as out:
        total = int(resp.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            if total:
                pct = done * 100 // total
                print(f"\r[fetch_ffmpeg]   {done // (1 << 20)} MiB / {total // (1 << 20)} MiB ({pct}%)", end="")
    print()


def extract_subset(zip_path: Path, out_dir: Path) -> None:
    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()
        # Archive root folder, e.g. "ffmpeg-n8.1.2-.../".
        root = names[0].split("/")[0] + "/"
        for name in names:
            if not name.startswith(root) or name.endswith("/"):
                continue
            rel = name[len(root):]
            keep = False
            if rel.startswith("include/") and rel[len("include/"):].startswith(INCLUDE_KEEP_TOP):
                keep = True
            elif rel.startswith("lib/") and Path(rel).name in LIB_KEEP:
                keep = True
            elif rel.startswith("bin/"):
                base = Path(rel).name.lower()
                if base in EXE_KEEP or base.endswith(".dll"):
                    keep = True
            if not keep:
                continue
            target = out_dir / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(name) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)


def verify_srt_protocol(ffmpeg_exe: Path) -> bool:
    try:
        out = subprocess.run(
            [str(ffmpeg_exe), "-hide_banner", "-protocols"],
            capture_output=True, text=True, timeout=30,
        ).stdout
    except OSError:
        return False
    return " srt" in out or "\nsrt" in out.replace("  ", " ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="Re-download and re-extract even if present.")
    parser.add_argument("--print-sha-only", action="store_true",
                        help="Download to temp, print sha256, do not extract (used to establish the pin).")
    args = parser.parse_args()

    out_dir = repo_root() / "third_party" / "ffmpeg"
    marker = out_dir / "VERSION.json"
    if marker.exists() and not args.force and not args.print_sha_only:
        existing = json.loads(marker.read_text(encoding="utf-8"))
        if existing.get("asset") == PIN_ASSET:
            print(f"[fetch_ffmpeg] already vendored ({PIN_ASSET}); use --force to refresh.")
            return 0

    # The zip is cached (gitignored) so re-extraction after a script fix does
    # not re-download ~75 MiB.
    tmp_zip = out_dir / "_download.zip"
    out_dir.mkdir(parents=True, exist_ok=True)
    if not (tmp_zip.exists() and sha256_of(tmp_zip) == PIN_SHA256):
        download(PIN_URL, tmp_zip)
    digest = sha256_of(tmp_zip)
    print(f"[fetch_ffmpeg] sha256 = {digest}")
    if args.print_sha_only:
        return 0
    if PIN_SHA256 and digest != PIN_SHA256:
        print(f"[fetch_ffmpeg] ERROR: sha256 mismatch (expected {PIN_SHA256}); refusing to extract.")
        tmp_zip.unlink()
        return 1

    for sub in ("include", "lib", "bin"):
        target = out_dir / sub
        if target.exists():
            shutil.rmtree(target)
    extract_subset(tmp_zip, out_dir)

    header = out_dir / "include" / "libavformat" / "avformat.h"
    lib = out_dir / "lib" / "avformat.lib"
    if not header.exists() or not lib.exists():
        print("[fetch_ffmpeg] ERROR: extraction incomplete (avformat header/lib missing).")
        return 1

    srt_ok = verify_srt_protocol(out_dir / "bin" / "ffmpeg.exe")
    if not srt_ok:
        print("[fetch_ffmpeg] ERROR: vendored build does not list the 'srt' protocol; "
              "SRT output would fail at runtime. Check the pinned asset.")
        return 1

    marker.write_text(json.dumps({
        "tag": PIN_TAG,
        "asset": PIN_ASSET,
        "url": PIN_URL,
        "sha256": digest,
        "srt_protocol_verified": srt_ok,
        "extracted_at": datetime.now(timezone.utc).isoformat(),
    }, indent=2) + "\n", encoding="utf-8")
    print(f"[fetch_ffmpeg] vendored to {out_dir} (srt protocol: OK). Reconfigure CMake to enable SRT output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
