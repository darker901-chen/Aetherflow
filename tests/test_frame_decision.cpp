// Unit test for FrameDecision::ProposeScene confidence-merge semantics.
//
// Contract (include/AetherFlow/IAIFrameAnalyzer.h, ~line 99):
//   The proposed scene wins iff no scene is set yet (scene.type == Unknown)
//   OR proposed.confidence > scene.confidence. Strict greater-than (`>`)
//   preserves first-writer-wins on ties so deterministic producers retain
//   stable ordering when they share the same confidence.
//
// This is header-only (FrameDecision / FrameScene are defined inline), so this
// translation unit links no AetherFlow source. On Windows IAIFrameAnalyzer.h
// pulls in <d3d11.h> for the (here-unused) texture pointer types; that is fine
// for a Windows test exe — no d3d11.lib link is required because no D3D11 API
// is called.

#include "AetherFlow/IAIFrameAnalyzer.h"

#include "test_assert.h"

using AetherFlow::FrameDecision;
using AetherFlow::FrameScene;
using AetherFlow::FrameSceneType;

namespace {

FrameScene MakeScene(FrameSceneType type, float confidence, const char* source) {
    FrameScene scene;
    scene.type = type;
    scene.confidence = confidence;
    scene.source = source;
    return scene;
}

} // namespace

int main() {
    // (a) First proposal on a fresh decision is accepted. A fresh FrameDecision
    //     has scene.type == Unknown, so ProposeScene must take it regardless of
    //     confidence.
    {
        FrameDecision decision;
        CHECK(decision.scene.type == FrameSceneType::Unknown);
        CHECK(!decision.HasScene());

        FrameScene first = MakeScene(FrameSceneType::GenericScreen, 0.5f, "baseline");
        decision.ProposeScene(first);

        CHECK(decision.HasScene());
        CHECK(decision.scene.type == FrameSceneType::GenericScreen);
        CHECK(decision.scene.confidence == 0.5f);
        CHECK(decision.scene.source == "baseline");
    }

    // (b) A strictly higher-confidence proposal replaces the current scene.
    {
        FrameDecision decision;
        decision.ProposeScene(MakeScene(FrameSceneType::GenericScreen, 0.5f, "baseline"));
        decision.ProposeScene(MakeScene(FrameSceneType::TextUi, 0.9f, "analyzer"));

        CHECK_MSG(decision.scene.type == FrameSceneType::TextUi,
                  "higher confidence proposal should replace");
        CHECK(decision.scene.confidence == 0.9f);
        CHECK(decision.scene.source == "analyzer");
    }

    // (c) An EQUAL-confidence proposal does NOT replace (first-writer-wins).
    //     Strict `>` means a tie keeps the incumbent.
    {
        FrameDecision decision;
        decision.ProposeScene(MakeScene(FrameSceneType::Slides, 1.0f, "first"));
        decision.ProposeScene(MakeScene(FrameSceneType::VideoContent, 1.0f, "second"));

        CHECK_MSG(decision.scene.type == FrameSceneType::Slides,
                  "equal confidence must NOT replace (first-writer-wins)");
        CHECK(decision.scene.source == "first");
        CHECK(decision.scene.confidence == 1.0f);
    }

    // (d) A lower-confidence proposal is ignored.
    {
        FrameDecision decision;
        decision.ProposeScene(MakeScene(FrameSceneType::SensitiveSurface, 0.8f, "strong"));
        decision.ProposeScene(MakeScene(FrameSceneType::GenericScreen, 0.4f, "weak"));

        CHECK_MSG(decision.scene.type == FrameSceneType::SensitiveSurface,
                  "lower confidence proposal must be ignored");
        CHECK(decision.scene.source == "strong");
        CHECK(decision.scene.confidence == 0.8f);
    }

    return aetherflow_test::Summary();
}
