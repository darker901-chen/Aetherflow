#include "AppConfig.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace AetherFlow {

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

int ParseIntOr(const std::string& v, int fallback) {
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(v, &consumed);
        return (consumed == v.size()) ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

bool ParseBoolOr(const std::string& v, bool fallback) {
    if (v == "1" || v == "true") return true;
    if (v == "0" || v == "false") return false;
    return fallback;
}

}  // namespace

void AppConfig::Normalize() {
    monitorIndex = (std::max)(0, monitorIndex);
    encoder = (std::min)(2, (std::max)(0, encoder));
    resolutionPreset = (std::min)(2, (std::max)(0, resolutionPreset));
    if (fps != 15 && fps != 30 && fps != 60) fps = 30;
    bitrateKbps = (std::min)(50000, (std::max)(500, bitrateKbps));
    maskMode = (std::min)(2, (std::max)(0, maskMode));
    srtPort = (std::min)(65535, (std::max)(1, srtPort));
    srtLatencyMs = (std::min)(5000, (std::max)(0, srtLatencyMs));
    // libsrt requires 10-79 chars; anything else falls back to no encryption
    // rather than shipping a value the listener would reject at runtime.
    if (!srtPassphrase.empty() &&
        (srtPassphrase.size() < 10 || srtPassphrase.size() > 79)) {
        srtPassphrase.clear();
    }
}

std::string AppConfig::ToIni() const {
    std::ostringstream out;
    out << "# AetherFlowStudio settings (auto-written; see docs/OPERATION_GUIDE.md)\n";
    out << "monitor_index=" << monitorIndex << "\n";
    out << "encoder=" << encoder << "\n";
    out << "resolution_preset=" << resolutionPreset << "\n";
    out << "fps=" << fps << "\n";
    out << "bitrate_kbps=" << bitrateKbps << "\n";
    out << "password_field_mask=" << (passwordFieldMask ? 1 : 0) << "\n";
    out << "notification_mask=" << (notificationMask ? 1 : 0) << "\n";
    out << "mask_mode=" << maskMode << "\n";
    out << "ai_scene_detection=" << (aiSceneDetection ? 1 : 0) << "\n";
    out << "srt_enabled=" << (srtEnabled ? 1 : 0) << "\n";
    out << "srt_port=" << srtPort << "\n";
    out << "srt_latency_ms=" << srtLatencyMs << "\n";
    out << "srt_passphrase=" << srtPassphrase << "\n";
    return out.str();
}

AppConfig AppConfig::FromIni(const std::string& text) {
    AppConfig cfg;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "monitor_index") cfg.monitorIndex = ParseIntOr(value, cfg.monitorIndex);
        else if (key == "encoder") cfg.encoder = ParseIntOr(value, cfg.encoder);
        else if (key == "resolution_preset") cfg.resolutionPreset = ParseIntOr(value, cfg.resolutionPreset);
        else if (key == "fps") cfg.fps = ParseIntOr(value, cfg.fps);
        else if (key == "bitrate_kbps") cfg.bitrateKbps = ParseIntOr(value, cfg.bitrateKbps);
        else if (key == "password_field_mask") cfg.passwordFieldMask = ParseBoolOr(value, cfg.passwordFieldMask);
        else if (key == "notification_mask") cfg.notificationMask = ParseBoolOr(value, cfg.notificationMask);
        else if (key == "mask_mode") cfg.maskMode = ParseIntOr(value, cfg.maskMode);
        else if (key == "ai_scene_detection") cfg.aiSceneDetection = ParseBoolOr(value, cfg.aiSceneDetection);
        else if (key == "srt_enabled") cfg.srtEnabled = ParseBoolOr(value, cfg.srtEnabled);
        else if (key == "srt_port") cfg.srtPort = ParseIntOr(value, cfg.srtPort);
        else if (key == "srt_latency_ms") cfg.srtLatencyMs = ParseIntOr(value, cfg.srtLatencyMs);
        else if (key == "srt_passphrase") cfg.srtPassphrase = value;
        // Unknown keys: ignored (forward compatibility).
    }
    cfg.Normalize();
    return cfg;
}

bool AppConfig::operator==(const AppConfig& other) const {
    return monitorIndex == other.monitorIndex &&
           encoder == other.encoder &&
           resolutionPreset == other.resolutionPreset &&
           fps == other.fps &&
           bitrateKbps == other.bitrateKbps &&
           passwordFieldMask == other.passwordFieldMask &&
           notificationMask == other.notificationMask &&
           maskMode == other.maskMode &&
           aiSceneDetection == other.aiSceneDetection &&
           srtEnabled == other.srtEnabled &&
           srtPort == other.srtPort &&
           srtLatencyMs == other.srtLatencyMs &&
           srtPassphrase == other.srtPassphrase;
}

bool AppConfig::SaveToFile(const std::filesystem::path& path) const {
    AppConfig normalized = *this;
    normalized.Normalize();
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << normalized.ToIni();
    return out.good();
}

bool AppConfig::LoadFromFile(const std::filesystem::path& path, AppConfig* out) {
    if (!out) return false;
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    *out = FromIni(buf.str());
    return true;
}

}  // namespace AetherFlow
