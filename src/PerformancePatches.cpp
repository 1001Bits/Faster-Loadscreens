#include "PCH.h"
#include "PerformancePatches.h"
#include "VRCompositorHelper.h"
#include <MinHook.h>

#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

namespace VRLoadingScreens
{
    // ========================================================================
    // Performance patches
    // ========================================================================

    void PerformancePatches::Apply(const PerformanceConfig& config)
    {
        bool isVR = REL::Module::IsVR();
        bool isNG = !isVR && REL::Module::IsNG();

        s_yieldEnabled = config.yieldCPUDuringLoading;

        // ================================================================
        // OG NOP5 VEH: catches access violation from corrupted epilogue return
        // Fixes the stack and redirects to the proper return address
        // ================================================================
        // NOP5 replaces "add rsp,0x20; pop rdi" before RET.
        // RET pops [RSP] (garbage from frame) and jumps to it → access violation.
        // VEH restores: RSP += 0x18 (remaining frame), pop RDI, pop return addr, continue.

        // ================================================================
        // Disable 3D model on loading screens
        // VR: NOP5 the SetForegroundModel CALL in AdvanceMovie (prevents per-frame model update)
        // Flat: RET at InitModel entry (prevents model creation entirely, enables luminance key)
        // ================================================================
        if (config.disable3DModel) {
            if (isVR) {
                // VR: NOP5 the per-frame SetForegroundModel call in AdvanceMovie
                REL::Relocation<std::uintptr_t> target{ REL::Offset(SetForegroundModel_Call_Offset_OG) };
                REL::safe_write(target.address(), NOP5, sizeof(NOP5));
                logger::info("VR: DisableAnimation NOP5 at {:x}", target.address());
            } else if (isNG) {
                // NG: NOP5 the animation loop instruction inside the loading screen function
                // Same approach as High FPS Physics Fix: REL::ID(2227631) + 0x223
                // This is NOT a function hook — it patches a specific instruction that may be
                // reached through inlining or mid-function jumps, not the normal entry point.
                try {
                    REL::Relocation<std::uintptr_t> loadingFunc{ REL::ID(DisableAnimation_ID_NG) };
                    auto patchAddr = loadingFunc.address() + DisableAnimation_Offset_NG;
                    REL::safe_write(patchAddr, NOP5, sizeof(NOP5));
                    logger::info("NG: DisableAnimation NOP5 at {:x} (ID {} + {:x})",
                        patchAddr, DisableAnimation_ID_NG, DisableAnimation_Offset_NG);
                } catch (...) {
                    logger::warn("NG: DisableAnimation ID {} not found", DisableAnimation_ID_NG);
                }
            } else {
                // OG flat: RET InitModel only (no 3D model, tips still render)
                // Dynamic animation loop NOP in LoadingScreenManager handles speed
                REL::Relocation<std::uintptr_t> initModel{ REL::ID(InitModel_ID) };
                REL::safe_write(initModel.address(), RET, sizeof(RET));
                logger::info("Flat: InitModel RET at {:x}", initModel.address());
            }
        }

        // ================================================================
        // DisableBlackLoadingScreens — patch conditional JNZ to unconditional JMP
        // Prevents game from showing completely black loading screen (no text/model).
        // ================================================================
        if (config.disableBlackLoadingScreens) {
            if (isNG) {
                REL::Relocation<std::uintptr_t> target{ REL::ID(DisableBlackLoading_ID_NG) };
                REL::safe_write(target.address() + DisableBlackLoading_Offset_NG, JMP_SHORT, sizeof(JMP_SHORT));
                logger::info("NG: DisableBlackLoadingScreens at {:x}+{:x}", target.address(), DisableBlackLoading_Offset_NG);
            } else {
                REL::Relocation<std::uintptr_t> target{ REL::Offset(DisableBlackLoadingScreens_Offset_OG) };
                REL::safe_write(target.address(), JMP_SHORT, sizeof(JMP_SHORT));
                logger::info("{}: DisableBlackLoadingScreens at {:x}", isVR ? "VR" : "OG", target.address());
            }
        }

        // ================================================================
        // VSync disable during loading
        // ================================================================
        if (config.disableVSyncWhileLoading) {
            if (isVR) {
                REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset_VR) };
                void* rendererData = *rendererDataPtr;
                if (rendererData) {
                    s_presentIntervalPtr = reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uintptr_t>(rendererData) + PresentInterval_Offset);
                    s_originalPresentInterval = *s_presentIntervalPtr;
                    s_vsyncEnabled = true;
                    logger::info("VR: VSync toggle ready (presentInterval={})", s_originalPresentInterval);
                }
            } else {
                // RendererData may not work on all NG versions — skip if device unavailable
                if (VRCompositorHelper::GetD3D11Device()) {
                    auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
                    if (rendererData) {
                        s_presentIntervalPtr = &rendererData->presentInterval;
                        s_originalPresentInterval = *s_presentIntervalPtr;
                        s_vsyncEnabled = true;
                        logger::info("Flat: VSync toggle ready (presentInterval={})", s_originalPresentInterval);
                    }
                } else {
                    logger::info("Flat: VSync toggle deferred (device not yet available)");
                }
            }
        }

        // ================================================================
        // iFPSClamp disable
        // ================================================================
        if (config.disableiFPSClamp) {
            try {
                if (auto* setting = RE::GetINISetting("iFPSClamp:General")) {
                    setting->SetInt(0);
                    logger::info("iFPSClamp:General set to 0");
                }
            } catch (...) {
                logger::warn("iFPSClamp disable skipped (address library unavailable)");
            }
        }

        // ================================================================
        // OneThreadWhileLoading — limit to single CPU core during loading
        // Reduces context switching, allows loading thread to max out one core.
        // ================================================================
        if (config.oneThreadWhileLoading) {
            HANDLE hProcess = GetCurrentProcess();
            DWORD_PTR procMask, sysMask;
            GetProcessAffinityMask(hProcess, &procMask, &sysMask);
            s_normalAffinityMask = procMask;
            s_oneThreadEnabled = true;
            logger::info("OneThreadWhileLoading enabled (normalMask={:x})", s_normalAffinityMask);
        }

        // ================================================================
        // PresentThread hook — flat only, blocks render thread during loading
        // NOPs the blocking wait at +0x30, hooks Present CALL at +0x48
        // Skipped when HFPF is detected (it does the same thing)
        // ================================================================
        // PresentThread — NOP the blocking wait to reduce CPU waste
        // Sleep(1) on PresentThread causes deadlock on OG (threads are interdependent)
        if (config.yieldCPUDuringLoading && !isVR) {
            try {
                auto ptId = isNG ? PresentThread_ID_NG : PresentThread_ID_OG;
                REL::Relocation<std::uintptr_t> pt{ REL::ID(ptId) };
                auto ptAddr = pt.address();

                static constexpr std::uint8_t NOP10[] = { 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
                REL::safe_write(ptAddr + PresentThread_BlockWait_Offset, NOP10, sizeof(NOP10));
                logger::info("PresentThread NOP at {:x}+{:x}", ptAddr, PresentThread_BlockWait_Offset);
            } catch (...) {
                logger::warn("PresentThread patch failed");
            }
        }

        logger::info("Performance patches applied (VR={}, NG={}, cpuYield={})", isVR, isNG, s_yieldEnabled);
    }

    void PerformancePatches::ApplyTimerPatches(const PerformanceConfig& config)
    {
        if (!REL::Module::IsVR()) return;

        static bool s_applied = false;
        if (s_applied) return;
        s_applied = true;

        if (config.untieSpeedFromFPS) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(UntieSpeedFromFPS_Offset_VR) };
            const std::uint8_t patch[] = { 0x00 };
            REL::safe_write(target.address(), patch, sizeof(patch));
            logger::info("VR: UntieSpeedFromFPS patched at {:x}", target.address());
        }

        if (config.disableiFPSClamp) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(DisableiFPSClamp_Offset_VR) };
            const std::uint8_t patch[] = { 0x38 };
            REL::safe_write(target.address(), patch, sizeof(patch));
            logger::info("VR: DisableiFPSClamp patched at {:x}", target.address());

            if (auto* setting = RE::GetINISetting("iFPSClamp:General")) {
                setting->SetInt(0);
            }
        }
    }

    void PerformancePatches::OnLoadingMenuOpen()
    {
        s_inLoading = true;

        // Retry deferred VSync setup if not initialized yet
        // Skip on NG — RendererData::GetSingleton may crash (report_and_fail)
        if (!s_vsyncEnabled && !s_presentIntervalPtr && !REL::Module::IsVR() && !REL::Module::IsNG()) {
            try {
                auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
                if (rendererData) {
                    s_presentIntervalPtr = &rendererData->presentInterval;
                    s_originalPresentInterval = *s_presentIntervalPtr;
                    s_vsyncEnabled = true;
                    logger::info("Flat: VSync toggle ready (deferred, presentInterval={})", s_originalPresentInterval);
                }
            } catch (...) {}
        }

        if (s_vsyncEnabled && s_presentIntervalPtr) {
            s_originalPresentInterval = *s_presentIntervalPtr;
            *s_presentIntervalPtr = 0;
            logger::info("VSync disabled for loading (was {})", s_originalPresentInterval);
        }

        if (s_oneThreadEnabled) {
            SetProcessAffinityMask(GetCurrentProcess(), 1);
            logger::info("Loading: limited to single CPU core");
        }

    }

    void PerformancePatches::OnLoadingMenuClose()
    {
        s_inLoading = false;

        if (s_vsyncEnabled && s_presentIntervalPtr) {
            *s_presentIntervalPtr = s_originalPresentInterval;
            logger::info("VSync restored to {}", s_originalPresentInterval);
        }

        if (s_oneThreadEnabled) {
            SetProcessAffinityMask(GetCurrentProcess(), s_normalAffinityMask);
            logger::info("Loading: restored CPU affinity ({:x})", s_normalAffinityMask);
        }

    }

    void PerformancePatches::OnPresent()
    {
        if (s_yieldEnabled && s_inLoading) {
            SwitchToThread();
        }
    }

    // ================================================================
    // NG loading detection hooks (MinHook)
    // ================================================================

    void __cdecl PerformancePatches::HookedInitModel(void* thisPtr)
    {
        // Block 3D model creation by NOT calling original.
        s_lastAdvanceMovieTime = std::chrono::steady_clock::now();
        logger::info("NG: InitModel called (blocked)");
    }

    void __cdecl PerformancePatches::HookedAdvanceMovie(void* thisPtr)
    {
        // Block all loading screen rendering by NOT calling original.
        // Record timestamp — called every frame during loading.
        auto now = std::chrono::steady_clock::now();
        if (!s_ngLoadingActive) {
            s_loadStartTime = now;
            logger::info("NG: HookedAdvanceMovie first call — loading detected");
        }
        s_lastAdvanceMovieTime = now;
    }

    bool PerformancePatches::IsNGLoadingActive()
    {
        return s_ngLoadingActive;
    }

    void PerformancePatches::UpdateNGLoadingState()
    {
        auto now = std::chrono::steady_clock::now();
        auto sinceLastCall = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - s_lastAdvanceMovieTime).count();

        bool wasActive = s_ngLoadingActive;

        // If AdvanceMovie was called within the last 500ms, loading is active
        // Use s_lastAdvanceMovieTime default (epoch) check to avoid false positive at startup
        bool recentActivity = (s_lastAdvanceMovieTime.time_since_epoch().count() > 0)
            && (sinceLastCall < 500);

        if (recentActivity && !wasActive) {
            // Loading started
            s_ngLoadingActive = true;
            s_ngLoadCount++;
            logger::info("NG: loading started (load #{})", s_ngLoadCount);
            OnLoadingMenuOpen();
        } else if (!recentActivity && wasActive) {
            // Loading ended
            s_ngLoadingActive = false;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s_loadStartTime).count();
            logger::info("NG: loading ended (load #{}, {:.2f}s)", s_ngLoadCount, elapsed / 1000.0);
            OnLoadingMenuClose();
        }
    }
}
