#pragma once

#include <atomic>
#include <cstdint>

namespace VRLoadingScreens
{
    struct PerformanceConfig
    {
        bool untieSpeedFromFPS = true;
        bool disableiFPSClamp = true;
        bool disableBlackLoadingScreens = false;
        bool disableVSyncWhileLoading = true;
        bool disable3DModel = true;
        // NG/AE's load-scoped animation NOP is distinct from disable3DModel's
        // SetModel RET. HFPF DisableAnimationOnLoadingScreens owns only this site.
        bool disableAnimationOnLoadingScreens = true;
        // Kept as a compatibility/config surface, but the former live ten-byte
        // PresentThread rewrite is deliberately disabled: there is no proven
        // atomic/hook implementation for changing it while the worker executes.
        bool yieldCPUDuringLoading = false;
        // Deliberately off: Fallout's loader is multithreaded. Pinning every load
        // to one core made the measured save load roughly twenty times slower.
        bool oneThreadWhileLoading = false;
    };

    class PerformancePatches
    {
    public:
        static void Apply(const PerformanceConfig& config);
        static void ApplyTimerPatches(const PerformanceConfig& config);
        static void OnLoadingMenuOpen();
        static void OnLoadingMenuClose();
        // NG/AE mode 3: apply the deferred DisableAnimation NOP5 once tips are
        // captured (AdvanceMovie frozen). Called from LoadingScreenManager::Update().
        static void ApplyNGDisableAnimDeferred();

    private:
        // ===== VR + OG 1.10.163 offsets =====
        static constexpr std::uintptr_t UntieSpeedFromFPS_Offset_VR = 0x1b962bb;
        static constexpr std::uintptr_t DisableiFPSClamp_Offset_VR = 0x1b96268;
        // LoadingMenu destination-state branch. These are not shared:
        // 0x1313EB6 is correct only in VR and is an unrelated CALL on OG.
        static constexpr std::uintptr_t DisableBlackLoadingScreens_Offset_OG = 0x1297076;
        static constexpr std::uintptr_t DisableBlackLoadingScreens_Offset_VR = 0x1313EB6;
        static constexpr std::uintptr_t SetForegroundModel_Call_Offset_OG = 0x131418b;

        // ===== DisableAnimationOnLoadingScreens =====
        // NG/AE NOP5s the verified animation epilogue at
        // REL::ID(2227631) + 0x223. OG uses a different mechanism: HFPF 0.8.13
        // NOP4s the CMP at RVA 0xCBFFCD, which overlaps the first four bytes of
        // LoadingScreenManager's exact per-load CMP+JE NOP10. Do not describe
        // or resolve the NG epilogue as an OG site.
        static constexpr std::uint32_t DisableAnimation_ID_NG = 2227631;
        static constexpr std::uintptr_t DisableAnimation_Offset_NG = 0x223;
        // NG per-load "set model" function (FUN_14099a1a0, sibling of
        // BackgroundScreenModel::InitModels). Called by LoadingMenu::Func0
        // and LoadingMenu::Func4 on each LoadingMenu open to pick which model
        // to display. RET at entry skips the model-selection vtable call so
        // the renderer never gets told to show anything.
        // Verified per-load (not one-shot startup) on 1.11.221 and 1.11.240:
        // the two callers are LoadingMenu virtual methods, not init code.
        static constexpr std::uint32_t BackgroundScreenModel_SetModel_ID_NG = 2219591;
        // DisableBlackLoadingScreens: REL::ID(2249217) + 0x116 → JMP (0xEB)
        static constexpr std::uint32_t DisableBlackLoading_ID_NG = 2249217;
        static constexpr std::uintptr_t DisableBlackLoading_Offset_NG = 0x116;

        // ===== LoadingMenu functions — disable 3D rendering on flat loading screens =====
        // InitModel:        prevents 3D model creation
        // AdvanceMovie:     prevents ALL loading screen rendering (3D, fog, spinner)
        //                   Tips text is lost but we show our own background image
        // SetForegroundModel: prevents model setup even if InitModel ran
        static constexpr std::uint32_t InitModel_ID = 724763;
        // 1.10.163 primary-vtable slot 4. ID 314582 is the LoadingMenu
        // destructor (RVA 0x1296D40) and must never be used as a RET target.
        static constexpr std::uint32_t AdvanceMovie_ID = 618896;

        // BSGraphics::RendererData (for VSync toggle)
        static constexpr std::uintptr_t BSGraphics_RendererData_Offset_VR = 0x060f3ce8;
        static constexpr std::uint32_t PresentInterval_Offset = 0x40;

        // Patch byte arrays
        static constexpr std::uint8_t NOP5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
        static constexpr std::uint8_t JMP_SHORT[] = { 0xEB };
        static constexpr std::uint8_t RET[] = { 0xC3 };

        // VSync toggle state — written by Apply() (main thread at startup) and
        // by OnLoadingMenuOpen/Close (message thread). The Present hook on the
        // render thread doesn't read these directly, but the pointer is
        // dereferenced on the message thread under load/save races: torn reads
        // on shutdown could write to freed RendererData. Atomic stops that.
        static inline std::atomic<std::uint32_t>  s_originalPresentInterval{ 0 };
        static inline std::atomic<bool>           s_vsyncEnabled{ false };
        static inline std::atomic<std::uint32_t*> s_presentIntervalPtr{ nullptr };

        // True only after Apply() ran (i.e. benchmarkMode != 0). Gates the
        // per-load OnLoadingMenuOpen/Close toggles (vsync disable, CPU affinity,
        // NG anim NOP) so benchmark=0 (baseline) is genuinely vanilla — it must
        // not disable vsync during loads (which previously leaked via the
        // deferred vsync-arm inside OnLoadingMenuOpen).
        static inline std::atomic<bool> s_patchesActive{ false };

        // Mirrors PerformanceConfig::disableVSyncWhileLoading, captured in Apply().
        // Gates BOTH the initial arm and the DEFERRED arm in OnLoadingMenuOpen, so
        // setting the config flag false fully disables the loading-screen vsync
        // toggle. (The deferred arm previously ignored the flag and re-armed it, so
        // flipping the config did nothing on OG flat.)
        static inline std::atomic<bool> s_vsyncToggleEnabled{ false };

        // OneThreadWhileLoading state
        static inline std::atomic<bool> s_oneThreadEnabled{ false };
        static inline DWORD_PTR s_normalAffinityMask = 0;

        // NG DisableAnimation — moved from one-shot Apply() to per-load apply/restore.
        // The patch NOPs the loading-render epilogue (same as HFPF) which is the
        // biggest single-source loading-time speedup, but also blocks LoadingScreen::
        // Render from advancing the Scaleform SWF that draws tips/level text. We
        // apply it immediately in modes 0/2, defer it until tips capture in mode 3,
        // and keep it off in mode 1.
        static inline std::uintptr_t s_ngDisableAnimAddr = 0;
        static inline std::uint8_t   s_ngDisableAnimOrigBytes[5] = {};
        static inline bool s_ngDisableAnimSaved = false;
        static inline bool s_ngDisableAnimApplied = false;

    };
}
