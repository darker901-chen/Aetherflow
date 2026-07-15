#include "AetherFlow/NotificationProducerModule.h"

#include <Windows.h>
#include <appmodel.h>
#include <Dwmapi.h>
#include <Psapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Psapi.lib")

namespace AetherFlow {

namespace {

constexpr const char* kSource = "notification-producer";
constexpr const char* kDebugLabel = "NotificationProducerModule";
constexpr int kMinWindowExtent = 8;
constexpr size_t kMaxMasksPerPoll = 32;

struct WindowCandidate {
    HWND hwnd = nullptr;
    unsigned long processId = 0;
    RECT rect = {};
    bool occludesLowerWindows = false;
};

int ScaleFloor(int value, int srcExtent, int dstExtent) {
    if (srcExtent <= 0 || dstExtent <= 0) {
        return 0;
    }
    return static_cast<int>((static_cast<long long>(value) * dstExtent) / srcExtent);
}

int ScaleCeil(int value, int srcExtent, int dstExtent) {
    if (srcExtent <= 0 || dstExtent <= 0) {
        return 0;
    }
    return static_cast<int>(
        (static_cast<long long>(value) * dstExtent + srcExtent - 1) / srcExtent);
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string LeafFilename(const std::string& path) {
    auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool IsTeamsAlias(const std::string& lowercaseLeaf) {
    return lowercaseLeaf == "teams.exe" ||
           lowercaseLeaf == "ms-teams.exe" ||
           lowercaseLeaf == "msteams.exe";
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

std::string QueryPackageFullNameLower(HANDLE process) {
    UINT32 length = 0;
    LONG rc = GetPackageFullName(process, &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return {};
    }

    std::wstring buffer(length, L'\0');
    rc = GetPackageFullName(process, &length, buffer.data());
    if (rc != ERROR_SUCCESS || length == 0) {
        return {};
    }
    return ToLowerAscii(WideToUtf8(buffer.c_str()));
}

std::string QueryApplicationUserModelIdLower(HANDLE process) {
    UINT32 length = 0;
    LONG rc = GetApplicationUserModelId(process, &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return {};
    }

    std::wstring buffer(length, L'\0');
    rc = GetApplicationUserModelId(process, &length, buffer.data());
    if (rc != ERROR_SUCCESS || length == 0) {
        return {};
    }
    return ToLowerAscii(WideToUtf8(buffer.c_str()));
}

bool ContainsAnyToken(const std::string& value, const std::vector<std::string>& tokens) {
    if (value.empty()) {
        return false;
    }
    for (const auto& token : tokens) {
        if (!token.empty() && value.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsDwmCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked;
}

bool IsEffectivelyTransparent(HWND hwnd) {
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TRANSPARENT) {
        return true;
    }

    if (exStyle & WS_EX_LAYERED) {
        COLORREF colorKey = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, &colorKey, &alpha, &flags) &&
            (flags & LWA_ALPHA) &&
            alpha < 16) {
            return true;
        }
    }

    return false;
}

bool TryGetWindowCandidate(HWND hwnd, WindowCandidate* outCandidate) {
    if (!outCandidate || !hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsDwmCloaked(hwnd)) {
        return false;
    }

    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect)) {
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < kMinWindowExtent || height < kMinWindowExtent) {
        return false;
    }

    if (IsEffectivelyTransparent(hwnd)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) {
        return false;
    }

    outCandidate->hwnd = hwnd;
    outCandidate->processId = static_cast<unsigned long>(processId);
    outCandidate->rect = rect;
    outCandidate->occludesLowerWindows = true;
    return true;
}

BOOL CALLBACK CollectTopLevelWindows(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowCandidate>*>(lParam);
    if (!windows) {
        return TRUE;
    }

    WindowCandidate candidate;
    if (TryGetWindowCandidate(hwnd, &candidate)) {
        windows->push_back(candidate);
    }
    return TRUE;
}

void AddOcclusionRect(HRGN occludedRegion, const RECT& rect) {
    if (!occludedRegion || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    HRGN windowRegion = CreateRectRgnIndirect(&rect);
    if (!windowRegion) {
        return;
    }
    CombineRgn(occludedRegion, occludedRegion, windowRegion, RGN_OR);
    DeleteObject(windowRegion);
}

bool ConvertScreenRectToMask(
    const RECT& rect,
    const FrameContext& context,
    int decisionWidth,
    int decisionHeight,
    int paddingPixels,
    FrameRegion* outMask) {
    if (!outMask || decisionWidth <= 0 || decisionHeight <= 0) {
        return false;
    }

    const int captureLeft = context.captureLeft;
    const int captureTop = context.captureTop;
    const int captureWidth = context.captureWidth > 0 ? context.captureWidth : context.width;
    const int captureHeight = context.captureHeight > 0 ? context.captureHeight : context.height;
    if (captureWidth <= 0 || captureHeight <= 0 || rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }

    const int captureRight = captureLeft + captureWidth;
    const int captureBottom = captureTop + captureHeight;
    const int clippedLeft = (std::max)(captureLeft, static_cast<int>(rect.left));
    const int clippedTop = (std::max)(captureTop, static_cast<int>(rect.top));
    const int clippedRight = (std::min)(captureRight, static_cast<int>(rect.right));
    const int clippedBottom = (std::min)(captureBottom, static_cast<int>(rect.bottom));
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
        return false;
    }

    const int relLeft = clippedLeft - captureLeft;
    const int relTop = clippedTop - captureTop;
    const int relRight = clippedRight - captureLeft;
    const int relBottom = clippedBottom - captureTop;

    FrameRegion mask;
    mask.purpose = FrameRegionPurpose::PrivacyMask;
    mask.left = ScaleFloor(relLeft, captureWidth, decisionWidth) - paddingPixels;
    mask.top = ScaleFloor(relTop, captureHeight, decisionHeight) - paddingPixels;
    mask.right = ScaleCeil(relRight, captureWidth, decisionWidth) + paddingPixels;
    mask.bottom = ScaleCeil(relBottom, captureHeight, decisionHeight) + paddingPixels;
    mask.left = (std::max)(0, (std::min)(decisionWidth, mask.left));
    mask.top = (std::max)(0, (std::min)(decisionHeight, mask.top));
    mask.right = (std::max)(0, (std::min)(decisionWidth, mask.right));
    mask.bottom = (std::max)(0, (std::min)(decisionHeight, mask.bottom));
    if (mask.right <= mask.left || mask.bottom <= mask.top) {
        return false;
    }

    mask.confidence = 1.0f;
    mask.source = kSource;
    mask.debugLabel = kDebugLabel;
    *outMask = std::move(mask);
    return true;
}

void AppendVisibleRegionMasks(
    HRGN visibleRegion,
    const FrameContext& context,
    int decisionWidth,
    int decisionHeight,
    int paddingPixels,
    std::vector<FrameRegion>* outMasks) {
    if (!visibleRegion || !outMasks || outMasks->size() >= kMaxMasksPerPoll) {
        return;
    }

    const DWORD dataSize = GetRegionData(visibleRegion, 0, nullptr);
    if (dataSize == 0) {
        return;
    }

    std::vector<std::uint8_t> data(dataSize);
    auto* regionData = reinterpret_cast<RGNDATA*>(data.data());
    if (GetRegionData(visibleRegion, dataSize, regionData) == 0) {
        return;
    }

    const RECT* rects = reinterpret_cast<const RECT*>(regionData->Buffer);
    for (DWORD i = 0; i < regionData->rdh.nCount && outMasks->size() < kMaxMasksPerPoll; ++i) {
        FrameRegion mask;
        if (ConvertScreenRectToMask(
                rects[i],
                context,
                decisionWidth,
                decisionHeight,
                paddingPixels,
                &mask)) {
            outMasks->push_back(std::move(mask));
        }
    }
}

} // namespace

NotificationProducerModule::NotificationProducerModule(
    std::vector<std::string> processWhitelist,
    int pollEveryFrames,
    int paddingPixels)
    : m_paddingPixels((std::max)(0, paddingPixels)) {
    const int frames = (std::max)(1, pollEveryFrames);
    m_pollIntervalMs = (std::min)(1000, (std::max)(50, frames * 1000 / 30));
    m_whitelist.reserve(processWhitelist.size());
    for (const auto& name : processWhitelist) {
        if (!name.empty()) {
            const std::string leaf = ToLowerAscii(LeafFilename(name));
            if (leaf.empty()) {
                continue;
            }
            m_whitelist.push_back(leaf);
            if (IsTeamsAlias(leaf)) {
                // Classic Teams used Teams.exe, while current packaged Teams
                // runs as ms-teams.exe under a WindowsApps MSTeams_* package.
                // Keep normal matching exact, but let the Teams default survive
                // executable leaf renames inside the same packaged app family.
                m_whitelist.push_back("teams.exe");
                m_whitelist.push_back("ms-teams.exe");
                m_whitelist.push_back("msteams.exe");
                m_identityTokens.push_back("msteams_");
                m_identityTokens.push_back("microsoftteams");
            }
        }
    }
    std::sort(m_whitelist.begin(), m_whitelist.end());
    m_whitelist.erase(std::unique(m_whitelist.begin(), m_whitelist.end()), m_whitelist.end());
    std::sort(m_identityTokens.begin(), m_identityTokens.end());
    m_identityTokens.erase(std::unique(m_identityTokens.begin(), m_identityTokens.end()), m_identityTokens.end());
}

NotificationProducerModule::~NotificationProducerModule() {
    StopPollThread();
}

bool NotificationProducerModule::Initialize(ID3D11Device* device, int width, int height) {
    (void)device;
    m_width = width;
    m_height = height;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cachedMasks.clear();
    }
    return m_width > 0 && m_height > 0 && !m_whitelist.empty();
}

void NotificationProducerModule::PublishGeometry(const FrameContext& context) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_geom.width = context.width;
    m_geom.height = context.height;
    m_geom.captureLeft = context.captureLeft;
    m_geom.captureTop = context.captureTop;
    m_geom.captureWidth = context.captureWidth;
    m_geom.captureHeight = context.captureHeight;
    m_geomReady = true;
}

void NotificationProducerModule::StartPollThreadOnce() {
    bool expected = false;
    if (!m_pollStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    m_pollStop.store(false);
    m_pollThread = std::thread([this]() { PollLoop(); });
}

void NotificationProducerModule::StopPollThread() {
    m_pollStop.store(true);
    m_pollCv.notify_all();
    if (m_pollThread.joinable()) {
        m_pollThread.join();
    }
}

void NotificationProducerModule::PollLoop() {
    while (!m_pollStop.load()) {
        FrameContext geom = {};
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            ready = m_geomReady;
            geom = m_geom;
        }
        if (ready) {
            std::vector<FrameRegion> fresh;
            RefreshMasks(geom, fresh);
            std::lock_guard<std::mutex> lk(m_mutex);
            m_cachedMasks.swap(fresh);
        }
        std::unique_lock<std::mutex> lk(m_mutex);
        m_pollCv.wait_for(lk, std::chrono::milliseconds(m_pollIntervalMs),
                          [this]() { return m_pollStop.load(); });
    }
}

void NotificationProducerModule::Warmup(const FrameContext& warmupContext) {
    PublishGeometry(warmupContext);
    StartPollThreadOnce();
}

bool NotificationProducerModule::MatchesWhitelist(unsigned long processId) const {
    if (m_whitelist.empty() || processId == 0) {
        return false;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(processId));
    if (!hProc) {
        return false;
    }

    char path[MAX_PATH] = {0};
    DWORD size = static_cast<DWORD>(MAX_PATH);
    BOOL ok = QueryFullProcessImageNameA(hProc, 0, path, &size);
    if (!ok || size == 0) {
        CloseHandle(hProc);
        return false;
    }

    const std::string fullPath = ToLowerAscii(std::string(path, size));
    const std::string leaf = LeafFilename(fullPath);
    if (std::binary_search(m_whitelist.begin(), m_whitelist.end(), leaf)) {
        CloseHandle(hProc);
        return true;
    }

    if (m_identityTokens.empty()) {
        CloseHandle(hProc);
        return false;
    }

    if (ContainsAnyToken(fullPath, m_identityTokens)) {
        CloseHandle(hProc);
        return true;
    }

    if (ContainsAnyToken(QueryPackageFullNameLower(hProc), m_identityTokens)) {
        CloseHandle(hProc);
        return true;
    }

    if (ContainsAnyToken(QueryApplicationUserModelIdLower(hProc), m_identityTokens)) {
        CloseHandle(hProc);
        return true;
    }

    CloseHandle(hProc);
    return false;
}

void NotificationProducerModule::Evaluate(const FrameContext& context, FrameDecision* decision) {
    if (!decision || m_width <= 0 || m_height <= 0 || m_whitelist.empty()) {
        return;
    }

    PublishGeometry(context);
    StartPollThreadOnce();

    std::vector<FrameRegion> masks;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        masks = m_cachedMasks;
    }
    if (masks.empty()) {
        return;
    }

    FrameScene proposed;
    proposed.type = FrameSceneType::SensitiveSurface;
    proposed.confidence = 1.0f;
    proposed.source = kSource;
    proposed.debugLabel = kDebugLabel;
    decision->ProposeScene(proposed);

    decision->privacyMasks.insert(
        decision->privacyMasks.end(),
        masks.begin(),
        masks.end());
}

void NotificationProducerModule::RefreshMasks(const FrameContext& context, std::vector<FrameRegion>& out) {
    out.clear();

    std::vector<WindowCandidate> windows;
    EnumWindows(CollectTopLevelWindows, reinterpret_cast<LPARAM>(&windows));
    if (windows.empty()) {
        return;
    }

    HRGN occludedRegion = CreateRectRgn(0, 0, 0, 0);
    if (!occludedRegion) {
        return;
    }

    const unsigned long currentProcessId = static_cast<unsigned long>(GetCurrentProcessId());
    std::unordered_map<unsigned long, bool> whitelistCache;

    for (const auto& window : windows) {
        bool whitelisted = false;
        if (window.processId != currentProcessId) {
            const auto cached = whitelistCache.find(window.processId);
            if (cached != whitelistCache.end()) {
                whitelisted = cached->second;
            } else {
                whitelisted = MatchesWhitelist(window.processId);
                whitelistCache.emplace(window.processId, whitelisted);
            }
        }

        if (whitelisted && out.size() < kMaxMasksPerPoll) {
            HRGN visibleRegion = CreateRectRgnIndirect(&window.rect);
            if (visibleRegion) {
                const int regionType = CombineRgn(
                    visibleRegion,
                    visibleRegion,
                    occludedRegion,
                    RGN_DIFF);
                if (regionType != ERROR && regionType != NULLREGION) {
                    AppendVisibleRegionMasks(
                        visibleRegion,
                        context,
                        m_width,
                        m_height,
                        m_paddingPixels,
                        &out);
                }
                DeleteObject(visibleRegion);
            }
        }

        if (window.occludesLowerWindows) {
            AddOcclusionRect(occludedRegion, window.rect);
        }
    }

    DeleteObject(occludedRegion);
}

} // namespace AetherFlow
