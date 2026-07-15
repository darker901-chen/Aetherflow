// Unit test for PolicyEngine hysteresis (mode-switch debounce).
//
// Contract (src/policy/PolicyEngine.cpp):
//   - Evaluate(const FrameContext&, FrameDecision*) is pure C++: it reads the
//     decision's merged scene (type + confidence + source) and FrameContext,
//     and mutates internal hysteresis state. No D3D11 work — FrameContext's
//     texture pointers stay null.
//   - To promote a new mode (count a switch) the engine needs:
//       * scene confidence >= kLowConfidenceThreshold (0.6), and a non-unknown
//         classified scene,
//       * the class seen kConsecutiveSameClassToSwitch (3) frames in a row,
//       * at least kMinFramesBetweenSwitches (150) frames since the last switch
//         (the FIRST-ever switch is unconstrained: m_lastSwitchFrame < 0 is
//         treated as "long enough").
//   - ModeSwitches() returns the number of mode_label transitions since
//     Initialize. The initial mode_label is "initial->balanced"; the first
//     real promotion (e.g. code_text->text_safe) counts as exactly one switch.
//
// This test drives the real PolicyEngine.cpp (linked by CMake) with synthetic
// FrameContext/FrameDecision values — no runtime, capture, or D3D11 needed.

#include "AetherFlow/IAIFrameAnalyzer.h"
#include "AetherFlow/policy/PolicyEngine.h"

#include "test_assert.h"

using AetherFlow::FrameContext;
using AetherFlow::FrameDecision;
using AetherFlow::FrameScene;
using AetherFlow::FrameSceneType;
using AetherFlow::PolicyEngine;

namespace {

// Build a FrameContext with null textures (no D3D needed) at a given frame.
FrameContext MakeContext(int frameIndex) {
    FrameContext ctx;                 // texture pointers default to nullptr
    ctx.frameIndex = frameIndex;
    ctx.elapsedSeconds = frameIndex / 30.0;
    ctx.width = 1920;
    ctx.height = 1080;
    return ctx;
}

// Build a FrameDecision whose merged scene is a high-confidence, non-baseline
// class (TextUi -> "code_text" in PolicyEngine::ClassifyScene).
FrameDecision MakeDecision(int frameIndex, FrameSceneType type, float confidence) {
    FrameDecision decision;
    decision.frameIndex = frameIndex;
    FrameScene scene;
    scene.type = type;
    scene.confidence = confidence;
    scene.source = "analyzer";     // non-panic source
    decision.scene = scene;
    return decision;
}

// Feed `count` consecutive frames of the same high-confidence class to a fresh
// engine and return the resulting mode-switch count.
int SwitchesAfterConsecutiveFrames(int count) {
    PolicyEngine engine;
    CHECK(engine.Initialize(/*device=*/nullptr, /*width=*/1920, /*height=*/1080));

    for (int i = 0; i < count; ++i) {
        FrameContext ctx = MakeContext(i);
        FrameDecision decision = MakeDecision(i, FrameSceneType::TextUi, 1.0f);
        engine.Evaluate(ctx, &decision);
    }
    return engine.ModeSwitches();
}

} // namespace

int main() {
    // Baseline: a freshly-initialized engine has made no switches.
    {
        PolicyEngine engine;
        CHECK(engine.Initialize(nullptr, 1920, 1080));
        CHECK_MSG(engine.ModeSwitches() == 0,
                  "fresh engine should report 0 mode switches");
    }

    // Fewer than kConsecutiveSameClassToSwitch (3) consecutive same-class
    // frames must NOT trigger a switch.
    {
        CHECK_MSG(SwitchesAfterConsecutiveFrames(1) == 0,
                  "1 consecutive frame -> 0 switches");
        CHECK_MSG(SwitchesAfterConsecutiveFrames(2) == 0,
                  "2 consecutive frames -> 0 switches");
    }

    // Exactly kConsecutiveSameClassToSwitch (3) consecutive same-class
    // high-confidence frames produce exactly 1 mode switch (the first-ever
    // switch is not gated by the 150-frame floor).
    {
        CHECK_MSG(SwitchesAfterConsecutiveFrames(3) == 1,
                  "3 consecutive frames -> exactly 1 switch");
    }

    // Low-confidence frames (< 0.6) must NOT promote even at 3+ consecutive,
    // because a low-confidence frame takes the fallback path and resets the
    // pending run.
    {
        PolicyEngine engine;
        CHECK(engine.Initialize(nullptr, 1920, 1080));
        for (int i = 0; i < 5; ++i) {
            FrameContext ctx = MakeContext(i);
            FrameDecision decision = MakeDecision(i, FrameSceneType::TextUi, 0.4f);
            engine.Evaluate(ctx, &decision);
        }
        CHECK_MSG(engine.ModeSwitches() == 0,
                  "low-confidence frames must not switch mode");
    }

    return aetherflow_test::Summary();
}
