#include "AetherFlow/PasswordFieldPrivacyMaskModule.h"

#include <Windows.h>
#include <UIAutomation.h>
#include <atlbase.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace AetherFlow {

namespace {

constexpr const char* kSource = "password-field-privacy-mask";
constexpr const char* kDebugLabel = "PasswordFieldPrivacyMaskModule";
constexpr int kMaxPasswordMasks = 16;

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

bool SameRect(const FrameRegion& lhs, const FrameRegion& rhs) {
    return lhs.left == rhs.left &&
        lhs.top == rhs.top &&
        lhs.right == rhs.right &&
        lhs.bottom == rhs.bottom;
}

bool IsDuplicate(const std::vector<FrameRegion>& masks, const FrameRegion& mask) {
    return std::any_of(masks.begin(), masks.end(), [&](const FrameRegion& existing) {
        return SameRect(existing, mask);
    });
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

bool MakePropertyCondition(
    IUIAutomation* automation,
    PROPERTYID propertyId,
    VARIANT value,
    IUIAutomationCondition** condition) {
    if (!automation || !condition) {
        return false;
    }
    *condition = nullptr;
    return SUCCEEDED(automation->CreatePropertyCondition(propertyId, value, condition)) && *condition;
}

} // namespace

PasswordFieldPrivacyMaskModule::PasswordFieldPrivacyMaskModule(int pollEveryFrames, int paddingPixels)
    : m_paddingPixels((std::max)(0, paddingPixels)) {
    // The UIA scan now runs on a dedicated poll thread, so the cadence is
    // time-based. Map the legacy frame count to a ~30fps interval, clamped.
    const int frames = (std::max)(1, pollEveryFrames);
    m_pollIntervalMs = (std::min)(1000, (std::max)(50, frames * 1000 / 30));
}

PasswordFieldPrivacyMaskModule::~PasswordFieldPrivacyMaskModule() {
    StopPollThread();
    // m_automation is created AND released on the poll thread (see PollLoop).
    // We deliberately never pair m_comInitOwned with CoUninitialize: the poll
    // thread keeps COM live for its lifetime and the process is exiting.
}

bool PasswordFieldPrivacyMaskModule::Initialize(ID3D11Device* device, int width, int height) {
    (void)device;
    m_width = width;
    m_height = height;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cachedMasks.clear();
    }
    return m_width > 0 && m_height > 0;
}

bool PasswordFieldPrivacyMaskModule::EnsureAutomation() {
    if (m_automation) {
        return true;
    }

    if (!m_comInitOwned) {
        // CoInitializeEx is per-thread. If the producer thread already has COM
        // initialized in a compatible mode, this returns S_FALSE and we leave
        // it alone (someone else owns the lifetime). Otherwise we own it for
        // the rest of the process — see the destructor for why we never pair
        // this with CoUninitialize.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
            m_comInitOwned = true;
        } else {
            return false;
        }
    }

    HRESULT hr = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_automation));
    if (FAILED(hr) || !m_automation) {
        m_automation = nullptr;
        return false;
    }
    return true;
}

void PasswordFieldPrivacyMaskModule::PublishGeometry(const FrameContext& context) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_geom.width = context.width;
    m_geom.height = context.height;
    m_geom.captureLeft = context.captureLeft;
    m_geom.captureTop = context.captureTop;
    m_geom.captureWidth = context.captureWidth;
    m_geom.captureHeight = context.captureHeight;
    m_geomReady = true;
}

void PasswordFieldPrivacyMaskModule::StartPollThreadOnce() {
    bool expected = false;
    if (!m_pollStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    m_pollStop.store(false);
    m_pollThread = std::thread([this]() { PollLoop(); });
}

void PasswordFieldPrivacyMaskModule::StopPollThread() {
    m_pollStop.store(true);
    m_pollCv.notify_all();
    if (m_pollThread.joinable()) {
        m_pollThread.join();
    }
}

void PasswordFieldPrivacyMaskModule::PollLoop() {
    // COM + the IUIAutomation pointer live entirely on this thread.
    EnsureAutomation();

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

    if (m_automation) {
        m_automation->Release();
        m_automation = nullptr;
    }
}

void PasswordFieldPrivacyMaskModule::Warmup(const FrameContext& warmupContext) {
    // The expensive UIA walk no longer runs on the producer thread, so warmup
    // just publishes capture geometry and starts the poll thread.
    PublishGeometry(warmupContext);
    StartPollThreadOnce();
}

void PasswordFieldPrivacyMaskModule::Evaluate(const FrameContext& context, FrameDecision* decision) {
    if (!decision || m_width <= 0 || m_height <= 0) {
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

void PasswordFieldPrivacyMaskModule::RefreshMasks(const FrameContext& context, std::vector<FrameRegion>& out) {
    out.clear();

    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == GetCurrentProcessId()) {
        return;
    }

    if (!EnsureAutomation()) {
        return;
    }

    CComPtr<IUIAutomationElement> root;
    HRESULT hr = m_automation->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        return;
    }

    VARIANT editValue = {};
    editValue.vt = VT_I4;
    editValue.lVal = UIA_EditControlTypeId;
    CComPtr<IUIAutomationCondition> editCondition;
    if (!MakePropertyCondition(m_automation, UIA_ControlTypePropertyId, editValue, &editCondition)) {
        return;
    }

    VARIANT passwordValue = {};
    passwordValue.vt = VT_BOOL;
    passwordValue.boolVal = VARIANT_TRUE;
    CComPtr<IUIAutomationCondition> passwordCondition;
    if (!MakePropertyCondition(m_automation, UIA_IsPasswordPropertyId, passwordValue, &passwordCondition)) {
        return;
    }

    CComPtr<IUIAutomationCondition> combinedCondition;
    hr = m_automation->CreateAndCondition(editCondition, passwordCondition, &combinedCondition);
    if (FAILED(hr) || !combinedCondition) {
        return;
    }

    CComPtr<IUIAutomationElementArray> matches;
    hr = root->FindAll(TreeScope_Subtree, combinedCondition, &matches);
    if (FAILED(hr) || !matches) {
        return;
    }

    int length = 0;
    if (FAILED(matches->get_Length(&length)) || length <= 0) {
        return;
    }

    const int limit = (std::min)(length, kMaxPasswordMasks);
    for (int i = 0; i < limit; ++i) {
        CComPtr<IUIAutomationElement> element;
        if (FAILED(matches->GetElement(i, &element)) || !element) {
            continue;
        }

        RECT rect = {};
        if (FAILED(element->get_CurrentBoundingRectangle(&rect))) {
            continue;
        }

        FrameRegion mask;
        if (!ConvertScreenRectToMask(rect, context, m_width, m_height, m_paddingPixels, &mask)) {
            continue;
        }
        if (IsDuplicate(out, mask)) {
            continue;
        }
        out.push_back(mask);
    }
}

} // namespace AetherFlow
