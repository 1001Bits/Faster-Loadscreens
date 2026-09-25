#include "PCH.h"

#include <ShlObj.h>

#include <SimpleIni.h>
#include <MinHook.h>
#include "LoadingScreenManager.h"
#include "VRCompositorHelper.h"
#include "PerformancePatches.h"
#include "PapyrusOptimizer.h"
#include "D3D11Compositor.h"
#include "GameSettingTweaks.h"
#include "CellWorldspaceGuard.h"
#include "DoorPrefetch.h"
#include "PreloadDiagnostics.h"
#include "SaveGamePrefetch.h"
#include "RuntimePolicy.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <mutex>
#include <type_traits>

using namespace VRLoadingScreens;

namespace
{
    // MCM config paths
    static const char* MCM_CONFIG_INI = "Data\\MCM\\Config\\FasterLoadscreens\\settings.ini";
    static const char* MCM_USER_INI = "Data\\MCM\\Settings\\FasterLoadscreens.ini";

    // Config
    struct Config
    {
        // 0=Black, 1=Native (no 3D model), 2=Background, 3=Background+Tips (default)
        int loadingScreenMode = 3;
        bool enableBackgroundImages = true;

        // Benchmark mode: 0=vanilla (timing only, no patches), 1=full plugin
        int benchmarkMode = 1;

        // Read-only preload tracing for controlled OG/VR experiments. This is a
        // hidden, startup-only diagnostic switch rather than a normal MCM option:
        // its exact-version hooks must be installed before any DoorPrefetch call
        // targets are resolved, and should stay out of clean timing runs.
        bool preloadDiagnostics = false;
        bool vrPresentationProbe = false;
        bool vrAnimationLoopNOP = true;

        // Read-only main-menu confirmation prefetch. This never starts Fallout's
        // load/deserialize path; it only warms the OS file cache while a
        // confirmation box is open and cancels before the real load begins.
        bool preloadSaveOnConfirm = true;

        // Fade / settle durations, mirrored into GameSettingTweaks::FadeConfig.
        // bVanillaFades=true restores all six values captured from the running
        // engine before this DLL's first custom write. The individual values are
        // used only by the opt-in custom mode.
        bool  vanillaFades            = true;
        float minSecondsForLoadFadeIn = 1.5f;
        float loadGameFadeSecs        = 1.0f;
        float fadeToBlackFadeSeconds  = 1.0f;
        float autoDoorFadeSecs        = 0.5f;
        float normalDoorFadeSecs      = 0.4f;
        float normalDoorFadeWait      = 0.01f;

        // Cross-worldspace exterior-gate proximity. One cell mirrors the useful
        // Skyrim experiment and, unlike Fallout's native linked-area exterior
        // branch, targets exterior-worldspace -> exterior-worldspace gates.
        // The Diamond City <-> Commonwealth gate is the primary diagnostic test.
        bool  prefetchExteriorGates = true;
        int   exteriorGateDistanceCells = 1;

        static constexpr float overlayAlpha = 1.0f;
        float backgroundWidth = 10.0f;
        static constexpr int overlayMode = 1;  // World-locked

        PerformanceConfig perfConfig;
        // perfConfig holds hardcoded defaults PLUS one-time HFPF/ENB/VR overrides
        // applied in OnGameDataReady. Those overrides must survive later Load()s, so
        // the hardcoded defaults are seeded only on the first Load() (this flag),
        // not re-stamped on every reload (which would clobber the overrides).
        bool perfConfigSeeded = false;

        static constexpr float budgetMaxFPS = 90.0f;
        static constexpr float updateBudgetBase = 1.2f;

        [[nodiscard]] bool Load()
        {
            CSimpleIniA ini;
            ini.SetUnicode();

            if (ini.LoadFile(MCM_CONFIG_INI) < 0) {
                logger::error(
                    "Config: could not read base settings {}; retaining prior snapshot",
                    MCM_CONFIG_INI);
                return false;
            }
            const auto userResult = ini.LoadFile(MCM_USER_INI);
            if (userResult >= 0) {
                logger::info("Loaded MCM user overrides from {}", MCM_USER_INI);
            } else {
                std::error_code existsError;
                const bool userExists = std::filesystem::exists(
                    MCM_USER_INI, existsError);
                if (userExists || existsError) {
                    logger::error(
                        "Config: could not read existing MCM user settings {}; "
                        "retaining prior snapshot",
                        MCM_USER_INI);
                    return false;
                }
            }

            loadingScreenMode = static_cast<int>(ini.GetLongValue(
                "Main", "iLoadingScreenMode", 3));
            if (loadingScreenMode < 0) loadingScreenMode = 0;
            if (loadingScreenMode > 3) loadingScreenMode = 3;
            benchmarkMode = static_cast<int>(ini.GetLongValue(
                "Main", "iBenchmarkMode", 1));
            preloadDiagnostics = ini.GetBoolValue(
                "Diagnostics", "bPreloadDiagnostics", false);
            // Off by default and deliberately so: this adds work to the OpenVR
            // submit thread across the post-close window, which is precisely
            // what 2.1.7 did accidentally and what reintroduced the end-of-load
            // flash. Enable only to diagnose that artifact, never for play.
            vrPresentationProbe = ini.GetBoolValue(
                "Diagnostics", "bVRPresentationProbe", false);
            // Default ON: this is the mod's main VR loading-speed patch. Set to
            // 0 to attribute load time to it without disabling anything else.
            vrAnimationLoopNOP = ini.GetBoolValue(
                "Main", "bVRAnimationLoopNOP", true);
            preloadSaveOnConfirm = ini.GetBoolValue(
                "Main", "bPreloadSaveOnConfirm", true);
            // Fade/settle durations. Clamp to a sane [0, 10]s: these values are
            // written straight into engine settings, and a negative or absurd value
            // has undefined engine behaviour.
            auto fadeClamp = [](double v, float fallback) {
                if (!std::isfinite(v)) return fallback;
                return static_cast<float>(v < 0.0 ? 0.0 : (v > 10.0 ? 10.0 : v));
            };
            vanillaFades = ini.GetBoolValue("Main", "bVanillaFades", true);
            minSecondsForLoadFadeIn = fadeClamp(ini.GetDoubleValue(
                "Main", "fMinSecondsForLoadFadeIn", 1.5), 1.5f);
            loadGameFadeSecs = fadeClamp(ini.GetDoubleValue(
                "Main", "fLoadGameFadeSecs", 1.0), 1.0f);
            fadeToBlackFadeSeconds = fadeClamp(ini.GetDoubleValue(
                "Main", "fFadeToBlackFadeSeconds", 1.0), 1.0f);
            autoDoorFadeSecs = fadeClamp(ini.GetDoubleValue(
                "Main", "fAutoDoorFadeSecs", 0.5), 0.5f);
            normalDoorFadeSecs = fadeClamp(ini.GetDoubleValue(
                "Main", "fNormalDoorFadeSecs", 0.4), 0.4f);
            normalDoorFadeWait = fadeClamp(ini.GetDoubleValue(
                "Main", "fNormalDoorFadeWait", 0.01), 0.01f);
            // The production path is exterior-gate proximity only. It scans
            // loaded references without a Havok ray. Legacy crosshair,
            // extended-range, interior-preload, menu-cell preload, and native
            // uGrids selector keys are deliberately not read.
            prefetchExteriorGates = ini.GetBoolValue(
                "DoorPrefetch", "bPreloadExteriorGates", true);
            long gateCellsIdx = ini.GetLongValue(
                "DoorPrefetch", "iExteriorGateDistanceCells", 0);
            if (gateCellsIdx < 0) gateCellsIdx = 0;
            if (gateCellsIdx > 3) gateCellsIdx = 3;
            exteriorGateDistanceCells = static_cast<int>(gateCellsIdx) + 1;

            // 0=Black screen, 1=Native (no 3D), 2=Background, 3=Background + Tips.
            // Background overlay shown for modes 2 and 3 only.
            enableBackgroundImages = (loadingScreenMode >= 2);

            // Background overlay width (VR). Clamp to a positive sane range — 0 or
            // negative would collapse/invert the overlay quad.
            backgroundWidth = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fBackgroundWidth", 10.0));
            if (!std::isfinite(backgroundWidth)) backgroundWidth = 10.0f;
            if (backgroundWidth < 0.1f) backgroundWidth = 0.1f;
            if (backgroundWidth > 100.0f) backgroundWidth = 100.0f;

            // Seed the hardcoded perfConfig defaults ONCE. OnGameDataReady applies
            // HFPF/ENB/VR overrides on top of these after the first Load(); reloads
            // must not re-stamp the defaults or they'd wipe those overrides.
            if (!perfConfigSeeded) {
                perfConfigSeeded = true;
                perfConfig.untieSpeedFromFPS = true;
                perfConfig.disableiFPSClamp = true;
                // Keep Bethesda's destination-state routing intact. The old
                // "DisableBlackLoadingScreens" branch flip is presentation-only
                // (not load I/O), and has caused unsafe/infinite transitions in
                // VR. Our background/compositor path does not require it.
                perfConfig.disableBlackLoadingScreens = false;
                // HFPF's measured fast path is SyncInterval=0 during LoadingMenu,
                // bounded by a separate 350-FPS Present limiter. Mirror that policy
                // on every runtime: the limiter prevents an unbounded render loop
                // from starving loader threads, while the menu-close path restores
                // the game's original interval. Gameplay therefore remains capped.
                // This is independent of the animation-loop NOP; the cap also
                // protects native/tips modes before their render loop is frozen.
                perfConfig.disableVSyncWhileLoading = true;
                perfConfig.disable3DModel = true;
                // The old FixCPUThreads-equivalent rewrote ten live bytes in
                // PresentThread. It cannot be toggled atomically while that
                // worker executes, so it is intentionally unavailable.
                perfConfig.yieldCPUDuringLoading = false;
                // KEEP OFF. HFPF's OneThreadWhileLoading is NEW-GAME-start only (a
                // quest-bug workaround, e.g. "Emogene Takes a Lover") — NOT a per-load
                // speed feature. Our hook applies the 1-core affinity on EVERY
                // LoadingMenu open, which pins FO4's multi-threaded loader to a single
                // core → ~20x slower (measured: a save load ballooned to 125.81s).
                // Verified dead end, do not re-enable here.
                perfConfig.oneThreadWhileLoading = false;
            }

            logger::info("Config: mode={} benchmark={} (bg={})",
                loadingScreenMode, benchmarkMode, enableBackgroundImages);
            return true;
        }

        void PublishSettingBindings() const
        {
            GameSettingTweaks::SetFades({ vanillaFades,
                minSecondsForLoadFadeIn, loadGameFadeSecs,
                fadeToBlackFadeSeconds, autoDoorFadeSecs,
                normalDoorFadeSecs, normalDoorFadeWait });
        }
    };

    Config g_config;
    bool g_isVR = false;
    std::atomic<bool> g_gameDataReady{ false };
    // Immutable after kGameDataReady. In this mode the DLL is only an F4SE
    // message + native LoadingMenu timestamp observer; it installs no engine,
    // renderer, compositor, OpenVR, preload, or crash-guard hook.
    std::atomic<bool> s_passiveMeasurementOnly{ false };
    std::atomic<bool> g_gameSessionLoaded{ false };
    // Menu/session events may arrive in either order when returning to the main
    // menu. Keep an independent latch so a late kPostLoadGame cannot resume the
    // prediction poller after MainMenu has already opened.
    static std::atomic<bool> s_mainMenuOpen{ false };
    // Covers the legacy kPreLoadGame -> LoadingMenu OPEN gap. The native menu
    // flags are not raised yet in that interval, but config polling/file IO must
    // already be excluded from the loader's critical path.
    static std::atomic<bool> s_flatLoadEntryArmed{ false };
    static std::atomic<bool> s_flatNativeLoadingMenuOpen{ false };
    static std::atomic<bool> s_flatNativeCloseInProgress{ false };
    // Serialize OG/VR LoadingMenu OPEN/CLOSE against late F4SE session messages.
    // A success arriving just after CLOSE must publish now, not wait for a
    // nonexistent next menu; one arriving during OPEN is consumed by that CLOSE.
    static std::recursive_mutex s_flatLifecycleMutex;
    std::mutex s_configMutex;
    std::mutex s_initMutex;
    // Patch/hook topology is selected once at kGameDataReady. A disk edit may
    // change this value only for the next process; accepting it live would make
    // a timing baseline grow hooks mid-session or activate uninstalled systems.
    int s_startupBenchmarkMode = -1;

    [[nodiscard]] static Policy::RuntimeVersionParts VersionParts(
        const REL::Version& a_version) noexcept
    {
        return {
            a_version[0], a_version[1], a_version[2], a_version[3]
        };
    }

    [[nodiscard]] static Policy::ModuleRuntimeKind ModuleRuntimeKind() noexcept
    {
        switch (REL::Module::GetRuntime()) {
        case REL::Module::Runtime::F4:
            return Policy::ModuleRuntimeKind::kFallout4;
        case REL::Module::Runtime::NG:
            return Policy::ModuleRuntimeKind::kFallout4NG;
        case REL::Module::Runtime::VR:
            return Policy::ModuleRuntimeKind::kFallout4VR;
        default:
            return Policy::ModuleRuntimeKind::kUnknown;
        }
    }

    [[nodiscard]] static bool IsMainExecutable(
        const REL::Module& a_module) noexcept
    {
        const auto mainHandle = REX::W32::GetModuleHandleW(nullptr);
        if (!mainHandle || a_module.pointer() != mainHandle) {
            return false;
        }

        const auto executableKind =
            Policy::ClassifyExecutable(a_module.filename());
        const wchar_t* expectedName = nullptr;
        if (executableKind == Policy::ExecutableKind::kFallout4VR) {
            expectedName = L"Fallout4VR.exe";
        } else if (executableKind == Policy::ExecutableKind::kFallout4) {
            expectedName = L"Fallout4.exe";
        }
        return expectedName &&
            REX::W32::GetModuleHandleW(expectedName) == mainHandle;
    }

    [[nodiscard]] static bool IsSupportedF4SEHost(
        const F4SE::QueryInterface& a_f4se) noexcept
    {
        const auto& module = REL::Module::get();
        return Policy::IsSupportedF4SEHost(
            VersionParts(a_f4se.RuntimeVersion()),
            VersionParts(module.version()),
            VersionParts(a_f4se.F4SEVersion()),
            Policy::ClassifyExecutable(module.filename()),
            ModuleRuntimeKind(),
            IsMainExecutable(module));
    }

    [[nodiscard]] static bool RequiresLegacyF4SEVRInterfaceShim(
        const F4SE::QueryInterface& a_f4se) noexcept
    {
        const auto& module = REL::Module::get();
        return Policy::RequiresLegacyF4SEVRInterfaceShim(
            VersionParts(module.version()),
            VersionParts(a_f4se.F4SEVersion()),
            Policy::ClassifyExecutable(module.filename()),
            ModuleRuntimeKind(),
            IsMainExecutable(module));
    }

    static Config GetConfigSnapshot()
    {
        std::lock_guard lock(s_configMutex);
        return g_config;
    }

    [[nodiscard]] static bool ReloadConfigSnapshot(Config& a_config)
    {
        std::lock_guard lock(s_configMutex);
        Config candidate = g_config;
        if (!candidate.Load()) {
            a_config = g_config;
            return false;
        }
        const auto reloadedBenchmarkMode = candidate.benchmarkMode;
        candidate.benchmarkMode = Policy::KeepStartupBenchmarkMode(
            s_startupBenchmarkMode, reloadedBenchmarkMode);
        if (candidate.benchmarkMode != reloadedBenchmarkMode) {
            logger::warn(
                "Config: iBenchmarkMode is startup-only; keeping {} until restart",
                candidate.benchmarkMode);
        }
        g_config = candidate;
        g_config.PublishSettingBindings();
        a_config = g_config;
        return true;
    }

    // Forward declarations
    static void ApplyDoorPrefetchConfig(const Config& config);
    static bool RefreshConfigFromDisk(
        const char* a_reason, bool a_resumePostLoadSystems = true);
    static void ResumePostLoadSystemsAtNativeClose();
    static void PublishOrDeferLegacySessionLoaded(const char* a_source);
    static void OnEngineSessionLost() noexcept;
    static void QueueNGEndLoadTweaks();
    static void NGFinishPresentationGapIfNeeded();
    static void RestoreLoadBudgetsWithRetry(
        std::uint64_t a_generation,
        unsigned a_attemptsRemaining = 4);
    static void RunNGEndLoadTweaks(
        std::uint64_t a_generation,
        unsigned a_attemptsRemaining);
    using VRMainLoopTarget = void (*)(std::uint64_t);
    static constexpr std::uintptr_t kVRMainLoopCallsite = 0xD8405E;
    static VRMainLoopTarget s_vrMainLoopOriginal = nullptr;
    static bool s_vrMainLoopHookInstalled = false;

    static void HookedVRMarchMainLoop(std::uint64_t a_context)
    {
        // March's framework invoked the plugin before the target that already
        // occupied this callsite. Preserve that chain order so a self-MoveTo
        // begins from the same game-thread boundary as v1.0.
        if (g_gameDataReady.load(std::memory_order_acquire)) {
            try {
                VRCompositorHelper::UpdateLastKnownPose();
                auto& manager = LoadingScreenManager::GetSingleton();
                if (manager.IsInLoadingScreen()) {
                    // v1.0 re-published the stored world-locked transform on
                    // every direct main-loop callback while the menu was open.
                    VRCompositorHelper::UpdateBackgroundOverlay();
                }
                manager.TickVRMarchMainLoop();
            } catch (...) {
                logger::error(
                    "VR March main-loop callback failed; chained target will "
                    "still run");
            }
        }
        if (s_vrMainLoopOriginal) {
            s_vrMainLoopOriginal(a_context);
        }
    }

    static bool InstallVRMarchMainLoopHook()
    {
        if (s_vrMainLoopHookInstalled) {
            return true;
        }
        if (!g_isVR) {
            return false;
        }

        REL::Relocation<std::uintptr_t> callsite{
            REL::Offset(kVRMainLoopCallsite)
        };
        const auto address = callsite.address();
        if (!address || *reinterpret_cast<const std::uint8_t*>(address) != 0xE8) {
            logger::error(
                "VR March main-loop hook refused: expected CALL at {:x}",
                address);
            return false;
        }

        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            reinterpret_cast<const void*>(address + 1),
            sizeof(displacement));
        const auto previousTarget = address + 5 + displacement;
        s_vrMainLoopOriginal =
            reinterpret_cast<VRMainLoopTarget>(previousTarget);

        auto& trampoline = F4SE::GetTrampoline();
        const auto chainedTarget = trampoline.write_call<5>(
            address, &HookedVRMarchMainLoop);
        if (chainedTarget != 0) {
            s_vrMainLoopOriginal =
                reinterpret_cast<VRMainLoopTarget>(chainedTarget);
        }
        s_vrMainLoopHookInstalled = true;
        logger::info(
            "VR March main-loop hook installed at {:x}; chained target={:x}",
            address,
            reinterpret_cast<std::uintptr_t>(s_vrMainLoopOriginal));
        return true;
    }

    static bool s_hfpfDetected = false;
    static bool s_enbDetected = false;
    // F4SE messaging fires from worker threads; the loading/render threads read
    // these flags. Promote them to atomic so reads/writes can't tear.
    static std::atomic<bool> s_pendingGameSessionLoaded{ false };
    static std::atomic<bool> s_vrTimerPatchesPending{ false };
    static std::atomic<bool> s_vrTimerPatchesApplied{ false };
    static std::atomic<std::uint64_t> s_flatBudgetGeneration{ 0 };
    static std::atomic<bool> s_flatBudgetLoadActive{ false };
    // Cross-thread timestamps are stored as steady_clock tick counts in atomics
    // (a time_point isn't trivially atomic). The tick is written BEFORE the flag
    // that gates its read is published, so the reader can never observe a fresh
    // flag paired with a stale timestamp. Reconstruct with steady_clock::duration.
    static std::atomic<std::int64_t> s_ngLastPostLoadTimeTicks{ 0 };
    static std::atomic<bool> s_ngLoadingActive{ false };
    // kPostLoadGame arrived (save-load end signal). The messaging thread ONLY sets
    // this + the timestamp; the render tick (TickNGDeferred) is the sole owner of the
    // enable/close decisions. This removes the cross-thread check-then-act race on
    // s_ngEnablePending (kPostLoadGame's exchange could previously steal the flag
    // mid-decision on the render thread, enabling the compositor with no close armed).
    static std::atomic<bool> s_ngPostLoadArrived{ false };
    // kPreLoadGame publishes a sticky save classification which survives the
    // subordinate LoadInterior call. A completed native Show consumes it and
    // publishes the classification used by the next Present. Presentation mode
    // is still the configured user mode; the classification describes only the
    // native transition/content path.
    static std::atomic<bool> s_ngSaveClassificationArmed{ false };
    static std::atomic<bool> s_ngEnableFromSaveLoad{ false };
    static std::atomic<bool> s_ngIsInterior{ false };
    // A new engine load can enter after the old native Hide but before Present
    // has closed the old custom presentation. Keep that entry distinct from the
    // presentation it supersedes: an intervening Present may close old pixels,
    // but it must not unwind the new generation's budgets/transition barrier or
    // erase the classification consumed by its eventual native Show.
    static std::atomic<bool> s_ngNextLoadEntryArmed{ false };
    static std::atomic<bool> s_ngNextLoadBodyReturned{ false };
    static std::atomic<bool> s_ngNextLoadWaitsForPost{ false };
    // CloseForChainedLoadingMenu deliberately retains the DDS-work barrier. If
    // the armed load ends without a Show, its outcome path releases that barrier
    // through FinishChainedLoadingMenuGap.
    static std::atomic<bool> s_ngPresentationGapClosed{ false };
    static std::atomic<bool> s_menuWatcherRegistered{ false };
    // NG deferred-close. kPostLoadGame fires before the LoadingMenu visually
    // finishes closing; if we disable the compositor immediately, the native
    // loading screen (bg + 3D model) flashes for ~1-2s before final close.
    // Set true on kPostLoadGame, fired by the per-frame tick after a delay.
    static std::atomic<bool> s_ngClosePending{ false };
    // ~2s wasn't enough — user reported "the last second of loading our bg
    // disappears and the games vanilla background is shown before loading
    // screen closes". Bumped to 3.5s to cover the engine's post-load fade
    // animation. The TES::HideLoadingMenu hook gives a precise close signal that
    // usually fires long before this fallback timer.
    static constexpr std::chrono::milliseconds kNGCloseDelay{ 3500 };
    // Watchdog for boundary-fallback states only. An exact native-visible menu is
    // exempt and remains custom for however long TES keeps it open.
    static constexpr std::chrono::milliseconds kNGWatchdogTimeout{ 120000 };
    // Time the compositor was last enabled (for the watchdog above).
    static std::atomic<std::int64_t> s_ngEnableTimeTicks{ 0 };

    // NG/AE visual enable is armed by the load entry signals but consumed only
    // after verified TES::ShowLoadingMenu returns. Invalid-save popup pairs and
    // menu-less/scripted loads never show that native menu and therefore never
    // display our background. If the exact-version boundary hooks are unavailable,
    // custom visuals stay off rather than guessing from a timer.
    static std::atomic<std::int64_t> s_ngPreLoadTimeTicks{ 0 };
    static std::atomic<bool> s_ngEnablePending{ false };
    static std::atomic<bool> s_ngShowMenuSignal{ false };
    static std::atomic<bool> s_ngChainedShowSignal{ false };
    // Initial native-selection serial visible immediately before the matching
    // TES::ShowLoadingMenu call. Tick requires a newer coherent publication,
    // so a chained Show can never route from the prior menu's model/tip choice.
    static std::atomic<std::uint64_t> s_ngShowSelectionBaselineSerial{ 0 };
    static std::atomic<bool> s_ngNativeMenuVisible{ false };
    static std::atomic<bool> s_showLoadingMenuHookInstalled{ false };
    static std::atomic<bool> s_hideLoadingMenuHookInstalled{ false };
    static std::atomic<bool> s_ngBoundaryHooksPoisoned{ false };
    static std::atomic<bool> s_ngPresentTickReady{ false };
    // Boundary hooks publish an in-flight count under this mutex, release it while
    // Bethesda runs, then publish the result under the mutex. Present refuses to
    // make lifecycle decisions while the count is nonzero. That avoids both the
    // old original-call deadlock and a stale show/hide race.
    static std::recursive_mutex s_ngBoundaryMutex;
    static std::uint32_t s_ngBoundaryCallsInFlight = 0;
    // BeginLoad runs synchronously on the game thread before an NG load enters
    // the engine. Close/cancel decisions happen later on the render thread, so
    // this flag transfers ownership of the one required EndLoad task and keeps
    // duplicate load signals from restoring the temporary budgets early.
    static std::atomic<bool> s_ngTweaksActive{ false };
    static std::atomic<std::uint64_t> s_ngTweaksGeneration{ 0 };
    static std::atomic<bool> s_ngConfigRefreshQueued{ false };
    static std::atomic<std::uint64_t> s_persistentTweaksGeneration{ 0 };

    struct ConfigFileStamp
    {
        bool exists{ false };
        bool readable{ true };
        std::uintmax_t size{ 0 };
        std::filesystem::file_time_type::rep writeTime{ 0 };

        bool operator==(const ConfigFileStamp&) const = default;
    };

    static std::mutex s_liveConfigReloadMutex;
    static Policy::ConfigReloadCoordinator s_liveConfigReload;
    static std::mutex s_liveConfigStampMutex;
    static ConfigFileStamp s_liveBaseConfigStamp;
    static ConfigFileStamp s_liveUserConfigStamp;
    static std::uint64_t s_liveConfigGeneration = 0;
    static std::atomic<bool> s_liveConfigDispatchReady{ false };
    // Declare the worker last so its destructor requests stop and joins before
    // any state touched by the loop is destroyed (reverse static destruction).
    static std::jthread s_liveConfigWatchThread;

    static ConfigFileStamp ReadConfigFileStamp(const char* a_path) noexcept
    {
        ConfigFileStamp stamp;
        std::error_code ec;
        stamp.exists = std::filesystem::exists(a_path, ec);
        if (ec) {
            stamp.readable = false;
            return stamp;
        }
        if (!stamp.exists) {
            return stamp;
        }
        stamp.size = std::filesystem::file_size(a_path, ec);
        if (ec) {
            stamp.readable = false;
            return stamp;
        }
        const auto writeTime = std::filesystem::last_write_time(a_path, ec);
        if (ec) {
            stamp.readable = false;
            return stamp;
        }
        stamp.writeTime = writeTime.time_since_epoch().count();
        return stamp;
    }

    static bool ConfigRefreshBlockedByLoad() noexcept
    {
        return
            s_flatLoadEntryArmed.load(std::memory_order_acquire) ||
            s_flatNativeLoadingMenuOpen.load(std::memory_order_acquire) ||
            s_flatNativeCloseInProgress.load(std::memory_order_acquire) ||
            s_ngNativeMenuVisible.load(std::memory_order_acquire) ||
            s_ngLoadingActive.load(std::memory_order_acquire) ||
            s_ngEnablePending.load(std::memory_order_acquire) ||
            s_ngClosePending.load(std::memory_order_acquire) ||
            s_ngNextLoadEntryArmed.load(std::memory_order_acquire) ||
            s_ngPresentationGapClosed.load(std::memory_order_acquire);
    }

    static std::int64_t ConfigWatcherNowMs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static bool ObserveConfigFileChange()
    {
        const auto nextBase = ReadConfigFileStamp(MCM_CONFIG_INI);
        const auto nextUser = ReadConfigFileStamp(MCM_USER_INI);
        bool changed = false;
        {
            std::lock_guard stampLock(s_liveConfigStampMutex);
            changed = nextBase != s_liveBaseConfigStamp ||
                nextUser != s_liveUserConfigStamp;
            if (changed) {
                s_liveBaseConfigStamp = nextBase;
                s_liveUserConfigStamp = nextUser;
            }
        }
        if (!changed) {
            return false;
        }

        s_liveConfigDispatchReady.store(false, std::memory_order_release);
        {
            std::lock_guard reloadLock(s_liveConfigReloadMutex);
            // Raw changes become pending immediately. Normal dispatch still
            // waits for the stable-write debounce; a load entry can synchronously
            // consume this generation so the very next load never sees stale MCM.
            (void)s_liveConfigReload.Observe(
                ++s_liveConfigGeneration, true);
        }
        return true;
    }

    static void TryQueueLiveConfigRefresh()
    {
        bool shouldQueue = false;
        {
            std::lock_guard lock(s_liveConfigReloadMutex);
            shouldQueue = s_liveConfigReload.TryQueue(
                ConfigRefreshBlockedByLoad());
        }
        if (!shouldQueue) {
            return;
        }

        auto* tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            std::lock_guard lock(s_liveConfigReloadMutex);
            (void)s_liveConfigReload.FinishTask(false);
            return;
        }

        tasks->AddTask([]() {
            {
                std::lock_guard lock(s_liveConfigReloadMutex);
                if (!s_liveConfigReload.IsPending()) {
                    (void)s_liveConfigReload.FinishTask(true);
                    return;
                }
            }
            if (ConfigRefreshBlockedByLoad() ||
                LoadingScreenManager::GetSingleton().IsInLoadingScreen()) {
                std::lock_guard lock(s_liveConfigReloadMutex);
                (void)s_liveConfigReload.FinishTask(false);
                return;
            }

            bool refreshed = false;
            try {
                refreshed = RefreshConfigFromDisk("mcm-settings-change");
                if (refreshed) {
                    // This task already owns the game thread. Publish the fade
                    // values before acknowledging the generation so an imminent
                    // load cannot overtake the separate queued apply task.
                    GameSettingTweaks::Apply();
                }
            } catch (const std::exception& e) {
                refreshed = false;
                logger::error("Live MCM config refresh failed: {}", e.what());
            } catch (...) {
                refreshed = false;
                logger::error("Live MCM config refresh failed");
            }

            bool stillPending = false;
            {
                std::lock_guard lock(s_liveConfigReloadMutex);
                stillPending = s_liveConfigReload.FinishTask(refreshed);
            }
            if (refreshed && !stillPending) {
                s_liveConfigDispatchReady.store(
                    false, std::memory_order_release);
            }
        });
    }

    static void RefreshLiveConfigBeforeLoadIfDirty(const char* a_reason)
    {
        // Never stat/parse INIs after Bethesda has raised a native LoadingMenu
        // but before custom presentation takes ownership.
        if (ConfigRefreshBlockedByLoad()) {
            return;
        }
        (void)ObserveConfigFileChange();

        std::uint64_t generation = 0;
        {
            std::lock_guard lock(s_liveConfigReloadMutex);
            if (!s_liveConfigReload.IsPending()) {
                return;
            }
            generation = s_liveConfigReload.Generation();
        }
        if (ConfigRefreshBlockedByLoad()) {
            return;
        }

        // Never wait at a native visual boundary: delaying custom ownership can
        // expose Fallout's title/3D screen. The INI write itself is synchronous;
        // a rare incomplete read is rejected transactionally and retried later.
        bool refreshed = false;
        try {
            refreshed = RefreshConfigFromDisk(a_reason, false);
        } catch (const std::exception& e) {
            logger::error(
                "Pre-load MCM config refresh failed: {}", e.what());
        } catch (...) {
            logger::error("Pre-load MCM config refresh failed");
        }
        if (refreshed) {
            // If the writer replaced the file while the transactional parse ran,
            // publish a newer dirty generation before acknowledging this one.
            (void)ObserveConfigFileChange();
            // This change-only fallback runs before load state/timing is armed,
            // so persistent fade values must be written now rather than left in
            // the normal queued task that the imminent load would defer.
            GameSettingTweaks::Apply();
            bool stillPending = false;
            {
                std::lock_guard lock(s_liveConfigReloadMutex);
                s_liveConfigReload.Acknowledge(generation);
                stillPending = s_liveConfigReload.IsPending();
            }
            if (!stillPending) {
                s_liveConfigDispatchReady.store(
                    false, std::memory_order_release);
            }
            logger::info(
                "MCM settings applied before load entry (generation={})",
                generation);
        }
    }

    static void StartLiveConfigWatcher()
    {
        if (s_liveConfigWatchThread.joinable()) {
            return;
        }

        {
            std::lock_guard stampLock(s_liveConfigStampMutex);
            s_liveBaseConfigStamp = ReadConfigFileStamp(MCM_CONFIG_INI);
            s_liveUserConfigStamp = ReadConfigFileStamp(MCM_USER_INI);
        }
        {
            std::lock_guard lock(s_liveConfigReloadMutex);
            s_liveConfigReload.Reset();
            s_liveConfigGeneration = 0;
        }
        s_liveConfigDispatchReady.store(false, std::memory_order_release);

        s_liveConfigWatchThread = std::jthread(
            [](std::stop_token a_stopToken) {
                Policy::ConfigChangeDebouncer debounce;
                std::uint64_t debounceGeneration = 0;
                std::int64_t lastQueueAttemptMs = 0;
                constexpr auto pollInterval = std::chrono::milliseconds(50);
                constexpr std::int64_t settleMs = 150;
                constexpr std::int64_t retryMs = 250;

                while (!a_stopToken.stop_requested()) {
                    std::this_thread::sleep_for(pollInterval);
                    const auto nowMs = ConfigWatcherNowMs();

                    // Timed loads own storage bandwidth. Changes made during a
                    // load are discovered by the first post-close poll, while
                    // the entry fallback already covered the preceding race.
                    if (ConfigRefreshBlockedByLoad()) {
                        continue;
                    }

                    (void)ObserveConfigFileChange();
                    std::uint64_t currentGeneration = 0;
                    {
                        std::lock_guard lock(s_liveConfigReloadMutex);
                        currentGeneration = s_liveConfigReload.Generation();
                    }
                    if (currentGeneration != debounceGeneration) {
                        debounceGeneration = currentGeneration;
                        debounce.ObserveChange(nowMs);
                    }

                    if (debounce.ConsumeIfStable(nowMs, settleMs)) {
                        bool pending = false;
                        {
                            std::lock_guard lock(s_liveConfigReloadMutex);
                            pending = s_liveConfigReload.IsPending();
                        }
                        s_liveConfigDispatchReady.store(
                            pending, std::memory_order_release);
                        const bool loadActive = ConfigRefreshBlockedByLoad();
                        if (pending) {
                            logger::info(
                                "MCM settings change detected; live refresh {}",
                                loadActive ?
                                    "deferred across active load" : "queued");
                        }
                    }
                    if (s_liveConfigDispatchReady.load(
                            std::memory_order_acquire) &&
                        nowMs - lastQueueAttemptMs >= retryMs) {
                        lastQueueAttemptMs = nowMs;
                        TryQueueLiveConfigRefresh();
                    }
                }
            });
        logger::info(
            "Live MCM config watcher started (50 ms poll, 150 ms stable-write debounce)");
    }

    static void QueueNGConfigRefresh(const char* a_reason)
    {
        if (s_ngConfigRefreshQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([a_reason]() {
                // A chained load may have started before this game-thread task
                // ran. Never introduce INI/MCM disk IO into that newer load;
                // its own close will enqueue the next refresh.
                if (s_ngLoadingActive.load(std::memory_order_acquire) ||
                    s_ngEnablePending.load(std::memory_order_acquire) ||
                    s_ngNativeMenuVisible.load(std::memory_order_acquire) ||
                    s_ngNextLoadEntryArmed.load(std::memory_order_acquire) ||
                    s_ngPresentationGapClosed.load(
                        std::memory_order_acquire)) {
                    s_ngConfigRefreshQueued.store(
                        false, std::memory_order_release);
                    logger::info(
                        "NG: config refresh deferred across chained load");
                    return;
                }
                try {
                    RefreshConfigFromDisk(a_reason);
                } catch (const std::exception& e) {
                    logger::error(
                        "NG: config refresh failed: {}", e.what());
                } catch (...) {
                    logger::error("NG: config refresh failed");
                }
                s_ngConfigRefreshQueued.store(
                    false, std::memory_order_release);
            });
        } else {
            s_ngConfigRefreshQueued.store(false, std::memory_order_release);
            logger::warn(
                "NG: config refresh skipped (task interface unavailable)");
        }
    }

    static bool NGExactBoundaryHooksReady()
    {
        return
            !s_ngBoundaryHooksPoisoned.load(std::memory_order_acquire) &&
            s_showLoadingMenuHookInstalled.load(std::memory_order_acquire) &&
            s_hideLoadingMenuHookInstalled.load(std::memory_order_acquire);
    }

    static bool NGArmPendingOnGameThread(
        bool fromSaveLoad, bool fromNativeShow = false)
    {
        Config config = GetConfigSnapshot();
        if (config.benchmarkMode == 0) {
            // The exact entry hooks remain installed to record native timings,
            // but timing-only mode must not enter any plugin subsystem.
            return false;
        }
        RefreshLiveConfigBeforeLoadIfDirty("ng-load-entry");
        config = GetConfigSnapshot();
        // Serialize an engine-entry publication with Present's old-close
        // decision. Without this transaction Present could restore the newly
        // raised generation between BeginLoad and the next-entry latch.
        std::lock_guard<std::recursive_mutex> boundaryLock(
            s_ngBoundaryMutex);
        if (fromSaveLoad) {
            // A save replaces the provenance domain even when a prior custom
            // presentation is still pending. Publish the transition barrier,
            // drain any producer, and clear evidence as one transaction before
            // any early-return path below can retain the old generation.
            DoorPrefetch::FlushQueuedLoads(true);
            s_ngSaveClassificationArmed.store(
                true, std::memory_order_release);
            if (s_ngNextLoadEntryArmed.load(
                    std::memory_order_acquire)) {
                s_ngNextLoadWaitsForPost.store(
                    true, std::memory_order_release);
            }
        }
        PapyrusOptimizer::GetSingleton().SetLoading(true);

        // A door hook can follow kPreLoadGame for the same save. The already-
        // pending arm owns that load, so do not begin twice.
        if (s_ngEnablePending.load(std::memory_order_relaxed)) {
            return true;
        }
        const bool alreadyActive = s_ngLoadingActive.load(std::memory_order_relaxed);

        // The old custom presentation may already have been closed at its exact
        // native Hide while this newer engine load remains in progress. A
        // verified Show transfers that preserved entry into the normal pending
        // visual state; duplicate kPre/subordinate LoadInterior signals must not
        // create another budget generation.
        if (s_ngNextLoadEntryArmed.load(std::memory_order_acquire)) {
            if (!alreadyActive && fromNativeShow) {
                s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
                s_ngEnablePending.store(true, std::memory_order_release);
                s_ngNextLoadEntryArmed.store(
                    false, std::memory_order_release);
                s_ngNextLoadBodyReturned.store(
                    false, std::memory_order_relaxed);
                s_ngNextLoadWaitsForPost.store(
                    false, std::memory_order_relaxed);
            }
            return true;
        }

        s_ngTweaksGeneration.fetch_add(
            1, std::memory_order_acq_rel);

        // Reassert a door/native-Show barrier inside the same lifecycle
        // transaction. Save loads already published and cleared theirs above,
        // before every possible early return.
        if (!fromSaveLoad) {
            DoorPrefetch::FlushQueuedLoads();
        }

        // Never perform filesystem/config parsing inside a timed load. Startup
        // and the prior session-close publish the next immutable snapshot.
        ApplyDoorPrefetchConfig(config);
        if (!s_ngTweaksActive.load(std::memory_order_acquire)) {
            GameSettingTweaks::BeginLoad();
            s_ngTweaksActive.store(true, std::memory_order_release);
        }

        // The Present callback is the sole compositor-lifecycle owner on AE.
        // If it could not be installed, retain native visuals and let the
        // synchronous load-return/kPost paths unwind the temporary settings.
        if (!s_ngPresentTickReady.load(std::memory_order_acquire)) {
            s_ngEnablePending.store(false, std::memory_order_relaxed);
            s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
            s_ngClosePending.store(false, std::memory_order_relaxed);
            s_ngNextLoadEntryArmed.store(
                false, std::memory_order_relaxed);
            s_ngNextLoadBodyReturned.store(
                false, std::memory_order_relaxed);
            s_ngNextLoadWaitsForPost.store(
                false, std::memory_order_relaxed);
            logger::warn(
                "NG: Present tick unavailable; custom visual arm skipped");
            return false;
        }

        // Chained loads retain the active compositor; only their config and
        // engine-budget state need refreshing. Their HideLoadingMenu/load-hook
        // signal will arm the eventual close.
        if (alreadyActive) {
            s_ngPreLoadTimeTicks.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
            s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
            s_ngNextLoadBodyReturned.store(
                false, std::memory_order_relaxed);
            s_ngNextLoadWaitsForPost.store(
                fromSaveLoad, std::memory_order_relaxed);
            // Release publishes the new generation, classification, barrier,
            // and outcome mode to Present's acquire read.
            s_ngNextLoadEntryArmed.store(
                true, std::memory_order_release);
            logger::info(
                "NG: next engine load armed behind active presentation "
                "(outcome={})",
                fromSaveLoad ? "kPostLoadGame" : "synchronous return");
            return true;
        }

        s_ngPreLoadTimeTicks.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
        s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
        s_ngNextLoadEntryArmed.store(false, std::memory_order_relaxed);
        s_ngNextLoadBodyReturned.store(false, std::memory_order_relaxed);
        s_ngNextLoadWaitsForPost.store(false, std::memory_order_relaxed);
        s_ngEnablePending.store(true, std::memory_order_release);
        return true;
    }

    // Exact AE native loading-menu boundaries. The official Address Library
    // maps these shared IDs on both verified builds (1.11.221 and 1.11.240),
    // while the public symbols identify the centralized TES methods; both
    // process the UI
    // message queue synchronously. Publishing after the original returns means
    // the next Present starts/stops custom pixels on the same first frame as the
    // native 3D loading screen. The prior address-library target for Hide
    // resolved inside AIProcess::ProcessTravel and must never be hooked.
    static std::atomic<bool> s_ngHideMenuSignal{ false };
    // Set by the TES::LoadInterior/LoadExterior entry hooks (door/cell/worldspace
    // transitions, which DON'T fire kPreLoadGame). Consumed at the load's end in
    // HookedHideLoadingMenu to arm the close — so the close timer is measured from
    // load-end (HideLoadingMenu), never mid-load.
    static std::atomic<bool> s_ngLoadHookPending{ false };
    using ShowLoadingMenuFn = void(__fastcall*)(void*, void*, bool, bool);
    using HideLoadingMenuFn = void(__fastcall*)(void*, void*, bool, float, bool);
    static ShowLoadingMenuFn s_origShowLoadingMenu = nullptr;
    static HideLoadingMenuFn s_origHideLoadingMenu = nullptr;
    static constexpr std::uint64_t kAEShowLoadingMenuID = 2192090;
    static constexpr std::uint64_t kAEHideLoadingMenuID = 2192091;
    static constexpr std::uint64_t kAELoadingMenuShownFlagID = 4796319;
    static constexpr std::uint64_t kAEPositionPlayerJobID = 2232905;
    static const volatile std::uint8_t* s_aeLoadingMenuShownFlag = nullptr;
    static std::atomic<bool> s_ngPositionExteriorShowContractReady{ false };
    static std::atomic<std::uintptr_t> s_ngPositionPlayerJobAddress{ 0 };

    // OG/VR patch only the verified PositionPlayerJob-owned CALL rather than
    // globally detouring TES::ShowLoadingMenu. The immutable stack layout and
    // original target are published before the CALL is replaced; readiness is
    // published last after every byte/RVA/data contract has passed.
    static std::atomic<ShowLoadingMenuFn>
        s_origLegacyPositionShowLoadingMenu{ nullptr };
    static const volatile std::uint8_t*
        s_legacyLoadingMenuShownFlag = nullptr;
    static std::atomic<bool>
        s_legacyPositionExteriorShowContractReady{ false };
    // One coherent lock-free snapshot for the legacy worker hook: low byte is
    // loading-screen mode and bit 8 means the full plugin arm is active.
    static std::atomic<std::uint32_t>
        s_legacyExteriorPresentationPolicy{ 0 };
    static std::uintptr_t s_legacyPositionShowReturnAddress = 0;
    static const char* s_legacyPositionRuntimeLabel = "legacy";

    struct LegacyPositionTargetLayout
    {
        std::size_t worldOffset{ 0 };
        std::size_t interiorOffset{ 0 };
        std::size_t transitionTeleportOffset{ 0 };
        std::size_t positionXOffset{ 0 };
        std::size_t positionYOffset{ 0 };
        std::size_t positionZOffset{ 0 };
        std::size_t requiredBytes{ 0 };
    };

    static LegacyPositionTargetLayout s_legacyPositionTargetLayout{};

    [[nodiscard]] static std::uintptr_t ExpectedNGRva(
        std::uintptr_t a_221,
        std::uintptr_t a_240) noexcept
    {
        const auto version = REL::Module::get().version();
        if (version[0] != 1 || version[1] != 11 || version[3] != 0) {
            return 0;
        }
        if (version[2] == 221) {
            return a_221;
        }
        if (version[2] == 240) {
            return a_240;
        }
        return 0;
    }

    [[nodiscard]] static bool MatchesExpectedNGRva(
        std::uintptr_t a_address,
        std::uintptr_t a_221,
        std::uintptr_t a_240) noexcept
    {
        const auto expected = ExpectedNGRva(a_221, a_240);
        const auto base = REL::Module::get().base();
        return expected != 0 && a_address >= base &&
            a_address - base == expected;
    }

    [[nodiscard]] static bool IsInModuleData(
        std::uintptr_t a_address,
        std::size_t a_size) noexcept
    {
        const auto data = REL::Module::get().segment(REL::Segment::data);
        if (!a_address || a_size == 0 || data.size() < a_size ||
            a_address < data.address()) {
            return false;
        }
        const auto offset = a_address - data.address();
        return offset <= data.size() && a_size <= data.size() - offset;
    }

    [[nodiscard]] static bool IsInModuleText(
        std::uintptr_t a_address,
        std::size_t a_size) noexcept
    {
        const auto text = REL::Module::get().segment(REL::Segment::text);
        if (!a_address || a_size == 0 || text.size() < a_size ||
            a_address < text.address()) {
            return false;
        }
        const auto offset = a_address - text.address();
        return offset <= text.size() && a_size <= text.size() - offset;
    }

    // Precise load timing, independent of benchmark mode / compositor: start at the
    // LoadInterior/LoadExterior hook, end at the FIRST HideLoadingMenu (the engine's
    // own "loading screen closed" signal). Works for door, worldspace, save AND fast
    // travel — fast travel's autosave-based end was the thing we couldn't measure.
    static std::chrono::steady_clock::time_point s_loadStart;
    static std::atomic<bool> s_loadTiming{ false };
    static const char*       s_loadKind = "";

    // Exact same-thread provenance for ShowLoadingMenu diagnostics. The
    // PositionPlayerJob Show occurs before either load entry; later calls carry
    // these depths and confirm whether they came from LoadInterior/LoadExterior.
    static thread_local std::uint32_t s_ngLoadInteriorDepth = 0;
    static thread_local std::uint32_t s_ngLoadExteriorDepth = 0;

    class ScopedNGLoadDepth
    {
    public:
        explicit ScopedNGLoadDepth(std::uint32_t& a_depth) noexcept :
            m_depth(a_depth)
        {
            ++m_depth;
        }

        ~ScopedNGLoadDepth()
        {
            --m_depth;
        }

        ScopedNGLoadDepth(const ScopedNGLoadDepth&) = delete;
        ScopedNGLoadDepth& operator=(const ScopedNGLoadDepth&) = delete;

    private:
        std::uint32_t& m_depth;
    };

    static bool ReadAELoadingMenuShown()
    {
        __try {
            return s_aeLoadingMenuShownFlag &&
                *s_aeLoadingMenuShownFlag != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    struct PositionTargetSnapshot
    {
        std::uintptr_t world{ 0 };
        std::uintptr_t interior{ 0 };
        std::uintptr_t transitionTeleport{ 0 };
        float positionX{ 0.0F };
        float positionY{ 0.0F };
        float positionZ{ 0.0F };
        bool fieldsReadable{ false };
    };

    static bool TryReadNGPositionTargetSnapshot_SEH(
        const std::byte* a_callerRsp,
        PositionTargetSnapshot* a_snapshot) noexcept
    {
        if (!a_snapshot) {
            return false;
        }
        *a_snapshot = {};
        __try {
            const auto base = reinterpret_cast<std::uintptr_t>(a_callerRsp);
            constexpr std::size_t kRequiredBytes = 0x74;
            if (!a_callerRsp || base < 0x10000 ||
                base > 0x7FFFFFFFFFFFULL - kRequiredBytes) {
                return false;
            }

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(
                    a_callerRsp, &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                return false;
            }
            const auto regionEnd =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                mbi.RegionSize;
            if (regionEnd < base || regionEnd - base < kRequiredBytes) {
                return false;
            }

            a_snapshot->world = *reinterpret_cast<const std::uintptr_t*>(
                a_callerRsp + 0x50);
            a_snapshot->interior = *reinterpret_cast<const std::uintptr_t*>(
                a_callerRsp + 0x58);
            a_snapshot->transitionTeleport =
                *reinterpret_cast<const std::uintptr_t*>(
                    a_callerRsp + 0x60);
            a_snapshot->positionX =
                *reinterpret_cast<const float*>(a_callerRsp + 0x68);
            a_snapshot->positionY =
                *reinterpret_cast<const float*>(a_callerRsp + 0x6C);
            a_snapshot->positionZ =
                *reinterpret_cast<const float*>(a_callerRsp + 0x70);
            a_snapshot->fieldsReadable = true;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *a_snapshot = {};
            return false;
        }
    }

    static bool ReadLegacyLoadingMenuShown() noexcept
    {
        __try {
            return s_legacyLoadingMenuShownFlag &&
                *s_legacyLoadingMenuShownFlag != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool TryReadLegacyPositionTargetSnapshot_SEH(
        const std::byte* a_callerRsp,
        PositionTargetSnapshot* a_snapshot) noexcept
    {
        if (!a_snapshot) {
            return false;
        }
        *a_snapshot = {};
        __try {
            const auto base = reinterpret_cast<std::uintptr_t>(a_callerRsp);
            const auto requiredBytes =
                s_legacyPositionTargetLayout.requiredBytes;
            if (!a_callerRsp || requiredBytes == 0 || base < 0x10000 ||
                base > 0x7FFFFFFFFFFFULL - requiredBytes) {
                return false;
            }

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(
                    a_callerRsp, &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                return false;
            }
            const auto regionEnd =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                mbi.RegionSize;
            if (regionEnd < base ||
                regionEnd - base < requiredBytes) {
                return false;
            }

            const auto& layout = s_legacyPositionTargetLayout;
            a_snapshot->world =
                *reinterpret_cast<const std::uintptr_t*>(
                    a_callerRsp + layout.worldOffset);
            a_snapshot->interior =
                *reinterpret_cast<const std::uintptr_t*>(
                    a_callerRsp + layout.interiorOffset);
            a_snapshot->transitionTeleport =
                *reinterpret_cast<const std::uintptr_t*>(
                    a_callerRsp + layout.transitionTeleportOffset);
            a_snapshot->positionX = *reinterpret_cast<const float*>(
                a_callerRsp + layout.positionXOffset);
            a_snapshot->positionY = *reinterpret_cast<const float*>(
                a_callerRsp + layout.positionYOffset);
            a_snapshot->positionZ = *reinterpret_cast<const float*>(
                a_callerRsp + layout.positionZOffset);
            a_snapshot->fieldsReadable = true;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *a_snapshot = {};
            return false;
        }
    }

    __declspec(noinline) static void __fastcall
        HookedLegacyPositionShowLoadingMenu(
            void* self,
            void* loc,
            bool loadingIntoInterior,
            bool deferredPayload)
    {
        // This wrapper replaces only PositionPlayerJob's verified direct CALL.
        // Its return slot therefore remains the exact caller discriminator and
        // callerRsp addresses the worker's copied destination snapshot.
        const auto* const returnSlot =
            reinterpret_cast<const std::byte*>(_AddressOfReturnAddress());
        const auto callerAddress = reinterpret_cast<std::uintptr_t>(
            _ReturnAddress());
        const auto* const callerRsp = returnSlot + sizeof(void*);
        const auto original =
            s_origLegacyPositionShowLoadingMenu.load(
                std::memory_order_acquire);
        if (!original) {
            return;
        }

        const bool contractReady =
            s_legacyPositionExteriorShowContractReady.load(
                std::memory_order_acquire);
        if (!contractReady) {
            original(self, loc, loadingIntoInterior, deferredPayload);
            return;
        }
        const bool exactPositionCaller =
            callerAddress == s_legacyPositionShowReturnAddress;
        const bool wasShown = ReadLegacyLoadingMenuShown();
        PositionTargetSnapshot positionTarget{};
        const bool targetReadable = exactPositionCaller &&
            TryReadLegacyPositionTargetSnapshot_SEH(
                callerRsp, &positionTarget);
        const bool matchingPluginExteriorSubmission =
            targetReadable && positionTarget.world != 0 &&
            positionTarget.interior == 0 &&
            DoorPrefetch::ConsumeExteriorSubmissionEvidence(
                reinterpret_cast<void*>(positionTarget.world),
                positionTarget.positionX,
                positionTarget.positionY,
                !wasShown);
        const auto presentationPolicy =
            s_legacyExteriorPresentationPolicy.load(
                std::memory_order_acquire);
        const auto loadingScreenMode =
            static_cast<int>(presentationPolicy & 0xFFU);
        const bool fullPluginActive =
            (presentationPolicy & 0x100U) != 0;
        const bool tipPresentationActive = fullPluginActive &&
            Policy::IsTipPresentationMode(
                loadingScreenMode);
        const bool saveClassificationArmed =
            s_flatLoadEntryArmed.load(std::memory_order_acquire);
        const bool correctExteriorPresentation =
            Policy::ShouldCorrectPositionExteriorShow(
                tipPresentationActive,
                contractReady,
                contractReady,
                wasShown,
                loadingIntoInterior,
                deferredPayload,
                saveClassificationArmed,
                exactPositionCaller,
                targetReadable,
                positionTarget.world != 0,
                positionTarget.interior != 0,
                matchingPluginExteriorSubmission);
        const bool effectiveLoadingIntoInterior =
            correctExteriorPresentation ? false : loadingIntoInterior;
        if (fullPluginActive &&
            (loadingIntoInterior || matchingPluginExteriorSubmission)) {
            const auto moduleBase = REL::Module::get().base();
            const auto callerRVA = callerAddress >= moduleBase ?
                callerAddress - moduleBase : 0;
            logger::info(
                "{}: PositionPlayerJob ShowLoadingMenu call "
                "(callerRVA={:#x}, shown={}, requestedMinimal={}, "
                "effectiveMinimal={}, deferred={}, saveArmed={}, "
                "contract={}, targetReadable={}, pluginSubmission={}, "
                "targetWorld={:#x}, targetInterior={:#x}, "
                "targetTeleport={:#x}, targetPos=({:.1f},{:.1f},{:.1f}), "
                "corrected={})",
                s_legacyPositionRuntimeLabel,
                callerRVA,
                wasShown,
                loadingIntoInterior,
                effectiveLoadingIntoInterior,
                deferredPayload,
                saveClassificationArmed,
                contractReady,
                targetReadable,
                matchingPluginExteriorSubmission,
                positionTarget.world,
                positionTarget.interior,
                positionTarget.transitionTeleport,
                positionTarget.positionX,
                positionTarget.positionY,
                positionTarget.positionZ,
                correctExteriorPresentation);
        }
        original(
            self, loc, effectiveLoadingIntoInterior, deferredPayload);
    }

    __declspec(noinline) static void __fastcall HookedShowLoadingMenu(
        void* self, void* loc, bool loadingIntoInterior, bool deferredPayload)
    {
        // MinHook reaches this detour by JMP, so this is still the original
        // TES::ShowLoadingMenu caller's return slot. Capture it in this frame;
        // an out-of-line helper would instead observe its own caller.
        const auto* const returnSlot =
            reinterpret_cast<const std::byte*>(_AddressOfReturnAddress());
        const auto callerAddress = reinterpret_cast<std::uintptr_t>(
            _ReturnAddress());
        const auto* const callerRsp = returnSlot + sizeof(void*);

        const auto original = s_origShowLoadingMenu;
        if (!original) {
            return;
        }
        // A few native Show callers bypass kPreLoadGame and both load-entry
        // detours. Refresh their dirty snapshot before Bethesda opens the menu;
        // doing this after original() would expose native pixels first.
        if (g_gameDataReady.load(std::memory_order_acquire) &&
            !s_ngEnablePending.load(std::memory_order_acquire) &&
            !s_ngLoadingActive.load(std::memory_order_acquire) &&
            !s_ngNextLoadEntryArmed.load(std::memory_order_acquire)) {
            RefreshLiveConfigBeforeLoadIfDirty("ng-native-show-entry");
        }
        D3D11Compositor::FlatNativeLoadingSelectionSnapshot
            priorNativeSelection{};
        D3D11Compositor::GetSingleton().
            TryGetFlatNativeLoadingSelection(priorNativeSelection);
        {
            std::lock_guard<std::recursive_mutex> boundaryLock(
                s_ngBoundaryMutex);
            ++s_ngBoundaryCallsInFlight;
        }
        const bool wasShown = ReadAELoadingMenuShown();
        const bool exactBoundaryReady = NGExactBoundaryHooksReady();
        const Config showConfig = GetConfigSnapshot();
        const bool fullPluginActive = showConfig.benchmarkMode != 0;
        const bool tipPresentationActive =
            fullPluginActive &&
            Policy::IsTipPresentationMode(
                showConfig.loadingScreenMode);
        const bool saveClassificationArmed =
            s_ngSaveClassificationArmed.load(std::memory_order_acquire);
        const bool positionContractReady =
            s_ngPositionExteriorShowContractReady.load(
                std::memory_order_acquire);
        const auto positionWorkerAddress =
            s_ngPositionPlayerJobAddress.load(std::memory_order_acquire);
        const bool exactPositionCaller =
            positionContractReady && positionWorkerAddress != 0 &&
            callerAddress == positionWorkerAddress + 0x8F7;
        PositionTargetSnapshot positionTarget{};
        const bool targetReadable =
            exactPositionCaller &&
            TryReadNGPositionTargetSnapshot_SEH(
                callerRsp, &positionTarget);
        const bool matchingPluginExteriorSubmission =
            targetReadable && positionTarget.world != 0 &&
            positionTarget.interior == 0 &&
            DoorPrefetch::ConsumeExteriorSubmissionEvidence(
                reinterpret_cast<void*>(positionTarget.world),
                positionTarget.positionX,
                positionTarget.positionY,
                !wasShown);
        const bool correctExteriorPresentation =
            Policy::ShouldCorrectPositionExteriorShow(
                tipPresentationActive,
                exactBoundaryReady,
                positionContractReady,
                wasShown,
                loadingIntoInterior,
                deferredPayload,
                saveClassificationArmed,
                exactPositionCaller,
                targetReadable,
                positionTarget.world != 0,
                positionTarget.interior != 0,
                matchingPluginExteriorSubmission);
        const bool effectiveLoadingIntoInterior =
            correctExteriorPresentation ? false : loadingIntoInterior;
        const auto moduleBase = REL::Module::get().base();
        const auto callerRVA = callerAddress >= moduleBase ?
            callerAddress - moduleBase : 0;
        if (fullPluginActive && exactBoundaryReady) {
            logger::info(
                "NG: ShowLoadingMenu call (callerRVA={:#x}, shown={}, "
                "requestedMinimal={}, effectiveMinimal={}, deferred={}, "
                "exteriorDepth={}, interiorDepth={}, saveArmed={}, "
                "positionContract={}, positionCaller={}, targetReadable={}, "
                "pluginSubmission={}, "
                "targetWorld={:#x}, targetInterior={:#x}, "
                "targetTeleport={:#x}, targetPos=({:.1f},{:.1f},{:.1f}), "
                "corrected={})",
                callerRVA, wasShown, loadingIntoInterior,
                effectiveLoadingIntoInterior, deferredPayload,
                s_ngLoadExteriorDepth,
                s_ngLoadInteriorDepth, saveClassificationArmed,
                positionContractReady, exactPositionCaller, targetReadable,
                matchingPluginExteriorSubmission,
                positionTarget.world, positionTarget.interior,
                positionTarget.transitionTeleport,
                positionTarget.positionX, positionTarget.positionY,
                positionTarget.positionZ,
                correctExteriorPresentation);
        }
        original(
            self, loc, effectiveLoadingIntoInterior, deferredPayload);
        const bool isShown = ReadAELoadingMenuShown();
        std::lock_guard<std::recursive_mutex> boundaryLock(
            s_ngBoundaryMutex);
        if (exactBoundaryReady && !wasShown && isShown) {
            s_ngNativeMenuVisible.store(true, std::memory_order_release);
            if (GetConfigSnapshot().benchmarkMode != 0) {
                const bool showMatchesArmedEntry =
                    s_ngEnablePending.load(std::memory_order_acquire) ||
                    s_ngNextLoadEntryArmed.load(
                        std::memory_order_acquire);
                // Consume the exact Show's classification. The sticky save bit
                // survives the subordinate LoadInterior hook, while Bethesda's
                // Show argument is authoritative for this native menu.
                const bool fromSave =
                    s_ngSaveClassificationArmed.exchange(
                        false, std::memory_order_acq_rel);
                s_ngEnableFromSaveLoad.store(
                    fromSave, std::memory_order_release);
                s_ngIsInterior.store(
                    effectiveLoadingIntoInterior, std::memory_order_release);
                s_ngShowSelectionBaselineSerial.store(
                    priorNativeSelection.initialSerial,
                    std::memory_order_release);

                // Only an actually completed native Show supersedes a prior
                // menu's pending close. kPreLoadGame never guesses this boundary.
                s_ngClosePending.store(false, std::memory_order_relaxed);
                s_ngHideMenuSignal.store(false, std::memory_order_relaxed);
                s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
                // A kPost which arrived after this entry belongs to this load,
                // not the old presentation. Preserve it so Show→Hide between
                // Presents still has a matching outcome/close.
                if (!showMatchesArmedEntry) {
                    s_ngPostLoadArrived.store(
                        false, std::memory_order_relaxed);
                }
                // Some native Show callers do not pass through kPreLoadGame or
                // our load hooks. Arm only after the engine proves a real 0→1
                // transition, so an early-out Show cannot strand settings.
                if (g_gameDataReady.load(std::memory_order_acquire) &&
                    !s_ngEnablePending.load(std::memory_order_acquire) &&
                    !s_ngLoadingActive.load(std::memory_order_acquire)) {
                    NGArmPendingOnGameThread(false, true);
                }
                if (s_ngEnablePending.load(std::memory_order_acquire)) {
                    s_ngShowMenuSignal.store(
                        true, std::memory_order_release);
                    logger::info(
                        "NG: native TES::ShowLoadingMenu completed — "
                        "custom open signalled");
                } else if (s_ngLoadingActive.load(
                               std::memory_order_acquire)) {
                    // The verified Show now owns the preserved entry. Its
                    // presentation re-arm will happen before this Present
                    // composites; the final native Hide owns cleanup.
                    s_ngNextLoadEntryArmed.store(
                        false, std::memory_order_release);
                    s_ngNextLoadBodyReturned.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadWaitsForPost.store(
                        false, std::memory_order_relaxed);
                    s_ngChainedShowSignal.store(
                        true, std::memory_order_release);
                    logger::info(
                        "NG: chained native TES::ShowLoadingMenu completed — "
                        "presentation re-arm signalled");
                }
            }
        }
        --s_ngBoundaryCallsInFlight;
    }

    static void __fastcall HookedHideLoadingMenu(
        void* self, void* loc, bool a3, float a4, bool a5)
    {
        const auto original = s_origHideLoadingMenu;
        if (!original) {
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> boundaryLock(
                s_ngBoundaryMutex);
            ++s_ngBoundaryCallsInFlight;
        }
        const bool wasShown = ReadAELoadingMenuShown();
        original(self, loc, a3, a4, a5);
        const bool isShown = ReadAELoadingMenuShown();
        std::lock_guard<std::recursive_mutex> boundaryLock(
            s_ngBoundaryMutex);
        if (NGExactBoundaryHooksReady() && wasShown && !isShown) {
            s_ngNativeMenuVisible.store(false, std::memory_order_release);

            // Precise load duration includes the native hide/fader/UI processing.
            if (s_loadTiming.exchange(false, std::memory_order_acquire)) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s_loadStart).count();
                logger::info("LOADTIME: {} — {:.2f}s", s_loadKind, ms / 1000.0);
            }
            if (GetConfigSnapshot().benchmarkMode != 0) {
                PapyrusOptimizer::GetSingleton().SetLoading(false);

                if (!s_ngPresentTickReady.load(std::memory_order_acquire)) {
                    s_ngEnablePending.store(false, std::memory_order_relaxed);
                    s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
                    s_ngChainedShowSignal.store(
                        false, std::memory_order_relaxed);
                    s_ngHideMenuSignal.store(false, std::memory_order_relaxed);
                    s_ngClosePending.store(false, std::memory_order_relaxed);
                    s_ngLoadHookPending.store(false, std::memory_order_relaxed);
                    s_ngSaveClassificationArmed.store(
                        false, std::memory_order_relaxed);
                    s_ngEnableFromSaveLoad.store(
                        false, std::memory_order_relaxed);
                    s_ngIsInterior.store(false, std::memory_order_relaxed);
                    s_ngNextLoadEntryArmed.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadBodyReturned.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadWaitsForPost.store(
                        false, std::memory_order_relaxed);
                    NGFinishPresentationGapIfNeeded();
                    QueueNGEndLoadTweaks();
                    DoorPrefetch::EndTransition();
                    ResumePostLoadSystemsAtNativeClose();
                    QueueNGConfigRefresh("ng-native-hide-no-present");
                    logger::info(
                        "NG: native Hide completed with no Present owner; "
                        "nonvisual load state unwound");
                } else {
                    // Signal only: the next Present consumes this before
                    // compositing, so no custom frame can be drawn after the
                    // native menu has disappeared.
                    s_ngHideMenuSignal.store(
                        true, std::memory_order_relaxed);
                    s_ngLastPostLoadTimeTicks.store(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count(),
                        std::memory_order_relaxed);
                    s_ngClosePending.store(
                        true, std::memory_order_release);
                    s_ngLoadHookPending.store(
                        false, std::memory_order_relaxed);
                    logger::info(
                        "NG: native TES::HideLoadingMenu completed — "
                        "custom close signalled");
                }
            }
        }
        --s_ngBoundaryCallsInFlight;
    }

    // TES::LoadInterior / TES::LoadExterior entry hooks — the NG door-load "open"
    // signal. Save loads fire kPreLoadGame; door/cell/worldspace transitions DON'T,
    // so on NG (which can't use MenuOpenCloseEvent — UI::GetSingleton is broken)
    // those loads engaged nothing → vanilla loading screen. These hooks arm the
    // SAME deferred-enable the kPreLoadGame path uses, so the compositor shows our
    // bg for door transitions too. Both functions call HideLoadingMenu at load-end,
    // which arms the close (above). The shared IDs, exact RVAs, prologues, and
    // ABIs are verified on NG 1.11.221.0 and 1.11.240.0.
    using LoadInteriorFn = void(__fastcall*)(void*, void*, float*);
    using LoadExteriorFn = void(__fastcall*)(void*, float*);
    static LoadInteriorFn s_origLoadInterior = nullptr;
    static LoadExteriorFn s_origLoadExterior = nullptr;
    static std::atomic<bool> s_ngLoadHookPairReady{ false };
    static std::atomic<bool> s_ngLoadHookPairPoisoned{ false };

    // NG destination classification. This records whether the synchronous load
    // entered an interior; presentation still uses the configured mode for every
    // verified native ShowLoadingMenu. The flag is lifecycle metadata only and is
    // consumed (and reset) once in the deferred-enable path.
    static void NGArmLoad()
    {
        // Consume the already-published config and raise temporary engine
        // budgets and the transition barrier before the original synchronous
        // load is entered. NGArmPending publishes them transactionally against
        // Present; no disk IO is permitted between the timer start and entry.
        s_ngLoadHookPending.store(
            NGArmPendingOnGameThread(false), std::memory_order_relaxed);
    }

    // After the (synchronous) engine load returns, guarantee a close is armed even
    // for menu-less / scripted transitions (elevators, the Vault 111 intro) that
    // never call TES::HideLoadingMenu. Without this the enable is never matched by a
    // close and the compositor stays on forever — the permanent-black-screen bug.
    // If HideLoadingMenu DID fire during the load it already cleared s_ngLoadHookPending
    // (and armed the close), so this no-ops. When it fires here the load has ended, so
    // signal the close directly (don't make it wait the 3.5s fallback timer).
    static void NGArmCloseIfLoadEnded()
    {
        if (s_ngNextLoadEntryArmed.load(std::memory_order_acquire) &&
            !s_ngNextLoadWaitsForPost.load(std::memory_order_acquire)) {
            // This synchronous return is the outcome for a door/cell load which
            // has not produced its own verified Show. A save's subordinate
            // LoadInterior must not cancel it early; that path waits for its
            // kPostLoadGame outcome instead.
            s_ngNextLoadBodyReturned.store(
                true, std::memory_order_release);
        }
        if (!s_ngPresentTickReady.load(std::memory_order_acquire)) {
            s_ngLoadHookPending.store(false, std::memory_order_relaxed);
            QueueNGEndLoadTweaks();
            DoorPrefetch::EndTransition();
            PapyrusOptimizer::GetSingleton().SetLoading(false);
            ResumePostLoadSystemsAtNativeClose();
            QueueNGConfigRefresh("ng-load-return-no-present");
            return;
        }
        if (NGExactBoundaryHooksReady() &&
            s_ngNativeMenuVisible.load(std::memory_order_acquire)) {
            // The synchronous load body can return before Bethesda finishes
            // the native LoadingMenu fade/close. Do not synthesize an early
            // custom CLOSE; the verified TES::HideLoadingMenu hook owns it.
            logger::info(
                "NG: load body returned; awaiting native HideLoadingMenu");
            return;
        }
        if (s_ngLoadHookPending.exchange(false, std::memory_order_relaxed)) {
            s_ngLastPostLoadTimeTicks.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
            s_ngHideMenuSignal.store(true, std::memory_order_relaxed);
            s_ngClosePending.store(true, std::memory_order_release);
            logger::info("NG: load returned with no HideLoadingMenu — close armed (menu-less transition)");
            PapyrusOptimizer::GetSingleton().SetLoading(false);
        }
    }

    static void __fastcall HookedLoadInterior(void* self, void* cell, float* a3)
    {
        const auto original = s_origLoadInterior;
        if (!original) {
            return;
        }
        if (!s_ngLoadHookPairReady.load(std::memory_order_acquire)) {
            original(self, cell, a3);
            return;
        }
        ScopedNGLoadDepth loadDepth(s_ngLoadInteriorDepth);
        if (GetConfigSnapshot().benchmarkMode == 0) {
            s_loadKind = "interior";
            s_loadStart = std::chrono::steady_clock::now();
            s_loadTiming.store(true, std::memory_order_release);
            original(self, cell, a3);
            return;
        }
        s_ngIsInterior.store(true, std::memory_order_relaxed);
        s_loadKind = "interior";
        s_loadStart = std::chrono::steady_clock::now();
        s_loadTiming.store(true, std::memory_order_release);
        logger::info(
            "NG: LoadInterior hook — engine load entered "
            "(interiorDepth={}, exteriorDepth={})",
            s_ngLoadInteriorDepth, s_ngLoadExteriorDepth);
        NGArmLoad();
        original(self, cell, a3);
        logger::info("NG: LoadInterior hook — engine load returned");
        NGArmCloseIfLoadEnded();
    }
    static void __fastcall HookedLoadExterior(void* self, float* a2)
    {
        const auto original = s_origLoadExterior;
        if (!original) {
            return;
        }
        if (!s_ngLoadHookPairReady.load(std::memory_order_acquire)) {
            original(self, a2);
            return;
        }
        ScopedNGLoadDepth loadDepth(s_ngLoadExteriorDepth);
        if (GetConfigSnapshot().benchmarkMode == 0) {
            s_loadKind = "exterior";
            s_loadStart = std::chrono::steady_clock::now();
            s_loadTiming.store(true, std::memory_order_release);
            original(self, a2);
            return;
        }
        s_ngIsInterior.store(false, std::memory_order_relaxed);  // worldspace → normal screen
        s_loadKind = "exterior";
        s_loadStart = std::chrono::steady_clock::now();
        s_loadTiming.store(true, std::memory_order_release);
        logger::info(
            "NG: LoadExterior hook — engine load entered "
            "(exteriorDepth={}, interiorDepth={})",
            s_ngLoadExteriorDepth, s_ngLoadInteriorDepth);
        NGArmLoad();
        original(self, a2);
        logger::info("NG: LoadExterior hook — engine load returned");
        NGArmCloseIfLoadEnded();
    }

    static bool MatchesExecutableBytes(
        std::uintptr_t address, const std::uint8_t* expected, std::size_t size)
    {
        __try {
            return address && expected && size > 0 &&
                std::memcmp(
                    reinterpret_cast<const void*>(address), expected, size) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool DecodesRelativeCallTarget(
        std::uintptr_t a_callAddress,
        std::uintptr_t a_expectedTarget) noexcept
    {
        __try {
            if (!a_callAddress || !a_expectedTarget ||
                *reinterpret_cast<const std::uint8_t*>(a_callAddress) !=
                    0xE8) {
                return false;
            }
            std::int32_t displacement = 0;
            std::memcpy(
                &displacement,
                reinterpret_cast<const void*>(a_callAddress + 1),
                sizeof(displacement));
            const auto decoded = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(a_callAddress + 5) +
                static_cast<std::intptr_t>(displacement));
            return decoded == a_expectedTarget;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void InstallLegacyPositionExteriorShowHook()
    {
        if (REL::Module::IsNG() ||
            s_legacyPositionExteriorShowContractReady.load(
                std::memory_order_acquire)) {
            return;
        }

        const auto version = REL::Module::get().version();
        const bool isOG163 = !REL::Module::IsVR() &&
            version[0] == 1 && version[1] == 10 &&
            version[2] == 163 && version[3] == 0;
        const bool isVR1272 = REL::Module::IsVR() &&
            version[0] == 1 && version[1] == 2 &&
            version[2] == 72 && version[3] == 0;
        if (!isOG163 && !isVR1272) {
            logger::warn(
                "Legacy: PositionPlayerJob exterior-loading-menu contract "
                "is unavailable on {}.{}.{}.{}; tip-mode exterior gate "
                "preloading remains disabled fail closed",
                version[0], version[1], version[2], version[3]);
            return;
        }

        const auto moduleBase = REL::Module::get().base();
        const char* const runtimeLabel = isVR1272 ? "VR" : "OG";
        const std::uintptr_t showRVA =
            isVR1272 ? 0xFB1D0 : 0xFB180;
        const std::uintptr_t hideRVA =
            isVR1272 ? 0xFB3B0 : 0xFB360;
        const std::uintptr_t shownFlagRVA =
            isVR1272 ? 0x5932343 : 0x58D08B3;
        const std::uintptr_t positionWorkerRVA =
            isVR1272 ? 0xF03BD0 : 0xE9AC60;
        const std::uintptr_t targetCopyOffset =
            isVR1272 ? 0x383 : 0x388;
        constexpr std::uintptr_t kShowSequenceOffset = 0x9A6;
        constexpr std::uintptr_t kShowCallOffset = 0x9C3;
        constexpr std::uintptr_t kShowReturnOffset = 0x9C8;
        const std::uintptr_t hideShownFlagWriteOffset =
            isVR1272 ? 0x32D : 0x3AC;
        const auto showAddress = moduleBase + showRVA;
        const auto hideAddress = moduleBase + hideRVA;
        const auto shownFlagAddress = moduleBase + shownFlagRVA;
        const auto positionWorkerAddress =
            moduleBase + positionWorkerRVA;
        const auto positionShowCallAddress =
            positionWorkerAddress + kShowCallOffset;

        static constexpr std::uint8_t showPrologue[] = {
            0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58
        };
        static constexpr std::uint8_t hidePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C
        };
        static constexpr std::uint8_t ogShowShownFlagRead[] = {
            0x80, 0x3D, 0x24, 0x57, 0x7D, 0x05, 0x00
        };
        static constexpr std::uint8_t vrShowShownFlagRead[] = {
            0x80, 0x3D, 0x64, 0x71, 0x83, 0x05, 0x00
        };
        static constexpr std::uint8_t ogShowShownFlagWrite[] = {
            0xC6, 0x05, 0xFF, 0x55, 0x7D, 0x05, 0x01
        };
        static constexpr std::uint8_t vrShowShownFlagWrite[] = {
            0xC6, 0x05, 0x3F, 0x70, 0x83, 0x05, 0x01
        };
        static constexpr std::uint8_t ogHideShownFlagWrite[] = {
            0xC6, 0x05, 0xA0, 0x51, 0x7D, 0x05, 0x00
        };
        static constexpr std::uint8_t vrHideShownFlagWrite[] = {
            0xC6, 0x05, 0x5F, 0x6C, 0x83, 0x05, 0x00
        };
        static constexpr std::uint8_t ogPositionPrologue[] = {
            0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8D,
            0xAC, 0x24, 0x58, 0xFE, 0xFF, 0xFF, 0x48, 0x81,
            0xEC, 0xA8, 0x02, 0x00, 0x00
        };
        static constexpr std::uint8_t vrPositionPrologue[] = {
            0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8D,
            0xAC, 0x24, 0x18, 0xFE, 0xFF, 0xFF, 0x48, 0x81,
            0xEC, 0xE8, 0x02, 0x00, 0x00
        };
        static constexpr std::uint8_t ogPositionTargetCopy[] = {
            0x49, 0x8B, 0x86, 0xE8, 0x08, 0x00, 0x00, 0x49,
            0x8B, 0xCE, 0xF3, 0x41, 0x0F, 0x10, 0x86, 0x00,
            0x09, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x10, 0x8E,
            0x04, 0x09, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
            0x60, 0x49, 0x8B, 0x86, 0xF0, 0x08, 0x00, 0x00,
            0xF3, 0x0F, 0x11, 0x44, 0x24, 0x78, 0xF3, 0x41,
            0x0F, 0x10, 0x86, 0x08, 0x09, 0x00, 0x00, 0xF3,
            0x0F, 0x11, 0x4C, 0x24, 0x7C, 0x48, 0x89, 0x44,
            0x24, 0x68, 0x49, 0x8B, 0x86, 0xF8, 0x08, 0x00,
            0x00, 0xF3, 0x41, 0x0F, 0x10, 0x8E, 0x0C, 0x09,
            0x00, 0x00, 0xF3, 0x0F, 0x11, 0x45, 0x80, 0xF3,
            0x41, 0x0F, 0x10, 0x86, 0x10, 0x09, 0x00, 0x00,
            0x48, 0x89, 0x44, 0x24, 0x70
        };
        static constexpr std::uint8_t vrPositionTargetCopy[] = {
            0x49, 0x8B, 0x86, 0x58, 0x0D, 0x00, 0x00, 0x49,
            0x8B, 0xCE, 0xF3, 0x41, 0x0F, 0x10, 0x86, 0x70,
            0x0D, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x10, 0x8E,
            0x74, 0x0D, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24,
            0x60, 0x49, 0x8B, 0x86, 0x60, 0x0D, 0x00, 0x00,
            0xF3, 0x0F, 0x11, 0x44, 0x24, 0x78, 0xF3, 0x41,
            0x0F, 0x10, 0x86, 0x78, 0x0D, 0x00, 0x00, 0xF3,
            0x0F, 0x11, 0x4C, 0x24, 0x7C, 0x48, 0x89, 0x44,
            0x24, 0x68, 0x49, 0x8B, 0x86, 0x68, 0x0D, 0x00,
            0x00, 0xF3, 0x41, 0x0F, 0x10, 0x8E, 0x7C, 0x0D,
            0x00, 0x00, 0xF3, 0x0F, 0x11, 0x45, 0x80, 0xF3,
            0x41, 0x0F, 0x10, 0x86, 0x80, 0x0D, 0x00, 0x00,
            0x48, 0x89, 0x44, 0x24, 0x70
        };
        static constexpr std::uint8_t ogPositionShowSequence[] = {
            0x44, 0x0F, 0xB6, 0xBD, 0xD8, 0x01, 0x00, 0x00,
            0x33, 0xF6, 0x48, 0x8B, 0x54, 0x24, 0x40, 0x48,
            0x8B, 0x0D, 0x6C, 0x8C, 0xC0, 0x04, 0x45, 0x33,
            0xC9, 0x44, 0x0F, 0xB6, 0xC7, 0xE8, 0x58, 0xFB,
            0x25, 0xFF
        };
        static constexpr std::uint8_t vrPositionShowSequence[] = {
            0x44, 0x0F, 0xB6, 0xBD, 0x18, 0x02, 0x00, 0x00,
            0x33, 0xF6, 0x48, 0x8B, 0x54, 0x24, 0x40, 0x48,
            0x8B, 0x0D, 0x34, 0xFD, 0xBF, 0x04, 0x45, 0x33,
            0xC9, 0x44, 0x0F, 0xB6, 0xC7, 0xE8, 0x38, 0x6C,
            0x1F, 0xFF
        };
        static_assert(sizeof(ogPositionPrologue) == 0x15);
        static_assert(
            sizeof(ogPositionPrologue) == sizeof(vrPositionPrologue));
        static_assert(sizeof(ogPositionTargetCopy) == 0x65);
        static_assert(
            sizeof(ogPositionTargetCopy) ==
            sizeof(vrPositionTargetCopy));
        static_assert(sizeof(ogPositionShowSequence) == 0x22);
        static_assert(
            sizeof(ogPositionShowSequence) ==
            sizeof(vrPositionShowSequence));

        const auto* const showShownFlagRead = isVR1272 ?
            vrShowShownFlagRead : ogShowShownFlagRead;
        const auto* const showShownFlagWrite = isVR1272 ?
            vrShowShownFlagWrite : ogShowShownFlagWrite;
        const auto* const hideShownFlagWrite = isVR1272 ?
            vrHideShownFlagWrite : ogHideShownFlagWrite;
        const auto* const positionPrologue = isVR1272 ?
            vrPositionPrologue : ogPositionPrologue;
        const auto* const positionTargetCopy = isVR1272 ?
            vrPositionTargetCopy : ogPositionTargetCopy;
        const auto* const positionShowSequence = isVR1272 ?
            vrPositionShowSequence : ogPositionShowSequence;

        const bool exactContract =
            IsInModuleText(showAddress, sizeof(showPrologue)) &&
            IsInModuleText(hideAddress, sizeof(hidePrologue)) &&
            IsInModuleText(
                positionWorkerAddress,
                kShowReturnOffset) &&
            IsInModuleData(shownFlagAddress, sizeof(std::uint8_t)) &&
            MatchesExecutableBytes(
                showAddress, showPrologue, sizeof(showPrologue)) &&
            MatchesExecutableBytes(
                hideAddress, hidePrologue, sizeof(hidePrologue)) &&
            MatchesExecutableBytes(
                showAddress + 0x8,
                showShownFlagRead,
                sizeof(ogShowShownFlagRead)) &&
            MatchesExecutableBytes(
                showAddress + 0x12D,
                showShownFlagWrite,
                sizeof(ogShowShownFlagWrite)) &&
            MatchesExecutableBytes(
                hideAddress + hideShownFlagWriteOffset,
                hideShownFlagWrite,
                sizeof(ogHideShownFlagWrite)) &&
            MatchesExecutableBytes(
                positionWorkerAddress,
                positionPrologue,
                sizeof(ogPositionPrologue)) &&
            MatchesExecutableBytes(
                positionWorkerAddress + targetCopyOffset,
                positionTargetCopy,
                sizeof(ogPositionTargetCopy)) &&
            MatchesExecutableBytes(
                positionWorkerAddress + kShowSequenceOffset,
                positionShowSequence,
                sizeof(ogPositionShowSequence)) &&
            DecodesRelativeCallTarget(
                positionShowCallAddress, showAddress);
        if (!exactContract) {
            logger::error(
                "{}: PositionPlayerJob exterior-loading-menu byte contract "
                "mismatch; mode-1/3 exterior gate preloading remains "
                "disabled fail closed",
                runtimeLabel);
            return;
        }

        auto& trampoline = F4SE::GetTrampoline();
        constexpr std::size_t kBranchIslandBytes = 14;
        if (trampoline.free_size() < kBranchIslandBytes) {
            logger::error(
                "{}: insufficient F4SE trampoline capacity for "
                "PositionPlayerJob ShowLoadingMenu CALL hook ({} available, "
                "{} required)",
                runtimeLabel, trampoline.free_size(), kBranchIslandBytes);
            return;
        }

        // Publish immutable wrapper inputs before replacing the CALL. A worker
        // which reaches the wrapper immediately can always forward safely;
        // readiness remains false until the patch result is verified below.
        s_legacyLoadingMenuShownFlag =
            reinterpret_cast<const volatile std::uint8_t*>(
                shownFlagAddress);
        s_legacyPositionShowReturnAddress =
            positionWorkerAddress + kShowReturnOffset;
        s_legacyPositionTargetLayout = {
            0x60, 0x68, 0x70, 0x78, 0x7C, 0x80, 0x84
        };
        s_legacyPositionRuntimeLabel = runtimeLabel;
        // Release-publish last among the pre-patch inputs. The wrapper may be
        // entered before capability readiness is published, but it must always
        // be able to forward the native call without a data race.
        s_origLegacyPositionShowLoadingMenu.store(
            reinterpret_cast<ShowLoadingMenuFn>(showAddress),
            std::memory_order_release);

        const auto originalTarget = trampoline.write_call<5>(
            positionShowCallAddress,
            reinterpret_cast<std::uintptr_t>(
                &HookedLegacyPositionShowLoadingMenu));
        if (originalTarget != showAddress) {
            constexpr std::size_t kRelativeCallBytes = 5;
            const auto* const originalCall =
                positionShowSequence +
                sizeof(ogPositionShowSequence) - kRelativeCallBytes;
            REL::safe_write(
                positionShowCallAddress,
                originalCall,
                kRelativeCallBytes);
            FlushInstructionCache(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(positionShowCallAddress),
                kRelativeCallBytes);
            // Do not clear the original pointer: a worker which entered the
            // wrapper before restoration must still forward native Show.
            logger::error(
                "{}: PositionPlayerJob CALL hook returned unexpected target "
                "{:#x} (expected {:#x}); original CALL restored and "
                "tip-mode exterior gate preloading remains disabled",
                runtimeLabel, originalTarget, showAddress);
            return;
        }

        s_legacyPositionExteriorShowContractReady.store(
            true, std::memory_order_release);
        logger::info(
            "{}: PositionPlayerJob exterior-loading-menu contract verified; "
            "ShowLoadingMenu CALL hook installed at RVA {:#x}",
            runtimeLabel,
            positionWorkerRVA + kShowCallOffset);
    }

    static void InstallNGLoadingMenuBoundaryHooks()
    {
        if (s_ngBoundaryHooksPoisoned.load(std::memory_order_acquire)) {
            logger::warn(
                "NG: LoadingMenu boundary hooks remain disabled after "
                "an earlier ownership/rollback failure");
            return;
        }
        if (s_showLoadingMenuHookInstalled.load(std::memory_order_acquire) &&
            s_hideLoadingMenuHookInstalled.load(std::memory_order_acquire)) return;
        if (!REL::Module::IsNG() || REL::Module::IsVR()) return;  // NG/AE flat only
        // Only the two builds whose Address Library mappings, entries, and ABIs
        // were verified may install these hooks. Other NG builds keep native
        // visuals because no timer can reproduce the real menu boundary.
        const auto ver = REL::Module::get().version();
        if (!(ver[0] == 1 && ver[1] == 11 &&
              (ver[2] == 221 || ver[2] == 240) && ver[3] == 0)) {
            logger::info(
                "NG: native LoadingMenu boundary hooks are unavailable on "
                "{}.{}.{}.{}",
                ver[0], ver[1], ver[2], ver[3]);
            return;
        }
        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) return;

        REL::Relocation<std::uintptr_t> showTarget{
            REL::ID(kAEShowLoadingMenuID)
        };
        REL::Relocation<std::uintptr_t> hideTarget{
            REL::ID(kAEHideLoadingMenuID)
        };
        REL::Relocation<std::uintptr_t> shownFlagTarget{
            REL::ID(kAELoadingMenuShownFlagID)
        };
        REL::Relocation<std::uintptr_t> positionPlayerJobTarget{
            REL::ID(kAEPositionPlayerJobID)
        };
        const auto showAddress = showTarget.address();
        const auto hideAddress = hideTarget.address();
        const auto shownFlagAddress = shownFlagTarget.address();
        const auto positionPlayerJobAddress =
            positionPlayerJobTarget.address();
        if (!MatchesExpectedNGRva(showAddress, 0x2D3EB0, 0x2D41D0) ||
            !MatchesExpectedNGRva(hideAddress, 0x2D4090, 0x2D43B0) ||
            !MatchesExpectedNGRva(
                shownFlagAddress, 0x30DD8C6, 0x30E8946) ||
            !IsInModuleData(shownFlagAddress, sizeof(std::uint8_t))) {
            logger::error(
                "NG: LoadingMenu boundary Address Library mapping does not "
                "match the exact {}.{}.{}.{} RVA set; native fallback retained",
                ver[0], ver[1], ver[2], ver[3]);
            return;
        }
        s_aeLoadingMenuShownFlag =
            reinterpret_cast<const volatile std::uint8_t*>(shownFlagAddress);
        static constexpr std::uint8_t showPrologue[] = {
            0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58
        };
        static constexpr std::uint8_t hidePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C
        };
        static constexpr std::uint8_t positionPlayerJobPrologue[] = {
            0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x56, 0x48,
            0x8D, 0xAC, 0x24, 0xD8, 0xFD, 0xFF, 0xFF, 0x48,
            0x81, 0xEC, 0x28, 0x03, 0x00, 0x00
        };
        static constexpr std::uint8_t positionTargetCopy[] = {
            0x49, 0x8D, 0x8E, 0xE8, 0x08, 0x00, 0x00, 0x48,
            0x8B, 0x01, 0x0F, 0x10, 0x41, 0x18, 0x48, 0x89,
            0x44, 0x24, 0x50, 0x48, 0x8B, 0x41, 0x08, 0xF3,
            0x0F, 0x10, 0x49, 0x2C, 0x48, 0x89, 0x44, 0x24,
            0x58, 0x48, 0x8B, 0x41, 0x10, 0x48, 0x89, 0x44,
            0x24, 0x60, 0x48, 0x8B, 0x41, 0x30, 0x48, 0x89,
            0x45, 0x80, 0x48, 0x8B, 0x41, 0x38, 0x48, 0x89,
            0x45, 0x88, 0x48, 0x8B, 0x41, 0x40, 0x48, 0x89,
            0x45, 0x90, 0x8B, 0x41, 0x48, 0x89, 0x45, 0x98,
            0x8B, 0x41, 0x4C, 0x89, 0x45, 0x9C, 0x0F, 0xB6,
            0x41, 0x54, 0x88, 0x45, 0xA4, 0x0F, 0xB6, 0x41,
            0x55, 0x88, 0x45, 0xA5, 0x0F, 0xB6, 0x41, 0x56,
            0x88, 0x45, 0xA6, 0x0F, 0xB6, 0x41, 0x57, 0x0F,
            0x11, 0x44, 0x24, 0x68
        };
        static constexpr std::uint8_t positionShowSequence221[] = {
            0x0F, 0xB6, 0xB5, 0x58, 0x02, 0x00, 0x00, 0x48,
            0x8B, 0x54, 0x24, 0x38, 0x45, 0x33, 0xC9, 0x48,
            0x8B, 0x0D, 0x3A, 0xB8, 0x57, 0x02, 0x45, 0x0F,
            0xB6, 0xC5, 0xE8, 0x19, 0xD6, 0x57, 0xFF
        };
        static constexpr std::uint8_t positionShowSequence240[] = {
            0x0F, 0xB6, 0xB5, 0x58, 0x02, 0x00, 0x00, 0x48,
            0x8B, 0x54, 0x24, 0x38, 0x45, 0x33, 0xC9, 0x48,
            0x8B, 0x0D, 0x3A, 0x65, 0x58, 0x02, 0x45, 0x0F,
            0xB6, 0xC5, 0xE8, 0xA9, 0xD5, 0x57, 0xFF
        };
        static_assert(sizeof(positionPlayerJobPrologue) == 0x16);
        static_assert(sizeof(positionTargetCopy) == 0x6C);
        static_assert(sizeof(positionShowSequence221) == 0x1F);
        static_assert(
            sizeof(positionShowSequence221) ==
            sizeof(positionShowSequence240));
        const auto* const positionShowSequence =
            ver[2] == 221 ? positionShowSequence221 :
                            positionShowSequence240;
        const bool positionExteriorShowContract =
            MatchesExpectedNGRva(
                positionPlayerJobAddress, 0xD55FA0, 0xD56330) &&
            MatchesExecutableBytes(
                positionPlayerJobAddress,
                positionPlayerJobPrologue,
                sizeof(positionPlayerJobPrologue)) &&
            MatchesExecutableBytes(
                positionPlayerJobAddress + 0x449,
                positionTargetCopy,
                sizeof(positionTargetCopy)) &&
            MatchesExecutableBytes(
                positionPlayerJobAddress + 0x8D8,
                positionShowSequence,
                sizeof(positionShowSequence221)) &&
            DecodesRelativeCallTarget(
                positionPlayerJobAddress + 0x8F2,
                showAddress) &&
            MatchesExpectedNGRva(
                positionPlayerJobAddress + 0x8F7,
                0xD56897, 0xD56C27);

        auto install = [](
            std::uintptr_t address, const std::uint8_t* expected,
            std::size_t expectedSize, void* detour, void** original,
            std::atomic<bool>& installed, const char* name) -> bool {
            if (installed.load(std::memory_order_acquire)) return true;
            if (!MatchesExecutableBytes(address, expected, expectedSize)) {
                logger::warn(
                    "NG: {} hook skipped — verified AE prologue mismatch at {:x}",
                    name, address);
                return false;
            }
            void* target = reinterpret_cast<void*>(address);
            const MH_STATUS create = MH_CreateHook(target, detour, original);
            if (create != MH_OK || !*original) {
                logger::warn("NG: {} hook create failed ({})", name, int(create));
                return false;
            }
            if (MH_EnableHook(target) != MH_OK) {
                MH_RemoveHook(target);
                *original = nullptr;
                logger::warn("NG: {} hook enable failed", name);
                return false;
            }
            installed.store(true, std::memory_order_release);
            logger::info(
                "NG: TES::{} hook installed at {:x} (native visual boundary)",
                name, address);
            return true;
        };

        const bool showInstalled = install(
            showAddress, showPrologue, sizeof(showPrologue),
            reinterpret_cast<void*>(&HookedShowLoadingMenu),
            reinterpret_cast<void**>(&s_origShowLoadingMenu),
            s_showLoadingMenuHookInstalled, "ShowLoadingMenu");
        const bool hideInstalled = showInstalled && install(
                hideAddress, hidePrologue, sizeof(hidePrologue),
                reinterpret_cast<void*>(&HookedHideLoadingMenu),
                reinterpret_cast<void**>(&s_origHideLoadingMenu),
                s_hideLoadingMenuHookInstalled, "HideLoadingMenu");
        if (!showInstalled || !hideInstalled) {
            // Never run a half-paired visual lifecycle. Remove only hooks this
            // function successfully owns; another MinHook client's target was
            // rejected at CreateHook and is not touched here.
            s_ngBoundaryHooksPoisoned.store(
                true, std::memory_order_release);
            auto disableOwned = [](
                std::atomic<bool>& installed,
                void* target,
                const char* name) {
                if (!installed.load(std::memory_order_acquire)) {
                    return true;
                }
                const auto status = MH_DisableHook(target);
                if (status != MH_OK && status != MH_ERROR_DISABLED) {
                    logger::error(
                        "NG: {} rollback disable failed ({}); "
                        "original pointer retained",
                        name, int(status));
                    return false;
                }
                installed.store(false, std::memory_order_release);
                return true;
            };
            void* showHookTarget = reinterpret_cast<void*>(showAddress);
            void* hideHookTarget = reinterpret_cast<void*>(hideAddress);
            const bool showDisabled = disableOwned(
                s_showLoadingMenuHookInstalled,
                showHookTarget,
                "ShowLoadingMenu");
            const bool hideDisabled = disableOwned(
                s_hideLoadingMenuHookInstalled,
                hideHookTarget,
                "HideLoadingMenu");
            if (showDisabled && hideDisabled &&
                (showInstalled || hideInstalled)) {
                // Retain disabled MinHook objects and original trampolines for
                // process lifetime. A thread may already have crossed the old
                // entry before DisableHook patched it back; removing/clearing
                // here could invalidate that in-flight original call.
                logger::warn(
                    "NG: disabled half-pair hook/trampoline retained "
                    "for in-flight-call safety");
            }
            logger::warn(
                "NG: exact LoadingMenu boundary pair unavailable; "
                "custom visuals disabled, native LoadingMenu retained");
            return;
        }
        if (positionExteriorShowContract) {
            s_ngPositionPlayerJobAddress.store(
                positionPlayerJobAddress, std::memory_order_relaxed);
            // Publish last: the detour's acquire read may use the worker
            // address and caller-stack layout only after every byte contract
            // and the decoded Show target have passed.
            s_ngPositionExteriorShowContractReady.store(
                true, std::memory_order_release);
            logger::info(
                "NG: PositionPlayerJob exterior-loading-menu contract "
                "verified at {:x}",
                positionPlayerJobAddress);
        } else {
            s_ngPositionExteriorShowContractReady.store(
                false, std::memory_order_release);
            s_ngPositionPlayerJobAddress.store(
                0, std::memory_order_relaxed);
            logger::warn(
                "NG: PositionPlayerJob exterior-loading-menu contract "
                "mismatch; first-Show correction disabled fail closed");
        }
        s_ngNativeMenuVisible.store(
            ReadAELoadingMenuShown(), std::memory_order_release);
    }

    // Install the NG door-load "open" hooks (TES::LoadInterior / LoadExterior).
    // The shared Address Library IDs and exact entry bytes are verified on AE
    // 1.11.221 and 1.11.240. Other NG builds skip these (door-transition
    // loadscreens stay vanilla there) rather than risk a wrong-address hook.
    // Save-load detection (kPreLoadGame) is unaffected.
    static void InstallNGLoadHooks()
    {
        if (s_ngLoadHookPairReady.load(std::memory_order_acquire)) return;
        if (s_ngLoadHookPairPoisoned.load(std::memory_order_acquire)) {
            logger::warn(
                "NG: door-load hook pair remains disabled after an earlier "
                "ownership/apply failure");
            return;
        }
        if (!REL::Module::IsNG() || REL::Module::IsVR()) return;
        const auto ver = REL::Module::get().version();
        if (!(ver[0] == 1 && ver[1] == 11 &&
              (ver[2] == 221 || ver[2] == 240) && ver[3] == 0)) {
            logger::info(
                "NG: door-load hooks are unavailable on {}.{}.{}.{} — "
                "door-transition loadscreens off on this build",
                ver[0], ver[1], ver[2], ver[3]);
            return;
        }
        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) return;

        static constexpr std::uint64_t kLoadInteriorID = 2192047;
        static constexpr std::uint64_t kLoadExteriorID = 2192048;
        REL::Relocation<std::uintptr_t> loadInteriorTarget{
            REL::ID(kLoadInteriorID)
        };
        REL::Relocation<std::uintptr_t> loadExteriorTarget{
            REL::ID(kLoadExteriorID)
        };
        const auto loadInteriorAddress = loadInteriorTarget.address();
        const auto loadExteriorAddress = loadExteriorTarget.address();
        // Exact Fallout4.exe AE entry bytes, verified against both installed
        // images and the symbolized 1.11.240 program. Validate
        // enough instructions for MinHook's entry trampoline before changing
        // either target; another plugin's existing detour fails closed.
        static constexpr std::uint8_t loadInteriorPrologue[] = {
            0x4C, 0x89, 0x44, 0x24, 0x18, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70
        };
        static constexpr std::uint8_t loadExteriorPrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
            0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57
        };
        if (!MatchesExpectedNGRva(
                loadInteriorAddress, 0x2D03D0, 0x2D06F0) ||
            !MatchesExpectedNGRva(
                loadExteriorAddress, 0x2D0AE0, 0x2D0E00) ||
            !MatchesExecutableBytes(
                loadInteriorAddress,
                loadInteriorPrologue,
                sizeof(loadInteriorPrologue)) ||
            !MatchesExecutableBytes(
                loadExteriorAddress,
                loadExteriorPrologue,
                sizeof(loadExteriorPrologue))) {
            logger::error(
                "NG: door-load hook pair failed exact RVA/prologue "
                "validation; both hooks remain disabled");
            return;
        }

        void* const interiorTarget =
            reinterpret_cast<void*>(loadInteriorAddress);
        void* const exteriorTarget =
            reinterpret_cast<void*>(loadExteriorAddress);
        bool interiorCreated = false;
        bool exteriorCreated = false;
        auto removeNeverEnabled = [&]() {
            s_ngLoadHookPairPoisoned.store(
                true, std::memory_order_release);
            auto removeOne = [](
                bool created,
                void* target,
                auto& original,
                const char* name) {
                if (!created) {
                    original = nullptr;
                    return;
                }
                const auto removeStatus = MH_RemoveHook(target);
                if (removeStatus == MH_OK ||
                    removeStatus == MH_ERROR_NOT_CREATED) {
                    original = nullptr;
                    return;
                }
                // A failed removal may leave the queued enable attached to
                // this hook record. Replace it with a queued disable, retain
                // the original trampoline, and poison retries. If another
                // MinHook ApplyQueued occurs later, an unexpected detour can
                // still pass through safely while pairReady remains false.
                const auto queueDisableStatus =
                    MH_QueueDisableHook(target);
                logger::error(
                    "NG: {} pre-apply hook removal failed ({}); "
                    "record/trampoline retained, queueDisable={}",
                    name, int(removeStatus), int(queueDisableStatus));
            };
            removeOne(
                interiorCreated,
                interiorTarget,
                s_origLoadInterior,
                "TES::LoadInterior");
            removeOne(
                exteriorCreated,
                exteriorTarget,
                s_origLoadExterior,
                "TES::LoadExterior");
        };

        s_origLoadInterior = nullptr;
        auto status = MH_CreateHook(
            interiorTarget,
            reinterpret_cast<void*>(&HookedLoadInterior),
            reinterpret_cast<void**>(&s_origLoadInterior));
        interiorCreated = status == MH_OK;
        if (!interiorCreated || !s_origLoadInterior) {
            logger::error(
                "NG: TES::LoadInterior hook create failed ({}); "
                "door-load pair disabled",
                int(status));
            removeNeverEnabled();
            return;
        }

        s_origLoadExterior = nullptr;
        status = MH_CreateHook(
            exteriorTarget,
            reinterpret_cast<void*>(&HookedLoadExterior),
            reinterpret_cast<void**>(&s_origLoadExterior));
        exteriorCreated = status == MH_OK;
        if (!exteriorCreated || !s_origLoadExterior) {
            logger::error(
                "NG: TES::LoadExterior hook create failed ({}); "
                "door-load pair rolled back",
                int(status));
            removeNeverEnabled();
            return;
        }

        // Queue both operations and apply them while MinHook has the other
        // threads suspended. This eliminates the one-at-a-time enable window
        // in which a LoadInterior detour could run before LoadExterior existed.
        status = MH_QueueEnableHook(interiorTarget);
        if (status != MH_OK) {
            logger::error(
                "NG: TES::LoadInterior hook queue failed ({}); "
                "door-load pair rolled back",
                int(status));
            removeNeverEnabled();
            return;
        }
        status = MH_QueueEnableHook(exteriorTarget);
        if (status != MH_OK) {
            logger::error(
                "NG: TES::LoadExterior hook queue failed ({}); "
                "door-load pair rolled back",
                int(status));
            removeNeverEnabled();
            return;
        }

        status = MH_ApplyQueued();
        if (status != MH_OK) {
            // ApplyQueued normally publishes both entries under one suspended-
            // thread transaction, but treat every failure as potentially
            // callable. Disable both and retain the hook records + original
            // trampolines for process lifetime; a detour that was already
            // entered will see pairReady=false and safely pass through.
            s_ngLoadHookPairPoisoned.store(
                true, std::memory_order_release);
            const auto interiorDisable = MH_DisableHook(interiorTarget);
            const auto exteriorDisable = MH_DisableHook(exteriorTarget);
            logger::error(
                "NG: transactional door-load hook apply failed ({}) — "
                "pair disabled/retained (interiorDisable={}, "
                "exteriorDisable={})",
                int(status), int(interiorDisable), int(exteriorDisable));
            return;
        }

        s_ngLoadHookPairReady.store(true, std::memory_order_release);
        logger::info(
            "NG: transactional door-load hook pair installed "
            "(LoadInterior={:x}, LoadExterior={:x})",
            loadInteriorAddress, loadExteriorAddress);
    }

    void OnGameSessionLoaded();

    // Push the exterior-only door-preload paths to live atomics. Called at init
    // and after every config reload, so supported MCM changes apply immediately.
    // It is also safe before Install(): the values are published to atomics and
    // consumed when the poller/sink is created later in game-data-ready startup.
    static void ApplyDoorPrefetchConfig(const Config& config)
    {
        s_legacyExteriorPresentationPolicy.store(
            static_cast<std::uint32_t>(config.loadingScreenMode & 0xFF) |
                (config.benchmarkMode != 0 ? 0x100U : 0U),
            std::memory_order_release);
        // The historical crosshair path is deliberately kept disabled even when
        // an old user INI still contains bPrefetchCellOnCrosshairDoor=1.
        DoorPrefetch::SetEnabled(false);

        // Exact PositionPlayerJob/Show contracts prove and correct only a
        // plugin-induced exterior minimal classification. This capability is
        // independent of tip capture/compositor readiness and is published to
        // DoorPrefetch before any successful PreloadWorld may arm provenance.
        const bool exactPresentationCorrectionReady =
            (REL::Module::IsNG() && NGExactBoundaryHooksReady() &&
             s_ngPositionExteriorShowContractReady.load(
                 std::memory_order_acquire)) ||
            (!REL::Module::IsNG() &&
             s_legacyPositionExteriorShowContractReady.load(
                 std::memory_order_acquire));
        DoorPrefetch::SetExteriorPresentationCorrectionReady(
            exactPresentationCorrectionReady);
        const bool exteriorPreloadEnabled =
            Policy::ShouldEnableExteriorPreloadForPresentation(
                config.prefetchExteriorGates,
                exactPresentationCorrectionReady,
                config.loadingScreenMode);
        DoorPrefetch::SetExteriorGateProximity(
            exteriorPreloadEnabled,
            static_cast<float>(config.exteriorGateDistanceCells * 4096),
            0);
        // The optional long Havok ray is retired. Keep it disabled even when an
        // older user settings file still contains iExtendedRange.
        DoorPrefetch::SetExtendedRay(false, 2.0f);
        logger::info(
            "DoorPrefetch config: crosshair={} exteriorGate={} "
            "exteriorGateRequested={} gateDistance={} arrivalMode={} "
            "extendedRay={} rayMultiplier={:.0f} presentationGuard={}",
            false,
            exteriorPreloadEnabled,
            config.prefetchExteriorGates,
            config.exteriorGateDistanceCells * 4096,
            "engine-single-cell",
            false,
            2.0f,
            exactPresentationCorrectionReady ?
                (REL::Module::IsNG() ?
                    "exact-ng" : (REL::Module::IsVR() ?
                        "exact-vr" : "exact-og")) :
                Policy::IsTipPresentationMode(
                    config.loadingScreenMode) ?
                    "tip-mode-no-correction" :
                    "presentation-independent");
    }

    static void RestoreLoadBudgetsWithRetry(
        std::uint64_t a_generation,
        unsigned a_attemptsRemaining)
    {
        if (!Policy::ShouldRestoreFlatLoadBudget(
                a_generation,
                s_flatBudgetGeneration.load(std::memory_order_acquire),
                s_flatBudgetLoadActive.load(std::memory_order_acquire))) {
            return;
        }
        if (GameSettingTweaks::EndLoad()) {
            return;
        }
        if (a_attemptsRemaining == 0) {
            logger::error(
                "GameSettingTweaks: load-budget restore still pending after "
                "bounded retries; snapshot retained for the next safe task");
            return;
        }
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([a_generation, a_attemptsRemaining]() {
                RestoreLoadBudgetsWithRetry(
                    a_generation, a_attemptsRemaining - 1);
            });
        } else {
            logger::warn(
                "GameSettingTweaks: retry deferred because task interface is unavailable");
        }
    }

    static void RunNGEndLoadTweaks(
        std::uint64_t a_generation,
        unsigned a_attemptsRemaining)
    {
        if (a_generation !=
                s_ngTweaksGeneration.load(std::memory_order_acquire) ||
            !s_ngTweaksActive.load(std::memory_order_acquire)) {
            return;
        }
        if (GameSettingTweaks::EndLoad()) {
            s_ngTweaksActive.store(false, std::memory_order_release);
            return;
        }
        if (a_attemptsRemaining == 0) {
            logger::error(
                "GameSettingTweaks: NG load-budget restore remains pending; "
                "state retained for the next close");
            return;
        }
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([a_generation, a_attemptsRemaining]() {
                RunNGEndLoadTweaks(
                    a_generation, a_attemptsRemaining - 1);
            });
        }
    }

    static void QueueNGEndLoadTweaks()
    {
        if (!s_ngTweaksActive.load(std::memory_order_acquire)) return;
        const auto generation = s_ngTweaksGeneration.load(std::memory_order_acquire);
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([generation]() {
                // A new synchronous load may have started before this queued
                // restore reached the game thread. Its generation inherits the
                // still-active budgets; never restore them underneath that load.
                if (generation != s_ngTweaksGeneration.load(std::memory_order_acquire)) {
                    return;
                }
                RunNGEndLoadTweaks(generation, 4);
            });
        } else {
            logger::warn("GameSettingTweaks: NG end-load task skipped (task interface unavailable)");
        }
    }

    static void QueuePersistentTweaks()
    {
        const auto generation =
            s_persistentTweaksGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        if (auto* tasks = F4SE::GetTaskInterface()) {
            tasks->AddTask([generation]() {
                if (generation !=
                    s_persistentTweaksGeneration.load(
                        std::memory_order_acquire)) {
                    return;
                }
                const bool nativeLoadActive =
                    s_flatLoadEntryArmed.load(std::memory_order_acquire) ||
                    s_flatNativeLoadingMenuOpen.load(
                        std::memory_order_acquire) ||
                    s_flatNativeCloseInProgress.load(
                        std::memory_order_acquire) ||
                    s_ngNativeMenuVisible.load(std::memory_order_acquire) ||
                    s_ngLoadingActive.load(std::memory_order_acquire) ||
                    s_ngEnablePending.load(std::memory_order_acquire) ||
                    s_ngClosePending.load(std::memory_order_acquire) ||
                    s_ngNextLoadEntryArmed.load(std::memory_order_acquire) ||
                    s_ngPresentationGapClosed.load(
                        std::memory_order_acquire) ||
                    LoadingScreenManager::GetSingleton().IsInLoadingScreen();
                if (nativeLoadActive) {
                    // The matching close/config-refresh queues a newer
                    // generation. Never let fade/preload lookups or writes leak
                    // into a chained load's measured/I/O interval.
                    logger::info(
                        "GameSettingTweaks: persistent apply deferred "
                        "across active load");
                    return;
                }
                GameSettingTweaks::Apply();
            });
        } else {
            logger::warn(
                "GameSettingTweaks: persistent apply skipped "
                "(task interface unavailable)");
        }
    }

    static void NGFinishPresentationGapIfNeeded()
    {
        if (s_ngPresentationGapClosed.exchange(
                false, std::memory_order_acq_rel)) {
            LoadingScreenManager::GetSingleton()
                .FinishChainedLoadingMenuGap();
            // The visual-only gap deliberately retained every performance
            // patch across the active engine load. Its no-Show outcome is the
            // first safe point to restore those settings.
            PerformancePatches::OnLoadingMenuClose();
        }
    }

    static void NGCancelNextLoadWithoutShow(const char* a_reason)
    {
        s_ngNextLoadEntryArmed.store(false, std::memory_order_relaxed);
        s_ngNextLoadBodyReturned.store(false, std::memory_order_relaxed);
        s_ngNextLoadWaitsForPost.store(false, std::memory_order_relaxed);
        s_ngSaveClassificationArmed.store(
            false, std::memory_order_relaxed);
        s_ngEnableFromSaveLoad.store(
            false, std::memory_order_relaxed);
        s_ngIsInterior.store(false, std::memory_order_relaxed);
        s_ngEnablePending.store(false, std::memory_order_relaxed);
        s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
        s_ngLoadHookPending.store(false, std::memory_order_relaxed);
        s_ngClosePending.store(false, std::memory_order_relaxed);
        s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
        s_ngHideMenuSignal.store(false, std::memory_order_relaxed);
        s_ngChainedShowSignal.store(false, std::memory_order_relaxed);
        PapyrusOptimizer::GetSingleton().SetLoading(false);
        NGFinishPresentationGapIfNeeded();
        QueueNGEndLoadTweaks();
        DoorPrefetch::EndTransition();
        ResumePostLoadSystemsAtNativeClose();
        QueueNGConfigRefresh("ng-next-load-ended-without-show");
        logger::info(
            "NG: preserved next load ended without native Show ({}) — "
            "nonvisual state unwound after its own outcome",
            a_reason ? a_reason : "unknown");
    }

    static bool NGClosePresentationForPendingEntry(
        const char* a_reason, long long a_elapsedMs)
    {
        if (!s_ngLoadingActive.exchange(
                false, std::memory_order_acq_rel)) {
            return false;
        }
        D3D11Compositor::GetSingleton().SetEnabled(false);
        LoadingScreenManager::GetSingleton()
            .CloseForChainedLoadingMenu();
        // Publish only after CloseForChainedLoadingMenu has left its DDS barrier
        // raised. Performance patches remain active too; a no-Show outcome can
        // now release/restore both explicitly.
        s_ngPresentationGapClosed.store(
            true, std::memory_order_release);
        PapyrusOptimizer::GetSingleton().SetLoading(true);
        logger::info(
            "NG: old presentation closed after {}ms ({}); "
            "new engine-load state preserved pending native Show/outcome",
            a_elapsedMs, a_reason ? a_reason : "native Hide gap");
        return true;
    }

    // Force-disable the NG compositor and run the close-side cleanup. Shared by the
    // normal deferred close and the watchdog. Returns true if it actually disabled
    // (i.e. the compositor was active). Render-thread only.
    static bool NGForceClose(const char* reason, long long elapsedMs)
    {
        s_ngNextLoadEntryArmed.store(false, std::memory_order_relaxed);
        s_ngNextLoadBodyReturned.store(false, std::memory_order_relaxed);
        s_ngNextLoadWaitsForPost.store(false, std::memory_order_relaxed);
        s_ngSaveClassificationArmed.store(
            false, std::memory_order_relaxed);
        s_ngEnableFromSaveLoad.store(
            false, std::memory_order_relaxed);
        s_ngIsInterior.store(false, std::memory_order_relaxed);
        s_ngChainedShowSignal.store(
            false, std::memory_order_relaxed);
        s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
        s_ngLoadHookPending.store(false, std::memory_order_relaxed);
        if (!s_ngLoadingActive.exchange(false)) {
            NGFinishPresentationGapIfNeeded();
            DoorPrefetch::EndTransition();
            ResumePostLoadSystemsAtNativeClose();
            QueueNGConfigRefresh("ng-native-close-no-custom");
            return false;
        }
        s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
        s_ngHideMenuSignal.store(false, std::memory_order_relaxed);
        D3D11Compositor::GetSingleton().SetEnabled(false);
        LoadingScreenManager::GetSingleton().OnLoadingMenuClose();
        s_ngPresentationGapClosed.store(
            false, std::memory_order_relaxed);
        PerformancePatches::OnLoadingMenuClose();
        QueueNGEndLoadTweaks();
        // NG uses kPreLoadGame/kPostLoadGame + the load hooks (no MenuOpenCloseEvent
        // — UI::GetSingleton is broken on NG), so the OG door-preload registration
        // site never runs here. The player is loaded now, so register the pick sink
        // (idempotent; guarded by s_registered inside DoorPrefetch).
        if (GetConfigSnapshot().benchmarkMode != 0) {
            DoorPrefetch::EnsureRegistered();
        }
        DoorPrefetch::EndTransition();
        ResumePostLoadSystemsAtNativeClose();
        QueueNGConfigRefresh("ng-native-close");
        logger::info("NG: deferred close fired after {}ms ({})", elapsedMs, reason);
        return true;
    }

    // Per-frame: consume the exact NG native-menu boundaries and the bounded
    // close/watchdog fallbacks. Called before custom composition by the flat
    // Present frame callback.
    static bool TryGetCurrentNGShowSelection(
        D3D11Compositor::FlatNativeLoadingSelectionSnapshot& a_snapshot)
    {
        auto& compositor = D3D11Compositor::GetSingleton();
        if (!compositor.TryGetFlatNativeLoadingSelection(a_snapshot)) {
            return false;
        }
        const auto baseline = s_ngShowSelectionBaselineSerial.load(
            std::memory_order_acquire);
        // Show owns a new initial SendLoadingText publication. Equality means
        // only the previous native menu's latch is visible; consuming it would
        // make a chained minimal screen inherit the old model/tip decision.
        return a_snapshot.initialSerial != baseline;
    }

    static void TickNGDeferred()
    {
        if (!REL::Module::IsNG() || !g_gameDataReady.load(std::memory_order_acquire)) return;
        std::lock_guard<std::recursive_mutex> boundaryLock(s_ngBoundaryMutex);
        if (s_ngBoundaryCallsInFlight != 0) {
            return;
        }
        const std::int64_t nowTicks =
            std::chrono::steady_clock::now().time_since_epoch().count();

        // A completed native Show can arrive while the previous custom
        // presentation is still active (Hide+Show between Present calls). Re-arm
        // every per-load cache/patch using this Show's classification before this
        // frame composites. The close half suppresses DDS preparation.
        if (s_ngChainedShowSignal.exchange(
                false, std::memory_order_acq_rel)) {
            if (s_ngLoadingActive.load(std::memory_order_acquire) &&
                s_ngNativeMenuVisible.load(std::memory_order_acquire)) {
                const Config config = GetConfigSnapshot();
                const bool isInterior =
                    s_ngIsInterior.exchange(
                        false, std::memory_order_acq_rel);
                const bool fromSave =
                    s_ngEnableFromSaveLoad.exchange(
                        false, std::memory_order_acq_rel);
                const bool interiorDoor = isInterior && !fromSave;
                D3D11Compositor::FlatNativeLoadingSelectionSnapshot
                    nativeSelection{};
                const bool exactSelection =
                    TryGetCurrentNGShowSelection(nativeSelection);
                int mode = Policy::SelectNGPresentationMode(
                    config.loadingScreenMode,
                    exactSelection ? nativeSelection.selection.model :
                        Policy::NativeModelSelection::kUnknown,
                    exactSelection ? nativeSelection.selection.content :
                        Policy::NativeLoadingContent::kUnknown);
                auto& manager = LoadingScreenManager::GetSingleton();
                manager.CloseForChainedLoadingMenu();
                PerformancePatches::OnLoadingMenuClose();
                if (exactSelection && mode == 3 &&
                    !D3D11Compositor::GetSingleton().
                        RestoreFlatNativeLoadingSelectionForUpcomingOpen(
                            nativeSelection)) {
                    // Mode 3 cannot preserve the new Show's exact native tip
                    // after the visual-only chained CLOSE. Keep the custom
                    // background without inventing or replaying missing UI.
                    mode = 2;
                    logger::warn(
                        "NG: chained native selection restore failed; "
                        "using background-only custom fallback");
                }
                manager.SetBackgroundsEnabled(
                    config.enableBackgroundImages);
                auto& compositor = D3D11Compositor::GetSingleton();
                compositor.SetFlatMode(mode);
                compositor.SetDisableVSyncWhileLoading(
                    config.perfConfig.disableVSyncWhileLoading);
                manager.OnLoadingMenuOpen();
                PerformancePatches::OnLoadingMenuOpen();
                logger::info(
                    "NG: chained native-show presentation re-armed "
                    "(mode={}, configuredMode={}, class={}, exact={}, "
                    "model={}, content={})",
                    mode, config.loadingScreenMode,
                    interiorDoor ? "interior-door" : "worldspace/save",
                    exactSelection,
                    exactSelection && nativeSelection.selection.model ==
                            Policy::NativeModelSelection::kModel ?
                        "model" : exactSelection ? "no-model" : "unknown",
                    exactSelection ?
                        Policy::NativeLoadingContentName(
                            nativeSelection.selection.content) :
                        "unknown");
            } else {
                s_ngIsInterior.store(false, std::memory_order_relaxed);
                s_ngEnableFromSaveLoad.store(
                    false, std::memory_order_relaxed);
            }
        }

        // Deferred enable. The render tick is the SOLE owner of the enable/close
        // decision; the messaging thread only sets s_ngEnablePending + timestamp
        // (kPreLoadGame / NGArmLoad) and s_ngPostLoadArrived + timestamp
        // (kPostLoadGame). That single-owner design removes the old cross-thread
        // check-then-act race where kPostLoadGame's exchange could steal the pending
        // flag mid-decision and leave the compositor enabled with no close armed.
        // acquire pairs with the release store of the pending flag so the timestamp
        // read is guaranteed fresh, never a stale prior-load value.
        if (s_ngEnablePending.load(std::memory_order_acquire)) {
            const std::int64_t preTicks = s_ngPreLoadTimeTicks.load(std::memory_order_relaxed);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::duration(nowTicks - preTicks));
            const bool exactBoundaryHooksReady =
                NGExactBoundaryHooksReady();
            const bool nativeShowFired =
                exactBoundaryHooksReady &&
                s_ngShowMenuSignal.exchange(false, std::memory_order_acq_rel);
            if (nativeShowFired &&
                s_ngNativeMenuVisible.load(std::memory_order_acquire)) {
                s_ngEnableTimeTicks.store(nowTicks, std::memory_order_relaxed);
                s_ngLoadingActive.store(true, std::memory_order_release);
                s_ngEnablePending.store(false, std::memory_order_relaxed);
                // The synchronous game-thread arm path already reloaded and
                // published this config. Present must remain memory-only.
                const Config config = GetConfigSnapshot();
                auto& lsm = LoadingScreenManager::GetSingleton();
                lsm.SetBackgroundsEnabled(config.enableBackgroundImages);
                const bool isInterior = s_ngIsInterior.exchange(false, std::memory_order_relaxed);
                const bool fromSave =
                    s_ngEnableFromSaveLoad.exchange(
                        false, std::memory_order_acq_rel);
                const bool interiorDoor = isInterior && !fromSave;
                D3D11Compositor::FlatNativeLoadingSelectionSnapshot
                    nativeSelection{};
                const bool exactSelection =
                    TryGetCurrentNGShowSelection(nativeSelection);
                const int ngMode = Policy::SelectNGPresentationMode(
                    config.loadingScreenMode,
                    exactSelection ? nativeSelection.selection.model :
                        Policy::NativeModelSelection::kUnknown,
                    exactSelection ? nativeSelection.selection.content :
                        Policy::NativeLoadingContent::kUnknown);
                auto& compositor = D3D11Compositor::GetSingleton();
                compositor.SetFlatMode(ngMode);
                compositor.SetDisableVSyncWhileLoading(
                    config.perfConfig.disableVSyncWhileLoading);
                lsm.OnLoadingMenuOpen();
                // A prior visual-only gap kept the DDS barrier raised. OPEN now
                // owns that barrier again; its final native close will release it.
                s_ngPresentationGapClosed.store(
                    false, std::memory_order_relaxed);
                PerformancePatches::OnLoadingMenuOpen();
                logger::info("NG: native-show enable fired after {}ms, mode={} "
                    "configuredMode={} ({}), exact={}, model={}, content={}, "
                    "visuals={}",
                    elapsed.count(), ngMode, config.loadingScreenMode,
                    interiorDoor ? "interior door" : "worldspace/save",
                    exactSelection,
                    exactSelection && nativeSelection.selection.model ==
                            Policy::NativeModelSelection::kModel ?
                        "model" : exactSelection ? "no-model" : "unknown",
                    exactSelection ?
                        Policy::NativeLoadingContentName(
                            nativeSelection.selection.content) :
                        "unknown",
                    ngMode == 1 ? "native" :
                        compositor.IsEnabled() ? "custom" : "native fallback");
            } else if (s_ngPostLoadArrived.load(std::memory_order_acquire)) {
                // The load already ENDED (kPostLoadGame arrived) before we engaged.
                // This is either an instant popup (kPre+kPost ~1ms apart) or a load
                // whose exact native Show/Hide both completed before this tick.
                // Never engage over live gameplay; a still-visible verified Show
                // is handled by the branch above even if kPost arrived early.
                s_ngEnablePending.store(false, std::memory_order_relaxed);
                s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
                s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
                s_ngIsInterior.store(false, std::memory_order_relaxed);
                s_ngSaveClassificationArmed.store(
                    false, std::memory_order_relaxed);
                s_ngEnableFromSaveLoad.store(
                    false, std::memory_order_relaxed);
                s_ngChainedShowSignal.store(
                    false, std::memory_order_relaxed);
                s_ngNextLoadEntryArmed.store(
                    false, std::memory_order_relaxed);
                s_ngNextLoadBodyReturned.store(
                    false, std::memory_order_relaxed);
                s_ngNextLoadWaitsForPost.store(
                    false, std::memory_order_relaxed);
                NGFinishPresentationGapIfNeeded();
                QueueNGEndLoadTweaks();
                DoorPrefetch::EndTransition();
                ResumePostLoadSystemsAtNativeClose();
                QueueNGConfigRefresh("ng-load-ended-before-enable");
                logger::info("NG: load ended before enable ({}ms) — compositor never engaged", elapsed.count());
            }
        }

        // Save-load close arming (render-owned). A save load ends with kPostLoadGame
        // (not the LoadInterior/LoadExterior HideLoadingMenu path), so its close is
        // armed here when kPost arrived while the compositor is active and no close
        // is pending yet. kPost already recorded s_ngLastPostLoadTimeTicks. The
        // HideLoadingMenu hook (which also fires for save loads) sets the precise
        // s_ngHideMenuSignal that the close block below consumes.
        if (s_ngLoadingActive.load(std::memory_order_relaxed) &&
            s_ngPostLoadArrived.load(std::memory_order_acquire) &&
            !s_ngClosePending.load(std::memory_order_relaxed)) {
            if (s_ngNextLoadEntryArmed.load(
                    std::memory_order_acquire)) {
                // This outcome belongs to the newer entry, not the old custom
                // presentation still awaiting its Hide→Present close. Keep it
                // published until that entry either Shows or is cancelled.
                if (NGExactBoundaryHooksReady() &&
                    s_ngNativeMenuVisible.load(std::memory_order_acquire)) {
                    logger::info(
                        "NG: next-load kPost arrived; awaiting its native Show "
                        "or the old presentation close");
                } else {
                    s_ngHideMenuSignal.store(
                        true, std::memory_order_relaxed);
                    s_ngClosePending.store(
                        true, std::memory_order_release);
                    logger::info(
                        "NG: next-load kPost armed old-presentation close "
                        "(no native menu visible)");
                }
            } else {
                s_ngPostLoadArrived.store(
                    false, std::memory_order_relaxed);
                if (NGExactBoundaryHooksReady() &&
                    s_ngNativeMenuVisible.load(std::memory_order_acquire)) {
                    logger::info(
                        "NG: kPostLoadGame arrived; awaiting native HideLoadingMenu");
                } else {
                    s_ngClosePending.store(true, std::memory_order_release);
                    logger::info(
                        "NG: kPostLoadGame close armed (boundary fallback)");
                }
            }
        }

        // A new native Show can follow a Hide between two Present calls. Never
        // let a stale close signal/timer from the previous menu close the custom
        // presentation while the paired hook says a native menu is open.
        const bool exactNativeMenuOpen =
            NGExactBoundaryHooksReady() &&
            s_ngNativeMenuVisible.load(std::memory_order_acquire);

        // A visual-only old close can leave the new engine entry alive with no
        // custom presentation. Do not infer cancellation from elapsed time: a
        // save owns kPostLoadGame, while a door/cell load owns its synchronous
        // return. Only that load's explicit outcome may unwind the preserved
        // budgets/transition barrier when no Show ever arrived.
        if (s_ngNextLoadEntryArmed.load(std::memory_order_acquire) &&
            !s_ngLoadingActive.load(std::memory_order_acquire) &&
            !exactNativeMenuOpen) {
            const bool waitsForPost =
                s_ngNextLoadWaitsForPost.load(std::memory_order_acquire);
            const bool ownOutcome = waitsForPost
                ? s_ngPostLoadArrived.load(std::memory_order_acquire)
                : s_ngNextLoadBodyReturned.load(std::memory_order_acquire);
            if (ownOutcome) {
                NGCancelNextLoadWithoutShow(
                    waitsForPost ? "kPostLoadGame" : "synchronous return");
            }
        }

        // Deferred close: fire when EITHER the TES::HideLoadingMenu hook
        // signalled the menu actually hid (precise — fires within ~1 frame of
        // the real close) OR the kNGCloseDelay timer expired (fallback for
        // when the hook didn't install / didn't fire). Whichever comes first.
        if (s_ngClosePending.load(std::memory_order_acquire) &&
            !exactNativeMenuOpen) {
            const std::int64_t postTicks = s_ngLastPostLoadTimeTicks.load(std::memory_order_relaxed);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::duration(nowTicks - postTicks));
            const bool hookFired = s_ngHideMenuSignal.load(std::memory_order_relaxed);
            if (hookFired || elapsed >= kNGCloseDelay) {
                s_ngClosePending.store(false, std::memory_order_relaxed);
                s_ngHideMenuSignal.store(false, std::memory_order_relaxed);
                s_ngShowMenuSignal.store(false, std::memory_order_relaxed);
                // The load ended before a verified native Show boundary ever
                // fired (a menu-less/scripted transition or cancelled load).
                // Cancel the still-pending enable instead of clearing the
                // close signal and letting the enable turn the compositor on with
                // no close left to match it — THAT was the elevator/scripted
                // permanent-black-screen bug. Nothing to disable here; just unwind.
                if (s_ngEnablePending.exchange(false, std::memory_order_relaxed)) {
                    s_ngIsInterior.store(false, std::memory_order_relaxed);  // don't leak the mode into the next load
                    s_ngSaveClassificationArmed.store(
                        false, std::memory_order_relaxed);
                    s_ngEnableFromSaveLoad.store(
                        false, std::memory_order_relaxed);
                    s_ngChainedShowSignal.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadEntryArmed.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadBodyReturned.store(
                        false, std::memory_order_relaxed);
                    s_ngNextLoadWaitsForPost.store(
                        false, std::memory_order_relaxed);
                    NGFinishPresentationGapIfNeeded();
                    QueueNGEndLoadTweaks();
                    DoorPrefetch::EndTransition();
                    ResumePostLoadSystemsAtNativeClose();
                    QueueNGConfigRefresh("ng-load-ended-without-custom-open");
                    logger::info("NG: load ended without a native custom-screen open ({}ms) — pending enable cancelled",
                        elapsed.count());
                } else if (s_ngNextLoadEntryArmed.load(
                               std::memory_order_acquire)) {
                    const bool waitsForPost =
                        s_ngNextLoadWaitsForPost.load(
                            std::memory_order_acquire);
                    const bool ownOutcome = waitsForPost
                        ? s_ngPostLoadArrived.load(
                              std::memory_order_acquire)
                        : s_ngNextLoadBodyReturned.load(
                              std::memory_order_acquire);
                    if (ownOutcome) {
                        if (s_ngLoadingActive.load(
                                std::memory_order_acquire)) {
                            NGForceClose(
                                waitsForPost
                                    ? "next load ended without Show (kPost)"
                                    : "next load ended without Show (return)",
                                elapsed.count());
                        } else {
                            NGCancelNextLoadWithoutShow(
                                waitsForPost
                                    ? "kPostLoadGame"
                                    : "synchronous return");
                        }
                    } else {
                        NGClosePresentationForPendingEntry(
                            hookFired
                                ? "intervening native Hide"
                                : "intervening close timeout",
                            elapsed.count());
                    }
                } else {
                    NGForceClose(
                        hookFired ? "HideLoadingMenu signal" :
                            "timeout fallback",
                        elapsed.count());
                }
            }
        }

        // Watchdog only protects the boundary-fallback path. With the exact
        // paired hooks installed, a legitimately long load remains visible
        // until TES::HideLoadingMenu supplies the native close boundary.
        if (s_ngLoadingActive.load(std::memory_order_relaxed) &&
            !s_ngClosePending.load(std::memory_order_relaxed) &&
            !s_ngNextLoadEntryArmed.load(std::memory_order_acquire) &&
            !exactNativeMenuOpen) {
            const std::int64_t enTicks = s_ngEnableTimeTicks.load(std::memory_order_relaxed);
            const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::duration(nowTicks - enTicks));
            if (held >= kNGWatchdogTimeout) {
                if (NGForceClose("watchdog", held.count())) {
                    logger::warn("NG: watchdog force-closed compositor after {}ms with no close signal", held.count());
                }
            }
        }
    }

    // ========================================================================
    // Menu event handler for LoadingMenu open/close
    // ========================================================================

    class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuWatcher* GetSingleton()
        {
            static MenuWatcher instance;
            return &instance;
        }

        void RecordPassivePreLoadGame()
        {
            const auto nowMs = PassiveNowMs();
            std::lock_guard lock(m_passiveMutex);
            ++m_passiveAttempt;
            m_passiveAttemptActive = true;
            m_passiveAttemptStartMs = nowMs;
            m_passivePostDurationMs = -1;
            m_passivePostObserved = false;
            m_passiveLoadSucceeded = false;
            m_passiveAttemptMenuOrdinal = 0;
            m_passiveOpenAttempt = 0;
            m_passiveOpenOrdinal = 0;
            m_passiveFirstMenuOpenMs = -1;
            m_passivePreviousCloseMs = -1;
            m_passiveGapFromPreviousCloseMs = -1;
            // A new real save request supersedes an incomplete/unmatched menu.
            // Keep the monotonic menuSequence but discard that stale OPEN.
            m_passiveMenuTimer.open = false;
        }

        void RecordPassivePostLoadGame(bool a_succeeded)
        {
            const auto nowMs = PassiveNowMs();
            std::lock_guard lock(m_passiveMutex);
            if (!m_passiveAttemptActive) {
                return;
            }
            m_passivePostObserved = true;
            m_passiveLoadSucceeded = a_succeeded;
            m_passivePostDurationMs =
                std::max<std::int64_t>(0, nowMs - m_passiveAttemptStartMs);
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (s_passiveMeasurementOnly.load(std::memory_order_acquire)) {
                return ProcessPassiveEvent(a_event);
            }

            if (a_event.menuName == "MessageBoxMenu") {
                SaveGamePrefetch::OnMessageBoxMenu(a_event.opening);
            }

            if (a_event.menuName == "PauseMenu" && !a_event.opening &&
                g_gameDataReady.load(std::memory_order_acquire) &&
                !m_loadingMenuOpen) {
                // MCM commits its INI while the pause stack is still up. Consume
                // that completed write before gameplay/input resumes, covering
                // legacy door loads which do not emit kPreLoadGame without ever
                // doing disk IO at LoadingMenu OPEN.
                RefreshLiveConfigBeforeLoadIfDirty("pause-menu-close");
            }

            if (a_event.menuName == "MainMenu") {
                {
                    // Serialize both directions of the MainMenu latch with late
                    // session publication. Whichever event wins this mutex owns
                    // the final state: OPEN cannot be overwritten by a stale
                    // kPostLoadGame publication, while CLOSE makes the newly
                    // entered session eligible to publish.
                    std::lock_guard lifecycleLock(s_flatLifecycleMutex);
                    s_mainMenuOpen.store(
                        a_event.opening, std::memory_order_release);
                    if (a_event.opening) {
                        g_gameSessionLoaded.store(
                            false, std::memory_order_release);
                        s_pendingGameSessionLoaded.store(
                            false, std::memory_order_release);
                        s_flatLoadEntryArmed.store(
                            false, std::memory_order_release);
                        s_flatNativeCloseInProgress.store(
                            false, std::memory_order_release);
                        LoadingScreenManager::GetSingleton()
                            .ClearGameSessionLoaded();
                    }
                }
                if (a_event.opening) {
                    // No usable player cell exists in the main menu. Keep the
                    // prediction poller parked until a completed load/new game
                    // publishes the first live game session.
                    DoorPrefetch::SetGameSessionActive(false);
                    PapyrusOptimizer::GetSingleton().SetGameSessionActive(false);
                    PapyrusOptimizer::GetSingleton().SetLoading(false);
                    // Main-menu entry is outside every timed LoadingMenu and is
                    // early enough for save-confirm cache warming to consume a
                    // newly edited MCM value.
                    if (!m_loadingMenuOpen) {
                        RefreshConfigFromDisk("main-menu-open");
                    }
                }
                SaveGamePrefetch::OnMainMenu(a_event.opening);
                LoadingScreenManager::GetSingleton().SetMainMenuOpen(a_event.opening);
                logger::info("MainMenu {}", a_event.opening ? "OPEN" : "CLOSE");
            }

            if (a_event.menuName == "LoadingMenu") {
                std::lock_guard lifecycleLock(s_flatLifecycleMutex);
                // Startup loading screen (before full init) — skip normal processing
                if (!g_gameDataReady.load(std::memory_order_acquire)) {
                    logger::info("LoadingMenu {} (startup, pre-init)",
                        a_event.opening ? "OPEN" : "CLOSE");
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (a_event.opening) {
                    if (m_loadingMenuOpen) {
                        logger::warn(
                            "LoadingMenu duplicate OPEN ignored (load #{})",
                            m_loadCount);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    // Claim the exact visual boundary using only the already
                    // published snapshot. File IO here can leave Bethesda's
                    // title/3D screen visible before our compositor takes over.
                    s_flatLoadEntryArmed.store(
                        true, std::memory_order_release);
                    m_loadingMenuOpen = true;
                    s_flatNativeLoadingMenuOpen.store(
                        true, std::memory_order_release);
                    m_loadCount++;
                    m_loadStart = std::chrono::steady_clock::now();

                    // Consume the already-published snapshot at this exact
                    // visual boundary. Disk IO here delayed the custom OPEN
                    // relative to Bethesda's native LoadingMenu event and also
                    // competed with the first-load storage burst.
                    const Config config = GetConfigSnapshot();
                    if (config.benchmarkMode != 0) {
                        PapyrusOptimizer::GetSingleton().SetLoading(true);
                        auto& compositor =
                            D3D11Compositor::GetSingleton();
                        compositor.SetFlatMode(config.loadingScreenMode);
                        compositor.SetDisableVSyncWhileLoading(
                            config.perfConfig.disableVSyncWhileLoading);
                    }

                    // Native LoadingMenu OPEN is the custom visual boundary.
                    // Engage presentation before diagnostics or engine-budget
                    // bookkeeping so no deliberate work shifts the first custom
                    // frame later than Bethesda's normal 3D screen.
                    LoadingScreenManager::GetSingleton().OnLoadingMenuOpen();
                    logger::info("LoadingMenu OPEN (load #{}, mode={})",
                        m_loadCount, config.loadingScreenMode);

                    // Engine fade/budget tweaks for the duration of this load.
                    // Do not raise the process priority: Fallout's loader also
                    // depends on graphics, VR-runtime, audio, and storage-driver
                    // workers which an ABOVE_NORMAL game process can starve.
                    // Gated by benchmarkMode so baseline (0) stays unmodified.
                    if (config.benchmarkMode != 0) {
                        SaveGamePrefetch::OnLoadingMenu(true);
                        PerformancePatches::OnLoadingMenuOpen();
                        // Stop new door-prefetch submissions after presentation
                        // is engaged. The transition barrier drains only a plugin
                        // request already inside an engine entry.
                        DoorPrefetch::FlushQueuedLoads();
                        if (Policy::ShouldApplyLegacyLoadBudgets(
                                g_isVR, config.benchmarkMode)) {
                            m_budgetGeneration =
                                s_flatBudgetGeneration.fetch_add(
                                    1, std::memory_order_acq_rel) + 1;
                            s_flatBudgetLoadActive.store(
                                true, std::memory_order_release);
                            GameSettingTweaks::BeginLoad();
                        } else {
                            // March VR never wrote the later experimental
                            // 500/500 ms background-load drain budgets.
                            m_budgetGeneration = 0;
                        }
                        ApplyDoorPrefetchConfig(config);  // hot-apply exterior-gate settings
                    } else {
                        m_budgetGeneration = 0;
                    }

                    // This observer remains active in benchmarkMode=0. It only
                    // labels the load and snapshots settings; it never queues a
                    // cell or changes an engine value.
                    PreloadDiagnostics::SetLoading(
                        true, static_cast<std::uint64_t>(m_loadCount));

                } else {
                    if (!m_loadingMenuOpen) {
                        // Early VR registration can observe a startup CLOSE for
                        // a menu whose OPEN occurred before this sink existed.
                        // This instance owns no timer, budgets, patches, or
                        // presentation for it, so do not run close-side state.
                        logger::info(
                            "LoadingMenu unmatched CLOSE ignored (startup/late registration)");
                        s_flatLoadEntryArmed.store(
                            false, std::memory_order_release);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    s_flatNativeCloseInProgress.store(
                        true, std::memory_order_release);
                    m_loadingMenuOpen = false;
                    s_flatNativeLoadingMenuOpen.store(
                        false, std::memory_order_release);
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_loadStart).count();
                    const Config config = GetConfigSnapshot();
                    if (config.benchmarkMode != 0) {
                        PapyrusOptimizer::GetSingleton().SetLoading(false);
                    }

                    // Native LoadingMenu CLOSE is the custom visual boundary.
                    // Tear presentation down before logging, diagnostics, setting
                    // restoration, or door-prefetch registration.
                    auto& loadingManager =
                        LoadingScreenManager::GetSingleton();
                    loadingManager.OnLoadingMenuClose();
                    if (config.benchmarkMode != 0) {
                        SaveGamePrefetch::OnLoadingMenu(false);
                        PerformancePatches::OnLoadingMenuClose();
                        if (g_isVR &&
                            s_vrTimerPatchesPending.exchange(
                                false, std::memory_order_acq_rel) &&
                            !s_vrTimerPatchesApplied.exchange(
                                true, std::memory_order_acq_rel)) {
                            try {
                                PerformancePatches::ApplyTimerPatches(
                                    config.perfConfig);
                                logger::info(
                                    "VR March parity: deferred timer/iFPSClamp "
                                    "patches applied after first successful save CLOSE");
                            } catch (...) {
                                s_vrTimerPatchesApplied.store(
                                    false, std::memory_order_release);
                                logger::warn(
                                    "VR deferred timer patches failed; exact-byte "
                                    "checks left native code intact");
                            }
                        }
                    }

                    logger::info("LoadingMenu CLOSE (load #{}) — {:.2f}s",
                        m_loadCount, elapsed / 1000.0);
                    PreloadDiagnostics::SetLoading(
                        false, static_cast<std::uint64_t>(m_loadCount));

                    const auto budgetGeneration = m_budgetGeneration;
                    m_budgetGeneration = 0;
                    if (budgetGeneration != 0) {
                        // Publish the native CLOSE before any queued restore
                        // attempt. A task from an older load can no longer write
                        // underneath a newer generation.
                        if (budgetGeneration ==
                            s_flatBudgetGeneration.load(
                                std::memory_order_acquire)) {
                            s_flatBudgetLoadActive.store(
                                false, std::memory_order_release);
                        }
                        RestoreLoadBudgetsWithRetry(budgetGeneration);
                    }
                    if (config.benchmarkMode != 0) {
                        // Player singleton exists now — register the door-preload
                        // pick sink (idempotent; flat-only until VR offset RE'd).
                        DoorPrefetch::EnsureRegistered();
                    }

                    if (config.benchmarkMode != 0) {
                        DoorPrefetch::EndTransition();
                    }

                    // Process a deferred OG/VR session message only AFTER
                    // OnLoadingMenuClose. The manager has already classified
                    // the one first-title-save March transition from the exact
                    // pre-publication state; session publication remains safely
                    // ordered after the authoritative native CLOSE.
                    if (s_pendingGameSessionLoaded.exchange(false, std::memory_order_acq_rel)) {
                        if (s_mainMenuOpen.load(std::memory_order_acquire)) {
                            logger::info(
                                "Legacy: deferred session discarded because "
                                "MainMenu opened during CLOSE");
                        } else {
                            OnGameSessionLoaded();
                            logger::info(
                                "Legacy: deferred session message processed");
                        }
                    }
                    ResumePostLoadSystemsAtNativeClose();

                    PreloadDiagnostics::ObservePlayerCell("loading-menu-close");
                    s_flatNativeCloseInProgress.store(
                        false, std::memory_order_release);
                    // Cover a session message which arrived re-entrantly after
                    // the first exchange but before close-side bookkeeping
                    // finished.
                    if (s_pendingGameSessionLoaded.exchange(
                            false, std::memory_order_acq_rel)) {
                        if (s_mainMenuOpen.load(std::memory_order_acquire)) {
                            logger::info(
                                "Legacy: re-entrant late session discarded "
                                "because MainMenu is open");
                        } else {
                            OnGameSessionLoaded();
                            ResumePostLoadSystemsAtNativeClose();
                            logger::info(
                                "Legacy: late session message consumed at native CLOSE");
                        }
                    }
                    // Reload only after Bethesda's native CLOSE and after the
                    // measured interval has been recorded. Config/MCM file IO
                    // must never compete with the loader or extend a timed load.
                    RefreshConfigFromDisk("loading-menu-close");
                    s_flatLoadEntryArmed.store(
                        false, std::memory_order_release);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        static std::int64_t PassiveNowMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        RE::BSEventNotifyControl ProcessPassiveEvent(
            const RE::MenuOpenCloseEvent& a_event)
        {
            // Ignore every other menu. In particular, do not enter the normal
            // MainMenu/MessageBox paths that touch prefetch, manager, Papyrus,
            // config-refresh, task, or session state.
            if (a_event.menuName != "LoadingMenu") {
                return RE::BSEventNotifyControl::kContinue;
            }

            const auto nowMs = PassiveNowMs();
            std::lock_guard lock(m_passiveMutex);
            const auto result = m_passiveMenuTimer.Observe(
                a_event.opening, nowMs);
            if (result.boundary == Policy::PassiveMenuBoundary::kIgnored) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (result.boundary == Policy::PassiveMenuBoundary::kOpened) {
                m_passiveOpenAttempt = 0;
                m_passiveOpenOrdinal = 0;
                m_passiveGapFromPreviousCloseMs = -1;
                if (m_passiveAttemptActive) {
                    m_passiveOpenAttempt = m_passiveAttempt;
                    m_passiveOpenOrdinal = ++m_passiveAttemptMenuOrdinal;
                    if (m_passiveOpenOrdinal == 1) {
                        m_passiveFirstMenuOpenMs = nowMs;
                    } else if (m_passivePreviousCloseMs >= 0) {
                        m_passiveGapFromPreviousCloseMs =
                            std::max<std::int64_t>(
                                0, nowMs - m_passivePreviousCloseMs);
                    }
                }
                // Do not emit or flush at OPEN: the timed load has begun.
                return RE::BSEventNotifyControl::kContinue;
            }

            // Timestamp was captured before formatting or file IO. One line at
            // CLOSE contains all boundaries needed to compare the March DLL's
            // primary menu and any following self-MoveTo menu without calling
            // that second menu another save load.
            if (m_passiveAttemptActive &&
                m_passiveOpenAttempt == m_passiveAttempt &&
                m_passiveOpenOrdinal != 0) {
                const auto requestToCloseMs = std::max<std::int64_t>(
                    0, nowMs - m_passiveAttemptStartMs);
                const auto nativeChainMs = m_passiveFirstMenuOpenMs >= 0 ?
                    std::max<std::int64_t>(
                        0, nowMs - m_passiveFirstMenuOpenMs) : -1;
                const char* success = !m_passivePostObserved ? "pending" :
                    (m_passiveLoadSucceeded ? "true" : "false");
                logger::info(
                    "PASSIVE_MEASURE attempt={} menuOrdinal={} menuSequence={} "
                    "success={} engineMs={} nativeMenuMs={} requestToCloseMs={} "
                    "nativeChainMs={} gapFromPreviousCloseMs={} passive=true",
                    m_passiveAttempt,
                    m_passiveOpenOrdinal,
                    result.menuSequence,
                    success,
                    m_passivePostDurationMs,
                    result.durationMs,
                    requestToCloseMs,
                    nativeChainMs,
                    m_passiveGapFromPreviousCloseMs);
                m_passivePreviousCloseMs = nowMs;
            } else {
                logger::info(
                    "PASSIVE_MEASURE attempt=none menuOrdinal=0 "
                    "menuSequence={} nativeMenuMs={} passive=true",
                    result.menuSequence,
                    result.durationMs);
            }
            // Passive mode has no periodic logger worker. Flush only after the
            // measurement timestamp has been sealed at native CLOSE.
            if (auto log = spdlog::default_logger()) {
                log->flush();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        int m_loadCount = 0;
        bool m_loadingMenuOpen = false;
        std::uint64_t m_budgetGeneration = 0;
        std::chrono::steady_clock::time_point m_loadStart;

        std::mutex m_passiveMutex;
        Policy::PassiveMenuTimer m_passiveMenuTimer;
        std::uint64_t m_passiveAttempt = 0;
        std::uint64_t m_passiveOpenAttempt = 0;
        std::uint64_t m_passiveOpenOrdinal = 0;
        std::uint64_t m_passiveAttemptMenuOrdinal = 0;
        bool m_passiveAttemptActive = false;
        bool m_passivePostObserved = false;
        bool m_passiveLoadSucceeded = false;
        std::int64_t m_passiveAttemptStartMs = 0;
        std::int64_t m_passivePostDurationMs = -1;
        std::int64_t m_passiveFirstMenuOpenMs = -1;
        std::int64_t m_passivePreviousCloseMs = -1;
        std::int64_t m_passiveGapFromPreviousCloseMs = -1;
    };

    // ========================================================================
    // D3D11 device acquisition and Present hook installation
    // ========================================================================

    // NG/AE loading detection:
    // - NG 1.10.x: RE::UI::GetSingleton() works (ID 2689028 correct) → register at init
    // - AE 1.11.x: RegisterSink deadlocks from any thread except during init → use kPreLoadGame fallback
    //   kPreLoadGame + 1s delay → enable, kPostLoadGame + 3s delay → disable

    // ========================================================================
    // Initialization — called when game data is ready
    // ========================================================================

    void OnGameDataReady()
    {
        std::lock_guard initLock(s_initMutex);
        if (g_gameDataReady.load(std::memory_order_acquire)) {
            // A duplicate readiness notification may still be useful for
            // retrying MenuWatcher registration if the UI was unavailable.
            if (!s_menuWatcherRegistered && !REL::Module::IsNG()) {
                try {
                    if (auto ui = RE::UI::GetSingleton()) {
                        ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                            MenuWatcher::GetSingleton());
                        s_menuWatcherRegistered = true;
                        logger::info("MenuWatcher registered (deferred to kGameDataReady)");
                    }
                } catch (...) {
                    logger::warn("Deferred MenuWatcher registration failed");
                }
            }
            logger::info("kGameDataReady: core already initialized");
            return;
        }

        logger::info("Game data ready, initializing LoadingScreens...");

        {
            std::lock_guard configLock(s_configMutex);
            if (!g_config.Load()) {
                logger::warn(
                    "Config: startup read failed; using compiled safe defaults");
            }
            g_config.PublishSettingBindings();
            const bool marchVRDllLoaded = g_isVR &&
                ::GetModuleHandleW(L"VRLoadingScreens.dll") != nullptr;
            const bool passiveMeasurement =
                Policy::ShouldUsePassiveMeasurement(
                    g_config.benchmarkMode, marchVRDllLoaded);
            if (marchVRDllLoaded &&
                !Policy::IsPassiveMeasurementMode(g_config.benchmarkMode)) {
                logger::critical(
                    "Legacy March VRLoadingScreens.dll detected while full mode "
                    "was configured; forcing this DLL into passive measurement "
                    "to prevent Submit/NOP/overlay/MoveTo collisions");
            }
            if (passiveMeasurement) {
                g_config.benchmarkMode = 0;
            }
            s_startupBenchmarkMode = g_config.benchmarkMode;
            s_passiveMeasurementOnly.store(
                passiveMeasurement, std::memory_order_release);
        }

        const bool timingOnly =
            s_passiveMeasurementOnly.load(std::memory_order_acquire);
        if (timingOnly) {
            // This is deliberately an early return before every plugin subsystem
            // and hook installer. MenuWatcher plus the already-registered F4SE
            // listener are the complete measurement surface.
            if (!REL::Module::IsNG()) {
                try {
                    if (auto ui = RE::UI::GetSingleton()) {
                        ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                            MenuWatcher::GetSingleton());
                        s_menuWatcherRegistered = true;
                    }
                } catch (...) {
                    logger::critical(
                        "PASSIVE_MEASURE could not register MenuOpenCloseEvent");
                }
            }
            g_gameDataReady.store(true, std::memory_order_release);
            logger::info(
                "PASSIVE_MEASURE ready: F4SE messages + native LoadingMenu "
                "timestamps only; no trampoline, periodic logger, manager, "
                "asset scan, worker, D3D/OpenVR hook, engine patch, overlay, "
                "fade/spinner edit, preload, crash guard, task, or MoveTo");
            if (auto log = spdlog::default_logger()) {
                log->flush();
            }
            return;
        }

        // Full mode installs executable/render hooks later in this function and
        // keeps the normal diagnostic log draining. Passive mode returned above
        // before allocating either resource.
        F4SE::AllocTrampoline(256);
        spdlog::flush_every(std::chrono::seconds(1));

        // Install before DoorPrefetch validates its engine preload entry points so
        // calls made by our crosshair loader pass through the same observer and
        // can be attributed with a thread-local scope. Install() still prints
        // the five effective native preload settings when tracing is disabled.
        PreloadDiagnostics::Configure(g_config.preloadDiagnostics);
        (void)PreloadDiagnostics::Install();

        LoadingScreenManager::GetSingleton().SetVRPresentationProbeEnabled(
            g_config.vrPresentationProbe);
        LoadingScreenManager::GetSingleton().SetVRAnimationLoopNOPEnabled(
            g_config.vrAnimationLoopNOP);
        if (!g_config.vrAnimationLoopNOP) {
            logger::warn(
                "Config: bVRAnimationLoopNOP=0 - the VR animation-loop NOP is "
                "DISABLED for this session (load-time attribution test)");
        }

        // Detect High FPS Physics Fix — skip patches that overlap with what HFPF
        // will already do, so we don't double-patch the same bytes (works fine in
        // theory but means HFPF being unloaded could leave the game in a half-patched
        // state).
        s_hfpfDetected = (GetModuleHandleA("HighFPSPhysicsFix.dll") != nullptr);
        if (s_hfpfDetected) {
            // HFPF always handles these regardless of its INI:
            // - DisableBlackLoadingScreens: exact RE shows this only spoofs the
            //   LoadingMenu destination fields and adds generic loadscreen-form
            //   selection work; it is not a loading-I/O speedup. Keep ours off.
            // - oneThreadWhileLoading: HFPF's NumOfThreadsWhileLoadingNewGame is the
            //   same effect; running both fights over SetProcessAffinityMask.
            g_config.perfConfig.disableBlackLoadingScreens = false;
            g_config.perfConfig.oneThreadWhileLoading = false;

            // Read HFPF.ini to see which other duplicate settings are enabled,
            // and skip our equivalent patches if so. Path is relative to the game
            // root (F4SE working directory).
            CSimpleIniA hfpf;
            hfpf.SetUnicode();
            const char* HFPF_INI = "Data\\F4SE\\Plugins\\HighFPSPhysicsFix.ini";
            if (hfpf.LoadFile(HFPF_INI) >= 0) {
                struct Overlap { const char* name; bool* ourFlag; const char* hfpfKey; };
                Overlap overlaps[] = {
                    { "UntieSpeedFromFPS",
                      &g_config.perfConfig.untieSpeedFromFPS,
                      "UntieSpeedFromFPS" },
                    { "DisableiFPSClamp",
                      &g_config.perfConfig.disableiFPSClamp,
                      "DisableiFPSClamp" },
                };
                for (auto& o : overlaps) {
                    if (hfpf.GetBoolValue("Main", o.hfpfKey, false)) {
                        *o.ourFlag = false;
                        logger::info("HFPF: {}=true — skipping our patch", o.name);
                    }
                }

                // HFPF owns only NG/AE's load-scoped animation NOP. Our NG
                // disable3DModel SetModel RET is a separate site; VR keeps its
                // SetForegroundModel call NOP and OG keeps its InitModel RET.
                if (REL::Module::IsNG() &&
                    hfpf.GetBoolValue("Main", "DisableAnimationOnLoadingScreens", false)) {
                    g_config.perfConfig.disableAnimationOnLoadingScreens = false;
                    logger::info("HFPF: DisableAnimationOnLoadingScreens=true (NG) — "
                                 "skipping only our duplicate animation NOP");
                }

                const bool hfpfOwnsPapyrusBudget =
                    hfpf.GetBoolValue(
                        "Papyrus", "DynamicUpdateBudget", false);
                PapyrusOptimizer::GetSingleton().SetExternalOwner(
                    hfpfOwnsPapyrusBudget);
                if (hfpfOwnsPapyrusBudget) {
                    logger::info(
                        "HFPF: DynamicUpdateBudget=true — parking our "
                        "Papyrus budget updater");
                }

                // FixCPUThreads overlaps with yieldCPUDuringLoading on flat: both
                // remove PresentThread's present-in-progress flag store. Defer to
                // HFPF when it owns those bytes.
                if (!g_isVR &&
                    hfpf.GetBoolValue("Fixes", "FixCPUThreads", false)) {
                    logger::info(
                        "HFPF: FixCPUThreads=true — HFPF owns PresentThread; "
                        "this plugin never rewrites that live site");
                }

                // The load-only Present limiter is independent of
                // FixCPUThreads. Stand down only when HFPF demonstrably owns the
                // complete bounded policy; otherwise keep our 350-FPS guard so
                // SyncInterval=0 can never become an unbounded render loop.
                const bool hfpfDisablesLoadingVSync =
                    hfpf.GetBoolValue(
                        "Main", "DisableVSyncWhileLoading", false);
                const double hfpfLoadingFPS =
                    hfpf.GetDoubleValue(
                        "Limiter", "LoadingScreenFPS", 0.0);
                if (hfpfDisablesLoadingVSync &&
                    std::isfinite(hfpfLoadingFPS) &&
                    hfpfLoadingFPS > 0.0) {
                    g_config.perfConfig.disableVSyncWhileLoading = false;
                    logger::info(
                        "HFPF: owns bounded loading Present policy "
                        "(VSync off, cap={:.1f}); skipping our duplicate",
                        hfpfLoadingFPS);
                }

                logger::info("HFPF: detected, INI parsed");
            } else {
                logger::info("HFPF: detected but HighFPSPhysicsFix.ini missing — "
                             "leaving overlapping patches on");
            }
        }

        // Detect ENB by loaded module only. INI-file checks false-positive on
        // leftovers from an uninstalled ENB; compatibility decisions must
        // reflect code that is actually in the process.
        // d3dcompiler_46e.dll is loaded by every FO4 ENB binary at startup
        // (we run at kGameDataReady, well after), so a loaded module = ENB
        // actually active, not just configured.
        s_enbDetected = (GetModuleHandleA("d3dcompiler_46e.dll") != nullptr) ||
                        (GetModuleHandleA("enbseries.dll") != nullptr);
        if (s_enbDetected) {
            logger::info("ENB detected (module loaded)");
            // Only disable what genuinely breaks with ENB. The loading-flow
            // JMP patch is documented to break ENB's loading; keep it off.
            g_config.perfConfig.disableBlackLoadingScreens = false;
            // The unsafe live PresentThread rewrite is disabled globally.
        }

        // VR-specific overrides — these patches are flat-only
        if (g_isVR) {
            g_config.perfConfig.disableBlackLoadingScreens = false;  // Changes VR loading flow, causes infinite load
            g_config.perfConfig.yieldCPUDuringLoading = false;       // Runs inside Submit hook, deadlocks VR
            g_config.perfConfig.oneThreadWhileLoading = false;       // Starves SteamVR compositor threads
        }

        // Initialize loading screen manager (textures, overlays, animation loop)
        auto& loadingManager = LoadingScreenManager::GetSingleton();
        // Publish timing-only before Init can scan/launch background work. Passing
        // false as well makes the no-DDS guarantee explicit at both layers.
        loadingManager.SetTimingOnly(timingOnly);
        loadingManager.Init(
            g_config.enableBackgroundImages && !timingOnly,
            g_config.overlayMode,
            g_config.overlayAlpha);
        if (timingOnly) {
            logger::info("BENCHMARK MODE: timing only — all patches and visuals disabled");
        }

        if (timingOnly) {
            // OG/VR have LoadingMenu events; NG uses its exact load-entry/close
            // hooks below. None require intercepting Present. Keeping DXGI fully
            // untouched is both cheaper and a truer vanilla baseline.
            logger::info(
                "Benchmark timing uses menu/F4SE load signals; Present left unhooked");
        }

        if (!timingOnly) {
            if (g_isVR) {
                // VR overlay layout settings
                VRCompositorHelper::SetBackgroundWidth(g_config.backgroundWidth);

                // D3D11 compositor for VR (Submit hook, luminance key shader)
                auto& comp = D3D11Compositor::GetSingleton();
                bool compositorHookReady = false;
                if (VRCompositorHelper::IsInitialized() &&
                    VRCompositorHelper::IsOverlayInitialized()) {
                    comp.SetMode(CompositeMode::LuminanceKey);
                    comp.SetFrameCallback([]() {
                        if (g_gameDataReady.load(std::memory_order_acquire)) {
                            PapyrusOptimizer::GetSingleton().Update();
                            auto& manager = LoadingScreenManager::GetSingleton();
                            manager.Update();
                        }
                        // Engine-setting writes are queued to the game thread;
                        // this callback remains render-state-only.
                    });
                    comp.SetFlatMode(g_config.loadingScreenMode);
                    compositorHookReady = comp.Initialize(
                        VRCompositorHelper::GetCompositor(),
                        VRCompositorHelper::GetD3D11Device());
                    // Install the shared LoadingMenu suppression/capture hooks
                    // before the first timed load. Modes 0 and 2 need the same
                    // owner-specific suppression path; mode 3 also captures tips.
                    if (compositorHookReady && !comp.InstallVRMode3Hooks()) {
                        logger::warn(
                            "VR: LoadingMenu visual hooks unavailable at startup; "
                            "native fallback retained");
                    }
                    // Independent of the tips batch: silences the loading
                    // model's sound loops while the model is suppressed.
                    // Failing this must not disable the visual pipeline.
                    if (compositorHookReady && !comp.InstallUpdateSoundsHook()) {
                        logger::warn(
                            "VR: loading-model sound suppression unavailable; "
                            "the native loading sounds may play without a model");
                    }
                }
                const bool overlayReady = VRCompositorHelper::IsOverlayInitialized();
                if (compositorHookReady && comp.IsRenderReady() && overlayReady) {
                    logger::info("VR visual pipeline ready (compositor=true, overlay=true)");
                } else {
                    logger::warn("VR visual pipeline unavailable "
                                 "(hook={}, renderReady={}, overlay={}); native fallback enabled",
                        compositorHookReady, comp.IsRenderReady(), overlayReady);
                }

                // VR Papyrus optimizer
                PapyrusOptimizer::GetSingleton().Init(
                    Config::budgetMaxFPS, Config::updateBudgetBase);
                if (!InstallVRMarchMainLoopHook()) {
                    logger::error(
                        "VR March main-loop hook unavailable; post-save "
                        "MoveTo/reveal cannot use the v1.0 execution boundary");
                }
            } else {
                // Flat mode: initialize D3D11 compositor with luminance key for background compositing
                // Strategy 1 (vtable hook) is ENB-compatible; MinHook fallback only if RendererData unavailable
                logger::info("Flat: initializing D3D11Compositor...");
                auto& comp = D3D11Compositor::GetSingleton();
                comp.SetMode(CompositeMode::LuminanceKey);
                comp.SetFlatMode(g_config.loadingScreenMode);
                comp.SetDisableVSyncWhileLoading(
                    g_config.perfConfig.disableVSyncWhileLoading);
                comp.SetFrameCallback([]() {
                    if (g_gameDataReady.load(std::memory_order_acquire)) {
                        LoadingScreenManager::GetSingleton().Update();
                    }
                    TickNGDeferred();

                    // Device creation is deferred on flat. Start DDS work on the
                    // joinable IO worker, then bind each published generation from
                    // this render callback. No disk IO or decode runs in Present.
                    static std::uint64_t s_boundGeneration = 0;
                    auto& lsm = LoadingScreenManager::GetSingleton();
                    if (VRCompositorHelper::GetD3D11Device()) {
                        lsm.RetryTextureLoad();
                    }
                    const auto generation = lsm.GetBackgroundGeneration();
                    if (generation != 0 && generation != s_boundGeneration) {
                        if (auto* bgTex = lsm.GetCurrentBgTexture()) {
                            D3D11Compositor::GetSingleton().SetBackgroundTexture(bgTex);
                            s_boundGeneration = generation;
                            logger::info("Flat: background texture generation {} bound", generation);
                        }
                    }
                });
                logger::info("Flat: calling InitializeFlat()...");
                const bool hookReady = comp.InitializeFlat();
                if (REL::Module::IsNG()) {
                    s_ngPresentTickReady.store(
                        hookReady, std::memory_order_release);
                }
                if (!hookReady) {
                    logger::warn("Flat: Present hook unavailable; custom visuals/NOPs disabled");
                } else if (comp.IsRenderReady()) {
                    logger::info("Flat: Present hook and render resources ready");
                } else {
                    logger::info("Flat: Present hook installed; render resources will initialize lazily");
                }
            }

            // Performance patches (VR: all, flat: InitModel RET, VSync, iFPSClamp, CPU yield)
            // Wrapped in try/catch — address library IDs may not resolve on all NG versions
            logger::info("Applying performance patches...");
            try {
                PerformancePatches::Apply(g_config.perfConfig);
                logger::info("Performance patches applied OK");
            } catch (...) {
                logger::warn("Performance patches failed (address library unavailable for this version)");
            }
        }

        // Register for LoadingMenu open/close events
        // NG: UI singleton not initialized during kGameDataReady — deferred to kPostLoadGame
        if (!s_menuWatcherRegistered && !REL::Module::IsNG()) {
            try {
                if (auto ui = RE::UI::GetSingleton()) {
                    ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                        MenuWatcher::GetSingleton());
                    s_menuWatcherRegistered = true;
                    logger::info("Registered for MenuOpenCloseEvent");
                }
            } catch (...) {
                logger::warn("Menu event registration failed");
            }
        } else if (REL::Module::IsNG()) {
            // NG/AE: RE::UI::GetSingleton() broken in CROSS_VR build (wrong ID).
            // Use kPreLoadGame/kPostLoadGame for loading detection.
            logger::info("NG/AE: using kPreLoadGame/kPostLoadGame detection");
            // Exact paired native boundaries: custom pixels are armed only after
            // TES::ShowLoadingMenu and removed after TES::HideLoadingMenu.
            InstallNGLoadingMenuBoundaryHooks();
            // Door/cell/worldspace transitions don't fire kPreLoadGame — hook the
            // load functions so the compositor engages for them too (not just save
            // loads). This is what makes the custom loadscreen show on door loads.
            InstallNGLoadHooks();
        }

        if (!REL::Module::IsNG()) {
            // OG/VR patch only the exact PositionPlayerJob-owned Show call.
            // The resulting capability is consumed by the initial DoorPrefetch
            // config apply immediately below.
            InstallLegacyPositionExteriorShowHook();
        }

        // Initial apply so the very first load already has the tweaks (each
        // load open re-asserts). SEH-guarded internally — safe if the INI
        // collection isn't fully built yet on NG.
        // Baseline mode changes no executable bytes. Active mode installs only
        // the exterior-only DoorPrefetch path; the native linked-area callsite
        // hook and every plugin-issued interior request are retired.
        if (g_config.benchmarkMode != 0) {
            QueuePersistentTweaks();
            SaveGamePrefetch::SetEnabled(g_config.preloadSaveOnConfirm);
            (void)SaveGamePrefetch::Install();
            // Door look-ahead. Always install the functions, poller and sink so
            // crosshair, exterior-gate proximity, and extended-ray settings remain
            // hot and independently testable.
            ApplyDoorPrefetchConfig(g_config);
            DoorPrefetch::SetSessionLostCallback(&OnEngineSessionLost);
            DoorPrefetch::Install();
        }

        // Guards a latent vanilla worldspace-transition CTD in full mode (see
        // CellWorldspaceGuard.h). Measurement-only returned before this point:
        // an engine detour, even a safety detour, does not belong in a passive
        // comparison against the March DLL.
        (void)CellWorldspaceGuard::Install();

        g_gameDataReady.store(true, std::memory_order_release);
        // Full mode only: passive measurement returned before creating any
        // worker. MCM writes are detected outside loads and published through a
        // coalesced game-thread task before the next transition.
        StartLiveConfigWatcher();
        const auto& compositor = D3D11Compositor::GetSingleton();
        logger::info("LoadingScreens core initialized "
                     "(VR={}, compositorHook={}, renderReady={}, overlay={})",
            g_isVR, compositor.IsInitialized(), compositor.IsRenderReady(),
            VRCompositorHelper::IsOverlayInitialized());
    }

    void OnGameSessionLoaded()
    {
        s_mainMenuOpen.store(false, std::memory_order_release);
        g_gameSessionLoaded.store(true, std::memory_order_release);
        // kPostLoadGame can arrive while Bethesda's LoadingMenu is still open.
        // Consume the pre-published snapshot here; the CLOSE handler performs
        // the next disk reload after it has ended the timer and presentation.
        const Config config = GetConfigSnapshot();
        SaveGamePrefetch::SetEnabled(
            config.benchmarkMode != 0 && config.preloadSaveOnConfirm);
        ApplyDoorPrefetchConfig(config);  // hot-apply independent door sources

        auto& lsm = LoadingScreenManager::GetSingleton();
        lsm.SetMainMenuOpen(false);
        lsm.SetGameSessionLoaded();
        lsm.SetBackgroundsEnabled(config.enableBackgroundImages);
        lsm.SetOverlayAlpha(config.overlayAlpha);
        lsm.SetVRPresentationProbeEnabled(config.vrPresentationProbe);
        lsm.SetVRAnimationLoopNOPEnabled(config.vrAnimationLoopNOP);
        auto& compositor = D3D11Compositor::GetSingleton();
        compositor.SetFlatMode(config.loadingScreenMode);
        compositor.SetDisableVSyncWhileLoading(
            config.perfConfig.disableVSyncWhileLoading);
        if (g_isVR && config.benchmarkMode != 0 &&
            compositor.IsInitialized() &&
            !compositor.InstallVRMode3Hooks()) {
            logger::warn(
                "VR: LoadingMenu visual hooks are not ready; native fallback retained");
        }

        if (g_isVR) {
            VRCompositorHelper::SetBackgroundWidth(config.backgroundWidth);
        }

        logger::info("Game session loaded (bgWidth={:.1f})", config.backgroundWidth);
        PreloadDiagnostics::ObservePlayerCell("game-session-loaded");
        if (REL::Module::IsNG() &&
            Policy::NGHasReachedSafeClose(
                s_ngNativeMenuVisible.load(std::memory_order_acquire),
                s_ngLoadingActive.load(std::memory_order_acquire),
                s_ngEnablePending.load(std::memory_order_acquire),
                s_ngClosePending.load(std::memory_order_acquire))) {
            // Menu-less NG loads have no later native Hide callback. The
            // synchronous load has returned and no render-owned close remains.
            ResumePostLoadSystemsAtNativeClose();
        }
    }

    static void ResumePostLoadSystemsAtNativeClose()
    {
        const bool sessionActive = Policy::ShouldResumePostLoadSystems(
            g_gameSessionLoaded.load(std::memory_order_acquire),
            s_mainMenuOpen.load(std::memory_order_acquire),
            GetConfigSnapshot().benchmarkMode);
        DoorPrefetch::SetGameSessionActive(sessionActive);
        auto& papyrus = PapyrusOptimizer::GetSingleton();
        papyrus.SetLoading(false);
        papyrus.SetGameSessionActive(sessionActive);
    }

    static void OnEngineSessionLost() noexcept
    {
        if (!REL::Module::IsNG()) {
            return;
        }
        // AE cannot safely register CommonLib's MenuOpenCloseEvent source. The
        // DoorPrefetch game-thread poll uses engine-owned player/cell validity
        // outside transition suppression as the verified return-to-menu signal.
        s_mainMenuOpen.store(true, std::memory_order_release);
        g_gameSessionLoaded.store(false, std::memory_order_release);
        s_pendingGameSessionLoaded.store(false, std::memory_order_release);
        s_ngSaveClassificationArmed.store(
            false, std::memory_order_relaxed);
        s_ngEnableFromSaveLoad.store(
            false, std::memory_order_relaxed);
        s_ngIsInterior.store(false, std::memory_order_relaxed);
        s_ngNextLoadEntryArmed.store(false, std::memory_order_relaxed);
        s_ngNextLoadBodyReturned.store(false, std::memory_order_relaxed);
        s_ngNextLoadWaitsForPost.store(false, std::memory_order_relaxed);
        NGFinishPresentationGapIfNeeded();
        auto& manager = LoadingScreenManager::GetSingleton();
        manager.ClearGameSessionLoaded();
        manager.SetMainMenuOpen(true);
        auto& papyrus = PapyrusOptimizer::GetSingleton();
        papyrus.SetLoading(false);
        papyrus.SetGameSessionActive(false);
        logger::info(
            "NG: engine player/cell lifecycle reported no live session; "
            "MainMenu state published and background helpers parked");
    }

    static void PublishOrDeferLegacySessionLoaded(const char* a_source)
    {
        std::lock_guard lifecycleLock(s_flatLifecycleMutex);
        if (s_flatNativeLoadingMenuOpen.load(std::memory_order_acquire) ||
            s_flatNativeCloseInProgress.load(std::memory_order_acquire)) {
            s_pendingGameSessionLoaded.store(true, std::memory_order_release);
            logger::info(
                "{}: successful session publication deferred to native CLOSE",
                a_source ? a_source : "legacy");
            return;
        }
        if (s_mainMenuOpen.load(std::memory_order_acquire)) {
            s_pendingGameSessionLoaded.store(
                false, std::memory_order_release);
            logger::info(
                "{}: late session publication discarded because MainMenu is open",
                a_source ? a_source : "legacy");
            return;
        }

        // CLOSE has already completed (or this was a menu-less load). Publish
        // exactly once now; otherwise the old pending-only path stranded the
        // session/poller until a future unrelated load.
        s_pendingGameSessionLoaded.store(false, std::memory_order_release);
        OnGameSessionLoaded();
        ResumePostLoadSystemsAtNativeClose();
        logger::info(
            "{}: successful session published after native CLOSE",
            a_source ? a_source : "legacy");
    }

    static bool RefreshConfigFromDisk(
        const char* a_reason, bool a_resumePostLoadSystems)
    {
        // This boundary is reached from a game-thread task after native CLOSE
        // (and also at a flat main-menu open). It is a later safe retry point,
        // separate from the immediate close tasks, for a transiently unavailable
        // engine setting collection.
        if (REL::Module::IsNG()) {
            if (s_ngTweaksActive.load(std::memory_order_acquire) &&
                Policy::NGHasReachedSafeClose(
                    s_ngNativeMenuVisible.load(std::memory_order_acquire),
                    s_ngLoadingActive.load(std::memory_order_acquire),
                    s_ngEnablePending.load(std::memory_order_acquire),
                    s_ngClosePending.load(std::memory_order_acquire))) {
                RunNGEndLoadTweaks(
                    s_ngTweaksGeneration.load(std::memory_order_acquire), 4);
            }
        } else {
            const auto generation =
                s_flatBudgetGeneration.load(std::memory_order_acquire);
            if (generation != 0 &&
                !s_flatBudgetLoadActive.load(std::memory_order_acquire)) {
                RestoreLoadBudgetsWithRetry(generation, 4);
            }
        }

        Config config;
        if (!ReloadConfigSnapshot(config)) {
            logger::warn(
                "Config refresh skipped because the settings snapshot could not be read ({})",
                a_reason ? a_reason : "unspecified");
            return false;
        }
        SaveGamePrefetch::SetEnabled(
            config.benchmarkMode != 0 && config.preloadSaveOnConfirm);
        ApplyDoorPrefetchConfig(config);

        auto& manager = LoadingScreenManager::GetSingleton();
        manager.SetBackgroundsEnabled(config.enableBackgroundImages);
        manager.SetOverlayAlpha(config.overlayAlpha);
        // These two were published once at startup and never here, so editing
        // them mid-session did nothing while every neighbouring [Main] key
        // hot-applied — and the tell for bVRAnimationLoopNOP is a log line that
        // fails to print, so a contaminated A/B arm looked exactly like a clean
        // one. Republish them with everything else.
        manager.SetVRPresentationProbeEnabled(config.vrPresentationProbe);
        manager.SetVRAnimationLoopNOPEnabled(config.vrAnimationLoopNOP);

        auto& compositor = D3D11Compositor::GetSingleton();
        compositor.SetFlatMode(config.loadingScreenMode);
        compositor.SetDisableVSyncWhileLoading(
            config.perfConfig.disableVSyncWhileLoading);
        if (g_isVR) {
            VRCompositorHelper::SetBackgroundWidth(config.backgroundWidth);
            if (config.benchmarkMode != 0 &&
                compositor.IsInitialized() &&
                !compositor.InstallVRMode3Hooks()) {
                logger::warn(
                    "VR: mode-3 hooks remain unavailable after config refresh; "
                    "native fallback retained");
            }
        }
        if (config.benchmarkMode != 0) {
            // Save loads can reset persistent engine settings. Re-assert them
            // only after native CLOSE so their game-task work cannot extend the
            // measured load or contend with world deserialization.
            QueuePersistentTweaks();
        }

        logger::info(
            "Config refreshed outside timed load ({}, mode={}, benchmark={}, "
            "vrLoopNOP={}, probe={})",
            a_reason ? a_reason : "unspecified",
            config.loadingScreenMode,
            config.benchmarkMode,
            config.vrAnimationLoopNOP,
            config.vrPresentationProbe);
        // Refresh may flip benchmark mode after the previous snapshot already
        // resumed (or parked) the background helpers. Re-evaluate them from the
        // newly published snapshot while still outside every timed load.
        if (a_resumePostLoadSystems) {
            ResumePostLoadSystemsAtNativeClose();
        }
        return true;
    }

    // ========================================================================
    // F4SE message handler
    // ========================================================================

    void F4SEAPI MessageHandler(F4SE::MessagingInterface::Message* message)
    {
        const bool passiveMeasurement =
            s_passiveMeasurementOnly.load(std::memory_order_acquire);
        if (!passiveMeasurement) {
            logger::info("F4SE message: type={}", message->type);
        }

        switch (message->type) {
        case F4SE::MessagingInterface::kPostPostLoad:
            logger::info("kPostPostLoad received");
            if (g_isVR) {
                // kPostPostLoad is too early for game settings, UI event sources,
                // player/cell state, or engine-preload hooks. Full initialization
                // belongs exclusively to kGameDataReady; doing it here could mark
                // the core initialized against incomplete engine state and make
                // the real event skip required work. The startup LoadingMenu stays
                // wholly native. kGameDataReady still precedes the first selectable
                // main-menu save and installs the VR speed/visual hooks for it.
                logger::info(
                    "VR: startup LoadingMenu retained natively; "
                    "full initialization deferred to kGameDataReady");
            }
            break;
        case F4SE::MessagingInterface::kGameDataReady:
            OnGameDataReady();
            break;
        case F4SE::MessagingInterface::kPreLoadGame: {
            if (passiveMeasurement) {
                MenuWatcher::GetSingleton()->RecordPassivePreLoadGame();
                break;
            }
            logger::info("kPreLoadGame received");
            if (!REL::Module::IsNG()) {
                // Save-load messaging precedes the visual LoadingMenu boundary,
                // so this is the last safe place to consume a just-written MCM
                // snapshot without exposing Bethesda's native/title screen.
                RefreshLiveConfigBeforeLoadIfDirty("legacy-pre-load-game");
                s_flatLoadEntryArmed.store(true, std::memory_order_release);
            }
            const Config preLoadConfig = GetConfigSnapshot();
            if (preLoadConfig.benchmarkMode != 0) {
                PapyrusOptimizer::GetSingleton().SetLoading(true);
                SaveGamePrefetch::OnPreLoadGame(
                    message->data, message->dataLen);
                // Park prediction before the engine starts rebuilding the
                // world. Only plugin bookkeeping is released; Fallout's global
                // loader is never cancelled.
                // NG publishes this barrier transactionally with its entry
                // generation in NGArmPendingOnGameThread below.
                if (!REL::Module::IsNG()) {
                    DoorPrefetch::FlushQueuedLoads(true);
                }
            }
            if (REL::Module::IsNG() &&
                preLoadConfig.benchmarkMode != 0) {
                // Do not cancel a prior native close merely because another load
                // is being prepared. HookedShowLoadingMenu cancels it only after
                // Bethesda has actually opened the next LoadingMenu. If a Present
                // occurs in between, custom pixels stop at the real intervening
                // close and resume at the next real show.
                // A save load owns the destination classification; clear any
                // stale subordinate LoadInterior flag before its native Show.
                s_ngIsInterior.store(false, std::memory_order_relaxed);
                // A fresh load supersedes any not-yet-consumed end signal from
                // the previous one, including chained loads that keep the
                // compositor and temporary budgets active across the boundary.
                s_ngPostLoadArrived.store(false, std::memory_order_relaxed);
                // Consume the pre-published config and raise temporary load
                // budgets on this synchronous game-thread signal. Visual enable waits for the
                // verified native TES::ShowLoadingMenu boundary, so popup-only
                // and menu-less paths never display a custom screen.
                if (NGArmPendingOnGameThread(true)) {
                    logger::info(
                        "NG: kPreLoadGame — armed; awaiting native LoadingMenu show");
                }
            }
            break;
        }
        case F4SE::MessagingInterface::kPostLoadGame: {
            // F4SE encodes LoadGame's bool return directly as the data pointer
            // (nullptr=false, reinterpret_cast<void*>(1)=true), dataLen=1.
            const bool loadSucceeded = Policy::ShouldPublishLoadedSession(
                message->data != nullptr);
            if (passiveMeasurement) {
                MenuWatcher::GetSingleton()->RecordPassivePostLoadGame(
                    loadSucceeded);
                break;
            }
            logger::info("kPostLoadGame received");
            SaveGamePrefetch::OnPostLoadGame(loadSucceeded);
            if (Policy::ShouldArmVRTimerPatches(
                    g_isVR,
                    loadSucceeded,
                    s_vrTimerPatchesApplied.load(
                        std::memory_order_acquire))) {
                s_vrTimerPatchesPending.store(
                    true, std::memory_order_release);
                logger::info(
                    "VR March parity: timer/iFPSClamp patches armed for "
                    "native CLOSE");
            }
            if (loadSucceeded) {
                // Marks the current load as one that entered a game session,
                // which gates the post-close MainMenu render suppression.
                LoadingScreenManager::GetSingleton().NotePostLoadGame();
            }
            if (!REL::Module::IsNG()) {
                if (!s_flatNativeLoadingMenuOpen.load(
                        std::memory_order_acquire)) {
                    // Menu-less or failed load: no native CLOSE will arrive to
                    // release the pre-load latch.
                    s_flatLoadEntryArmed.store(
                        false, std::memory_order_release);
                }
                if (loadSucceeded) {
                    PublishOrDeferLegacySessionLoaded(
                        g_isVR ? "VR kPostLoadGame" : "OG kPostLoadGame");
                } else {
                    s_pendingGameSessionLoaded.store(
                        false, std::memory_order_release);
                    logger::info(
                        "{}: load failed; no session-loaded state will publish",
                        g_isVR ? "VR" : "OG");
                    if (!s_flatNativeLoadingMenuOpen.load(
                            std::memory_order_acquire)) {
                        DoorPrefetch::EndTransition();
                        ResumePostLoadSystemsAtNativeClose();
                    }
                }
            } else {
                // Messaging thread does NOT decide enable/close (that was a race —
                // the render tick owns those decisions). Only record the load-end
                // signal + timestamp; TickNGDeferred reacts: if the enable hasn't
                // fired yet it cancels it (instant/popup), otherwise it arms the
                // close. Write the timestamp BEFORE publishing the arrived flag
                // (release) so the render thread can't read a stale time.
                if (!s_ngPresentTickReady.load(std::memory_order_acquire)) {
                    if (s_ngNativeMenuVisible.load(
                            std::memory_order_acquire)) {
                        // The exact Hide hook owns the tail even without a
                        // Present callback. Do not restore budgets or resume
                        // prediction while Bethesda still shows LoadingMenu.
                        logger::info(
                            "NG: load body returned with no Present owner; "
                            "awaiting native HideLoadingMenu");
                    } else {
                        s_ngEnablePending.store(
                            false, std::memory_order_relaxed);
                        s_ngShowMenuSignal.store(
                            false, std::memory_order_relaxed);
                        s_ngPostLoadArrived.store(
                            false, std::memory_order_relaxed);
                        s_ngSaveClassificationArmed.store(
                            false, std::memory_order_relaxed);
                        s_ngEnableFromSaveLoad.store(
                            false, std::memory_order_relaxed);
                        s_ngIsInterior.store(
                            false, std::memory_order_relaxed);
                        s_ngChainedShowSignal.store(
                            false, std::memory_order_relaxed);
                        s_ngNextLoadEntryArmed.store(
                            false, std::memory_order_relaxed);
                        s_ngNextLoadBodyReturned.store(
                            false, std::memory_order_relaxed);
                        s_ngNextLoadWaitsForPost.store(
                            false, std::memory_order_relaxed);
                        NGFinishPresentationGapIfNeeded();
                        QueueNGEndLoadTweaks();
                        DoorPrefetch::EndTransition();
                        ResumePostLoadSystemsAtNativeClose();
                        QueueNGConfigRefresh(
                            "ng-post-load-no-present-no-menu");
                    }
                } else {
                    s_ngLastPostLoadTimeTicks.store(
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        std::memory_order_relaxed);
                    s_ngPostLoadArrived.store(true, std::memory_order_release);
                }
                if (loadSucceeded) {
                    OnGameSessionLoaded();
                }
            }
            break;
        }
        case F4SE::MessagingInterface::kNewGame:
            if (passiveMeasurement) {
                DoorPrefetch::ClearExteriorPresentationEvidence(
                    "new-game-passive");
                break;
            }
            logger::info("kNewGame received");
            DoorPrefetch::SetGameSessionActive(false);
            DoorPrefetch::ClearExteriorPresentationEvidence("new-game");
            // VR: defer to the menu-close handler like kPostLoadGame — running
            // OnGameSessionLoaded on the messaging thread mid-load can prevent the
            // LoadingMenu from closing (the same reason kPostLoadGame is deferred).
            if (REL::Module::IsNG()) {
                OnGameSessionLoaded();
            } else {
                PublishOrDeferLegacySessionLoaded(
                    g_isVR ? "VR kNewGame" : "OG kNewGame");
            }
            break;
        }
    }

    // ========================================================================
    // Logging setup
    // ========================================================================

    // Logging handled by CommonLibF4 — writes to correct game-specific F4SE directory
}

// ========================================================================
// F4SE Plugin Exports
// ========================================================================

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    if (!a_f4se ||
        a_f4se->IsEditor() ||
        !IsSupportedF4SEHost(*a_f4se)) {
        return false;
    }

    const auto& module = REL::Module::get();
    g_isVR = Policy::ClassifyExecutable(module.filename()) ==
        Policy::ExecutableKind::kFallout4VR;

    // CommonLibF4 revision 2b64114 reads GetPluginInfo unconditionally, while
    // official F4SEVR 0.6.21 exposes only the seven-field prefix ending at
    // GetReleaseIndex. Copy exactly that proven prefix into a zero-initialized
    // current-layout view, so CommonLib sees null extension fields rather than
    // reading beyond F4SEVR's object. F4SE::Init consumes this view
    // synchronously and retains only copied values and queried interfaces.
    static_assert(std::is_standard_layout_v<F4SE::detail::F4SEInterface>);
    static_assert(
        offsetof(F4SE::detail::F4SEInterface, GetPluginInfo) ==
        sizeof(std::uint32_t) * 4 + sizeof(void*) * 3);
    F4SE::detail::F4SEInterface legacyInterfaceView{};
    const F4SE::LoadInterface* initInterface = a_f4se;
    if (RequiresLegacyF4SEVRInterfaceShim(*a_f4se)) {
        constexpr auto legacyPrefixSize =
            offsetof(F4SE::detail::F4SEInterface, GetPluginInfo);
        std::memcpy(&legacyInterfaceView, a_f4se, legacyPrefixSize);
        initInterface =
            reinterpret_cast<const F4SE::LoadInterface*>(&legacyInterfaceView);
    }
    // Rotate the previous session's log BEFORE the sink truncates the new one.
    // The sink truncates on every launch and keeps no history, so a relaunch
    // destroys the evidence for whatever the player just reported. That has
    // now cost this project three separate field sessions.
    //
    // The path is built BY HAND, not via F4SE::log::log_directory(): the 2.1.21
    // audit proved that call is a silent no-op here because it runs before
    // F4SE::Init, when GetSaveFolderName() still returns its empty pre-Init
    // string - the rotation was checking "My Games\F4SE\..." with the game
    // folder missing and never finding a file. Hand-building from g_isVR cannot
    // diverge from where the sink actually writes: CommonLib's own ternary only
    // accepts the proxy string when it is EMPTY, so it always takes the
    // REL::Module::IsVR() fallback this mirrors. Best effort: any failure here
    // must never keep the plugin from loading; the outcome is recorded and
    // logged after the logger exists.
    const char* logRotateOutcome = "not-attempted";
    try {
        wchar_t* documents = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(
                FOLDERID_Documents, 0, nullptr, &documents)) &&
            documents) {
            std::filesystem::path logDir{ documents };
            ::CoTaskMemFree(documents);
            logDir /= "My Games";
            logDir /= g_isVR ? "Fallout4VR" : "Fallout4";
            logDir /= "F4SE";
            const auto current = logDir / "LoadingScreens.log";
            if (std::filesystem::exists(current) &&
                std::filesystem::file_size(current) > 0) {
                std::filesystem::copy_file(
                    current, logDir / "LoadingScreens.prev.log",
                    std::filesystem::copy_options::overwrite_existing);
                logRotateOutcome = "rotated";
            } else {
                logRotateOutcome = "no-previous-log";
            }
        } else {
            logRotateOutcome = "documents-folder-unavailable";
        }
    } catch (...) {
        logRotateOutcome = "failed";
    }

    F4SE::Init(initInterface);

    spdlog::set_level(spdlog::level::info);
    // The rotation above ran before this logger existed; report its outcome now
    // so a missing prev.log is diagnosable instead of silent (the 2.1.21
    // rotation was a silent no-op for exactly the lack of this line).
    logger::info("Log rotation: {}", logRotateOutcome);
    // Flush on warn+ only (not every info record). Per-record flushing meant dozens
    // of synchronous disk writes during each load — on the game/render threads,
    // contending with the loader's own disk IO (a measurable VR load-time cost, and
    // the old VR-only mod avoided it by logging at warn level). The 1s timer below
    // still drains buffered info lines, and warn/error (the CTD-relevant ones) still
    // flush immediately, so crash diagnostics are unaffected.
    spdlog::flush_on(spdlog::level::warn);

    auto messaging = F4SE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logger::critical("Unable to register F4SE messaging listener; refusing partial initialization");
        return false;
    }

    return true;
}

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginName("LoadingScreens");
    v.PluginVersion({ 2, 1, 1, 0 });
    v.UsesAddressLibrary(true);
    // This plugin reads runtime-specific RendererData, PlayerCharacter, cell-map,
    // and event-source layouts. Advertising structure independence lets F4SE load
    // it on builds for which those offsets are not valid.
    v.HasNoStructUse(false);
    v.IsLayoutDependent(true);
    v.CompatibleVersions({
        { 1, 2, 72, 0 },   // Fallout 4 VR
        // Official F4SEVR 0.6.21 reports this proxy in its legacy interface.
        // Query and Load still require the actual main Fallout4VR.exe to be
        // exactly 1.2.72.0, so flat Fallout 4 1.10.138 remains rejected.
        { 1, 10, 138, 0 },
        { 1, 10, 163, 0 }, // original flat runtime
        { 1, 11, 221, 0 },
        { 1, 11, 240, 0 }  // verified current AE runtime
    });
    return v;
}();

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* pluginInfo)
{
    if (!a_f4se || !pluginInfo) {
        return false;
    }
    pluginInfo->name = F4SEPlugin_Version.pluginName;
    pluginInfo->infoVersion = F4SE::PluginInfo::kVersion;
    pluginInfo->version = F4SEPlugin_Version.pluginVersion;
    return !a_f4se->IsEditor() &&
        IsSupportedF4SEHost(*a_f4se);
}
