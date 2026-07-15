// MacosNotificationProducerModule.mm
//
// Deterministic visible-window producer for macOS messenger / notification
// surfaces. Mirrors the Windows NotificationProducerModule structure --
// including its off-thread poll model -- but uses CGWindowListCopyWindowInfo
// instead of EnumWindows + GetWindowRect.
//
// Threading model: the CGWindowListCopyWindowInfo scan is a window-server
// round trip and must never run on the producer/capture thread. It runs on a
// dedicated background poll thread (time-based interval); Evaluate() only
// copies a mutex-protected cached snapshot. This is the macOS counterpart of
// the Windows NotificationProducerModule fix that removed the periodic
// every-N-frame producer-thread stall and the resulting recorded-video
// judder.
//
// Whitelist semantics: kCGWindowOwnerName (the localized application owner
// name -- e.g. "Slack", "Microsoft Teams", "Messages"). Case-insensitive
// match. NOT the executable leaf filename -- macOS bundles do not expose a
// per-window exe path the way Win32 does, so we use the owner name as the
// deterministic stable identifier.
//
// TCC / permissions: CGWindowListCopyWindowInfo with
// kCGWindowListOptionOnScreenOnly does not itself require Screen Recording
// permission for bounds + owner-name fields, so the producer is operational
// even before the user grants TCC. The downstream compositor (encoder agent's
// stage) is what actually needs Screen Recording.
//
// Spaces / multi-display: CGWindowList only returns windows on the current
// Space, in the global desktop coordinate system. The producer clips each
// rect to the active FrameContext capture region; windows on another Space
// or fully outside the capture region produce no mask.

#include "AetherFlow/platform/mac/MacosNotificationProducerModule.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace AetherFlow {
namespace platform {
namespace mac {

namespace {

constexpr const char* kSource = "notification-producer";
constexpr const char* kDebugLabel = "MacosNotificationProducerModule";
constexpr int kMinWindowExtent = 8;
constexpr size_t kMaxMasksPerPoll = 32;

struct IntRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool Valid() const { return right > left && bottom > top; }
};

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

bool CFNumberToInt32(CFNumberRef num, int32_t* out) {
    if (!num || !out) return false;
    return CFNumberGetValue(num, kCFNumberSInt32Type, out) != false;
}

bool CFNumberToDouble(CFNumberRef num, double* out) {
    if (!num || !out) return false;
    return CFNumberGetValue(num, kCFNumberDoubleType, out) != false;
}

std::string CFStringToStd(CFStringRef s) {
    if (!s) return {};
    if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) {
        return std::string(fast);
    }
    CFIndex length = CFStringGetLength(s);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(maxBytes), '\0');
    if (CFStringGetCString(s,
                           out.data(),
                           maxBytes,
                           kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    return {};
}

bool RectsIntersect(const IntRect& a, const IntRect& b) {
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

// Axis-aligned subtraction: split `r` against `occluder` and append the
// surviving pieces. Up to 4 fragments produced (top/bottom/left/right
// strips). Pieces with zero area are skipped.
void SubtractRect(const IntRect& r, const IntRect& occluder,
                  std::vector<IntRect>* out) {
    if (!RectsIntersect(r, occluder)) {
        out->push_back(r);
        return;
    }
    // Top strip
    if (r.top < occluder.top) {
        IntRect piece = r;
        piece.bottom = std::min(r.bottom, occluder.top);
        if (piece.Valid()) out->push_back(piece);
    }
    // Bottom strip
    if (r.bottom > occluder.bottom) {
        IntRect piece = r;
        piece.top = std::max(r.top, occluder.bottom);
        if (piece.Valid()) out->push_back(piece);
    }
    // Left strip (within vertical overlap range)
    const int innerTop = std::max(r.top, occluder.top);
    const int innerBottom = std::min(r.bottom, occluder.bottom);
    if (innerBottom > innerTop) {
        if (r.left < occluder.left) {
            IntRect piece;
            piece.left = r.left;
            piece.right = std::min(r.right, occluder.left);
            piece.top = innerTop;
            piece.bottom = innerBottom;
            if (piece.Valid()) out->push_back(piece);
        }
        if (r.right > occluder.right) {
            IntRect piece;
            piece.left = std::max(r.left, occluder.right);
            piece.right = r.right;
            piece.top = innerTop;
            piece.bottom = innerBottom;
            if (piece.Valid()) out->push_back(piece);
        }
    }
}

std::vector<IntRect> SubtractOccluders(const IntRect& candidate,
                                       const std::vector<IntRect>& occluders) {
    std::vector<IntRect> current = { candidate };
    for (const auto& occ : occluders) {
        if (current.empty()) break;
        std::vector<IntRect> next;
        next.reserve(current.size() * 2);
        for (const auto& piece : current) {
            SubtractRect(piece, occ, &next);
        }
        current.swap(next);
    }
    return current;
}

bool ClipToCaptureAndPad(const IntRect& globalRect,
                        const FrameContext& context,
                        int paddingPixels,
                        IntRect* outRel) {
    if (!outRel) return false;
    const int captureLeft = context.captureLeft;
    const int captureTop = context.captureTop;
    const int captureWidth = context.captureWidth > 0 ? context.captureWidth : context.width;
    const int captureHeight = context.captureHeight > 0 ? context.captureHeight : context.height;
    if (captureWidth <= 0 || captureHeight <= 0) return false;

    const int captureRight = captureLeft + captureWidth;
    const int captureBottom = captureTop + captureHeight;

    const int clippedLeft = std::max(captureLeft, globalRect.left);
    const int clippedTop = std::max(captureTop, globalRect.top);
    const int clippedRight = std::min(captureRight, globalRect.right);
    const int clippedBottom = std::min(captureBottom, globalRect.bottom);
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
        return false;
    }

    int relLeft = clippedLeft - captureLeft - paddingPixels;
    int relTop = clippedTop - captureTop - paddingPixels;
    int relRight = clippedRight - captureLeft + paddingPixels;
    int relBottom = clippedBottom - captureTop + paddingPixels;
    relLeft = std::max(0, std::min(captureWidth, relLeft));
    relTop = std::max(0, std::min(captureHeight, relTop));
    relRight = std::max(0, std::min(captureWidth, relRight));
    relBottom = std::max(0, std::min(captureHeight, relBottom));
    if (relRight <= relLeft || relBottom <= relTop) return false;

    outRel->left = relLeft;
    outRel->top = relTop;
    outRel->right = relRight;
    outRel->bottom = relBottom;
    return true;
}

} // namespace

MacosNotificationProducerModule::MacosNotificationProducerModule(
    std::vector<std::string> ownerNameWhitelist,
    int pollEveryFrames,
    int paddingPixels)
    : m_paddingPixels(std::max(0, paddingPixels)) {
    // Reinterpret the legacy frame-count knob as a wall-clock poll interval
    // (~30fps assumption), matching the Windows NotificationProducerModule
    // off-thread model. The poll thread is time-driven, not frame-gated.
    const int frames = std::max(1, pollEveryFrames);
    m_pollIntervalMs = std::min(1000, std::max(50, frames * 1000 / 30));
    m_whitelistLower.reserve(ownerNameWhitelist.size());
    for (auto& name : ownerNameWhitelist) {
        std::string trimmed = TrimAscii(name);
        if (trimmed.empty()) continue;
        m_whitelistLower.push_back(ToLowerAscii(std::move(trimmed)));
    }
    std::sort(m_whitelistLower.begin(), m_whitelistLower.end());
    m_whitelistLower.erase(
        std::unique(m_whitelistLower.begin(), m_whitelistLower.end()),
        m_whitelistLower.end());
}

MacosNotificationProducerModule::~MacosNotificationProducerModule() {
    StopPollThread();
}

bool MacosNotificationProducerModule::Initialize(ID3D11Device* device, int width, int height) {
    (void)device;
    m_width = width;
    m_height = height;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cachedMasks.clear();
    }
    return m_width > 0 && m_height > 0 && !m_whitelistLower.empty();
}

void MacosNotificationProducerModule::PublishGeometry(const FrameContext& context) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_geom.width = context.width;
    m_geom.height = context.height;
    m_geom.captureLeft = context.captureLeft;
    m_geom.captureTop = context.captureTop;
    m_geom.captureWidth = context.captureWidth;
    m_geom.captureHeight = context.captureHeight;
    m_geomReady = true;
}

void MacosNotificationProducerModule::StartPollThreadOnce() {
    bool expected = false;
    if (!m_pollStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    m_pollStop.store(false);
    m_pollThread = std::thread([this]() { PollLoop(); });
}

void MacosNotificationProducerModule::StopPollThread() {
    m_pollStop.store(true);
    m_pollCv.notify_all();
    if (m_pollThread.joinable()) {
        m_pollThread.join();
    }
}

void MacosNotificationProducerModule::PollLoop() {
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

void MacosNotificationProducerModule::Warmup(const FrameContext& warmupContext) {
    if (m_width <= 0 || m_height <= 0 || m_whitelistLower.empty()) {
        return;
    }
    // Start the off-thread poller now. CGWindowListCopyWindowInfo has a
    // measurable cold-start cost on the first call after process launch
    // (CoreGraphics service connection); it is paid on the poll thread's
    // first iteration -- off the producer/capture thread -- instead of
    // stalling the first encode frames.
    PublishGeometry(warmupContext);
    StartPollThreadOnce();
}

void MacosNotificationProducerModule::Evaluate(const FrameContext& context, FrameDecision* decision) {
    if (!decision || m_width <= 0 || m_height <= 0 || m_whitelistLower.empty()) {
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

void MacosNotificationProducerModule::RefreshMasks(const FrameContext& context,
                                                   std::vector<FrameRegion>& out) {
    out.clear();

    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windowList) return;

    const int32_t selfPid = static_cast<int32_t>(getpid());
    // CGWindowListCopyWindowInfo returns windows in front-to-back order. Each
    // entry that we iterate (whitelisted or not) potentially occludes lower
    // entries, so we accumulate every accepted rect into `occluders`.
    std::vector<IntRect> occluders;
    occluders.reserve(static_cast<size_t>(CFArrayGetCount(windowList)));

    const CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; ++i) {
        if (out.size() >= kMaxMasksPerPoll) break;

        CFDictionaryRef dict = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(windowList, i));
        if (!dict) continue;

        // Owner PID -- skip self so we never mask our own preview window.
        int32_t ownerPid = 0;
        CFNumberRef pidNum = static_cast<CFNumberRef>(
            CFDictionaryGetValue(dict, kCGWindowOwnerPID));
        if (CFNumberToInt32(pidNum, &ownerPid) && ownerPid == selfPid) {
            continue;
        }

        // Layer -- only normal windows (layer == 0). Higher layers are
        // system overlays (menubar, Dock, dropdowns); lower are wallpaper.
        int32_t layer = 0;
        CFNumberRef layerNum = static_cast<CFNumberRef>(
            CFDictionaryGetValue(dict, kCGWindowLayer));
        if (!CFNumberToInt32(layerNum, &layer) || layer != 0) {
            continue;
        }

        // Alpha -- skip effectively-invisible windows (matches Windows
        // WS_EX_LAYERED low-alpha guard).
        double alpha = 1.0;
        CFNumberRef alphaNum = static_cast<CFNumberRef>(
            CFDictionaryGetValue(dict, kCGWindowAlpha));
        if (alphaNum && CFNumberToDouble(alphaNum, &alpha) && alpha <= 0.0625) {
            continue;
        }

        // OnScreen flag -- defensive even though we passed
        // kCGWindowListOptionOnScreenOnly; the key may be absent.
        CFBooleanRef onScreen = static_cast<CFBooleanRef>(
            CFDictionaryGetValue(dict, kCGWindowIsOnscreen));
        if (onScreen && CFBooleanGetValue(onScreen) == false) {
            continue;
        }

        // Bounds (global desktop coords).
        CFDictionaryRef boundsDict = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(dict, kCGWindowBounds));
        if (!boundsDict) continue;
        CGRect cgRect = CGRectZero;
        if (!CGRectMakeWithDictionaryRepresentation(boundsDict, &cgRect)) {
            continue;
        }
        IntRect rect;
        rect.left = static_cast<int>(std::floor(cgRect.origin.x));
        rect.top = static_cast<int>(std::floor(cgRect.origin.y));
        rect.right = static_cast<int>(std::ceil(cgRect.origin.x + cgRect.size.width));
        rect.bottom = static_cast<int>(std::ceil(cgRect.origin.y + cgRect.size.height));
        if (rect.right - rect.left < kMinWindowExtent ||
            rect.bottom - rect.top < kMinWindowExtent) {
            continue;
        }

        // Owner name -> case-insensitive whitelist match.
        CFStringRef ownerName = static_cast<CFStringRef>(
            CFDictionaryGetValue(dict, kCGWindowOwnerName));
        std::string ownerLower = ToLowerAscii(CFStringToStd(ownerName));
        const bool whitelisted = !ownerLower.empty() &&
            std::binary_search(m_whitelistLower.begin(),
                               m_whitelistLower.end(),
                               ownerLower);

        if (whitelisted) {
            // Subtract every already-iterated window (front-to-back),
            // emit the surviving fragments as privacy mask regions.
            std::vector<IntRect> visible = SubtractOccluders(rect, occluders);
            for (const auto& piece : visible) {
                if (out.size() >= kMaxMasksPerPoll) break;
                IntRect rel;
                if (!ClipToCaptureAndPad(piece, context, m_paddingPixels, &rel)) {
                    continue;
                }
                FrameRegion mask;
                mask.purpose = FrameRegionPurpose::PrivacyMask;
                mask.left = rel.left;
                mask.top = rel.top;
                mask.right = rel.right;
                mask.bottom = rel.bottom;
                mask.confidence = 1.0f;
                mask.source = kSource;
                mask.debugLabel = kDebugLabel;
                out.push_back(std::move(mask));
            }
        }

        // Any iterated window may occlude windows behind it, regardless of
        // whitelist status. Add after consideration so we do not subtract a
        // window from itself.
        occluders.push_back(rect);
    }

    CFRelease(windowList);
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
