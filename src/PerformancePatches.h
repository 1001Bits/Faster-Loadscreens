#pragma once

namespace VRLoadingScreens
{
    struct PerformanceConfig
    {
        bool untieSpeedFromFPS = true;
        bool disableiFPSClamp = true;
        bool disableBlackLoadingScreens = false;
        bool disableVSyncWhileLoading = true;
        bool disable3DModel = true;
        bool disableAnimationLoop = true;       // NOP loading animation (biggest speedup)
        bool yieldCPUDuringLoading = true;      // Yield render thread during loading (20-40% speedup)
        bool oneThreadWhileLoading = true;      // Limit to single CPU core during loading
    };

    class PerformancePatches
    {
    public:
        static void Apply(const PerformanceConfig& config);
        static void ApplyTimerPatches(const PerformanceConfig& config);
        static void OnLoadingMenuOpen();
        static void OnLoadingMenuClose();
        static void OnPresent();

        // NG: check if loading screen is active (based on AdvanceMovie hook)
        static bool IsNGLoadingActive();
        static void UpdateNGLoadingState();

    private:
        // ===== VR + OG 1.10.163 offsets (shared RVAs) =====
        static constexpr std::uintptr_t UntieSpeedFromFPS_Offset_VR = 0x1b962bb;
        static constexpr std::uintptr_t DisableiFPSClamp_Offset_VR = 0x1b96268;
        static constexpr std::uintptr_t DisableBlackLoadingScreens_Offset_OG = 0x1313eb6;
        static constexpr std::uintptr_t SetForegroundModel_Call_Offset_OG = 0x131418b;

        // ===== DisableAnimationOnLoadingScreens — NOP5 the animation epilogue =====
        // Found via RTTI chain: BSRagdollDriverInterface vtable -> sub-function -> epilogue
        // OG: FUN_140fc99a0 + 0x213, NG: REL::ID(2227631) + 0x223
        static constexpr std::uint32_t DisableAnimation_ID_NG = 2227631;
        static constexpr std::uintptr_t DisableAnimation_Offset_NG = 0x223;
        // DisableBlackLoadingScreens: REL::ID(2249217) + 0x116 → JMP (0xEB)
        static constexpr std::uint32_t DisableBlackLoading_ID_NG = 2249217;
        static constexpr std::uintptr_t DisableBlackLoading_Offset_NG = 0x116;

        // ===== LoadingMenu functions — disable 3D rendering on flat loading screens =====
        // InitModel:        prevents 3D model creation
        // AdvanceMovie:     prevents ALL loading screen rendering (3D, fog, spinner)
        //                   Tips text is lost but we show our own background image
        // SetForegroundModel: prevents model setup even if InitModel ran
        static constexpr std::uint32_t InitModel_ID = 724763;
        static constexpr std::uint32_t AdvanceMovie_ID = 314582;  // LoadingMenu::AdvanceMovie

        // BSGraphics::RendererData (for VSync toggle)
        static constexpr std::uintptr_t BSGraphics_RendererData_Offset_VR = 0x060f3ce8;
        static constexpr std::uint32_t PresentInterval_Offset = 0x40;

        // ===== PresentThread — flat only (no VR equivalent) =====
        // BSGraphics::Renderer::PresentThread: hooks blocking wait + Present call
        static constexpr std::uint32_t PresentThread_ID_NG = 2276834;     // NG address library ID
        static constexpr std::uint32_t PresentThread_ID_OG = 700869;     // OG address library ID
        static constexpr std::uintptr_t PresentThread_BlockWait_Offset = 0x30;   // +0x30: blocking wait (10 bytes)
        static constexpr std::uintptr_t PresentThread_PresentCall_Offset = 0x48; // +0x48: CALL to Present

        // Patch byte arrays
        static constexpr std::uint8_t NOP5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
        static constexpr std::uint8_t JMP_SHORT[] = { 0xEB };
        static constexpr std::uint8_t RET[] = { 0xC3 };
        // NOP sled (10 bytes) — placeholder before write_branch overwrites first 5
        static constexpr std::uint8_t NOP10[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

        static inline std::uint32_t s_originalPresentInterval = 0;
        static inline bool s_vsyncEnabled = false;
        static inline std::uint32_t* s_presentIntervalPtr = nullptr;

        static inline bool s_yieldEnabled = false;
        static inline bool s_inLoading = false;

        // NG loading screen detection via AdvanceMovie/InitModel hooks
        static inline std::chrono::steady_clock::time_point s_lastAdvanceMovieTime{};
        static inline std::chrono::steady_clock::time_point s_loadStartTime{};
        static inline bool s_ngLoadingActive = false;
        static inline int s_ngLoadCount = 0;
        using AdvanceMovieFn = void(*)(void* thisPtr);
        static inline AdvanceMovieFn s_origAdvanceMovie = nullptr;
        using InitModelFn = void(*)(void* thisPtr);
        static inline InitModelFn s_origInitModel = nullptr;
        static void __cdecl HookedAdvanceMovie(void* thisPtr);
        static void __cdecl HookedInitModel(void* thisPtr);

        // OneThreadWhileLoading state
        static inline bool s_oneThreadEnabled = false;
        static inline DWORD_PTR s_normalAffinityMask = 0;

    };
}
