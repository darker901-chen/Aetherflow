cmake_minimum_required(VERSION 3.15)

if(APPLE)
    # Must be set before project() so the deployment target propagates to all
    # toolchain probes. AetherFlow phase 1 macOS support targets macOS 13.0
    # (ScreenCaptureKit availability).
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(AetherFlow)

# C++/WinRT 需要 C++17 以上
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(aetherflow_enable_msvc_utf8 target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
endfunction()

# Cross-platform target. Platform-specific sources are added in the platform
# blocks below.
add_executable(AetherFlow src/main.cpp)
aetherflow_enable_msvc_utf8(AetherFlow)

# Cross-platform sources (pure C++; no platform SDK dependency). The async
# analyzer bridge, mock slow analyzer, and policy engine build on every
# supported platform so the same FramePolicyEngine wiring works on Windows
# and macOS.
target_sources(AetherFlow PRIVATE
    src/AsyncAnalyzerBridgeModule.cpp
    src/MockSlowAnalyzer.cpp
    src/policy/PolicyEngine.cpp
)

target_include_directories(AetherFlow PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(WIN32)
    # 1. 處理 OneVPL 子專案
    add_subdirectory(third_party/onevpl)

    # Optional NVENC backend (auto-detected; can be force-disabled)
    option(AETHERFLOW_DISABLE_NVENC "Force disable NVENC backend" OFF)
    set(NVENC_SDK_ROOT "" CACHE PATH "Optional path to NVIDIA Video Codec SDK root (contains Interface/nvEncodeAPI.h)")

    set(AETHERFLOW_ENABLE_NVENC OFF)
    set(AETHERFLOW_NVENC_INCLUDE_DIR "")
    if(NOT AETHERFLOW_DISABLE_NVENC)
        set(_nvenc_candidates)
        if(NVENC_SDK_ROOT)
            list(APPEND _nvenc_candidates "${NVENC_SDK_ROOT}/Interface")
        endif()
        # Bundled fallback path in this repo.
        list(APPEND _nvenc_candidates "${CMAKE_CURRENT_SOURCE_DIR}/external/VideoCodecSDK/Interface")

        foreach(_cand IN LISTS _nvenc_candidates)
            if(EXISTS "${_cand}/nvEncodeAPI.h")
                set(AETHERFLOW_ENABLE_NVENC ON)
                set(AETHERFLOW_NVENC_INCLUDE_DIR "${_cand}")
                break()
            endif()
        endforeach()
    endif()

    # 2. Windows-specific source files
    target_sources(AetherFlow PRIVATE
        src/ScreenCapture.cpp
        src/PasswordFieldPrivacyMaskModule.cpp
        src/NotificationProducerModule.cpp
        src/PrivacyMaskCompositor.cpp
        src/VplH264Wrapper.cpp
        src/ROIGenerator.cpp
    )

    if(AETHERFLOW_ENABLE_NVENC)
        target_sources(AetherFlow PRIVATE
            src/NvencH264Wrapper.cpp
        )
        target_include_directories(AetherFlow PRIVATE
            "${AETHERFLOW_NVENC_INCLUDE_DIR}"
        )
        target_compile_definitions(AetherFlow PRIVATE AETHERFLOW_ENABLE_NVENC=1)
        message(STATUS "AetherFlow: NVENC backend enabled (include: ${AETHERFLOW_NVENC_INCLUDE_DIR})")
    else()
        message(STATUS "AetherFlow: NVENC backend disabled (header not found or AETHERFLOW_DISABLE_NVENC=ON).")
    endif()

    # 3. 設定 Include 目錄 (Windows 專屬)
    target_include_directories(AetherFlow PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onevpl/api"
    )

    # Phase 4 P0.1: optional ONNX Runtime + DirectML scene classifier.
    # Vendored under third_party/onnxruntime/ (see third_party/onnxruntime/SOURCE.md).
    # When the headers are not present (binary not yet downloaded), the
    # SceneClassifierOnnx source is #ifdef-guarded out so the build still
    # succeeds without ORT.
    option(AETHERFLOW_ENABLE_SCENE_CLASSIFIER "Enable Phase 4 P0.1 ONNX scene classifier" ON)
    set(AETHERFLOW_ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
    set(AETHERFLOW_ORT_HEADER "${AETHERFLOW_ORT_ROOT}/include/onnxruntime_cxx_api.h")
    set(AETHERFLOW_ORT_LIB "${AETHERFLOW_ORT_ROOT}/lib/onnxruntime.lib")
    set(AETHERFLOW_ORT_DLL "${AETHERFLOW_ORT_ROOT}/bin/onnxruntime.dll")
    if(AETHERFLOW_ENABLE_SCENE_CLASSIFIER AND EXISTS "${AETHERFLOW_ORT_HEADER}" AND EXISTS "${AETHERFLOW_ORT_LIB}")
        target_sources(AetherFlow PRIVATE
            src/ai/SceneClassifierOnnx.cpp
        )
        target_include_directories(AetherFlow PRIVATE
            "${AETHERFLOW_ORT_ROOT}/include"
        )
        target_link_libraries(AetherFlow PRIVATE
            "${AETHERFLOW_ORT_LIB}"
        )
        target_compile_definitions(AetherFlow PRIVATE AETHERFLOW_ENABLE_SCENE_CLASSIFIER=1)
        message(STATUS "AetherFlow: SceneClassifierOnnx enabled (ORT: ${AETHERFLOW_ORT_ROOT})")
    else()
        if(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
            message(WARNING "AetherFlow: SceneClassifierOnnx requested but ONNX Runtime headers not found at ${AETHERFLOW_ORT_HEADER}. "
                            "Run third_party/onnxruntime/SOURCE.md SOP to vendor it, then reconfigure. "
                            "Building without the scene classifier.")
        else()
            message(STATUS "AetherFlow: SceneClassifierOnnx disabled (AETHERFLOW_ENABLE_SCENE_CLASSIFIER=OFF).")
        endif()
        set(AETHERFLOW_ENABLE_SCENE_CLASSIFIER OFF)
    endif()

    # SRT live output (spec Delta A): optional libavformat/libsrt MPEG-TS
    # streaming stage after the encoder. Vendored FFmpeg shared SDK under
    # third_party/ffmpeg/ (gitignored; SOP in third_party/ffmpeg/SOURCE.md,
    # fetch via `python tools/fetch_ffmpeg.py`). When the SDK is absent the
    # stage is compiled out (CI builds without it) and --srt-output reports
    # "not built with SRT support" at runtime.
    option(AETHERFLOW_ENABLE_SRT_OUTPUT "Enable SRT/MPEG-TS live output stage" ON)
    set(AETHERFLOW_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
    set(AETHERFLOW_FFMPEG_HEADER "${AETHERFLOW_FFMPEG_ROOT}/include/libavformat/avformat.h")
    set(AETHERFLOW_FFMPEG_AVFORMAT_LIB "${AETHERFLOW_FFMPEG_ROOT}/lib/avformat.lib")
    if(AETHERFLOW_ENABLE_SRT_OUTPUT AND EXISTS "${AETHERFLOW_FFMPEG_HEADER}" AND EXISTS "${AETHERFLOW_FFMPEG_AVFORMAT_LIB}")
        target_sources(AetherFlow PRIVATE
            src/streaming/AnnexB.cpp
            src/streaming/SrtStreamOutput.cpp
        )
        target_include_directories(AetherFlow PRIVATE
            "${AETHERFLOW_FFMPEG_ROOT}/include"
        )
        target_link_libraries(AetherFlow PRIVATE
            "${AETHERFLOW_FFMPEG_ROOT}/lib/avformat.lib"
            "${AETHERFLOW_FFMPEG_ROOT}/lib/avcodec.lib"
            "${AETHERFLOW_FFMPEG_ROOT}/lib/avutil.lib"
            "${AETHERFLOW_FFMPEG_ROOT}/lib/swresample.lib"
            ws2_32
        )
        target_compile_definitions(AetherFlow PRIVATE AETHERFLOW_ENABLE_SRT_OUTPUT=1)
        message(STATUS "AetherFlow: SRT output enabled (FFmpeg SDK: ${AETHERFLOW_FFMPEG_ROOT})")
        # Runtime DLLs AetherFlow.exe actually loads (avformat + its deps).
        # The extra DLLs in bin/ (avfilter/avdevice/swscale) exist only for the
        # bundled ffmpeg.exe test caller and are not copied.
        file(GLOB AETHERFLOW_FFMPEG_RUNTIME_DLLS
            "${AETHERFLOW_FFMPEG_ROOT}/bin/avformat-*.dll"
            "${AETHERFLOW_FFMPEG_ROOT}/bin/avcodec-*.dll"
            "${AETHERFLOW_FFMPEG_ROOT}/bin/avutil-*.dll"
            "${AETHERFLOW_FFMPEG_ROOT}/bin/swresample-*.dll"
        )
        foreach(_ffdll IN LISTS AETHERFLOW_FFMPEG_RUNTIME_DLLS)
            add_custom_command(TARGET AetherFlow POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ffdll}"
                "$<TARGET_FILE_DIR:AetherFlow>"
            )
        endforeach()
    else()
        if(AETHERFLOW_ENABLE_SRT_OUTPUT)
            message(STATUS "AetherFlow: SRT output disabled (FFmpeg SDK not found under ${AETHERFLOW_FFMPEG_ROOT}; run `python tools/fetch_ffmpeg.py`, then reconfigure).")
        else()
            message(STATUS "AetherFlow: SRT output disabled (AETHERFLOW_ENABLE_SRT_OUTPUT=OFF).")
        endif()
        set(AETHERFLOW_ENABLE_SRT_OUTPUT OFF)
    endif()

    # 4. 連結 Library
    target_link_libraries(AetherFlow PRIVATE
        VPL
        dwmapi
        d3d11
        dxgi
        mf
        mfplat
        mfreadwrite
        mfuuid
        ole32
        uiautomationcore
        windowsapp
    )

    # 5. 編譯器設定
    if(MSVC)
        # /permissive- 讓 MSVC 嚴格遵守標準
        # /bigobj 處理 C++/WinRT 產生的大量符號
        target_compile_options(AetherFlow PRIVATE "/permissive-" "/bigobj")
    endif()

    # 6. 自動複製 DLL 到輸出目錄
    add_custom_command(TARGET AetherFlow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:VPL>"
        "$<TARGET_FILE_DIR:AetherFlow>"
    )

    # ONNX Runtime DLL copy (only when ORT was found above). DirectML.dll is
    # picked up from C:\Windows\System32 (Windows 10+ ships it). A follow-on
    # hardening pass can vendor Microsoft.AI.DirectML for a fully
    # self-contained ship.
    if(AETHERFLOW_ENABLE_SCENE_CLASSIFIER AND EXISTS "${AETHERFLOW_ORT_DLL}")
        add_custom_command(TARGET AetherFlow POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AETHERFLOW_ORT_DLL}"
            "$<TARGET_FILE_DIR:AetherFlow>"
        )
    endif()

    # ------------------------------------------------------------------
    # AetherFlowStudio (spec Delta B): Dear ImGui settings UI around the SAME
    # pipeline translation units as AetherFlow — src/main.cpp is compiled into
    # both targets and AETHERFLOW_STUDIO_BUILD guards out main(), so pipeline
    # behavior cannot fork between the CLI and the UI. Vendored ImGui is
    # gitignored (third_party/imgui/SOURCE.md; python tools/fetch_imgui.py);
    # absent ⇒ the Studio target simply is not generated (CI unaffected).
    # ------------------------------------------------------------------
    set(AETHERFLOW_IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    if(EXISTS "${AETHERFLOW_IMGUI_ROOT}/imgui.h")
        add_executable(AetherFlowStudio WIN32
            src/ui/StudioMain.cpp
            src/ui/AppConfig.cpp
            src/main.cpp
            src/AsyncAnalyzerBridgeModule.cpp
            src/MockSlowAnalyzer.cpp
            src/policy/PolicyEngine.cpp
            src/ScreenCapture.cpp
            src/PasswordFieldPrivacyMaskModule.cpp
            src/NotificationProducerModule.cpp
            src/PrivacyMaskCompositor.cpp
            src/VplH264Wrapper.cpp
            src/ROIGenerator.cpp
            "${AETHERFLOW_IMGUI_ROOT}/imgui.cpp"
            "${AETHERFLOW_IMGUI_ROOT}/imgui_draw.cpp"
            "${AETHERFLOW_IMGUI_ROOT}/imgui_tables.cpp"
            "${AETHERFLOW_IMGUI_ROOT}/imgui_widgets.cpp"
            "${AETHERFLOW_IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
            "${AETHERFLOW_IMGUI_ROOT}/backends/imgui_impl_dx11.cpp"
        )
        aetherflow_enable_msvc_utf8(AetherFlowStudio)
        target_compile_definitions(AetherFlowStudio PRIVATE AETHERFLOW_STUDIO_BUILD=1)
        target_include_directories(AetherFlowStudio PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onevpl/api"
            "${AETHERFLOW_IMGUI_ROOT}"
        )
        if(AETHERFLOW_ENABLE_NVENC)
            target_sources(AetherFlowStudio PRIVATE src/NvencH264Wrapper.cpp)
            target_include_directories(AetherFlowStudio PRIVATE "${AETHERFLOW_NVENC_INCLUDE_DIR}")
            target_compile_definitions(AetherFlowStudio PRIVATE AETHERFLOW_ENABLE_NVENC=1)
        endif()
        if(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
            target_sources(AetherFlowStudio PRIVATE src/ai/SceneClassifierOnnx.cpp)
            target_include_directories(AetherFlowStudio PRIVATE "${AETHERFLOW_ORT_ROOT}/include")
            target_link_libraries(AetherFlowStudio PRIVATE "${AETHERFLOW_ORT_LIB}")
            target_compile_definitions(AetherFlowStudio PRIVATE AETHERFLOW_ENABLE_SCENE_CLASSIFIER=1)
        endif()
        if(AETHERFLOW_ENABLE_SRT_OUTPUT)
            target_sources(AetherFlowStudio PRIVATE
                src/streaming/AnnexB.cpp
                src/streaming/SrtStreamOutput.cpp
            )
            target_include_directories(AetherFlowStudio PRIVATE "${AETHERFLOW_FFMPEG_ROOT}/include")
            target_link_libraries(AetherFlowStudio PRIVATE
                "${AETHERFLOW_FFMPEG_ROOT}/lib/avformat.lib"
                "${AETHERFLOW_FFMPEG_ROOT}/lib/avcodec.lib"
                "${AETHERFLOW_FFMPEG_ROOT}/lib/avutil.lib"
                "${AETHERFLOW_FFMPEG_ROOT}/lib/swresample.lib"
                ws2_32
            )
            target_compile_definitions(AetherFlowStudio PRIVATE AETHERFLOW_ENABLE_SRT_OUTPUT=1)
            foreach(_ffdll IN LISTS AETHERFLOW_FFMPEG_RUNTIME_DLLS)
                add_custom_command(TARGET AetherFlowStudio POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ffdll}"
                    "$<TARGET_FILE_DIR:AetherFlowStudio>"
                )
            endforeach()
        endif()
        target_link_libraries(AetherFlowStudio PRIVATE
            VPL dwmapi d3d11 dxgi mf mfplat mfreadwrite mfuuid ole32
            uiautomationcore windowsapp
        )
        if(MSVC)
            target_compile_options(AetherFlowStudio PRIVATE "/permissive-" "/bigobj")
        endif()
        add_custom_command(TARGET AetherFlowStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:VPL>"
            "$<TARGET_FILE_DIR:AetherFlowStudio>"
        )
        if(AETHERFLOW_ENABLE_SCENE_CLASSIFIER AND EXISTS "${AETHERFLOW_ORT_DLL}")
            add_custom_command(TARGET AetherFlowStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHERFLOW_ORT_DLL}"
                "$<TARGET_FILE_DIR:AetherFlowStudio>"
            )
        endif()
        message(STATUS "AetherFlow: Studio UI enabled (imgui: ${AETHERFLOW_IMGUI_ROOT})")
    else()
        message(STATUS "AetherFlow: Studio UI disabled (Dear ImGui not found under ${AETHERFLOW_IMGUI_ROOT}; run `python tools/fetch_imgui.py`, then reconfigure).")
    endif()
endif()

if(APPLE)
    enable_language(OBJCXX)
    target_sources(AetherFlow PRIVATE
        src/platform/mac/MacosScreenCapture.mm
        src/platform/mac/MacosSecureTextFieldPrivacyMaskModule.mm
        src/platform/mac/MacosNotificationProducerModule.mm
        src/platform/mac/MacosPrivacyMaskCompositor.mm
        src/platform/mac/MacosPlatformShim.mm
        src/platform/mac/VideoToolboxH264Encoder.mm
    )
    foreach(_fw ScreenCaptureKit CoreMedia CoreVideo VideoToolbox AVFoundation CoreImage Metal CoreGraphics CoreFoundation Foundation AppKit ApplicationServices)
        find_library(${_fw}_FRAMEWORK ${_fw} REQUIRED)
        target_link_libraries(AetherFlow PRIVATE ${${_fw}_FRAMEWORK})
    endforeach()
    target_compile_options(AetherFlow PRIVATE -fobjc-arc)
endif()

# ---------------------------------------------------------------------------
# First-party unit tests (CTest). Self-contained: a tiny CHECK() assertion
# header (tests/test_assert.h), no external test framework (no gtest/catch2).
# Each test is its own small executable; main returns nonzero on any failure.
# The tests exercise PURE LOGIC only — no capture, encoder, or D3D11 runtime.
# Guarded behind AETHERFLOW_BUILD_TESTS so they never interfere with the main
# AetherFlow target; the default build still produces the app unchanged.
# ---------------------------------------------------------------------------
option(AETHERFLOW_BUILD_TESTS "Build first-party unit tests" ON)
if(AETHERFLOW_BUILD_TESTS)
    enable_testing()

    # Test 1: FrameDecision::ProposeScene confidence-merge semantics.
    # Header-only (FrameScene / FrameDecision are inline in IAIFrameAnalyzer.h),
    # so this links no AetherFlow source. On Windows the header includes
    # <d3d11.h> for the (unused) texture pointer types; linking d3d11 keeps the
    # toolchain happy even though no D3D11 API is called.
    add_executable(aetherflow_test_frame_decision
        tests/test_frame_decision.cpp
    )
    aetherflow_enable_msvc_utf8(aetherflow_test_frame_decision)
    target_include_directories(aetherflow_test_frame_decision PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    if(WIN32)
        target_link_libraries(aetherflow_test_frame_decision PRIVATE d3d11)
    endif()
    add_test(NAME FrameDecisionProposeScene
             COMMAND aetherflow_test_frame_decision)

    # Test 2: PolicyEngine hysteresis (mode-switch debounce). Drives the real
    # src/policy/PolicyEngine.cpp with synthetic pure-C++ FrameContext /
    # FrameDecision values (null textures). PolicyEngine.cpp is cross-platform
    # pure C++, so this test builds on every platform.
    add_executable(aetherflow_test_policy_engine
        tests/test_policy_engine.cpp
        src/policy/PolicyEngine.cpp
    )
    aetherflow_enable_msvc_utf8(aetherflow_test_policy_engine)
    target_include_directories(aetherflow_test_policy_engine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    if(WIN32)
        target_link_libraries(aetherflow_test_policy_engine PRIVATE d3d11)
    endif()
    add_test(NAME PolicyEngineHysteresis
             COMMAND aetherflow_test_policy_engine)

    # Test 3: AnnexB NAL scan helpers used by the SRT/TS output stage
    # (mid-stream join: SPS/PPS detect + prepend decision). Pure C++ — no
    # FFmpeg, no D3D11 — so it builds and runs everywhere, including CI where
    # the vendored FFmpeg SDK is absent.
    add_executable(aetherflow_test_annexb
        tests/test_annexb.cpp
        src/streaming/AnnexB.cpp
    )
    aetherflow_enable_msvc_utf8(aetherflow_test_annexb)
    target_include_directories(aetherflow_test_annexb PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    add_test(NAME AnnexBScan COMMAND aetherflow_test_annexb)

    # Test 4: AetherFlowStudio settings persistence (INI round-trip, defaults,
    # clamping). Pure C++ — no ImGui/Win32 dependency — so it runs in CI
    # without the vendored UI SDK.
    add_executable(aetherflow_test_appconfig
        tests/test_appconfig.cpp
        src/ui/AppConfig.cpp
    )
    aetherflow_enable_msvc_utf8(aetherflow_test_appconfig)
    target_include_directories(aetherflow_test_appconfig PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    add_test(NAME AppConfigIni COMMAND aetherflow_test_appconfig)
endif()
