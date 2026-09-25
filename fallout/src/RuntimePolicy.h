#pragma once

#include <cstdint>
#include <string_view>

namespace VRLoadingScreens::Policy
{
    enum class ExecutableKind : std::uint8_t
    {
        kUnknown,
        kFallout4,
        kFallout4VR
    };

    enum class ModuleRuntimeKind : std::uint8_t
    {
        kUnknown,
        kFallout4,
        kFallout4NG,
        kFallout4VR
    };

    enum class VRPostCloseReleaseDecision : std::uint8_t
    {
        kHold,
        kReady,
        kHardBackstop
    };

    enum class VRSceneFadeLatchDecision : std::uint8_t
    {
        kArm,
        kWait,
        kFreshPairs,
        kHardBackstop
    };

    enum class PassiveMenuBoundary : std::uint8_t
    {
        kIgnored,
        kOpened,
        kClosed
    };

    enum class FlatTipsFreezeDecision : std::uint8_t
    {
        kWait,
        kPresentReadyFrame,
        kFreezeTips,
        kFreezeBackgroundOnly
    };

    enum class VRTipsFreezeDecision : std::uint8_t
    {
        kWait,
        kFreezeTips,
        kFreezeBackgroundOnly
    };

    // The exact choice made by the verified Fallout 4 and Fallout 4 VR
    // runtimes inside LoadingMenu::SendLoadingText. "BackgroundOnly" means the
    // native movie deliberately did not receive SetLoadingText for this
    // transition. NG still calls SetLevel on that branch, but LoadingMenu.as
    // keeps both level widgets hidden while InMinimalMode is true; SetLevel is
    // therefore not visible-content proof.
    enum class NativeLoadingContent : std::uint8_t
    {
        kUnknown,
        kBackgroundOnly,
        kTipAndLevel
    };

    // SendLoadingText's text choice and AdvanceMovie's 3D-model choice are
    // related but not identical. `artScreen` feeds both; a remaining
    // `validScreens` candidate feeds SetLoadingText only. Keep the initial
    // model decision separate so later text rotations cannot downgrade it.
    enum class NativeModelSelection : std::uint8_t
    {
        kUnknown,
        kNoModel,
        kModel
    };

    struct NativeLoadingSelection
    {
        NativeLoadingContent content{ NativeLoadingContent::kUnknown };
        NativeModelSelection model{ NativeModelSelection::kUnknown };
    };

    // Kept as a source-compatible name for the flat proof state machine. The
    // underlying native choice is now also consumed by Fallout4VR 1.2.72.
    using FlatNativeLoadingContent = NativeLoadingContent;

    struct PassiveMenuBoundaryResult
    {
        PassiveMenuBoundary boundary{ PassiveMenuBoundary::kIgnored };
        std::uint64_t menuSequence{ 0 };
        std::int64_t durationMs{ 0 };
    };

    // Pure single-flight state used by the live MCM file watcher. A changed
    // generation remains pending while a load is active, coalesces repeated
    // writes while a task is queued, and is consumed only by a successful
    // outside-load refresh.
    struct ConfigReloadCoordinator
    {
        void Reset(std::uint64_t a_generation = 0) noexcept
        {
            latestGeneration = a_generation;
            queuedGeneration = a_generation;
            pending = false;
            queued = false;
        }

        [[nodiscard]] bool Observe(
            std::uint64_t a_generation, bool a_loadActive) noexcept
        {
            if (a_generation != latestGeneration) {
                latestGeneration = a_generation;
                pending = true;
            }
            return TryQueue(a_loadActive);
        }

        [[nodiscard]] bool TryQueue(bool a_loadActive) noexcept
        {
            if (!pending || queued || a_loadActive) {
                return false;
            }
            queued = true;
            queuedGeneration = latestGeneration;
            return true;
        }

        [[nodiscard]] bool FinishTask(bool a_succeeded) noexcept
        {
            if (!queued) {
                return false;
            }
            if (a_succeeded && queuedGeneration == latestGeneration) {
                pending = false;
            }
            queued = false;
            return pending;
        }

        void Acknowledge(std::uint64_t a_generation) noexcept
        {
            if (a_generation == latestGeneration) {
                pending = false;
            }
        }

        [[nodiscard]] bool IsPending() const noexcept { return pending; }
        [[nodiscard]] bool IsQueued() const noexcept { return queued; }
        [[nodiscard]] std::uint64_t Generation() const noexcept
        {
            return latestGeneration;
        }

        std::uint64_t latestGeneration{ 0 };
        std::uint64_t queuedGeneration{ 0 };
        bool pending{ false };
        bool queued{ false };
    };

    // Files written through Win32 INI APIs can generate several timestamp/size
    // changes. Wait for one short stable interval before parsing the merged MCM
    // settings so a partial write can never become the published snapshot.
    struct ConfigChangeDebouncer
    {
        void ObserveChange(std::int64_t a_nowMs) noexcept
        {
            dirty = true;
            changedAtMs = a_nowMs;
        }

        [[nodiscard]] bool ConsumeIfStable(
            std::int64_t a_nowMs, std::int64_t a_settleMs) noexcept
        {
            if (!dirty || a_settleMs < 0 ||
                a_nowMs - changedAtMs < a_settleMs) {
                return false;
            }
            dirty = false;
            return true;
        }

        bool dirty{ false };
        std::int64_t changedAtMs{ 0 };
    };

    // Pure state machine used by the measurement-only build path. It observes
    // native LoadingMenu boundaries without installing an engine/render hook.
    // Duplicate OPEN and unmatched CLOSE notifications must not reset a timer
    // or create a second sample.
    struct PassiveMenuTimer
    {
        [[nodiscard]] constexpr PassiveMenuBoundaryResult Observe(
            bool a_opening, std::int64_t a_nowMs) noexcept
        {
            if (a_opening) {
                if (open) {
                    return {};
                }
                open = true;
                openedAtMs = a_nowMs;
                return {
                    PassiveMenuBoundary::kOpened,
                    ++menuSequence,
                    0
                };
            }

            if (!open) {
                return {};
            }
            open = false;
            const auto duration = a_nowMs >= openedAtMs ?
                a_nowMs - openedAtMs : 0;
            return {
                PassiveMenuBoundary::kClosed,
                menuSequence,
                duration
            };
        }

        bool open{ false };
        std::uint64_t menuSequence{ 0 };
        std::int64_t openedAtMs{ 0 };
    };

    inline constexpr std::int64_t kVRPostCloseReadyFloorMs = 600;
    inline constexpr std::int64_t kVRPostCloseHardBackstopMs = 2500;
    // FadeToColor(alpha=1) and FadeToColor(alpha=0) must not be issued in the
    // same Submit callback: OpenVR may only observe the second target. Keep the
    // artwork above an opaque scene fade across later accepted eye pairs before
    // beginning the reveal. The wall-clock bound is deliberately short and
    // fail-open once right-eye callbacks resume, so rejected submissions do
    // not strand the artwork. With no callbacks at all, retaining the artwork
    // is intentional because there is no new compositor work to reveal.
    inline constexpr std::int64_t kVRSceneFadeLatchMinimumMs = 32;
    inline constexpr int kVRSceneFadeLatchRequiredPairs = 2;
    inline constexpr std::int64_t kVRSceneFadeLatchBackstopMs = 250;
    inline constexpr int kVRPlayspaceRepairMinimumSamples = 3;
    inline constexpr std::int64_t kVRPlayspaceRepairDwellMs = 1500;
    // Exact March v1.0 timing. The first title-menu load keeps Bethesda's fade
    // alive for 500 ms; later tip loads get 100 ms for Scaleform, while black
    // and background-only modes freeze on the next right-eye Submit.
    inline constexpr std::int64_t kVRMarchFirstLoadDelayMs = 500;
    inline constexpr std::int64_t kVRMarchTipsDelayMs = 100;
    // A verified tip-bearing screen may outlive March's 100 ms speed floor:
    // field evidence has its first visible DisplayMenu at +254 ms. Keep the
    // old bounded 600 ms capture window, then allow an already-issued GPU
    // visibility query one final 100 ms to retire. Neither boundary can delay
    // native background-only transitions beyond the March floor.
    inline constexpr std::int64_t kVRTipsCaptureDeadlineMs = 600;
    inline constexpr std::int64_t kVRTipsPendingQueryDeadlineMs = 700;
    inline constexpr std::int64_t kVRMarchPostCloseHoldMs = 200;

    // Mirrors the native SendLoadingText branch before that function mutates
    // its candidate array. Interior/minimal transitions and an empty candidate
    // list do not call SetLoadingText. artScreen is the explicit candidate;
    // validScreensSize is the random-candidate pool used otherwise.
    [[nodiscard]] constexpr NativeLoadingSelection
        ClassifyNativeLoadingSelection(
        bool a_fieldsReadable,
        bool a_loadingIntoInterior,
        bool a_hasArtScreen,
        std::uint32_t a_validScreensSize) noexcept
    {
        if (!a_fieldsReadable) {
            return {};
        }
        return {
            !a_loadingIntoInterior &&
                    (a_hasArtScreen || a_validScreensSize != 0) ?
                NativeLoadingContent::kTipAndLevel :
                NativeLoadingContent::kBackgroundOnly,
            !a_loadingIntoInterior && a_hasArtScreen ?
                NativeModelSelection::kModel :
                NativeModelSelection::kNoModel
        };
    }

    [[nodiscard]] constexpr NativeLoadingContent
        ClassifyNativeLoadingContent(
        bool a_fieldsReadable,
        bool a_loadingIntoInterior,
        bool a_hasArtScreen,
        std::uint32_t a_validScreensSize) noexcept
    {
        return ClassifyNativeLoadingSelection(
            a_fieldsReadable, a_loadingIntoInterior, a_hasArtScreen,
            a_validScreensSize).content;
    }

    [[nodiscard]] constexpr FlatNativeLoadingContent
        ClassifyFlatNativeLoadingContent(
        bool a_fieldsReadable,
        bool a_loadingIntoInterior,
        bool a_hasArtScreen,
        std::uint32_t a_validScreensSize) noexcept
    {
        return ClassifyNativeLoadingContent(
            a_fieldsReadable, a_loadingIntoInterior, a_hasArtScreen,
            a_validScreensSize);
    }

    // Kept as a named NG entry point because its native layout is verified
    // independently. Its visual semantics are the same: SetLevel without a
    // preceding SetLoadingText remains hidden in LoadingMenu's minimal mode.
    [[nodiscard]] constexpr NativeLoadingSelection
        ClassifyNGFlatNativeLoadingSelection(
        bool a_fieldsReadable,
        bool a_loadingIntoInterior,
        bool a_hasArtScreen,
        std::uint32_t a_validScreensSize) noexcept
    {
        return ClassifyNativeLoadingSelection(
            a_fieldsReadable, a_loadingIntoInterior, a_hasArtScreen,
            a_validScreensSize);
    }

    [[nodiscard]] constexpr FlatNativeLoadingContent
        ClassifyNGFlatNativeLoadingContent(
        bool a_fieldsReadable,
        bool a_loadingIntoInterior,
        bool a_hasArtScreen,
        std::uint32_t a_validScreensSize) noexcept
    {
        return ClassifyNGFlatNativeLoadingSelection(
            a_fieldsReadable, a_loadingIntoInterior, a_hasArtScreen,
            a_validScreensSize).content;
    }

    [[nodiscard]] constexpr bool IsTipPresentationMode(int a_mode) noexcept
    {
        return a_mode == 1 || a_mode == 3;
    }

    // Mode 3 follows Bethesda's exact per-load content choice. A verified
    // background-only selection is the native minimal screen, so custom art
    // must not replace it; the custom presenter owns an opaque black frame
    // instead. Explicit user modes retain their configured meaning: mode 0 is
    // always black, mode 1 remains native, and mode 2 always shows custom art.
    // Unknown/unverified mode-3 selections stay on the established custom-art
    // fallback until the exact SendLoadingText hook publishes a decision.
    [[nodiscard]] constexpr bool ShouldPresentSolidBlack(
        int a_loadingScreenMode,
        bool a_nativeSelectionVerified,
        NativeLoadingContent a_nativeContent) noexcept
    {
        return a_loadingScreenMode == 0 ||
            (a_loadingScreenMode == 3 && a_nativeSelectionVerified &&
             a_nativeContent == NativeLoadingContent::kBackgroundOnly);
    }

    // A plugin-issued exterior PreloadWorld call can make PositionPlayerJob
    // classify a normally full exterior transition as minimal. In a
    // tip-preserving mode, keep the optimization only when this exact runtime
    // has a byte-validated PositionPlayerJob -> ShowLoadingMenu correction
    // contract. Presentation-independent modes do not need that correction.
    [[nodiscard]] constexpr bool ShouldEnableExteriorPreloadForPresentation(
        bool a_requested,
        bool a_exactPresentationCorrectionReady,
        int a_loadingScreenMode) noexcept
    {
        return a_requested &&
            (a_exactPresentationCorrectionReady ||
             !IsTipPresentationMode(a_loadingScreenMode));
    }

    [[nodiscard]] constexpr bool ShouldArmExteriorPresentationEvidence(
        bool a_exactPresentationCorrectionReady,
        bool a_engineCallReturned) noexcept
    {
        return a_exactPresentationCorrectionReady && a_engineCallReturned;
    }

    [[nodiscard]] constexpr bool ShouldConsumeExteriorPresentationEvidence(
        bool a_evidencePresent,
        bool a_exactLiveArrivalObserved) noexcept
    {
        return a_evidencePresent && a_exactLiveArrivalObserved;
    }

    [[nodiscard]] constexpr bool ShouldRetainExteriorPresentationEvidence(
        std::uint32_t a_recordedSourceWorldFormID,
        std::uint32_t a_arrivedWorldFormID) noexcept
    {
        return a_recordedSourceWorldFormID != 0 &&
            a_recordedSourceWorldFormID == a_arrivedWorldFormID;
    }

    // PositionPlayerJob asks whether its destination is already resident and
    // passes that result as LoadingMenu's minimal/interior flag. Exterior
    // look-ahead can make this true before the first Show even though the real
    // destination is an exterior. Only the exact, byte-validated worker call
    // may substitute false, after its copied target proves world && !interior,
    // and only for a user mode which actually presents Bethesda's tip/level UI.
    [[nodiscard]] constexpr bool ShouldCorrectPositionExteriorShow(
        bool a_tipPresentationActive,
        bool a_exactBoundaryHooksReady,
        bool a_positionCallerContractReady,
        bool a_menuAlreadyShown,
        bool a_requestedMinimal,
        bool a_deferredPayload,
        bool a_saveClassificationArmed,
        bool a_exactPositionCaller,
        bool a_targetReadable,
        bool a_targetHasWorld,
        bool a_targetHasInterior,
        bool a_matchingPluginExteriorSubmission) noexcept
    {
        return a_tipPresentationActive && a_exactBoundaryHooksReady &&
            a_positionCallerContractReady && !a_menuAlreadyShown &&
            a_requestedMinimal && !a_deferredPayload &&
            !a_saveClassificationArmed && a_exactPositionCaller &&
            a_targetReadable && a_targetHasWorld && !a_targetHasInterior &&
            a_matchingPluginExteriorSubmission;
    }

    // Only SetLoadingText leaves minimal mode and can produce the visible tip
    // plus level UI. It therefore needs an exact owner/serial-scoped
    // DisplayMovie delta before flat mode 3 may freeze.
    [[nodiscard]] constexpr bool FlatNativeContentNeedsVisibleProof(
        FlatNativeLoadingContent a_content) noexcept
    {
        return a_content == FlatNativeLoadingContent::kTipAndLevel;
    }

    [[nodiscard]] constexpr const char* NativeLoadingContentName(
        NativeLoadingContent a_content) noexcept
    {
        switch (a_content) {
        case NativeLoadingContent::kBackgroundOnly:
            return "background-only";
        case NativeLoadingContent::kTipAndLevel:
            return "tip+level";
        default:
            return "unknown";
        }
    }

    // Flat mode's proof and persistent replay shaders remove the entire
    // bottom-right spinner region, so native GFx mutation is not required to
    // keep the spinner out of a published capture. VR uses a different delta
    // shader and must still prove that the native spinner was hidden.
    [[nodiscard]] constexpr bool ShouldEnableMode3TipCapture(
        bool a_isVR,
        int a_loadingScreenMode,
        bool a_nativeSpinnerHidden) noexcept
    {
        return a_loadingScreenMode == 3 &&
            (!a_isVR || a_nativeSpinnerHidden);
    }

    // An exact flat mode-3 minimal publication still owns the black frame that
    // must be presented before AdvanceMovie can freeze. Disabling the unused
    // tip extractor at OPEN must therefore cancel only capture resources, not
    // erase that publication's owner/content/serial. Every unverified, VR, or
    // non-minimal path keeps the ordinary full-invalidation behavior.
    [[nodiscard]] constexpr bool
        ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
            bool a_isVR,
            bool a_loadingMenuOpen,
            int a_loadingScreenMode,
            bool a_nativeSelectionVerified,
            NativeLoadingContent a_nativeContent) noexcept
    {
        return !a_isVR && a_loadingMenuOpen && a_loadingScreenMode == 3 &&
            a_nativeSelectionVerified &&
            a_nativeContent == NativeLoadingContent::kBackgroundOnly;
    }

    // Every verified NG LoadingMenu Show retains the user's configured integer
    // mode. Within mode 3, ShouldPresentSolidBlack independently selects black
    // for an exact native-minimal load while the content choice controls tip
    // capture. Explicit black/native/background modes remain explicit user
    // choices. Keep the selection parameters here so NG's call site continues
    // to pass one coherent native snapshot without rewriting configuration.
    [[nodiscard]] constexpr int SelectNGPresentationMode(
        int a_configuredMode,
        [[maybe_unused]] NativeModelSelection a_nativeModel,
        [[maybe_unused]] NativeLoadingContent a_nativeContent) noexcept
    {
        return a_configuredMode;
    }

    // Flat mode 3 follows the native per-load visual choice. A true
    // background-only/minimal load needs one successfully presented solid-black
    // frame. Tip+level additionally needs an owner/serial-matched, visible
    // DisplayMovie result before its custom-art Present.
    // The frame floor remains a secondary speed/configuration guard and can
    // never substitute for either proof.
    [[nodiscard]] constexpr FlatTipsFreezeDecision DecideFlatTipsFreeze(
        bool a_selectionHookInstalled,
        FlatNativeLoadingContent a_nativeContent,
        bool a_drawHooksInstalled,
        bool a_nativeTipDrawCompleted,
        bool a_nativeOwnerKnown,
        bool a_postPublicationPresentCompleted,
        std::uint32_t a_presentCount,
        int a_minimumPresentCount) noexcept
    {
        if (!a_selectionHookInstalled ||
            a_nativeContent == FlatNativeLoadingContent::kUnknown ||
            !a_nativeOwnerKnown ||
            a_minimumPresentCount <= 0) {
            return FlatTipsFreezeDecision::kWait;
        }
        const bool needsVisibleProof =
            FlatNativeContentNeedsVisibleProof(a_nativeContent);
        if (needsVisibleProof &&
            (!a_drawHooksInstalled || !a_nativeTipDrawCompleted)) {
            return FlatTipsFreezeDecision::kWait;
        }
        if (!a_postPublicationPresentCompleted) {
            return FlatTipsFreezeDecision::kPresentReadyFrame;
        }
        if (a_presentCount <
                static_cast<std::uint32_t>(a_minimumPresentCount)) {
            return FlatTipsFreezeDecision::kWait;
        }
        return needsVisibleProof ?
            FlatTipsFreezeDecision::kFreezeTips :
            FlatTipsFreezeDecision::kFreezeBackgroundOnly;
    }

    // A frozen movie may keep its certified pixels only for a genuine refresh
    // of the same LoadingMenu publication. A new Show can arrive before NG's
    // Present-thread chained CLOSE restores AdvanceMovie; retaining in that
    // case would drop the new menu's model/tip selection.
    [[nodiscard]] constexpr bool ShouldRetainFrozenFlatPublication(
        bool a_advanceMovieKilled,
        bool a_sameOwner,
        bool a_initialSelection) noexcept
    {
        return a_advanceMovieKilled && a_sameOwner && !a_initialSelection;
    }

    // Flat proof pixels already passed the live compositor's luminance key.
    // Reject tiny fade-start fragments and broad UI/full-surface repaints.
    [[nodiscard]] constexpr std::uint64_t FlatVisibleTipDeltaMinimum(
        std::uint64_t a_surfaceSamples) noexcept
    {
        const auto scaled = a_surfaceSamples / 2000;  // 0.05%
        return scaled > 64 ? scaled : 64;
    }

    [[nodiscard]] constexpr bool AcceptFlatVisibleTipDelta(
        std::uint64_t a_visibleSamples,
        std::uint64_t a_surfaceSamples) noexcept
    {
        return a_surfaceSamples != 0 &&
            a_visibleSamples >= FlatVisibleTipDeltaMinimum(a_surfaceSamples) &&
            a_visibleSamples <= a_surfaceSamples / 10;  // at most 10%
    }

    [[nodiscard]] constexpr std::int64_t VRMarchRenderDelayMs(
        int a_loadingScreenMode,
        bool a_gameSessionLoaded) noexcept
    {
        if (!a_gameSessionLoaded) {
            return kVRMarchFirstLoadDelayMs;
        }
        return a_loadingScreenMode == 3 ? kVRMarchTipsDelayMs : 0;
    }

    // Content-aware VR mode-3 terminal policy. March's 500/100 ms value remains
    // a minimum speed boundary. A verified native no-tip choice freezes at that
    // boundary; a verified tip choice may wait for the exact owner-scoped delta,
    // but never beyond the bounded capture/query deadlines. The retained
    // self-MoveTo presentation is already complete and must keep March timing
    // regardless of what the synthetic LoadingMenu selects underneath it.
    [[nodiscard]] constexpr VRTipsFreezeDecision DecideVRTipsFreeze(
        bool a_retainedMarchPresentation,
        bool a_nativeSelectionSeen,
        NativeLoadingContent a_nativeContent,
        bool a_tipCompositeAttached,
        bool a_captureHopeless,
        bool a_deltaProofPending,
        std::int64_t a_elapsedMs,
        std::int64_t a_marchFloorMs) noexcept
    {
        if (a_elapsedMs < a_marchFloorMs) {
            return VRTipsFreezeDecision::kWait;
        }
        if (a_retainedMarchPresentation || a_tipCompositeAttached) {
            return a_tipCompositeAttached ?
                VRTipsFreezeDecision::kFreezeTips :
                VRTipsFreezeDecision::kFreezeBackgroundOnly;
        }
        // A disabled/unavailable pipeline and a provably non-convergent
        // capture are terminal even when SendLoadingText itself was never
        // observed. Preserve the March floor above, then fail background-only
        // immediately instead of spending the full 600 ms on impossible work.
        if (a_captureHopeless) {
            return VRTipsFreezeDecision::kFreezeBackgroundOnly;
        }
        if (a_nativeSelectionSeen) {
            if (a_nativeContent != NativeLoadingContent::kTipAndLevel) {
                return VRTipsFreezeDecision::kFreezeBackgroundOnly;
            }
        } else if (a_elapsedMs < kVRTipsCaptureDeadlineMs) {
            // SendLoadingText normally publishes at OPEN. A late publication
            // remains possible, but it gets the same finite bound as capture.
            return VRTipsFreezeDecision::kWait;
        }

        if (a_nativeSelectionSeen &&
            a_nativeContent == NativeLoadingContent::kTipAndLevel &&
            a_elapsedMs < kVRTipsCaptureDeadlineMs) {
            return VRTipsFreezeDecision::kWait;
        }
        if (a_deltaProofPending &&
            a_elapsedMs < kVRTipsPendingQueryDeadlineMs) {
            return VRTipsFreezeDecision::kWait;
        }
        return VRTipsFreezeDecision::kFreezeBackgroundOnly;
    }

    [[nodiscard]] constexpr bool ShouldExecuteVRMarchMoveTo(
        bool a_waitingForMoveTo,
        bool a_commandIssued,
        bool a_loadingMenuOpen,
        bool a_mainMenuOpen,
        bool a_player3DReady,
        bool a_worldPresentationLive,
        int a_stableMainLoopFrames,
        std::int64_t a_elapsedCloseMs) noexcept
    {
        (void)a_mainMenuOpen;
        (void)a_player3DReady;
        (void)a_worldPresentationLive;
        (void)a_stableMainLoopFrames;
        (void)a_elapsedCloseMs;
        // March issued the command on the very next direct main-loop update
        // after CLOSE. It did not wait on world/player probes or a task queue.
        return a_waitingForMoveTo &&
            !a_commandIssued &&
            !a_loadingMenuOpen;
    }

    // The first title-save CLOSE is only the first half of the retained March
    // transaction. Its raw playspace state is expected to be detached, so the
    // newer repair must not compete with the required self-MoveTo. The repair
    // belongs to the final synthetic CLOSE (or an ordinary successful load).
    [[nodiscard]] constexpr bool ShouldArmVRPlayspaceRepairAtClose(
        bool a_saveLoadClose,
        bool a_firstTitleSaveClose,
        bool a_marchSecondClose) noexcept
    {
        return a_marchSecondClose ||
            (a_saveLoadClose && !a_firstTitleSaveClose);
    }

    // A save-load handoff is not safe merely because the playspace is healthy
    // at CLOSE: that can describe the old title/menu render. Require a bounded
    // post-close floor plus continuously sampled readiness before revealing the
    // world. The hard fallback is finite so an unreadable signal can never
    // strand the loading artwork indefinitely.
    [[nodiscard]] constexpr VRPostCloseReleaseDecision
    DecideVRPostCloseRelease(
        bool a_gateActive,
        bool a_transitionReady,
        std::int64_t a_elapsedCloseMs) noexcept
    {
        if (!a_gateActive) {
            return VRPostCloseReleaseDecision::kReady;
        }
        if (a_transitionReady &&
            a_elapsedCloseMs >= kVRPostCloseReadyFloorMs) {
            return VRPostCloseReleaseDecision::kReady;
        }
        if (a_elapsedCloseMs >= kVRPostCloseHardBackstopMs) {
            return VRPostCloseReleaseDecision::kHardBackstop;
        }
        return VRPostCloseReleaseDecision::kHold;
    }

    [[nodiscard]] constexpr VRSceneFadeLatchDecision
    DecideVRSceneFadeLatch(
        bool a_armed,
        std::int64_t a_elapsedMs,
        int a_postArmAcceptedPairs) noexcept
    {
        if (!a_armed) {
            return VRSceneFadeLatchDecision::kArm;
        }
        // Prefer the evidence-backed reason at the exact backstop boundary.
        if (a_elapsedMs >= kVRSceneFadeLatchMinimumMs &&
            a_postArmAcceptedPairs >= kVRSceneFadeLatchRequiredPairs) {
            return VRSceneFadeLatchDecision::kFreshPairs;
        }
        if (a_elapsedMs >= kVRSceneFadeLatchBackstopMs) {
            return VRSceneFadeLatchDecision::kHardBackstop;
        }
        return VRSceneFadeLatchDecision::kWait;
    }

    struct VRSceneFadePairState
    {
        bool sawAcceptedLeft;
        int acceptedPairs;

        [[nodiscard]] constexpr bool operator==(
            const VRSceneFadePairState&) const noexcept = default;
    };

    // Pair matching is evidence of post-command compositor work, not evidence
    // about texture contents. A rejected right consumes the pending left so a
    // later right can never complete a pair across an OpenVR error.
    [[nodiscard]] constexpr VRSceneFadePairState
    AdvanceVRSceneFadePair(
        VRSceneFadePairState a_state,
        int a_eye,
        bool a_accepted,
        int a_pairLimit = kVRSceneFadeLatchRequiredPairs) noexcept
    {
        if (a_eye == 0) {
            a_state.sawAcceptedLeft = a_accepted;
            return a_state;
        }
        if (a_eye != 1) {
            return a_state;
        }
        if (a_accepted && a_state.sawAcceptedLeft &&
            a_state.acceptedPairs < a_pairLimit) {
            ++a_state.acceptedPairs;
        }
        a_state.sawAcceptedLeft = false;
        return a_state;
    }

    [[nodiscard]] constexpr bool IsVRPlayspaceSnapshotSane(
        std::int32_t a_counter,
        std::uint8_t a_detached) noexcept
    {
        return a_counter >= 0 && a_counter <= 8 && a_detached <= 1;
    }

    [[nodiscard]] constexpr bool IsVRTransitionHealthy(
        std::int32_t a_counter,
        std::uint8_t a_detached) noexcept
    {
        return IsVRPlayspaceSnapshotSane(a_counter, a_detached) &&
            a_counter == 0 && a_detached == 0;
    }

    [[nodiscard]] constexpr bool ShouldScheduleVRPlayspaceRepair(
        bool a_sane,
        bool a_stuck,
        bool a_sameSignature,
        int a_stableSamples,
        std::int64_t a_stableElapsedMs,
        int a_minimumSamples = kVRPlayspaceRepairMinimumSamples,
        std::int64_t a_minimumElapsedMs =
            kVRPlayspaceRepairDwellMs) noexcept
    {
        return a_sane &&
            a_stuck &&
            a_sameSignature &&
            a_stableSamples >= a_minimumSamples &&
            a_stableElapsedMs >= a_minimumElapsedMs;
    }

    struct RuntimeVersionParts
    {
        std::uint16_t major;
        std::uint16_t minor;
        std::uint16_t patch;
        std::uint16_t build;

        [[nodiscard]] constexpr bool operator==(
            const RuntimeVersionParts&) const noexcept = default;
    };

    [[nodiscard]] constexpr bool IsSupportedRuntime(
        std::uint16_t a_major,
        std::uint16_t a_minor,
        std::uint16_t a_patch,
        std::uint16_t a_build) noexcept
    {
        return a_build == 0 &&
            ((a_major == 1 && a_minor == 2 && a_patch == 72) ||
             (a_major == 1 && a_minor == 10 && a_patch == 163) ||
             (a_major == 1 && a_minor == 11 &&
              (a_patch == 221 || a_patch == 240)));
    }

    [[nodiscard]] constexpr bool IsSupportedRuntime(
        RuntimeVersionParts a_version) noexcept
    {
        return IsSupportedRuntime(
            a_version.major,
            a_version.minor,
            a_version.patch,
            a_version.build);
    }

    [[nodiscard]] constexpr bool VersionAtLeast(
        RuntimeVersionParts a_version,
        RuntimeVersionParts a_minimum) noexcept
    {
        if (a_version.major != a_minimum.major) {
            return a_version.major > a_minimum.major;
        }
        if (a_version.minor != a_minimum.minor) {
            return a_version.minor > a_minimum.minor;
        }
        if (a_version.patch != a_minimum.patch) {
            return a_version.patch > a_minimum.patch;
        }
        return a_version.build >= a_minimum.build;
    }

    [[nodiscard]] constexpr wchar_t AsciiLower(wchar_t a_character) noexcept
    {
        return a_character >= L'A' && a_character <= L'Z' ?
            static_cast<wchar_t>(a_character + (L'a' - L'A')) :
            a_character;
    }

    [[nodiscard]] constexpr bool AsciiEqualsIgnoreCase(
        std::wstring_view a_left,
        std::wstring_view a_right) noexcept
    {
        if (a_left.size() != a_right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < a_left.size(); ++index) {
            if (AsciiLower(a_left[index]) != AsciiLower(a_right[index])) {
                return false;
            }
        }
        return true;
    }

    // Classify by the actual loaded image name as well as its version. CommonLib
    // derives IsVR from the version's minor component alone, so IsVR by itself
    // would allow a renamed or version-spoofed flat executable through this gate.
    [[nodiscard]] constexpr ExecutableKind ClassifyExecutable(
        std::wstring_view a_path) noexcept
    {
        const auto separator = a_path.find_last_of(L"\\/");
        const auto filename = separator == std::wstring_view::npos ?
            a_path :
            a_path.substr(separator + 1);

        if (AsciiEqualsIgnoreCase(filename, L"Fallout4VR.exe")) {
            return ExecutableKind::kFallout4VR;
        }
        if (AsciiEqualsIgnoreCase(filename, L"Fallout4.exe")) {
            return ExecutableKind::kFallout4;
        }
        return ExecutableKind::kUnknown;
    }

    // F4SEVR 0.6.21 exposes 0x010A08A0 through QueryInterface::runtimeVersion,
    // which CommonLib decodes as 1.10.138.0 even though the loaded executable is
    // Fallout4VR.exe 1.2.72.0. Accept that historical proxy only when the main
    // image name, CommonLib runtime kind, F4SE version, and independently read
    // file version all prove the exact supported VR host. Never treat 1.10.138
    // as a generally supported flat runtime.
    [[nodiscard]] constexpr bool IsSupportedF4SEHost(
        RuntimeVersionParts a_reported,
        RuntimeVersionParts a_executable,
        RuntimeVersionParts a_f4se,
        ExecutableKind a_executableKind,
        ModuleRuntimeKind a_moduleRuntime,
        bool a_isMainExecutable) noexcept
    {
        constexpr RuntimeVersionParts kFallout4VR{ 1, 2, 72, 0 };
        constexpr RuntimeVersionParts kF4SEVRProxy{ 1, 10, 138, 0 };
        constexpr RuntimeVersionParts kF4SEVR0621{ 0, 6, 21, 0 };
        constexpr RuntimeVersionParts kF4SEWithPluginInfo{ 0, 6, 22, 0 };

        if (!a_isMainExecutable) {
            return false;
        }

        if (a_executableKind == ExecutableKind::kFallout4VR) {
            return a_executable == kFallout4VR &&
                a_moduleRuntime == ModuleRuntimeKind::kFallout4VR &&
                VersionAtLeast(a_f4se, kF4SEVR0621) &&
                (a_reported == kFallout4VR ||
                 (a_reported == kF4SEVRProxy &&
                  a_f4se == kF4SEVR0621));
        }

        // On flat Fallout, require the script extender and executable to agree
        // on the same exact supported build. This fails closed on old flat
        // 1.10.138, pre-GetPluginInfo interfaces, and every unverified NG/AE
        // version.
        if (a_executableKind != ExecutableKind::kFallout4 ||
            !VersionAtLeast(a_f4se, kF4SEWithPluginInfo) ||
            a_reported != a_executable ||
            !IsSupportedRuntime(a_executable)) {
            return false;
        }
        if (a_executable == RuntimeVersionParts{ 1, 10, 163, 0 }) {
            return a_moduleRuntime == ModuleRuntimeKind::kFallout4;
        }
        if (a_executable == RuntimeVersionParts{ 1, 11, 221, 0 } ||
            a_executable == RuntimeVersionParts{ 1, 11, 240, 0 }) {
            return a_moduleRuntime == ModuleRuntimeKind::kFallout4NG;
        }
        return false;
    }

    // The reviewed CommonLib pin assumes F4SEInterface::GetPluginInfo exists,
    // but the official F4SEVR 0.6.21 interface ends immediately before it. The
    // caller uses this exact predicate to provide a zero-extended, synchronous
    // compatibility view to F4SE::Init.
    [[nodiscard]] constexpr bool RequiresLegacyF4SEVRInterfaceShim(
        RuntimeVersionParts a_executable,
        RuntimeVersionParts a_f4se,
        ExecutableKind a_executableKind,
        ModuleRuntimeKind a_moduleRuntime,
        bool a_isMainExecutable) noexcept
    {
        return a_isMainExecutable &&
            a_executableKind == ExecutableKind::kFallout4VR &&
            a_moduleRuntime == ModuleRuntimeKind::kFallout4VR &&
            a_executable == RuntimeVersionParts{ 1, 2, 72, 0 } &&
            a_f4se == RuntimeVersionParts{ 0, 6, 21, 0 };
    }

    [[nodiscard]] constexpr bool ShouldRunPapyrusUpdater(
        bool a_initialized,
        bool a_gameSessionActive,
        bool a_loading,
        bool a_externalOwner) noexcept
    {
        return a_initialized &&
            a_gameSessionActive &&
            !a_loading &&
            !a_externalOwner;
    }

    [[nodiscard]] constexpr bool ShouldRestoreFlatLoadBudget(
        std::uint64_t a_closeGeneration,
        std::uint64_t a_currentGeneration,
        bool a_loadStillActive) noexcept
    {
        return a_closeGeneration != 0 &&
            a_closeGeneration == a_currentGeneration &&
            !a_loadStillActive;
    }

    [[nodiscard]] constexpr bool ShouldResumePostLoadSystems(
        bool a_sessionLoaded,
        bool a_mainMenuOpen,
        int a_startupBenchmarkMode) noexcept
    {
        return a_sessionLoaded &&
            !a_mainMenuOpen &&
            a_startupBenchmarkMode != 0;
    }

    [[nodiscard]] constexpr bool NGHasReachedSafeClose(
        bool a_nativeMenuVisible,
        bool a_loadingActive,
        bool a_enablePending,
        bool a_closePending) noexcept
    {
        return !a_nativeMenuVisible &&
            !a_loadingActive &&
            !a_enablePending &&
            !a_closePending;
    }

    [[nodiscard]] constexpr int KeepStartupBenchmarkMode(
        int a_startupMode, int a_reloadedMode) noexcept
    {
        return a_startupMode >= 0 ? a_startupMode : a_reloadedMode;
    }

    [[nodiscard]] constexpr bool IsPassiveMeasurementMode(
        int a_benchmarkMode) noexcept
    {
        return a_benchmarkMode == 0;
    }

    // The March v1 DLL has a distinct filename/F4SE identity, so F4SE can load
    // it alongside the current plugin. Full mode would then compete for the
    // same Submit hook, NOP site, overlay keys, and self-MoveTo lifecycle.
    // Fail closed into passive observation whenever that companion is present.
    [[nodiscard]] constexpr bool ShouldUsePassiveMeasurement(
        int a_benchmarkMode, bool a_marchVRDllLoaded) noexcept
    {
        return IsPassiveMeasurementMode(a_benchmarkMode) ||
            a_marchVRDllLoaded;
    }

    // March never replaced Fallout VR's 20/5 ms background-load drain budgets
    // with the later 500/500 ms experiment. Keep that experiment flat-only.
    [[nodiscard]] constexpr bool ShouldApplyLegacyLoadBudgets(
        bool a_isVR, int a_benchmarkMode) noexcept
    {
        return !a_isVR && a_benchmarkMode != 0;
    }

    // The normal VR release should have no diagnostic heartbeat competing with
    // the loader. An explicitly enabled presentation probe may opt back in.
    [[nodiscard]] constexpr bool ShouldStartLoadHeartbeat(
        bool a_isVR, bool a_vrPresentationProbe) noexcept
    {
        return !a_isVR || a_vrPresentationProbe;
    }

    // March deferred its timer/iFPSClamp writes until the first successful save
    // had reached native CLOSE. kPostLoadGame only arms that close-side action.
    [[nodiscard]] constexpr bool ShouldArmVRTimerPatches(
        bool a_isVR, bool a_loadSucceeded, bool a_alreadyApplied) noexcept
    {
        return a_isVR && a_loadSucceeded && !a_alreadyApplied;
    }

    [[nodiscard]] constexpr bool ShouldPublishLoadedSession(
        bool a_loadSucceeded) noexcept
    {
        return a_loadSucceeded;
    }
}
