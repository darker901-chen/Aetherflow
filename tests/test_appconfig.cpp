// Unit tests for the AetherFlowStudio settings persistence (AppConfig):
// INI round-trip, defaults on missing/unknown keys, and Normalize() clamping
// so a hand-edited file can never produce an invalid pipeline setup.

#include "../src/ui/AppConfig.h"
#include "test_assert.h"

using AetherFlow::AppConfig;

int main() {
    // --- Defaults match the spec table (monitor 0, Auto, 1080p, 30 fps,
    //     6 Mbps, masks on, blur, SRT 8888/120/no passphrase).
    AppConfig def;
    CHECK(def.monitorIndex == 0);
    CHECK(def.encoder == 0);
    CHECK(def.resolutionPreset == 1);
    CHECK(def.fps == 30);
    CHECK(def.bitrateKbps == 6000);
    CHECK(def.passwordFieldMask);
    CHECK(def.notificationMask);
    CHECK(def.maskMode == 1);
    CHECK_MSG(!def.aiSceneDetection, "AI scene detection is opt-in (default OFF)");
    CHECK(def.srtEnabled);
    CHECK(def.srtPort == 8888);
    CHECK(def.srtLatencyMs == 120);
    CHECK(def.srtPassphrase.empty());

    // --- Round-trip: every field survives ToIni -> FromIni.
    AppConfig a;
    a.monitorIndex = 1;
    a.encoder = 2;
    a.resolutionPreset = 2;
    a.fps = 60;
    a.bitrateKbps = 2500;
    a.passwordFieldMask = false;
    a.notificationMask = false;
    a.maskMode = 2;
    a.aiSceneDetection = true;
    a.srtEnabled = false;
    a.srtPort = 9001;
    a.srtLatencyMs = 250;
    a.srtPassphrase = "correct-horse-battery";
    const AppConfig b = AppConfig::FromIni(a.ToIni());
    CHECK_MSG(a == b, "ToIni/FromIni round-trip must be lossless");

    // --- Missing keys keep defaults; unknown keys and comments are ignored.
    const AppConfig c = AppConfig::FromIni(
        "# comment\n"
        "unknown_key=42\n"
        "srt_port=9500\n"
        "\n"
        "; another comment style\n");
    CHECK(c.srtPort == 9500);
    CHECK(c.fps == 30);
    CHECK(c.bitrateKbps == 6000);

    // --- Whitespace tolerance around key/value.
    const AppConfig ws = AppConfig::FromIni("  fps =  60  \n");
    CHECK(ws.fps == 60);

    // --- Clamping: out-of-range / garbage values normalize instead of
    //     propagating into the pipeline.
    const AppConfig d = AppConfig::FromIni(
        "monitor_index=-3\n"
        "encoder=9\n"
        "resolution_preset=7\n"
        "fps=24\n"
        "bitrate_kbps=50\n"
        "mask_mode=5\n"
        "srt_port=70000\n"
        "srt_latency_ms=999999\n"
        "srt_passphrase=short\n");
    CHECK(d.monitorIndex == 0);
    CHECK(d.encoder == 2);
    CHECK(d.resolutionPreset == 2);
    CHECK_MSG(d.fps == 30, "unsupported fps falls back to 30");
    CHECK(d.bitrateKbps == 500);
    CHECK(d.maskMode == 2);
    CHECK(d.srtPort == 65535);
    CHECK(d.srtLatencyMs == 5000);
    CHECK_MSG(d.srtPassphrase.empty(), "libsrt-invalid passphrase (<10 chars) is dropped");

    // --- Non-numeric values keep the field's default.
    const AppConfig e = AppConfig::FromIni("fps=abc\nsrt_port=12x\n");
    CHECK(e.fps == 30);
    CHECK(e.srtPort == 8888);

    // --- ai_scene_detection parses 1/0/true/false; garbage keeps default OFF.
    CHECK(AppConfig::FromIni("ai_scene_detection=1\n").aiSceneDetection);
    CHECK(!AppConfig::FromIni("ai_scene_detection=0\n").aiSceneDetection);
    CHECK_MSG(!AppConfig::FromIni("ai_scene_detection=maybe\n").aiSceneDetection,
              "garbage bool keeps the opt-in default OFF");

    return aetherflow_test::Summary();
}
