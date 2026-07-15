#pragma once

// AetherFlowStudio persisted settings (spec Delta B §5): every UI control maps
// to one field here; Save/Load round-trips them through a tiny key=value INI
// (`aetherflow_studio.ini` beside the exe) so the app remembers the last
// choices. Pure logic — no ImGui / Win32 / FFmpeg dependency — so it
// unit-tests everywhere (tests/test_appconfig.cpp).

#include <filesystem>
#include <string>

namespace AetherFlow {

struct AppConfig {
    // Capture / encode
    int monitorIndex = 0;
    int encoder = 0;          // 0 Auto, 1 NVENC, 2 oneVPL (EncoderPreference order)
    int resolutionPreset = 1; // 0 Native, 1 1080p, 2 720p
    int fps = 30;
    int bitrateKbps = 6000;   // spec default 6 Mbps

    // Deterministic privacy masks
    bool passwordFieldMask = true;
    bool notificationMask = true;
    int maskMode = 1;         // 0 blackout, 1 blur, 2 mosaic (blur = v0.2 default)

    // AI scene detection (advisory indicator only). Default OFF — the AI is
    // opt-in product-wide. The UI additionally requires the ONNX model to be
    // present on disk before the toggle can take effect; a saved `true` with
    // no model renders grayed out and starts sessions without a model path.
    bool aiSceneDetection = false;

    // SRT live output
    bool srtEnabled = true;   // the Studio exists to stream; CLI default stays off
    int srtPort = 8888;
    int srtLatencyMs = 120;
    std::string srtPassphrase;

    // Serialize to / parse from the INI text format ("key=value" lines,
    // '#' comments). Unknown keys are ignored; missing keys keep defaults;
    // out-of-range values are clamped by Normalize().
    std::string ToIni() const;
    static AppConfig FromIni(const std::string& text);

    // Clamp every field into its valid range (applied by FromIni and before
    // Save so a hand-edited file cannot produce an invalid pipeline setup).
    void Normalize();

    bool operator==(const AppConfig& other) const;

    // File helpers (thin wrappers over ToIni/FromIni; return false on I/O
    // failure — callers treat that as "use defaults"). filesystem::path keeps
    // non-ACP exe directories working (fstream path overloads end-to-end).
    bool SaveToFile(const std::filesystem::path& path) const;
    static bool LoadFromFile(const std::filesystem::path& path, AppConfig* out);
};

}  // namespace AetherFlow
