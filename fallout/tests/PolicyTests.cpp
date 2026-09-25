#include "RuntimePolicy.h"
#include "DDSOverlayCodec.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace
{
    struct RuntimeCase
    {
        std::array<std::uint16_t, 4> version;
        bool expected;
    };

    struct CancellationState
    {
        unsigned checksBeforeCancellation;
        unsigned checks = 0;
    };

    bool CancelAfterChecks(void* context)
    {
        auto* state = static_cast<CancellationState*>(context);
        return state &&
            state->checks++ >= state->checksBeforeCancellation;
    }

    bool FlatReplayShaderContractIsSafe()
    {
        const auto sourcePath = std::filesystem::path(__FILE__)
            .parent_path().parent_path() / "src" / "D3D11Compositor.cpp";
        std::ifstream input(sourcePath, std::ios::binary);
        if (!input) {
            return false;
        }
        const std::string source{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return source.find(
                   "float replayAlpha = smoothstep(0.40, 0.45, luminance);") !=
                std::string::npos &&
            source.find("clip(changed - 0.004);") != std::string::npos &&
            source.find("clip(luminance - 0.425);") != std::string::npos &&
            source.find("return float4(afterColor.rgb, replayAlpha);") !=
                std::string::npos &&
            source.find("smoothstep(0.004, 0.02, changed) *") ==
                std::string::npos;
    }
}

int main()
{
    using namespace VRLoadingScreens::DDSOverlayCodec;
    if (!FlatReplayShaderContractIsSafe()) {
        return EXIT_FAILURE;
    }
    {
        constexpr std::array<std::uint8_t, 8> bgra{
            3, 2, 1, 4,
            30, 20, 10, 40
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                bgra, 2, 1, SourceFormat::kBGRA8, false, decoded) ||
            decoded != std::vector<std::uint8_t>{
                1, 2, 3, 4,
                10, 20, 30, 40
            }) {
            return EXIT_FAILURE;
        }
        if (!DecodeTopMipToRGBA8(
                bgra, 2, 1, SourceFormat::kBGRA8, true, decoded) ||
            decoded[3] != 0xff || decoded[7] != 0xff) {
            return EXIT_FAILURE;
        }
    }
    {
        constexpr std::array<std::uint8_t, 8> rgba{
            1, 2, 3, 4,
            10, 20, 30, 40
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                rgba, 2, 1, SourceFormat::kRGBA8, false, decoded) ||
            decoded != std::vector<std::uint8_t>{
                1, 2, 3, 4,
                10, 20, 30, 40
            } ||
            DecodeTopMipToRGBA8(
                std::span<const std::uint8_t>(rgba.data(), 7),
                2, 1, SourceFormat::kRGBA8, false, decoded)) {
            return EXIT_FAILURE;
        }
    }
    {
        // BC1 endpoints: red, green. The first row selects entries 0,1,2,3.
        constexpr std::array<std::uint8_t, 8> bc1{
            0x00, 0xf8, 0xe0, 0x07,
            0xe4, 0x00, 0x00, 0x00
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                bc1, 4, 4, SourceFormat::kBC1, false, decoded)) {
            return EXIT_FAILURE;
        }
        constexpr std::array<std::uint8_t, 16> firstRow{
            255, 0, 0, 255,
            0, 255, 0, 255,
            170, 85, 0, 255,
            85, 170, 0, 255
        };
        if (!std::equal(
                firstRow.begin(), firstRow.end(), decoded.begin())) {
            return EXIT_FAILURE;
        }
    }
    {
        // A partial 3x2 edge block must write exactly six in-bounds pixels.
        constexpr std::array<std::uint8_t, 8> redBC1{
            0x00, 0xf8, 0xe0, 0x07,
            0x00, 0x00, 0x00, 0x00
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                redBC1, 3, 2, SourceFormat::kBC1, false, decoded) ||
            decoded.size() != 3u * 2u * 4u) {
            return EXIT_FAILURE;
        }
        for (std::size_t offset = 0; offset < decoded.size(); offset += 4) {
            if (decoded[offset + 0] != 0xff ||
                decoded[offset + 1] != 0 ||
                decoded[offset + 2] != 0 ||
                decoded[offset + 3] != 0xff) {
                return EXIT_FAILURE;
            }
        }
    }
    {
        // BC1's color0 <= color1 branch makes palette entry 3 transparent.
        constexpr std::array<std::uint8_t, 8> bc1Transparent{
            0x00, 0x00, 0xff, 0xff,
            0x03, 0x00, 0x00, 0x00
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                bc1Transparent, 1, 1, SourceFormat::kBC1,
                false, decoded) ||
            decoded != std::vector<std::uint8_t>{ 0, 0, 0, 0 }) {
            return EXIT_FAILURE;
        }
        if (!DecodeTopMipToRGBA8(
                bc1Transparent, 1, 1, SourceFormat::kBC1,
                true, decoded) ||
            decoded != std::vector<std::uint8_t>{ 0, 0, 0, 0xff }) {
            return EXIT_FAILURE;
        }
    }
    {
        // BC3 alpha index 2 with endpoints 255/0 interpolates to 218.
        constexpr std::array<std::uint8_t, 16> bc3{
            0xff, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xf8, 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                bc3, 1, 1, SourceFormat::kBC3, false, decoded) ||
            decoded != std::vector<std::uint8_t>{ 255, 0, 0, 218 }) {
            return EXIT_FAILURE;
        }
        if (DecodeTopMipToRGBA8(
                std::span<const std::uint8_t>(bc3.data(), 15),
                1, 1, SourceFormat::kBC3, false, decoded)) {
            return EXIT_FAILURE;
        }
    }
    {
        // BC3's alpha0 <= alpha1 branch uses four interpolants followed by
        // explicit transparent and opaque entries. Pixels 0..2 select 2,6,7.
        constexpr std::array<std::uint8_t, 16> bc3LowAlpha{
            0x00, 0x64, 0xf2, 0x01, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xf8, 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00
        };
        std::vector<std::uint8_t> decoded;
        if (!DecodeTopMipToRGBA8(
                bc3LowAlpha, 3, 1, SourceFormat::kBC3,
                false, decoded) ||
            decoded != std::vector<std::uint8_t>{
                255, 0, 0, 20,
                255, 0, 0, 0,
                255, 0, 0, 255
            }) {
            return EXIT_FAILURE;
        }
    }
    {
        constexpr std::array<std::uint8_t, 16> source{};
        std::vector<std::uint8_t> decoded{ 1, 2, 3, 4 };
        if (DecodeTopMipToRGBA8(
                source, 0, 1, SourceFormat::kRGBA8,
                false, decoded) ||
            DecodeTopMipToRGBA8(
                source, kMaxDecodedDimension + 1, 1,
                SourceFormat::kRGBA8, false, decoded) ||
            DecodeTopMipToRGBA8(
                source, kMaxDecodedDimension, 2049,
                SourceFormat::kRGBA8, false, decoded) ||
            DecodeTopMipToRGBA8(
                source, 1, 1, static_cast<SourceFormat>(999),
                false, decoded)) {
            return EXIT_FAILURE;
        }
    }
    {
        constexpr std::array<std::uint8_t, 32> source{};

        // Cancellation before allocation must not leave stale pixels that a
        // caller could accidentally publish as a successfully decoded image.
        std::vector<std::uint8_t> decoded{ 1, 2, 3, 4 };
        CancellationState immediate{ 0 };
        if (DecodeTopMipToRGBA8(
                source, 2, 2, SourceFormat::kRGBA8, false, decoded,
                &CancelAfterChecks, &immediate) ||
            !decoded.empty()) {
            return EXIT_FAILURE;
        }

        // Checks 0/1 occur before and after allocation; check 2 admits the
        // first BC row and check 3 cancels before the second block row.
        constexpr std::array<std::uint8_t, 16> twoBC1Blocks{
            0x00, 0xf8, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xf8, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x00
        };
        CancellationState midDecode{ 3 };
        if (DecodeTopMipToRGBA8(
                twoBC1Blocks, 4, 8, SourceFormat::kBC1, false, decoded,
                &CancelAfterChecks, &midDecode) ||
            !decoded.empty()) {
            return EXIT_FAILURE;
        }
    }

    constexpr std::array runtimeCases{
        RuntimeCase{ { 1, 2, 72, 0 }, true },
        RuntimeCase{ { 1, 10, 163, 0 }, true },
        RuntimeCase{ { 1, 11, 221, 0 }, true },
        RuntimeCase{ { 1, 11, 240, 0 }, true },
        RuntimeCase{ { 1, 10, 980, 0 }, false },
        RuntimeCase{ { 1, 10, 984, 0 }, false },
        RuntimeCase{ { 1, 11, 191, 0 }, false },
        RuntimeCase{ { 1, 11, 221, 1 }, false },
        RuntimeCase{ { 1, 11, 240, 1 }, false },
        RuntimeCase{ { 0, 0, 0, 0 }, false }
    };
    for (const auto& test : runtimeCases) {
        if (VRLoadingScreens::Policy::IsSupportedRuntime(
                test.version[0],
                test.version[1],
                test.version[2],
                test.version[3]) != test.expected) {
            return EXIT_FAILURE;
        }
    }

    using VRLoadingScreens::Policy::IsSupportedF4SEHost;
    using VRLoadingScreens::Policy::ClassifyExecutable;
    using VRLoadingScreens::Policy::ExecutableKind;
    using VRLoadingScreens::Policy::ModuleRuntimeKind;
    using VRLoadingScreens::Policy::RuntimeVersionParts;
    constexpr RuntimeVersionParts vrExecutable{ 1, 2, 72, 0 };
    constexpr RuntimeVersionParts vrProxy{ 1, 10, 138, 0 };
    constexpr RuntimeVersionParts ogExecutable{ 1, 10, 163, 0 };
    constexpr RuntimeVersionParts aeExecutable{ 1, 11, 221, 0 };
    constexpr RuntimeVersionParts aeLatestExecutable{ 1, 11, 240, 0 };
    constexpr RuntimeVersionParts f4seVR{ 0, 6, 21, 0 };
    constexpr RuntimeVersionParts f4seFlat{ 0, 6, 23, 0 };
    constexpr RuntimeVersionParts f4seAE{ 0, 7, 2, 0 };
    constexpr RuntimeVersionParts f4seLatest{ 0, 7, 9, 0 };

    if (ClassifyExecutable(L"Fallout4VR.exe") !=
            ExecutableKind::kFallout4VR ||
        ClassifyExecutable(L"C:\\Games\\Fallout 4 VR\\FALLOUT4VR.EXE") !=
            ExecutableKind::kFallout4VR ||
        ClassifyExecutable(L"C:/Games/Fallout 4/Fallout4.exe") !=
            ExecutableKind::kFallout4 ||
        ClassifyExecutable(L"renamed.exe") != ExecutableKind::kUnknown ||
        !IsSupportedF4SEHost(
            vrProxy,
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        !IsSupportedF4SEHost(
            vrExecutable,
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        !IsSupportedF4SEHost(
            ogExecutable,
            ogExecutable,
            f4seFlat,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4,
            true) ||
        !IsSupportedF4SEHost(
            aeExecutable,
            aeExecutable,
            f4seAE,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4NG,
            true) ||
        !IsSupportedF4SEHost(
            aeLatestExecutable,
            aeLatestExecutable,
            f4seLatest,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4NG,
            true) ||
        // The VR proxy must never make flat Fallout 1.10.138 load.
        IsSupportedF4SEHost(
            vrProxy,
            vrProxy,
            f4seFlat,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4,
            true) ||
        // A spoofed proxy cannot make an unverified VR executable load.
        IsSupportedF4SEHost(
            vrProxy,
            RuntimeVersionParts{ 1, 2, 71, 0 },
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        // The legacy 1.10.138 proxy belongs specifically to F4SEVR 0.6.21.
        IsSupportedF4SEHost(
            vrProxy,
            vrExecutable,
            RuntimeVersionParts{ 0, 6, 22, 0 },
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        // The raw 1.2.72.2 token has not been verified against the installed
        // extender and therefore remains fail-closed.
        IsSupportedF4SEHost(
            RuntimeVersionParts{ 1, 2, 72, 2 },
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        // CommonLib's version-only IsVR classification is not enough: the
        // loaded executable name must independently agree with that runtime.
        IsSupportedF4SEHost(
            vrProxy,
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        IsSupportedF4SEHost(
            ogExecutable,
            ogExecutable,
            f4seFlat,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4,
            true) ||
        IsSupportedF4SEHost(
            vrProxy,
            vrExecutable,
            f4seVR,
            ExecutableKind::kUnknown,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        IsSupportedF4SEHost(
            vrProxy,
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            false) ||
        // F4SE/executable disagreement fails closed on flat runtimes.
        IsSupportedF4SEHost(
            vrProxy,
            ogExecutable,
            f4seFlat,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4,
            true) ||
        IsSupportedF4SEHost(
            RuntimeVersionParts{ 1, 10, 163, 0 },
            aeExecutable,
            f4seAE,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4NG,
            true) ||
        IsSupportedF4SEHost(
            aeExecutable,
            aeLatestExecutable,
            f4seLatest,
            ExecutableKind::kFallout4,
            ModuleRuntimeKind::kFallout4NG,
            true) ||
        !VRLoadingScreens::Policy::RequiresLegacyF4SEVRInterfaceShim(
            vrExecutable,
            f4seVR,
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true) ||
        VRLoadingScreens::Policy::RequiresLegacyF4SEVRInterfaceShim(
            vrExecutable,
            RuntimeVersionParts{ 0, 6, 22, 0 },
            ExecutableKind::kFallout4VR,
            ModuleRuntimeKind::kFallout4VR,
            true)) {
        return EXIT_FAILURE;
    }

    using VRLoadingScreens::Policy::ShouldRunPapyrusUpdater;
    if (!ShouldRunPapyrusUpdater(true, true, false, false) ||
        ShouldRunPapyrusUpdater(false, true, false, false) ||
        ShouldRunPapyrusUpdater(true, false, false, false) ||
        ShouldRunPapyrusUpdater(true, true, true, false) ||
        ShouldRunPapyrusUpdater(true, true, false, true)) {
        return EXIT_FAILURE;
    }

    using namespace VRLoadingScreens::Policy;

    // March executes the required self-MoveTo on the next direct main-loop
    // update. Only ownership and duplicate/menu-open gates matter; world/player
    // probes were introduced later and changed the working sequence.
    if (ShouldExecuteVRMarchMoveTo(
            false, false, false, false, true, true, 0, 0) ||
        ShouldExecuteVRMarchMoveTo(
            true, true, false, false, true, true, 0, 0) ||
        ShouldExecuteVRMarchMoveTo(
            true, false, true, false, true, true, 0, 0) ||
        !ShouldExecuteVRMarchMoveTo(
            true, false, false, true, false, false, 0, 0)) {
        return EXIT_FAILURE;
    }

    if (VRMarchRenderDelayMs(3, false) != 500 ||
        VRMarchRenderDelayMs(3, true) != 100 ||
        VRMarchRenderDelayMs(2, true) != 0 ||
        VRMarchRenderDelayMs(0, true) != 0 ||
        kVRTipsCaptureDeadlineMs != 600 ||
        kVRTipsPendingQueryDeadlineMs != 700 ||
        kVRMarchPostCloseHoldMs != 200) {
        return EXIT_FAILURE;
    }

    // The detached state at the retained first CLOSE belongs to the upcoming
    // self-MoveTo, not to the raw repair. The final synthetic CLOSE owns the
    // repair even though it does not receive another kPostLoadGame message.
    if (ShouldArmVRPlayspaceRepairAtClose(true, true, false) ||
        !ShouldArmVRPlayspaceRepairAtClose(false, false, true) ||
        !ShouldArmVRPlayspaceRepairAtClose(true, false, false) ||
        ShouldArmVRPlayspaceRepairAtClose(false, false, false)) {
        return EXIT_FAILURE;
    }

    // An unarmed path stays on the helper's ordinary timer. Once armed,
    // readiness cannot release before the post-close floor and the hard
    // backstop bounds every unresolved/unreadable path.
    using VRDecision = VRPostCloseReleaseDecision;
    if (DecideVRPostCloseRelease(
            false, false, 0) != VRDecision::kReady ||
        DecideVRPostCloseRelease(
            true, true, 599) != VRDecision::kHold ||
        DecideVRPostCloseRelease(
            true, true, 600) != VRDecision::kReady ||
        // Readiness has priority over the elapsed hard threshold.
        DecideVRPostCloseRelease(
            true, true, 2600) != VRDecision::kReady ||
        DecideVRPostCloseRelease(
            true, false, 2499) != VRDecision::kHold ||
        DecideVRPostCloseRelease(
            true, false, 2500) !=
                VRDecision::kHardBackstop) {
        return EXIT_FAILURE;
    }

    using FadeLatchDecision = VRSceneFadeLatchDecision;
    if (DecideVRSceneFadeLatch(false, 0, 0) !=
            FadeLatchDecision::kArm ||
        DecideVRSceneFadeLatch(true, 31, 2) !=
            FadeLatchDecision::kWait ||
        DecideVRSceneFadeLatch(true, 32, 1) !=
            FadeLatchDecision::kWait ||
        DecideVRSceneFadeLatch(true, 32, 2) !=
            FadeLatchDecision::kFreshPairs ||
        DecideVRSceneFadeLatch(true, 249, 0) !=
            FadeLatchDecision::kWait ||
        DecideVRSceneFadeLatch(true, 250, 0) !=
            FadeLatchDecision::kHardBackstop ||
        // Fresh-pair evidence wins when it arrives on the bound itself.
        DecideVRSceneFadeLatch(true, 250, 2) !=
            FadeLatchDecision::kFreshPairs) {
        return EXIT_FAILURE;
    }

    constexpr VRSceneFadePairState noPair{ false, 0 };
    constexpr auto acceptedLeft =
        AdvanceVRSceneFadePair(noPair, 0, true);
    constexpr auto rejectedRight =
        AdvanceVRSceneFadePair(acceptedLeft, 1, false);
    constexpr auto orphanRight =
        AdvanceVRSceneFadePair(rejectedRight, 1, true);
    constexpr auto completePair = AdvanceVRSceneFadePair(
        AdvanceVRSceneFadePair(noPair, 0, true), 1, true);
    constexpr auto rejectedLeft =
        AdvanceVRSceneFadePair(noPair, 0, false);
    constexpr auto cappedPair = AdvanceVRSceneFadePair(
        VRSceneFadePairState{ true, 2 }, 1, true);
    if (acceptedLeft != VRSceneFadePairState{ true, 0 } ||
        rejectedRight != noPair ||
        orphanRight != noPair ||
        completePair != VRSceneFadePairState{ false, 1 } ||
        rejectedLeft != noPair ||
        cappedPair != VRSceneFadePairState{ false, 2 } ||
        AdvanceVRSceneFadePair(noPair, 2, true) != noPair) {
        return EXIT_FAILURE;
    }

    if (!IsVRPlayspaceSnapshotSane(0, 0) ||
        !IsVRPlayspaceSnapshotSane(8, 1) ||
        IsVRPlayspaceSnapshotSane(-1, 0) ||
        IsVRPlayspaceSnapshotSane(9, 0) ||
        IsVRPlayspaceSnapshotSane(
            0, static_cast<std::uint8_t>(-1)) ||
        IsVRPlayspaceSnapshotSane(0, 2) ||
        !IsVRTransitionHealthy(0, 0) ||
        IsVRTransitionHealthy(1, 0) ||
        IsVRTransitionHealthy(0, 1)) {
        return EXIT_FAILURE;
    }

    if (!ShouldScheduleVRPlayspaceRepair(true, true, true, 3, 1500) ||
        ShouldScheduleVRPlayspaceRepair(false, true, true, 3, 1500) ||
        ShouldScheduleVRPlayspaceRepair(true, false, true, 3, 1500) ||
        ShouldScheduleVRPlayspaceRepair(true, true, false, 3, 1500) ||
        ShouldScheduleVRPlayspaceRepair(true, true, true, 2, 1500) ||
        ShouldScheduleVRPlayspaceRepair(true, true, true, 3, 1499) ||
        !ShouldScheduleVRPlayspaceRepair(
            true, true, true, 5, 2000, 5, 2000) ||
        ShouldScheduleVRPlayspaceRepair(
            true, true, true, 4, 2000, 5, 2000) ||
        ShouldScheduleVRPlayspaceRepair(
            true, true, true, 5, 1999, 5, 2000)) {
        return EXIT_FAILURE;
    }

    if (!ShouldRestoreFlatLoadBudget(4, 4, false) ||
        ShouldRestoreFlatLoadBudget(0, 0, false) ||
        ShouldRestoreFlatLoadBudget(3, 4, false) ||
        ShouldRestoreFlatLoadBudget(4, 4, true)) {
        return EXIT_FAILURE;
    }

    if (ClassifyNativeLoadingContent(false, false, true, 1) !=
            NativeLoadingContent::kUnknown ||
        ClassifyNativeLoadingContent(true, true, true, 1) !=
            NativeLoadingContent::kBackgroundOnly ||
        ClassifyNativeLoadingContent(true, false, false, 0) !=
            NativeLoadingContent::kBackgroundOnly ||
        ClassifyNativeLoadingContent(true, false, true, 0) !=
            NativeLoadingContent::kTipAndLevel ||
        ClassifyNativeLoadingContent(true, false, false, 1) !=
            NativeLoadingContent::kTipAndLevel ||
        ClassifyFlatNativeLoadingContent(false, false, true, 1) !=
            FlatNativeLoadingContent::kUnknown ||
        ClassifyFlatNativeLoadingContent(true, true, true, 1) !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        ClassifyFlatNativeLoadingContent(true, false, false, 0) !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        ClassifyFlatNativeLoadingContent(true, false, true, 0) !=
            FlatNativeLoadingContent::kTipAndLevel ||
        ClassifyFlatNativeLoadingContent(true, false, false, 1) !=
            FlatNativeLoadingContent::kTipAndLevel ||
        ClassifyNGFlatNativeLoadingContent(false, false, true, 1) !=
            FlatNativeLoadingContent::kUnknown ||
        ClassifyNGFlatNativeLoadingContent(true, true, true, 1) !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        ClassifyNGFlatNativeLoadingContent(true, false, false, 0) !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        ClassifyNGFlatNativeLoadingContent(true, false, true, 0) !=
            FlatNativeLoadingContent::kTipAndLevel ||
        ClassifyNGFlatNativeLoadingContent(true, false, false, 1) !=
            FlatNativeLoadingContent::kTipAndLevel ||
        FlatNativeContentNeedsVisibleProof(
            FlatNativeLoadingContent::kUnknown) ||
        FlatNativeContentNeedsVisibleProof(
            FlatNativeLoadingContent::kBackgroundOnly) ||
        !FlatNativeContentNeedsVisibleProof(
            FlatNativeLoadingContent::kTipAndLevel)) {
        return EXIT_FAILURE;
    }

    if (IsTipPresentationMode(0) ||
        !IsTipPresentationMode(1) ||
        IsTipPresentationMode(2) ||
        !IsTipPresentationMode(3) ||
        IsTipPresentationMode(4) ||
        !ShouldPresentSolidBlack(
            0, false, NativeLoadingContent::kUnknown) ||
        ShouldPresentSolidBlack(
            1, true, NativeLoadingContent::kBackgroundOnly) ||
        ShouldPresentSolidBlack(
            2, true, NativeLoadingContent::kBackgroundOnly) ||
        ShouldPresentSolidBlack(
            3, false, NativeLoadingContent::kBackgroundOnly) ||
        ShouldPresentSolidBlack(
            3, true, NativeLoadingContent::kUnknown) ||
        ShouldPresentSolidBlack(
            3, true, NativeLoadingContent::kTipAndLevel) ||
        !ShouldPresentSolidBlack(
            3, true, NativeLoadingContent::kBackgroundOnly) ||
        ShouldEnableExteriorPreloadForPresentation(false, true, 3) ||
        !ShouldEnableExteriorPreloadForPresentation(true, true, 3) ||
        !ShouldEnableExteriorPreloadForPresentation(true, true, 0) ||
        !ShouldEnableExteriorPreloadForPresentation(true, true, 1) ||
        !ShouldEnableExteriorPreloadForPresentation(true, true, 2) ||
        !ShouldEnableExteriorPreloadForPresentation(true, false, 0) ||
        ShouldEnableExteriorPreloadForPresentation(true, false, 1) ||
        !ShouldEnableExteriorPreloadForPresentation(true, false, 2) ||
        ShouldEnableExteriorPreloadForPresentation(true, false, 3) ||
        ShouldArmExteriorPresentationEvidence(false, false) ||
        ShouldArmExteriorPresentationEvidence(false, true) ||
        ShouldArmExteriorPresentationEvidence(true, false) ||
        !ShouldArmExteriorPresentationEvidence(true, true) ||
        ShouldConsumeExteriorPresentationEvidence(false, false) ||
        ShouldConsumeExteriorPresentationEvidence(false, true) ||
        ShouldConsumeExteriorPresentationEvidence(true, false) ||
        !ShouldConsumeExteriorPresentationEvidence(true, true) ||
        ShouldRetainExteriorPresentationEvidence(0, 0x3C) ||
        ShouldRetainExteriorPresentationEvidence(0xF94, 0x3C) ||
        !ShouldRetainExteriorPresentationEvidence(0xF94, 0xF94)) {
        return EXIT_FAILURE;
    }

    if (!ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            false, true, true, false, true, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, false, true, false, true, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, false, false, true, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, true, true, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, false, false,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, true,
            false, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            true, true, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, false, true, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, false, true, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, true, false, false, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, true, true, true, true) ||
        ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, true, true, false, false)) {
        return EXIT_FAILURE;
    }

    const auto unreadableSelection =
        ClassifyNativeLoadingSelection(false, false, true, 1);
    const auto modelAndTipSelection =
        ClassifyNativeLoadingSelection(true, false, true, 1);
    const auto tipOnlySelection =
        ClassifyNativeLoadingSelection(true, false, false, 1);
    const auto minimalSelection =
        ClassifyNativeLoadingSelection(true, false, false, 0);
    const auto interiorSelection =
        ClassifyNativeLoadingSelection(true, true, true, 1);
    if (unreadableSelection.model != NativeModelSelection::kUnknown ||
        unreadableSelection.content != NativeLoadingContent::kUnknown ||
        modelAndTipSelection.model != NativeModelSelection::kModel ||
        modelAndTipSelection.content !=
            NativeLoadingContent::kTipAndLevel ||
        tipOnlySelection.model != NativeModelSelection::kNoModel ||
        tipOnlySelection.content != NativeLoadingContent::kTipAndLevel ||
        minimalSelection.model != NativeModelSelection::kNoModel ||
        minimalSelection.content != NativeLoadingContent::kBackgroundOnly ||
        interiorSelection.model != NativeModelSelection::kNoModel ||
        interiorSelection.content !=
            NativeLoadingContent::kBackgroundOnly) {
        return EXIT_FAILURE;
    }

    // VR mode 3 keeps March's floor, but it no longer mistakes that floor for
    // a tip-capture deadline. The +254 ms case is the field regression: it must
    // remain live until capture, while a verified no-tip transition still
    // freezes at 100 ms. All failure paths remain bounded.
    if (DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kBackgroundOnly,
            false, false, false, 99, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kBackgroundOnly,
            false, false, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, false, 100, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, true, 254, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            true, false, false, 254, 100) !=
            VRTipsFreezeDecision::kFreezeTips ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, true, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, false, NativeLoadingContent::kUnknown,
            false, true, false, 99, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, false, NativeLoadingContent::kUnknown,
            false, true, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, false, 599, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, false, 600, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, true, 600, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, true, 699, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kTipAndLevel,
            false, false, true, 700, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, false, NativeLoadingContent::kUnknown,
            false, false, false, 599, 100) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, false, NativeLoadingContent::kUnknown,
            false, false, false, 600, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kUnknown,
            false, false, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kBackgroundOnly,
            false, false, false, 499, 500) !=
            VRTipsFreezeDecision::kWait ||
        DecideVRTipsFreeze(
            false, true, NativeLoadingContent::kBackgroundOnly,
            false, false, false, 500, 500) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideVRTipsFreeze(
            true, false, NativeLoadingContent::kUnknown,
            true, false, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeTips ||
        DecideVRTipsFreeze(
            true, false, NativeLoadingContent::kUnknown,
            false, false, false, 100, 100) !=
            VRTipsFreezeDecision::kFreezeBackgroundOnly) {
        return EXIT_FAILURE;
    }

    if (!ShouldEnableMode3TipCapture(false, 3, false) ||
        !ShouldEnableMode3TipCapture(false, 3, true) ||
        ShouldEnableMode3TipCapture(false, 2, true) ||
        ShouldEnableMode3TipCapture(true, 3, false) ||
        !ShouldEnableMode3TipCapture(true, 3, true) ||
        ShouldEnableMode3TipCapture(true, 2, true)) {
        return EXIT_FAILURE;
    }

    if (!ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, true, 3, true,
            NativeLoadingContent::kBackgroundOnly) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, false, 3, true,
            NativeLoadingContent::kBackgroundOnly) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, true, 3, false,
            NativeLoadingContent::kBackgroundOnly) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, true, 3, true, NativeLoadingContent::kTipAndLevel) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, true, 3, true, NativeLoadingContent::kUnknown) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            false, true, 2, true,
            NativeLoadingContent::kBackgroundOnly) ||
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            true, true, 3, true,
            NativeLoadingContent::kBackgroundOnly)) {
        return EXIT_FAILURE;
    }

    if (SelectNGPresentationMode(
            3, NativeModelSelection::kModel,
            NativeLoadingContent::kTipAndLevel) != 3 ||
        SelectNGPresentationMode(
            3, NativeModelSelection::kModel,
            NativeLoadingContent::kBackgroundOnly) != 3 ||
        SelectNGPresentationMode(
            3, NativeModelSelection::kNoModel,
            NativeLoadingContent::kTipAndLevel) != 3 ||
        SelectNGPresentationMode(
            3, NativeModelSelection::kNoModel,
            NativeLoadingContent::kBackgroundOnly) != 3 ||
        SelectNGPresentationMode(
            3, NativeModelSelection::kUnknown,
            NativeLoadingContent::kUnknown) != 3 ||
        SelectNGPresentationMode(
            2, NativeModelSelection::kModel,
            NativeLoadingContent::kTipAndLevel) != 2 ||
        SelectNGPresentationMode(
            2, NativeModelSelection::kNoModel,
            NativeLoadingContent::kTipAndLevel) != 2 ||
        SelectNGPresentationMode(
            1, NativeModelSelection::kModel,
            NativeLoadingContent::kTipAndLevel) != 1 ||
        SelectNGPresentationMode(
            0, NativeModelSelection::kUnknown,
            NativeLoadingContent::kUnknown) != 0) {
        return EXIT_FAILURE;
    }

    if (DecideFlatTipsFreeze(
            false, FlatNativeLoadingContent::kTipAndLevel,
            true, true, true, true, 350, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kUnknown,
            true, true, true, true, 350, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kBackgroundOnly,
            false, false, false, false, 350, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kBackgroundOnly,
            false, false, true, false, 350, 60) !=
            FlatTipsFreezeDecision::kPresentReadyFrame ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kBackgroundOnly,
            false, false, true, true, 59, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kBackgroundOnly,
            false, false, true, true, 60, 60) !=
            FlatTipsFreezeDecision::kFreezeBackgroundOnly ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            false, true, true, false, 350, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            true, false, true, false, 350, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            true, true, true, false, 350, 60) !=
            FlatTipsFreezeDecision::kPresentReadyFrame ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            true, true, true, true, 59, 60) !=
            FlatTipsFreezeDecision::kWait ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            true, true, true, true, 60, 60) !=
            FlatTipsFreezeDecision::kFreezeTips ||
        DecideFlatTipsFreeze(
            true, FlatNativeLoadingContent::kTipAndLevel,
            true, true, true, true, 350, 0) !=
            FlatTipsFreezeDecision::kWait) {
        return EXIT_FAILURE;
    }

    if (!ShouldRetainFrozenFlatPublication(true, true, false) ||
        ShouldRetainFrozenFlatPublication(true, true, true) ||
        ShouldRetainFrozenFlatPublication(true, false, false) ||
        ShouldRetainFrozenFlatPublication(false, true, false)) {
        return EXIT_FAILURE;
    }

    if (FlatVisibleTipDeltaMinimum(0) != 64 ||
        FlatVisibleTipDeltaMinimum(1'048'576) != 524 ||
        AcceptFlatVisibleTipDelta(0, 1'048'576) ||
        AcceptFlatVisibleTipDelta(523, 1'048'576) ||
        !AcceptFlatVisibleTipDelta(524, 1'048'576) ||
        !AcceptFlatVisibleTipDelta(22'000, 1'048'576) ||
        AcceptFlatVisibleTipDelta(104'858, 1'048'576) ||
        AcceptFlatVisibleTipDelta(1'000, 0)) {
        return EXIT_FAILURE;
    }

    // Exterior look-ahead can make PositionPlayerJob request a minimal first
    // Show. The exact caller and copied world/interior target are required to
    // restore the non-minimal argument before Bethesda selects candidates.
    const auto incorrectPrefetchedSelection =
        ClassifyNGFlatNativeLoadingSelection(true, true, false, 0);
    const auto correctedExteriorSelection =
        ClassifyNGFlatNativeLoadingSelection(true, false, true, 1);
    const auto realInteriorSelection =
        ClassifyNGFlatNativeLoadingSelection(true, true, true, 1);
    if (incorrectPrefetchedSelection.content !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        !ShouldCorrectPositionExteriorShow(
            true, true, true, false, true, false,
            false, true, true, true, false, true) ||
        correctedExteriorSelection.content !=
            FlatNativeLoadingContent::kTipAndLevel ||
        correctedExteriorSelection.model != NativeModelSelection::kModel ||
        !FlatNativeContentNeedsVisibleProof(
            correctedExteriorSelection.content) ||
        ShouldPresentSolidBlack(
            3, true, correctedExteriorSelection.content) ||
        realInteriorSelection.content !=
            FlatNativeLoadingContent::kBackgroundOnly ||
        realInteriorSelection.model != NativeModelSelection::kNoModel ||
        FlatNativeContentNeedsVisibleProof(realInteriorSelection.content) ||
        !ShouldPresentSolidBlack(
            3, true, realInteriorSelection.content) ||
        SelectNGPresentationMode(
            3, correctedExteriorSelection.model,
            correctedExteriorSelection.content) != 3 ||
        SelectNGPresentationMode(
            3, realInteriorSelection.model,
            realInteriorSelection.content) != 3) {
        return EXIT_FAILURE;
    }

    if (!ShouldResumePostLoadSystems(true, false, 1) ||
        ShouldResumePostLoadSystems(false, false, 1) ||
        ShouldResumePostLoadSystems(true, true, 1) ||
        ShouldResumePostLoadSystems(true, false, 0)) {
        return EXIT_FAILURE;
    }

    if (!NGHasReachedSafeClose(false, false, false, false) ||
        NGHasReachedSafeClose(true, false, false, false) ||
        NGHasReachedSafeClose(false, true, false, false) ||
        NGHasReachedSafeClose(false, false, true, false) ||
        NGHasReachedSafeClose(false, false, false, true)) {
        return EXIT_FAILURE;
    }

    if (KeepStartupBenchmarkMode(0, 1) != 0 ||
        KeepStartupBenchmarkMode(1, 0) != 1 ||
        KeepStartupBenchmarkMode(-1, 1) != 1 ||
        !IsPassiveMeasurementMode(0) ||
        IsPassiveMeasurementMode(1) ||
        IsPassiveMeasurementMode(-1) ||
        !ShouldUsePassiveMeasurement(0, false) ||
        !ShouldUsePassiveMeasurement(1, true) ||
        ShouldUsePassiveMeasurement(1, false) ||
        ShouldApplyLegacyLoadBudgets(true, 1) ||
        !ShouldApplyLegacyLoadBudgets(false, 1) ||
        ShouldApplyLegacyLoadBudgets(false, 0) ||
        ShouldStartLoadHeartbeat(true, false) ||
        !ShouldStartLoadHeartbeat(true, true) ||
        !ShouldStartLoadHeartbeat(false, false) ||
        !ShouldArmVRTimerPatches(true, true, false) ||
        ShouldArmVRTimerPatches(true, false, false) ||
        ShouldArmVRTimerPatches(true, true, true) ||
        ShouldArmVRTimerPatches(false, true, false) ||
        !ShouldPublishLoadedSession(true) ||
        ShouldPublishLoadedSession(false)) {
        return EXIT_FAILURE;
    }

    {
        ConfigReloadCoordinator reload{};
        reload.Reset(10);
        if (reload.Observe(10, false) || reload.IsPending() ||
            reload.IsQueued()) {
            return EXIT_FAILURE;
        }
        if (reload.Observe(11, true) || !reload.IsPending() ||
            reload.IsQueued()) {
            return EXIT_FAILURE;
        }
        if (!reload.TryQueue(false) || !reload.IsQueued() ||
            reload.TryQueue(false)) {
            return EXIT_FAILURE;
        }
        if (reload.Observe(12, false) || !reload.IsPending() ||
            !reload.IsQueued()) {
            return EXIT_FAILURE;
        }
        if (!reload.FinishTask(true) || reload.IsQueued() ||
            !reload.IsPending() || !reload.TryQueue(false) ||
            reload.FinishTask(false) == false || !reload.IsPending() ||
            !reload.TryQueue(false) || reload.FinishTask(true) ||
            reload.IsPending() || reload.IsQueued()) {
            return EXIT_FAILURE;
        }
    }

    {
        ConfigChangeDebouncer debounce{};
        if (debounce.ConsumeIfStable(100, 100)) {
            return EXIT_FAILURE;
        }
        debounce.ObserveChange(100);
        if (debounce.ConsumeIfStable(199, 100)) {
            return EXIT_FAILURE;
        }
        debounce.ObserveChange(180);
        if (debounce.ConsumeIfStable(279, 100) ||
            !debounce.ConsumeIfStable(280, 100) ||
            debounce.ConsumeIfStable(500, 100)) {
            return EXIT_FAILURE;
        }
    }

    {
        ConfigReloadCoordinator reload{};
        reload.Reset(5);
        if (reload.Observe(0, true) || !reload.IsPending()) {
            return EXIT_FAILURE;
        }
        reload.Acknowledge(5);
        if (!reload.IsPending()) {
            return EXIT_FAILURE;
        }
        reload.Acknowledge(0);
        if (reload.IsPending()) {
            return EXIT_FAILURE;
        }

        reload.Reset(20);
        if (!reload.Observe(21, false) ||
            reload.Observe(22, false)) {
            return EXIT_FAILURE;
        }
        reload.Acknowledge(21);
        if (!reload.IsPending() || !reload.FinishTask(true) ||
            !reload.IsPending()) {
            return EXIT_FAILURE;
        }
        reload.Acknowledge(22);
        if (reload.IsPending() || reload.IsQueued()) {
            return EXIT_FAILURE;
        }
    }

    {
        PassiveMenuTimer timer{};
        const auto ignoredClose = timer.Observe(false, 10);
        const auto firstOpen = timer.Observe(true, 20);
        const auto duplicateOpen = timer.Observe(true, 50);
        const auto firstClose = timer.Observe(false, 125);
        const auto duplicateClose = timer.Observe(false, 140);
        const auto secondOpen = timer.Observe(true, 200);
        const auto secondClose = timer.Observe(false, 260);
        if (ignoredClose.boundary != PassiveMenuBoundary::kIgnored ||
            firstOpen.boundary != PassiveMenuBoundary::kOpened ||
            firstOpen.menuSequence != 1 ||
            duplicateOpen.boundary != PassiveMenuBoundary::kIgnored ||
            firstClose.boundary != PassiveMenuBoundary::kClosed ||
            firstClose.menuSequence != 1 || firstClose.durationMs != 105 ||
            duplicateClose.boundary != PassiveMenuBoundary::kIgnored ||
            secondOpen.boundary != PassiveMenuBoundary::kOpened ||
            secondOpen.menuSequence != 2 ||
            secondClose.boundary != PassiveMenuBoundary::kClosed ||
            secondClose.menuSequence != 2 || secondClose.durationMs != 60) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
