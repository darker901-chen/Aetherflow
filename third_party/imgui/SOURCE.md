# Vendored Dear ImGui (AetherFlowStudio settings UI)

Everything here except this file is **gitignored** — the pinned Dear ImGui
subset the `AetherFlowStudio` target compiles (core + Win32/D3D11 backends).
Same vendoring model as `third_party/ffmpeg/` and `third_party/onnxruntime/`.

## SOP — recreate this directory

```bash
python tools/fetch_imgui.py
```

Pin (also hardcoded in `tools/fetch_imgui.py`):

| Field | Value |
|---|---|
| Tag | `v1.92.8` |
| Archive | `https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip` |
| SHA256 | `27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870` |
| License | MIT (`LICENSE.txt` is extracted alongside the sources) |

## What gets extracted

Core: `imgui.h`, `imgui_internal.h`, `imconfig.h`, `imstb_*.h`, `imgui.cpp`,
`imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`
(not compiled), `LICENSE.txt`. Backends: `backends/imgui_impl_win32.*`,
`backends/imgui_impl_dx11.*`.

CMake auto-detects `third_party/imgui/imgui.h`; absent ⇒ the Studio target is
not generated (the headless `AetherFlow.exe` and CI are unaffected).
