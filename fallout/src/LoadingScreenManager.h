#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace VRLoadingScreens
{
    class LoadingScreenManager
    {
    public:
        static LoadingScreenManager& GetSingleton()
        {
            static LoadingScreenManager instance;
            return instance;
        }

        // overlayMode: 0=HMD-relative, 1=World-locked, 2=Cinema, 3=Submit composite, 4=ClearRTV
        void Init(bool enableBackgrounds = true, int overlayMode = 0,
                  float overlayAlpha = 0.5f);
        void OnLoadingMenuOpen();
        void OnLoadingMenuClose();
        // Close the old per-load visual state before a chained AE native Show is
        // reopened. This deliberately suppresses next-background preparation so
        // no DDS worker is launched inside the chained load.
        void CloseForChainedLoadingMenu();
        // Complete a chained gap which ended without another native Show. The
        // visual state was already closed by CloseForChainedLoadingMenu(); this
        // only releases its background-work barrier and schedules safe post-load
        // preparation.
        void FinishChainedLoadingMenuGap();
        void Update();

        void SetGameSessionLoaded();
        // Called from the F4SE messaging thread when kPostLoadGame arrives, so
        // the native CLOSE can distinguish a load that entered the game from a
        // quit-to-menu when arming post-close MainMenu suppression.
        void NotePostLoadGame() {
            m_postLoadGameSeenThisLoad.store(true, std::memory_order_release);
        }
        // Post-save-load VR playspace repair (see LoadingScreenManager.cpp).
        // Armed at a save-load CLOSE, dispatched to the game thread once the
        // player's cell has attached and the hold has proven stuck, and
        // consumed exactly once per load.
        void RepairVRPlayspaceHoldOnGameThread(
            std::uint64_t a_loadGeneration);
        // MUST be called from the frame callback immediately after Update(),
        // never from inside it: queueing an F4SE task while holding
        // m_stateMutex inverts the lock order against the task pump.
        void DispatchPendingVRPlayspaceRepair();
        // Called from the verified Fallout4VR main-loop callsite, after the
        // chained FRIK/ROCK body update for this frame. The required self-MoveTo
        // and the final one-step reveal run here directly: never from F4SE's
        // task pump, which holds its queue lock while callbacks execute.
        void TickVRMarchMainLoop();
        void ClearGameSessionLoaded();
        void SetMainMenuOpen(bool open) { m_mainMenuOpen.store(open); }
        bool IsInLoadingScreen() const { return m_inLoadingScreen.load(); }
        void SetBackgroundsEnabled(bool enabled);
        void SetOverlayMode(int mode);
        void SetOverlayAlpha(float alpha);
        void SetTimingOnly(bool timingOnly);
        // See m_vrPresentationProbeEnabled. Diagnostic-only; default OFF.
        void SetVRPresentationProbeEnabled(bool a_enabled) {
            m_vrPresentationProbeEnabled.store(
                a_enabled, std::memory_order_release);
        }
        static bool IsVRPresentationProbeEnabled();

        // Called at each renderer-device publication. This starts the first
        // landscape prewarm immediately when possible; it is idempotent and
        // leaves flat/VR lazy device discovery intact.
        void OnD3DDeviceReady();
        // Called after the renderer publishes a replacement device or reports
        // device removal. Invalidates textures created on the old device and
        // makes the current load fail open to Bethesda's native presentation.
        void OnD3DDeviceChanged();

        // Retry texture loading after device becomes available (NG deferred init)
        // or after a load caused an in-flight prepare to defer.
        void RetryTextureLoad() {
            if (!m_currentBgTexture.load(std::memory_order_acquire) ||
                m_bgPrepareDeferred.load(std::memory_order_acquire)) {
                PrepareNextBackgroundAsync();
            }
        }
        void* GetCurrentBgTexture() const {
            return m_currentBgTexture.load(std::memory_order_acquire);
        }
        std::uint64_t GetBackgroundGeneration() const {
            return m_backgroundGeneration.load(std::memory_order_acquire);
        }

    private:
        LoadingScreenManager() = default;
        ~LoadingScreenManager();

        void ScanForTextures();
        std::string PickRandomTexture();
        // Async variant: picks the path under the texture-state lock, then does
        // disk IO + texture creation on a joinable worker. The synchronous
        // version measured a 4.8-5.7s block
        // on the close path (disk contention at load-end + software BC decode)
        // — on OG that froze the game thread, on NG the render thread.
        void PrepareNextBackgroundAsync();
        void CloseLoadingMenu(bool a_prepareNextBackground);
        bool HideNativeLoadingSpinnerForCustomScreen();
        bool RestoreNativeLoadingSpinner();
        static bool ShouldDeferBackgroundWork(void* context);
        static bool TryBeginBackgroundUpload(void* context);
        static void EndBackgroundUpload(void* context);

        std::vector<std::string> m_texturePaths;
        std::string m_currentTexturePath;
        // The current texture is published atomically by the IO worker. The
        // previous texture remains retained for one complete prepare cycle, so a
        // render callback that loaded the old pointer before a swap cannot observe
        // freed COM storage.
        std::atomic<void*> m_currentBgTexture{ nullptr };  // ID3D11Texture2D*
        // Async prep state: single-flight guard + one-cycle deferred release.
        // The old texture is parked in m_retiredBgTexture instead of being
        // released at swap time — a reader (OPEN path / VR Update) may have
        // fetched the old pointer just before the swap; by the NEXT prepare a
        // full load cycle has passed and nothing can still reference it.
        std::atomic<bool> m_bgPrepareInFlight{ false };
        // Published at the very start of a native OPEN, before lifecycle-state
        // initialization takes the manager mutex. This gate is exclusively for
        // background IO/upload; m_inLoadingScreen is published only after the
        // current load's state has been initialized.
        static constexpr std::uint32_t kBgLoadActive = 1u << 0;
        static constexpr std::uint32_t kBgUploadAdmitted = 1u << 1;
        std::atomic<std::uint32_t> m_bgWorkState{ 0 };
        // Set when a prepare could not start/finish because LoadingMenu became
        // active. Render callbacks retry it only after the load has closed.
        std::atomic<bool> m_bgPrepareDeferred{ false };
        void* m_retiredBgTexture = nullptr;
        std::jthread m_bgPrepareThread;
        std::mutex m_bgThreadMutex;

        // Load-progress heartbeat.
        //
        // On VR, Update() is driven by the per-frame callback inside the Submit
        // hook. The animation-loop NOP stops the game submitting during a load:
        // an entire load produces about FOUR eye pairs (measured -
        // "Eye scrub [close]: ok=8"). So Update() runs ~4 times no matter how
        // long the load takes, and a 62-second load logged NOTHING between the
        // NOP going on and the native CLOSE. That silence was mistaken for
        // "nothing is happening"; it actually means we have no observer at all.
        //
        // This thread is the observer. It is deliberately independent of every
        // engine callback, reads only plain global bytes through the existing
        // SEH-guarded helpers (never a game object - see the 2.1.7 regression),
        // and does no GPU work, so it cannot perturb what it measures.
        std::jthread m_loadHeartbeatThread;
        void StartLoadHeartbeat();

    public:
        // `[Main] bVRAnimationLoopNOP` (default 1). Disables ONLY the VR
        // animation-loop NOP, leaving overlays, tips, prefetch and every other
        // subsystem untouched, so load time can be attributed to it in a single
        // session.
        //
        // Why this exists: the heartbeat showed the loads that freeze the render
        // loop are exactly the slow ones. Loads at eyePairs/s=0.0 measured 12.6,
        // 14.3, 56.3 and 62.1 s; loads where the loop kept running (22.0 and
        // 12.9 pairs/s) measured 1.4-2.8 s. The NOP is what stops the loop, and
        // vanilla does not exhibit these durations. Correlation, not yet proof -
        // this switch is how it gets proven or refuted.
        void SetVRAnimationLoopNOPEnabled(bool a_enabled) {
            m_vrAnimationLoopNOPEnabled.store(
                a_enabled, std::memory_order_release);
        }

    private:
        std::atomic<bool> m_vrAnimationLoopNOPEnabled{ true };
        std::mutex m_textureStateMutex;
        std::atomic<std::uint64_t> m_backgroundGeneration{ 0 };
        std::atomic<std::uint64_t> m_d3dDeviceGeneration{ 0 };
        std::atomic<std::int64_t> m_bgRetryAfterTicks{ 0 };
        int m_lastIndex = -1;
        std::atomic<bool> m_backgroundsEnabled{ true };
        bool m_isVR = false;
        int m_overlayMode = 0; // 0=HMD, 1=World-locked, 2=Cinema, 3=Submit, 4=ClearRTV
        float m_overlayAlpha = 0.5f;
        std::atomic<bool> m_timingOnly{ false };
        std::mt19937 m_rng{ std::random_device{}() };
        std::chrono::steady_clock::time_point m_loadStartTime;
        std::chrono::steady_clock::time_point m_lastCloseTime;
        bool m_pendingPrepareNext = false;
        bool m_tipsOverlayAttached = false;
        // The vanilla LoadingMenu movie persists for the session. Hide only its
        // VaultTecLogo_mc while our mode-3 visual is active. Preserve and restore
        // its actual pre-load Boolean: the vanilla SWF defaults it on, while the
        // optional SpinnerOnly loose SWF intentionally defaults it off.
        bool m_spinnerRestorePending = false;
        bool m_spinnerOriginalVisible = true;
        // Set only when the current load opened with a usable compositor (and,
        // in VR, a usable overlay). Visual fallback is deliberately independent
        // of the VR animation-loop speed patch: modes 0/2 can still request that
        // patch when Submit is ready even if the custom overlay is unavailable.
        bool m_visualPipelineActiveThisLoad = false;
        bool m_tipsPipelineActiveThisLoad = false;
        // VR mode-3 animation-loop-NOP policy. March's 500 ms first-load / 100 ms
        // in-session value is the minimum freeze floor. A verified native tip
        // choice may keep Scaleform live to the bounded 600 ms capture deadline,
        // or 700 ms only when its GPU proof is already pending. Background-only
        // and unavailable/hopeless capture paths freeze at the March floor.
        // m_vrNOPRequested prevents duplicate requests.
        bool m_vrNOPWatchdogFired = false;
        bool m_vrNOPRequested = false;
        bool m_gameSessionLoaded = false;
        // March-compatible title-save handoff. The first successful save from
        // MainMenu keeps the complete custom presentation across a real
        // `player.moveto player` LoadingMenu transition. State is owned by
        // m_stateMutex; only the ready generations are cross-thread signals.
        enum class VRMarchPhase : std::uint8_t
        {
            kIdle,
            kWaitingForMoveToOpen,
            kMoveToLoadOpen
        };
        VRMarchPhase m_vrMarchPhase = VRMarchPhase::kIdle;
        std::uint64_t m_vrMarchSourceGeneration = 0;
        std::uint64_t m_vrMarchChainedGeneration = 0;
        bool m_vrMarchCommandIssued = false;

        // Final custom-presentation removal is executed by the direct Fallout
        // main-loop hook, matching March's game-frame-aligned one-step reveal.
        std::chrono::steady_clock::time_point m_vrPresentationReleaseAt{};
        std::uint64_t m_vrPresentationReleaseGeneration = 0;
        // March's 500/100/0 ms freeze delay is captured once at native OPEN.
        // Visual ownership is immediate; only the animation-loop NOP waits.
        std::int64_t m_vrMarchDelayMs = 0;
        // kPostLoadGame arrived during the current load (messaging thread sets,
        // game-thread CLOSE reads). Gates the post-close MainMenu suppression:
        // only a close that actually entered a game session may blank the
        // engine's lingering title-menu renders.
        std::atomic<bool> m_postLoadGameSeenThisLoad{ false };
        // Armed at a save-load CLOSE; cleared when the dwell test concludes.
        std::atomic<bool> m_vrPlayspaceRepairPending{ false };
        // Exact load epoch that owns the pending/queued repair. A queued F4SE
        // task can run after another transition; this tag prevents it from
        // mutating that newer load's playspace state.
        std::atomic<std::uint64_t> m_vrPlayspaceRepairGeneration{ 0 };
        // Nonzero once the hold is proven stuck; consumed by the post-Update
        // dispatcher, which captures this generation in the game-thread task.
        std::atomic<std::uint64_t> m_vrPlayspaceRepairReadyGeneration{ 0 };
        // Dwell tracking (m_stateMutex). Holds a composite signature of the
        // stuck state (counter in the low bits, detached in bit 16), not a
        // bare counter.
        std::int32_t m_vrPlayspaceLastCounter = -1;
        int m_vrPlayspaceStableSamples = 0;
        std::chrono::steady_clock::time_point m_vrPlayspaceStableSince{};
        // A successful custom save-load retains its existing artwork until
        // Fallout's post-close transition is healthy, or until one of the
        // bounded active/hard fallbacks wins. All fields except the resolved
        // generation are owned under m_stateMutex; the repair task publishes
        // resolution atomically from the game thread.
        std::atomic<std::uint64_t> m_vrPostCloseGateGeneration{ 0 };
        std::atomic<std::uint64_t> m_vrPostCloseResolvedGeneration{ 0 };
        int m_vrPostCloseHealthySamples = 0;
        bool m_vrPostCloseBackstopLogged = false;
        // Per-load one-shot diagnostics (m_stateMutex).
        bool m_vrStereoResumeLogged = false;
        bool m_vrPlayspaceReadFailLogged = false;
        int m_vrStereoProbesLogged = 0;
        // Opt-in, default OFF. Everything it enables runs on the OpenVR submit
        // thread inside the post-close window — the per-frame presentation
        // probe and the pre-scrub eye readback. 2.1.7 shipped that work on by
        // default (labelled "no behaviour change") and brought the end-of-load
        // flash back on a build 2.1.6 was confirmed clean of, so the default
        // path must stay free of it and only a deliberate diagnostic run pays
        // the cost.
        std::atomic<bool> m_vrPresentationProbeEnabled{ false };
        // The texture currently shown on the VR bg overlay — the plain bg DDS
        // until the bg+tips composite is built, then the composite. The pose
        // re-lock re-shows THIS (not the raw DDS) so it doesn't clobber the
        // composited tips back to a plain background.
        void* m_vrDisplayTex = nullptr;
        int m_bgPoseRelockFrames = 0;

        // March animation-loop NOP — requested at the OPEN-captured fixed
        // deadline, applied after right-eye Submit, restored at native CLOSE.
        std::uintptr_t m_loopAddress = 0;
        std::uint8_t m_originalLoopBytes[10] = {};
        bool m_originalBytesSaved = false;
        int m_loadCount = 0;
        std::atomic<std::uint64_t> m_loadGeneration{ 0 };
        std::atomic<bool> m_inLoadingScreen{ false };
        std::atomic<bool> m_loopNOPApplied{ false };
        // OG mode 3: animation-loop NOP deferred until the tips are captured
        // (AdvanceMovie frozen), then applied in Update() to reclaim speed.
        bool m_pendingLoopNOP = false;
        std::atomic<bool> m_mainMenuOpen{ false };

        // Menu events, F4SE messages, and Present/Submit callbacks are delivered
        // on different threads. All non-atomic lifecycle fields above are owned
        // under this mutex; expensive DDS IO uses the separate texture locks.
        mutable std::mutex m_stateMutex;
    };
}
