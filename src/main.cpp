#include "PCH.h"

#include "ModBase.h"
#include "ConfigBase.h"
#include "LoadingScreenManager.h"
#include "VRCompositorHelper.h"
#include "PerformancePatches.h"
#include "PapyrusOptimizer.h"
#include "D3D11Compositor.h"

using namespace VRLoadingScreens;
using namespace f4cf;

namespace
{
    // MCM config paths
    static const char* MCM_CONFIG_INI = "Data\\MCM\\Config\\VRLoadingScreens\\settings.ini";
    static const char* MCM_USER_INI = "Data\\MCM\\Settings\\VRLoadingScreens.ini";

    class Config : public ConfigBase
    {
    public:
        Config() : ConfigBase("VRLoadingScreens",
            MCM_CONFIG_INI, 0) {}

        // Override load to bypass embedded resource (we don't have one)
        void load() override
        {
            logger::info("Loading VRLoadingScreens config...");

            CSimpleIniA ini;
            ini.SetUnicode();

            // Load defaults from MCM Config path
            if (std::filesystem::exists(MCM_CONFIG_INI)) {
                ini.LoadFile(MCM_CONFIG_INI);
            }

            // Override with user settings from MCM Settings path (written by MCM VR)
            if (std::filesystem::exists(MCM_USER_INI)) {
                ini.LoadFile(MCM_USER_INI);
                logger::info("Loaded MCM user overrides from {}", MCM_USER_INI);
            }

            loadIniConfigInternal(ini);
        }

        // MCM setting: 0=Black (fastest), 1=Background only, 2=Background + Tips
        int loadingScreenMode = 2;

        // Derived from loadingScreenMode
        bool enableBackgroundImages = true;
        bool showTipDisplay = true;
        float renderDelaySeconds = 0.1f;

        // Hardcoded optimal values
        static constexpr float overlayAlpha = 1.0f;
        static constexpr float backgroundWidth = 10.0f;
        static constexpr float tipDisplayOffsetX = -1.0f;
        static constexpr float tipDisplayOffsetY = -2.5f;
        static constexpr float tipDisplayYaw = 0.0f;
        static constexpr float tipDisplayPitch = -25.0f;
        static constexpr int overlayMode = 1;           // World-locked
        static constexpr bool timingOnly = false;

        PerformanceConfig perfConfig;

        static constexpr bool dynamicUpdateBudget = true;
        static constexpr float budgetMaxFPS = 90.0f;
        static constexpr float updateBudgetBase = 1.2f;

    protected:
        void loadIniConfigInternal(const CSimpleIniA& ini) override
        {
            // Single MCM setting: loading screen mode
            loadingScreenMode = static_cast<int>(ini.GetLongValue(
                "Main", "iLoadingScreenMode", 2));

            // Derive flags from mode
            // 0=Black (fastest), 1=Background only, 2=Background + Tips
            enableBackgroundImages = (loadingScreenMode >= 1);
            showTipDisplay = (loadingScreenMode >= 2);
            renderDelaySeconds = showTipDisplay ? 0.1f : 0.0f;

            // Hardcode performance settings
            perfConfig.untieSpeedFromFPS = true;
            perfConfig.disableiFPSClamp = true;
            perfConfig.disableBlackLoadingScreens = false;
            perfConfig.disableVSyncWhileLoading = true;
            perfConfig.disable3DModel = true;

            logger::info("Config: mode={} (bg={}, tips={}, delay={:.1f}s)",
                loadingScreenMode, enableBackgroundImages, showTipDisplay, renderDelaySeconds);
        }

        void saveIniConfigInternal(CSimpleIniA& ini) override {}
    };

    Config g_config;

    // Timer patches deferred until after first save load (VR camera desync fix)
    static bool g_pendingTimerPatches = false;

    // Menu event handler for LoadingMenu open/close
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
                    LoadingScreenManager::GetSingleton().OnLoadingMenuOpen();
                    PerformancePatches::OnLoadingMenuOpen();
                } else {
                    LoadingScreenManager::GetSingleton().OnLoadingMenuClose();
                    PerformancePatches::OnLoadingMenuClose();

                    // Apply timer patches after first save load completes
                    if (g_pendingTimerPatches) {
                        g_pendingTimerPatches = false;
                        PerformancePatches::ApplyTimerPatches(g_config.perfConfig);
                    }
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class VRLoadingScreensMod : public ModBase
    {
    public:
        VRLoadingScreensMod() :
            ModBase(Settings("VRLoadingScreens", "1.0.0", &g_config, 64, true))
        {
        }

    protected:
        void onModLoaded(const F4SE::LoadInterface* f4SE) override
        {
            logger::info("VRLoadingScreens v1.0.0 loaded");
        }

        void onGameLoaded() override
        {
            logger::info("Game loaded, initializing VRLoadingScreens...");

            g_config.load();

            // Loading screen features (backgrounds + delayed loop NOP + desync fix)
            LoadingScreenManager::GetSingleton().Init(
                g_config.enableBackgroundImages,
                g_config.renderDelaySeconds,
                g_config.overlayMode,
                g_config.overlayAlpha);

            // Apply overlay layout settings (hardcoded optimal values)
            VRCompositorHelper::SetBackgroundWidth(Config::backgroundWidth);
            VRCompositorHelper::SetTipDisplayOffset(Config::tipDisplayOffsetX, Config::tipDisplayOffsetY);
            VRCompositorHelper::SetTipDisplayRotation(Config::tipDisplayYaw, Config::tipDisplayPitch);

            // Initialize D3D11 compositor
            if (VRCompositorHelper::IsInitialized()) {
                auto& comp = D3D11Compositor::GetSingleton();
                comp.SetMode(CompositeMode::LuminanceKey);
                comp.SetShowCapturedOverlay(g_config.showTipDisplay);
                comp.Initialize(
                    VRCompositorHelper::GetCompositor(),
                    VRCompositorHelper::GetD3D11Device());
            }

            // Performance patches (non-timer: VSync, black loading screens)
            PerformancePatches::Apply(g_config.perfConfig);

            // Papyrus dynamic update budget
            PapyrusOptimizer::GetSingleton().Init(
                g_config.budgetMaxFPS, g_config.updateBudgetBase);

            // Register for menu events
            if (auto ui = RE::UI::GetSingleton()) {
                ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                    MenuWatcher::GetSingleton());
                logger::info("Registered for MenuOpenCloseEvent");
            }

            logger::info("VRLoadingScreens initialized");
        }

        void onGameSessionLoaded() override
        {
            g_config.load(); // re-read MCM settings
            g_pendingTimerPatches = true;

            auto& lsm = LoadingScreenManager::GetSingleton();
            lsm.SetGameSessionLoaded();
            lsm.SetBackgroundsEnabled(g_config.enableBackgroundImages);
            lsm.SetRenderDelay(g_config.renderDelaySeconds);
            lsm.SetOverlayAlpha(g_config.overlayAlpha);

            // Update tip display flag (live-update after MCM close)
            D3D11Compositor::GetSingleton().SetShowCapturedOverlay(g_config.showTipDisplay);

            logger::info("Game session loaded, timer patches pending");
        }

        void onFrameUpdate() override
        {
            VRCompositorHelper::UpdateLastKnownPose();
            PapyrusOptimizer::GetSingleton().Update();
            LoadingScreenManager::GetSingleton().Update();
        }
    };
}

static VRLoadingScreensMod g_mod_instance;

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_skse, F4SE::PluginInfo* a_info)
{
    g_mod = &g_mod_instance;
    return g_mod->onF4SEPluginQuery(a_skse, a_info);
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    return g_mod->onF4SEPluginLoad(a_f4se);
}
