#include "PCH.h"

#include <SimpleIni.h>
#include "LoadingScreenManager.h"
#include "VRCompositorHelper.h"
#include "PerformancePatches.h"
#include "PapyrusOptimizer.h"
#include "D3D11Compositor.h"

using namespace VRLoadingScreens;

namespace
{
    // MCM config paths
    static const char* MCM_CONFIG_INI = "Data\\MCM\\Config\\FasterLoadscreens\\settings.ini";
    static const char* MCM_USER_INI = "Data\\MCM\\Settings\\FasterLoadscreens.ini";

    // Config
    struct Config
    {
        // 0=Black, 1=Native (no 3D model), 2=Background only, 3=Background+Tips
        int loadingScreenMode = 3;
        bool enableBackgroundImages = true;
        bool showTipDisplay = true;
        float renderDelaySeconds = 0.5f;

        // Benchmark mode: 0=vanilla (timing only, no patches), 1=full plugin
        int benchmarkMode = 1;

        static constexpr float overlayAlpha = 1.0f;
        float backgroundWidth = 10.0f;
        float tipDisplayOffsetX = 0.0f;
        float tipDisplayOffsetY = -2.5f;
        float tipDisplayYaw = 0.0f;
        float tipDisplayPitch = 0.0f;
        static constexpr int overlayMode = 1;  // World-locked

        PerformanceConfig perfConfig;

        static constexpr float budgetMaxFPS = 90.0f;
        static constexpr float updateBudgetBase = 1.2f;

        void Load()
        {
            CSimpleIniA ini;
            ini.SetUnicode();

            if (std::filesystem::exists(MCM_CONFIG_INI)) {
                ini.LoadFile(MCM_CONFIG_INI);
            }
            if (std::filesystem::exists(MCM_USER_INI)) {
                ini.LoadFile(MCM_USER_INI);
                logger::info("Loaded MCM user overrides from {}", MCM_USER_INI);
            }

            loadingScreenMode = static_cast<int>(ini.GetLongValue(
                "Main", "iLoadingScreenMode", 3));
            benchmarkMode = static_cast<int>(ini.GetLongValue(
                "Main", "iBenchmarkMode", 1));

            // 0=Black, 1=Native (no 3D), 2=BG only, 3=BG+Tips
            enableBackgroundImages = (loadingScreenMode >= 2);
            showTipDisplay = (loadingScreenMode >= 3);
            renderDelaySeconds = showTipDisplay ? 0.5f : 0.0f;

            backgroundWidth = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fBackgroundWidth", 10.0));
            tipDisplayOffsetX = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fTipOffsetX", 0.0));
            tipDisplayOffsetY = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fTipOffsetY", -2.5));
            tipDisplayYaw = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fTipYaw", 0.0));
            tipDisplayPitch = static_cast<float>(ini.GetDoubleValue(
                "TipOverlay", "fTipPitch", 0.0));

            perfConfig.untieSpeedFromFPS = true;
            perfConfig.disableiFPSClamp = true;
            perfConfig.disableBlackLoadingScreens = true;
            perfConfig.disableVSyncWhileLoading = true;
            perfConfig.disable3DModel = true;
            perfConfig.yieldCPUDuringLoading = true;
            perfConfig.oneThreadWhileLoading = false;

            logger::info("Config: mode={} benchmark={} (bg={}, tips={}, delay={:.1f}s)",
                loadingScreenMode, benchmarkMode, enableBackgroundImages, showTipDisplay, renderDelaySeconds);
        }
    };

    Config g_config;
    bool g_isVR = false;
    bool g_pendingTimerPatches = false;
    bool g_gameDataReady = false;
    bool g_gameSessionLoaded = false;

    // Forward declarations
    static bool s_hfpfDetected = false;
    static bool s_pendingGameSessionLoaded = false;
    static bool s_ngMenuEventsRegistered = false;
    static std::chrono::steady_clock::time_point s_ngLastPostLoadTime{};
    static bool s_ngLoadingActive = false;
    static bool s_ngHeaderReadPending = false;  // true after first kPreLoadGame, cleared by immediate kPostLoadGame
    void OnGameSessionLoaded();

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

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event.menuName == "LoadingMenu") {
                if (a_event.opening) {
                    m_loadCount++;
                    m_loadStart = std::chrono::steady_clock::now();

                    // Re-read MCM settings so changes apply immediately
                    g_config.Load();
                    auto& lsm = LoadingScreenManager::GetSingleton();
                    lsm.SetBackgroundsEnabled(g_config.enableBackgroundImages);
                    lsm.SetRenderDelay(g_config.renderDelaySeconds);
                    D3D11Compositor::GetSingleton().SetFlatMode(g_config.loadingScreenMode);

                    logger::info("LoadingMenu OPEN (load #{}, mode={})", m_loadCount, g_config.loadingScreenMode);

                    if (!g_isVR) {
                        D3D11Compositor::GetSingleton().SetEnabled(true);
                    }
                    LoadingScreenManager::GetSingleton().OnLoadingMenuOpen();
                    PerformancePatches::OnLoadingMenuOpen();

                } else {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_loadStart).count();
                    logger::info("LoadingMenu CLOSE (load #{}) — {:.2f}s",
                        m_loadCount, elapsed / 1000.0);

                    if (!g_isVR) {
                        D3D11Compositor::GetSingleton().SetEnabled(false);
                    }
                    LoadingScreenManager::GetSingleton().OnLoadingMenuClose();
                    PerformancePatches::OnLoadingMenuClose();

                    // VR: process deferred kPostLoadGame AFTER OnLoadingMenuClose.
                    // OnLoadingMenuClose already consumed m_needsDesyncFix for THIS load.
                    // SetGameSessionLoaded sets it for the NEXT load's close (desync fix).
                    if (s_pendingGameSessionLoaded) {
                        s_pendingGameSessionLoaded = false;
                        OnGameSessionLoaded();
                        logger::info("VR: deferred kPostLoadGame processed");
                    }

                    if (g_isVR && g_pendingTimerPatches) {
                        g_pendingTimerPatches = false;
                        PerformancePatches::ApplyTimerPatches(g_config.perfConfig);
                    }
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        int m_loadCount = 0;
        std::chrono::steady_clock::time_point m_loadStart;
    };

    // ========================================================================
    // IDXGISwapChain::Present hook for per-frame updates (flat + VR)
    // ========================================================================

    using PresentFn = HRESULT(WINAPI*)(void* swapChain, UINT syncInterval, UINT flags);
    static PresentFn s_originalPresent = nullptr;

    static HRESULT WINAPI HookedPresent(void* swapChain, UINT syncInterval, UINT flags)
    {
        // Per-frame update (VR mode only — flat mode uses D3D11Compositor's Present hook)
        if (g_isVR) {
            VRCompositorHelper::UpdateLastKnownPose();
        }
        if (g_gameDataReady) {
            if (g_isVR) {
                PapyrusOptimizer::GetSingleton().Update();
            }
            LoadingScreenManager::GetSingleton().Update();
        }
        PerformancePatches::OnPresent();

        return s_originalPresent(swapChain, syncInterval, flags);
    }

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
        g_gameDataReady = true;
        logger::info("Game data ready, initializing LoadingScreens...");

        g_config.Load();

        // Detect High FPS Physics Fix — skip conflicting patches if present
        s_hfpfDetected = (GetModuleHandleA("HighFPSPhysicsFix.dll") != nullptr);
        if (s_hfpfDetected) {
            logger::info("HFPF detected — compatibility mode: skipping VSync/affinity/black screen patches");
            g_config.perfConfig.disableVSyncWhileLoading = false;
            g_config.perfConfig.disableBlackLoadingScreens = false;
            g_config.perfConfig.oneThreadWhileLoading = false;
            g_config.perfConfig.yieldCPUDuringLoading = false;
        }

        // VR-specific overrides — these patches are flat-only
        if (g_isVR) {
            g_config.perfConfig.disableBlackLoadingScreens = false;  // Changes VR loading flow, causes infinite load
            g_config.perfConfig.yieldCPUDuringLoading = false;       // Runs inside Submit hook, deadlocks VR
            g_config.perfConfig.oneThreadWhileLoading = false;       // Starves SteamVR compositor threads
        }

        bool timingOnly = (g_config.benchmarkMode == 0);

        // Initialize loading screen manager (textures, overlays, animation loop)
        LoadingScreenManager::GetSingleton().Init(
            g_config.enableBackgroundImages,
            g_config.renderDelaySeconds,
            g_config.overlayMode,
            g_config.overlayAlpha);
        if (timingOnly) {
            LoadingScreenManager::GetSingleton().SetTimingOnly(true);
            logger::info("BENCHMARK MODE: timing only — all patches and visuals disabled");
        }

        // Flat benchmarkMode=0: need Present hook for timing (skip if HFPF hooks Present)
        if (timingOnly && !g_isVR && !s_hfpfDetected) {
            logger::info("Flat benchmark: initializing Present hook for timing only...");
            auto& comp = D3D11Compositor::GetSingleton();
            comp.SetFrameCallback([]() {});
            comp.InitializeFlat();
            logger::info("Flat benchmark: Present hook ready for timing");
        } else if (timingOnly && s_hfpfDetected) {
            logger::info("Flat benchmark: HFPF detected, using menu events only for timing");
        }

        if (!timingOnly) {
            if (g_isVR) {
                // VR overlay layout settings
                VRCompositorHelper::SetBackgroundWidth(g_config.backgroundWidth);
                VRCompositorHelper::SetTipDisplayOffset(g_config.tipDisplayOffsetX, g_config.tipDisplayOffsetY);
                VRCompositorHelper::SetTipDisplayRotation(g_config.tipDisplayYaw, g_config.tipDisplayPitch);

                // D3D11 compositor for VR (Submit hook, alpha key shader)
                if (VRCompositorHelper::IsInitialized()) {
                    auto& comp = D3D11Compositor::GetSingleton();
                    comp.SetMode(CompositeMode::LuminanceKey);
                    comp.SetShowCapturedOverlay(g_config.showTipDisplay);
                    comp.SetFrameCallback([]() {
                        if (g_gameDataReady) {
                            PapyrusOptimizer::GetSingleton().Update();
                            LoadingScreenManager::GetSingleton().Update();
                        }
                        PerformancePatches::OnPresent();
                    });
                    comp.Initialize(
                        VRCompositorHelper::GetCompositor(),
                        VRCompositorHelper::GetD3D11Device());
                }

                // VR Papyrus optimizer
                PapyrusOptimizer::GetSingleton().Init(
                    Config::budgetMaxFPS, Config::updateBudgetBase);
            } else {
                // Flat mode: initialize D3D11 compositor with luminance key for background compositing
                logger::info("Flat: initializing D3D11Compositor...");
                auto& comp = D3D11Compositor::GetSingleton();
                comp.SetMode(CompositeMode::LuminanceKey);
                comp.SetShowCapturedOverlay(false);  // Tips rendered by game natively in flat
                comp.SetFlatMode(g_config.loadingScreenMode);
                comp.SetFrameCallback([]() {
                    if (g_gameDataReady) {
                        LoadingScreenManager::GetSingleton().Update();
                    }
                    PerformancePatches::OnPresent();

                    // Flat: deferred texture load (device not available at init time)
                    static bool s_flatTextureLoaded = false;
                    if (!s_flatTextureLoaded) {
                        auto* dev = VRCompositorHelper::GetD3D11Device();
                        if (dev) {
                            s_flatTextureLoaded = true;
                            logger::info("Flat: device available, loading deferred texture...");
                            auto& lsm = LoadingScreenManager::GetSingleton();
                            lsm.RetryTextureLoad();
                            auto* bgTex = lsm.GetCurrentBgTexture();
                            if (bgTex) {
                                D3D11Compositor::GetSingleton().SetBackgroundTexture(bgTex);
                                logger::info("Flat: background texture ready");
                            }
                        }
                    }
                });
                logger::info("Flat: calling InitializeFlat()...");
                comp.InitializeFlat();
                logger::info("Flat: D3D11Compositor initialized OK");
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
        if (!REL::Module::IsNG()) {
            try {
                if (auto ui = RE::UI::GetSingleton()) {
                    ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                        MenuWatcher::GetSingleton());
                    logger::info("Registered for MenuOpenCloseEvent");
                }
            } catch (...) {
                logger::warn("Menu event registration failed");
            }
        } else {
            // NG/AE: RE::UI::GetSingleton() broken in CROSS_VR build (wrong ID).
            // Use kPreLoadGame/kPostLoadGame for loading detection.
            logger::info("NG/AE: using kPreLoadGame/kPostLoadGame detection");
        }

        logger::info("LoadingScreens initialized (VR={})", g_isVR);
    }

    void OnGameSessionLoaded()
    {
        g_gameSessionLoaded = true;
        g_config.Load();  // re-read MCM settings

        auto& lsm = LoadingScreenManager::GetSingleton();
        lsm.SetGameSessionLoaded();
        lsm.SetBackgroundsEnabled(g_config.enableBackgroundImages);
        lsm.SetRenderDelay(g_config.renderDelaySeconds);
        lsm.SetOverlayAlpha(g_config.overlayAlpha);
        D3D11Compositor::GetSingleton().SetFlatMode(g_config.loadingScreenMode);

        if (g_isVR) {
            g_pendingTimerPatches = true;
            D3D11Compositor::GetSingleton().SetShowCapturedOverlay(g_config.showTipDisplay);
            VRCompositorHelper::SetBackgroundWidth(g_config.backgroundWidth);
            VRCompositorHelper::SetTipDisplayOffset(g_config.tipDisplayOffsetX, g_config.tipDisplayOffsetY);
            VRCompositorHelper::SetTipDisplayRotation(g_config.tipDisplayYaw, g_config.tipDisplayPitch);
        }

        logger::info("Game session loaded (tipOffset=[{:.1f},{:.1f}] tipRot=[{:.1f},{:.1f}] bgWidth={:.1f})",
            g_config.tipDisplayOffsetX, g_config.tipDisplayOffsetY,
            g_config.tipDisplayYaw, g_config.tipDisplayPitch, g_config.backgroundWidth);
    }

    // ========================================================================
    // F4SE message handler
    // ========================================================================

    void F4SEAPI MessageHandler(F4SE::MessagingInterface::Message* message)
    {
        logger::info("F4SE message: type={}", message->type);

        switch (message->type) {
        case F4SE::MessagingInterface::kGameDataReady:
            OnGameDataReady();
            break;
        case F4SE::MessagingInterface::kPreLoadGame:
            logger::info("kPreLoadGame received");
            // NG/AE: enable compositor on kPreLoadGame
            if (REL::Module::IsNG() && !s_ngMenuEventsRegistered && !s_ngLoadingActive) {
                s_ngLoadingActive = true;
                D3D11Compositor::GetSingleton().SetEnabled(true);
                LoadingScreenManager::GetSingleton().OnLoadingMenuOpen();
                PerformancePatches::OnLoadingMenuOpen();
                logger::info("NG: loading started (kPreLoadGame)");
            }

            break;
        case F4SE::MessagingInterface::kPostLoadGame:
            logger::info("kPostLoadGame received");
            if (g_isVR) {
                s_pendingGameSessionLoaded = true;
                logger::info("VR: kPostLoadGame deferred to menu close");
            } else if (REL::Module::IsNG()) {
                s_ngLastPostLoadTime = std::chrono::steady_clock::now();
                // Close loading if active
                if (s_ngLoadingActive) {
                    s_ngLoadingActive = false;
                    D3D11Compositor::GetSingleton().SetEnabled(false);
                    LoadingScreenManager::GetSingleton().OnLoadingMenuClose();
                    PerformancePatches::OnLoadingMenuClose();
                    logger::info("NG: loading ended (kPostLoadGame)");
                }
                OnGameSessionLoaded();
            } else {
                OnGameSessionLoaded();
            }
            break;
        case F4SE::MessagingInterface::kNewGame:
            logger::info("kNewGame received");
            OnGameSessionLoaded();
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
    g_isVR = REL::Module::IsVR();

    F4SE::Init(a_f4se);

    spdlog::set_level(spdlog::level::warn);

    auto messaging = F4SE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(MessageHandler);
    }

    F4SE::AllocTrampoline(256);

    return true;
}

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginName("LoadingScreens");
    v.PluginVersion({ 1, 5, 0, 0 });
    v.UsesAddressLibrary(true);
    v.HasNoStructUse(true);
    v.CompatibleVersions({ { 1, 10, 163, 0 }, { 1, 10, 980, 0 }, { 1, 10, 984, 0 }, { 1, 11, 191, 0 } });
    return v;
}();

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface*, F4SE::PluginInfo* pluginInfo)
{
    pluginInfo->name = F4SEPlugin_Version.pluginName;
    pluginInfo->infoVersion = F4SE::PluginInfo::kVersion;
    pluginInfo->version = F4SEPlugin_Version.pluginVersion;
    return true;
}
