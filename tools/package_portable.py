#!/usr/bin/env python3
"""Build the AetherFlow portable folder + zip (spec Delta C v1).

Stages everything a clean Windows machine needs into a folder and zips it:

  AetherFlowStudio.exe      the double-click settings UI (spec Delta B)
  AetherFlow.exe            headless CLI (agent harness / scripting)
  libvpl.dll                Intel oneVPL dispatcher (vendored build output)
  onnxruntime.dll           only when the scene classifier was built
  avformat/avcodec/avutil/swresample DLLs   only when SRT was built
  msvcp140*/vcruntime140*   app-local VC++ runtime (no vc_redist install)
  LICENSES/                 FFmpeg LGPL + ImGui MIT + oneVPL MIT notices
  README_PORTABLE.txt       how to run, viewer URLs, firewall + SmartScreen notes

Usage (from the repo root, after a Release build):

  python tools/package_portable.py [--with-model] [--out-dir output]

--with-model additionally bundles models/scene_classifier_v1.onnx (~335 MB) so
the optional AI scene demo works offline. DirectML.dll is NOT bundled — Windows
10 1903+ ships it in System32 (documented in the README).

The script verifies the staged folder actually runs BEFORE zipping:
`AetherFlow.exe` must complete a short capture/encode run executed FROM the
staged folder (a missing DLL fails loudly), and `AetherFlowStudio.exe
--ui-smoke` must exit 0.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD_BIN = REPO / "build" / "Release"

FFMPEG_DLL_PREFIXES = ("avformat", "avcodec", "avutil", "swresample")
VCRT_NAMES = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
              "vcruntime140.dll", "vcruntime140_1.dll")

README_TEXT = """AetherFlow portable build
=========================

Double-click **AetherFlowStudio.exe** — pick the monitor, encoder, masks and
SRT settings in the window, press Start, and paste the shown srt:// URL into
VLC / ffplay on any device in the same LAN. Settings are remembered in
aetherflow_studio.ini next to the exe.

AetherFlow.exe is the command-line version of the same pipeline
(run `AetherFlow.exe --srt-output`; set AETHERFLOW_MAX_FRAMES=0 to run until
Ctrl+C).

Notes
-----
- Windows SmartScreen: this build is not code-signed, so the first launch may
  show "Windows protected your PC" — click "More info" -> "Run anyway".
- Firewall: to let OTHER devices watch the SRT stream, allow inbound UDP on
  the SRT port (default 8888) for the exe when Windows asks.
- Viewer delay: VLC buffers ~1 s by default (its network-caching setting).
  For lower delay: vlc --network-caching=200 srt://<ip>:8888
- Encoders: NVIDIA NVENC needs an NVIDIA driver; Intel oneVPL needs an Intel
  GPU. "Auto" picks whichever is present.
- The optional AI scene demo needs the ONNX model; builds packaged without it
  simply keep that feature off. DirectML.dll comes with Windows 10 1903+.
- Privacy: password fields and messenger windows (LINE/Teams/Slack/...) are
  masked BEFORE encoding, so they are already hidden in the stream.
- Recordings: every session also saves the encoded video + a per-frame trace
  into the `output` folder NEXT TO the exe (shown as "Recording to:" in the
  Studio window). Long sessions grow it at roughly 2.7 GB/hour at the default
  6 Mbps — delete it freely.

Licenses: see the LICENSES folder — FFmpeg (LGPL 2.1+, dynamically linked;
source: https://github.com/BtbN/FFmpeg-Builds), Dear ImGui (MIT), Intel oneVPL
dispatcher (MIT), ONNX Runtime (MIT), AetherFlow itself (MIT).
"""


def fail(msg: str) -> "sys.NoReturn":
    print(f"[package] ERROR: {msg}")
    sys.exit(1)


def copy(src: Path, dst_dir: Path, required: bool, manifest: list[str], note: str = "") -> bool:
    if not src.exists():
        if required:
            fail(f"required file missing: {src}")
        return False
    shutil.copy2(src, dst_dir / src.name)
    manifest.append(f"{src.name}  <- {src}{('  (' + note + ')') if note else ''}")
    return True


def find_vc_runtime() -> list[Path]:
    """App-local VC++ runtime DLLs. Prefer the VS redist directory; fall back
    to System32 copies (works, but the redist directory is the official
    redistributable source — the manifest records which one was used)."""
    candidates: list[Path] = []
    for vs_root in (Path(r"C:\Program Files\Microsoft Visual Studio"),
                    Path(r"C:\Program Files (x86)\Microsoft Visual Studio")):
        if vs_root.exists():
            candidates += sorted(vs_root.glob(r"*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14?.CRT"))
    if candidates:
        def toolset_key(p: Path) -> tuple:
            # numeric version sort ("14.40..." > "14.9...") — lexicographic
            # sorting would mis-pick here.
            m = re.search(r"MSVC[\\/]+([\d.]+)", str(p))
            return tuple(int(x) for x in m.group(1).split(".") if x.isdigit()) if m else ()
        crt_dir = max(candidates, key=toolset_key)
        found = [crt_dir / n for n in VCRT_NAMES if (crt_dir / n).exists()]
        if found:
            return found
    sys32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    return [sys32 / n for n in VCRT_NAMES if (sys32 / n).exists()]


def stage(out_root: Path, with_model: bool) -> Path:
    stage_dir = out_root / "AetherFlow-portable"
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True)
    manifest: list[str] = []

    copy(BUILD_BIN / "AetherFlow.exe", stage_dir, required=True, manifest=manifest)
    studio = copy(BUILD_BIN / "AetherFlowStudio.exe", stage_dir, required=False, manifest=manifest)
    if not studio:
        print("[package] NOTE: AetherFlowStudio.exe not built (no vendored imgui?) — CLI-only package.")
    copy(BUILD_BIN / "libvpl.dll", stage_dir, required=True, manifest=manifest)
    copy(BUILD_BIN / "onnxruntime.dll", stage_dir, required=False, manifest=manifest,
         note="scene classifier")
    for dll in sorted(BUILD_BIN.glob("*.dll")):
        if dll.name.lower().startswith(FFMPEG_DLL_PREFIXES):
            copy(dll, stage_dir, required=False, manifest=manifest, note="SRT output")

    vcrt = find_vc_runtime()
    if not vcrt:
        fail("no VC++ runtime DLLs found (VS redist dir or System32)")
    for dll in vcrt:
        copy(dll, stage_dir, required=True, manifest=manifest, note="app-local VC++ runtime")

    if with_model:
        models_dir = stage_dir / "models"
        models_dir.mkdir(exist_ok=True)
        copy(REPO / "models" / "scene_classifier_v1.onnx", models_dir,
             required=True, manifest=manifest, note="CLIP scene classifier (--with-model)")

    # License notices for every bundled binary (MIT retain-notice obligation
    # covers onnxruntime.dll and libvpl.dll too, not just FFmpeg/ImGui).
    licenses = stage_dir / "LICENSES"
    licenses.mkdir()
    ff_license = REPO / "third_party" / "ffmpeg" / "SOURCE.md"
    if ff_license.exists():
        shutil.copy2(ff_license, licenses / "FFMPEG-LGPL-NOTICE.md")
        manifest.append("LICENSES/FFMPEG-LGPL-NOTICE.md")
    imgui_license = REPO / "third_party" / "imgui" / "LICENSE.txt"
    if imgui_license.exists():
        shutil.copy2(imgui_license, licenses / "IMGUI-MIT-LICENSE.txt")
        manifest.append("LICENSES/IMGUI-MIT-LICENSE.txt")
    vpl_license = REPO / "third_party" / "onevpl" / "LICENSE"
    if vpl_license.exists():
        shutil.copy2(vpl_license, licenses / "ONEVPL-MIT-LICENSE.txt")
        manifest.append("LICENSES/ONEVPL-MIT-LICENSE.txt")
    if (stage_dir / "onnxruntime.dll").exists():
        # The vendored ORT tree (NuGet subset) does not carry its license file;
        # write the standard MIT notice so the shipped DLL is covered.
        (licenses / "ONNXRUNTIME-MIT-NOTICE.md").write_text(
            "ONNX Runtime (onnxruntime.dll)\n"
            "https://github.com/microsoft/onnxruntime\n\n"
            "MIT License\n\nCopyright (c) Microsoft Corporation\n\n"
            "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
            "of this software and associated documentation files (the \"Software\"), to deal\n"
            "in the Software without restriction, including without limitation the rights\n"
            "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
            "copies of the Software, and to permit persons to whom the Software is\n"
            "furnished to do so, subject to the following conditions:\n\n"
            "The above copyright notice and this permission notice shall be included in all\n"
            "copies or substantial portions of the Software.\n\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
            "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
            "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
            "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
            "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
            "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
            "SOFTWARE.\n",
            encoding="utf-8")
        manifest.append("LICENSES/ONNXRUNTIME-MIT-NOTICE.md")
    repo_license = REPO / "LICENSE"
    if repo_license.exists():
        shutil.copy2(repo_license, licenses / "AETHERFLOW-LICENSE.txt")
        manifest.append("LICENSES/AETHERFLOW-LICENSE.txt")

    (stage_dir / "README_PORTABLE.txt").write_text(README_TEXT, encoding="utf-8")
    manifest.append("README_PORTABLE.txt")
    (stage_dir / "package_manifest.txt").write_text(
        "\n".join(manifest) + "\n", encoding="utf-8")
    return stage_dir


def verify_staged(stage_dir: Path, verify_srt: bool) -> None:
    """Run the staged binaries FROM the staged folder so missing DLLs fail.

    With verify_srt (default when the FFmpeg DLLs are staged and the vendored
    ffmpeg.exe test caller exists), the staged run also opens the SRT listener
    and an ffmpeg caller must actually decode frames from it — the packaging
    plan's "SRT-probed run from the staged folder" gate.
    """
    env = os.environ.copy()
    # SRT leg streams ~10 s so the client probe has a comfortable window to
    # connect, buffer past probing, and decode its 30 frames.
    env["AETHERFLOW_MAX_FRAMES"] = "300" if verify_srt else "30"
    env["AETHERFLOW_OUTPUT_DIR"] = str(stage_dir / "_selftest_out")
    args = [str(stage_dir / "AetherFlow.exe")]
    probe = None
    if verify_srt:
        args += ["--srt-output", "--srt-port=8897"]
        print("[package] verifying staged AetherFlow.exe (300-frame run + SRT probe)...")
    else:
        print("[package] verifying staged AetherFlow.exe (30-frame run)...")
    app = subprocess.Popen(args, cwd=str(stage_dir), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace")
    probe_ok = None
    if verify_srt:
        ffmpeg = REPO / "third_party" / "ffmpeg" / "bin" / "ffmpeg.exe"
        probe_ok = False
        for _ in range(6):
            time.sleep(1)
            try:
                probe = subprocess.run(
                    [str(ffmpeg), "-hide_banner", "-loglevel", "error",
                     "-probesize", "100000", "-analyzeduration", "500000",
                     "-i", "srt://127.0.0.1:8897", "-frames:v", "30", "-f", "null", "-"],
                    capture_output=True, text=True, timeout=45,
                    encoding="utf-8", errors="replace")
            except subprocess.TimeoutExpired:
                continue  # connected-but-starved or listener not up yet; retry
            if probe.returncode == 0:
                probe_ok = True
                break
    try:
        out, _ = app.communicate(timeout=300)
    except subprocess.TimeoutExpired:
        app.kill()
        fail("staged AetherFlow.exe did not finish in time")
    if app.returncode != 0:
        print(out[-2000:])
        fail(f"staged AetherFlow.exe exited {app.returncode}")
    if verify_srt and not probe_ok:
        print((probe.stderr if probe else "")[-500:])
        print("[package] staged app [SRT] lines for diagnosis:")
        for line in out.splitlines():
            if "[SRT]" in line:
                print("   ", line)
        fail("staged SRT probe never decoded frames (srt://127.0.0.1:8897)")
    if verify_srt:
        print("[package] staged SRT probe decoded 30 frames OK")
    studio = stage_dir / "AetherFlowStudio.exe"
    if studio.exists():
        print("[package] verifying staged AetherFlowStudio.exe --ui-smoke...")
        proc = subprocess.run([str(studio), "--ui-smoke"], cwd=str(stage_dir),
                              timeout=120)
        if proc.returncode != 0:
            fail(f"staged AetherFlowStudio.exe --ui-smoke exited {proc.returncode}")
    # Self-test byproducts must not ship (and must not shadow first-run state).
    shutil.rmtree(stage_dir / "_selftest_out", ignore_errors=True)
    shutil.rmtree(stage_dir / "output", ignore_errors=True)
    for leftover in ("aetherflow_studio.ini", "imgui.ini"):
        try:
            (stage_dir / leftover).unlink()
        except FileNotFoundError:
            pass
    print("[package] staged-folder verification OK")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--with-model", action="store_true",
                        help="bundle models/scene_classifier_v1.onnx (~335 MB)")
    parser.add_argument("--out-dir", default=str(REPO / "output"))
    parser.add_argument("--skip-verify", action="store_true",
                        help="skip the staged-folder self-test (not recommended)")
    parser.add_argument("--skip-srt-probe", action="store_true",
                        help="staged self-test without the SRT client probe")
    args = parser.parse_args()

    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    stage_dir = stage(out_root, args.with_model)
    if not args.skip_verify:
        srt_possible = (stage_dir / "avformat-62.dll").exists() or \
                       any(stage_dir.glob("avformat-*.dll"))
        ffmpeg_ok = (REPO / "third_party" / "ffmpeg" / "bin" / "ffmpeg.exe").exists()
        verify_staged(stage_dir,
                      verify_srt=srt_possible and ffmpeg_ok and not args.skip_srt_probe)

    zip_path = out_root / f"AetherFlow-portable-{date.today():%Y%m%d}.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(stage_dir.rglob("*")):
            if path.is_file():
                zf.write(path, Path("AetherFlow-portable") / path.relative_to(stage_dir))
    size_mb = zip_path.stat().st_size / (1024 * 1024)
    print(f"[package] wrote {zip_path} ({size_mb:.1f} MiB)")
    print(f"[package] staged folder kept at {stage_dir} (delete freely)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
