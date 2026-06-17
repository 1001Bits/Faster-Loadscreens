#include "PCH.h"
#include "LoadingLoopHook.h"
#include "Config.h"
#include "DoorPrefetchHook.h"
#include "PatternScan.h"

#include <Windows.h>
#include <MinHook.h>

namespace FasterLoadscreens
{
    namespace
    {
        // ============================================================
        // Signatures (?? = wildcard). Each verified to match exactly once:
        //   flat: SE 1.5.97 and AE 1.6.1170
        //   vr:   SkyrimVR 1.4.15
        // ============================================================

        // JobListManager::ServingThread state-check: the loading loop's
        // condition function. `state` lives at +0x68 (SE/AE) / +0x70 (VR).
        constexpr const char* SIG_STATECHECK_FLAT =
            "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 12 85 C9 75 07 "
            "39 48 68 0F 95 C0 C3 39 48 68 0F 94 C0 C3";
        constexpr const char* SIG_STATECHECK_VR =
            "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 12 85 C9 75 07 "
            "39 48 70 0F 95 C0 C3 39 48 70 0F 94 C0 C3";

        // JobListManager::ServingThread::DisplayLoadingScreen
        constexpr const char* SIG_DLS_FLAT =
            "40 57 48 83 EC 50 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 68 "
            "0F 29 74 24 40 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 05 "
            "E8 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 0F 57 F6 "
            "B9 02 00 00 00 E8 ?? ?? ?? ?? 84 C0";
        constexpr const char* SIG_DLS_VR =
            "4C 8B DC 57 48 81 EC C0 00 00 00 48 C7 44 24 30 FE FF FF FF "
            "49 89 5B 10 49 89 6B 18 49 89 73 20 41 0F 29 73 E8 41 0F 29 7B D8 "
            "48 8D 2D ?? ?? ?? ?? 0F 57 F6 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84";

        // Known-version fast paths (signature still verified at the RVA).
        struct KnownRVAs
        {
            std::uint16_t major, minor, patch;
            std::uint32_t dls, stateCheck;
        };
        constexpr KnownRVAs KNOWN[] = {
            { 1, 5, 97,   0x63FA50, 0x5763B0 },   // SE 1.5.97
            { 1, 6, 1170, 0x6D2120, 0x5FBFF0 },   // AE 1.6.1170
            { 1, 4, 15,   0x648AE0, 0x57C980 },   // VR 1.4.15
        };

        // ============================================================
        // Target resolution (shared scanner + known-RVA fast path)
        // ============================================================

        std::uintptr_t Resolve(const scan::Pattern& p, std::uint32_t knownRva, const char* tag)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
            if (knownRva) {
                const auto addr = base + knownRva;
                if (scan::MatchAt(reinterpret_cast<const std::uint8_t*>(addr), p)) {
                    logger::info("{}: resolved via known RVA {:x}", tag, knownRva);
                    return addr;
                }
                logger::warn("{}: bytes at known RVA {:x} don't match — falling back to scan",
                    tag, knownRva);
            }
            const auto addr = scan::FindUnique(p, tag);
            if (addr) {
                logger::info("{}: resolved via signature scan at RVA {:x}", tag, addr - base);
            }
            return addr;
        }

        // ============================================================
        // Hook state
        // ============================================================

        using DisplayLoadingScreenFn = void (*)();
        using StateCheckFn = bool (*)(std::int32_t);

        DisplayLoadingScreenFn s_origDLS = nullptr;
        StateCheckFn s_origStateCheck = nullptr;

        thread_local bool tls_inLoadingLoop = false;
        std::atomic<bool> s_loadingActive{ false };
        // Latched true on every DisplayLoadingScreen; SceneReadyHold consumes it
        // to gate "is this close ending a real loading screen?".
        std::atomic<bool> s_loadingScreenSeen{ false };

        // Loop statistics for the current loading screen (serving thread only).
        thread_local std::uint32_t tls_iterations = 0;
        thread_local bool tls_frozen = false;

        bool s_isVR = false;

        // SSE Display Tweaks coexistence. DT's loading-screen FPS limiter
        // busy-waits (spins) to hold its target FPS, which re-occupies the CPU
        // core our throttle-sleep frees for the loader — negating the throttle
        // (measured: doors ~2.3x slower with DT). Detected lazily on the first
        // loading screen (DT, also an SKSE plugin, is loaded by then, unlike at
        // our SKSEPluginLoad-time Install()). When present we promote throttle
        // (mode 1) to freeze (mode 2) on flat: the loop exits, so DT's limiter
        // has no frame to spin on.
        std::atomic<bool> s_dtPresent{ false };
        std::atomic<bool> s_dtChecked{ false };

        void HookedDisplayLoadingScreen()
        {
            const auto& cfg = Config::Get();
            if (!s_dtChecked.exchange(true, std::memory_order_relaxed)) {
                const bool dt = ::GetModuleHandleA("SSEDisplayTweaks.dll") != nullptr;
                s_dtPresent.store(dt, std::memory_order_relaxed);
                if (dt) {
                    const char* how =
                        s_isVR                     ? "VR — leaving loop mode as configured" :
                        cfg.displayTweaksMode == 1 ? "mode 1 FREEZE — throttle promotes to freeze (loop exits, "
                                                     "DT's busy-wait limiter has no frame to spin on)" :
                        cfg.displayTweaksMode == 2 ? "mode 2 NEUTRALIZE — screen stays animated; loading serving "
                                                     "thread dropped to LOWEST priority so DT's busy-wait yields "
                                                     "to the loader (DT's in-game FPS untouched)" :
                                                     "mode 0 OFF — leaving loop mode as configured (DT may negate the throttle)";
                    logger::info("SSE Display Tweaks detected — {}", how);
                }
            }

            // (Reverted: the tracked-ExteriorCellLoader grid + its FlushQueuedLoads reconcile
            //  are gone — door prefetch is back to the fast single-cell QueuePreload that gave
            //  the -55%. Running the loader's Finish here with nothing of ours queued risked
            //  touching the engine's OWN transition loads and slowing every transition.)

            // NEUTRALIZE (mode 2): drop THIS (the loading serving) thread to LOWEST
            // priority for the duration of the loading screen. DT's loading-screen
            // limiter busy-waits on this thread; at LOWEST priority that spin yields
            // to the loader (the process is HIGH-priority-boosted during loads), so
            // the loader keeps the CPU while the screen still animates. DT is untouched.
            const bool neutralize = !s_isVR && cfg.loopMode == 1 &&
                cfg.displayTweaksMode == 2 && s_dtPresent.load(std::memory_order_relaxed);
            const int savedPriority = neutralize ? ::GetThreadPriority(::GetCurrentThread())
                                                 : THREAD_PRIORITY_NORMAL;
            if (neutralize) {
                ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            }

            const auto start = std::chrono::steady_clock::now();
            tls_inLoadingLoop = true;
            tls_iterations = 0;
            tls_frozen = false;
            s_loadingActive.store(true, std::memory_order_relaxed);
            s_loadingScreenSeen.store(true, std::memory_order_relaxed);

            s_origDLS();

            s_loadingActive.store(false, std::memory_order_relaxed);
            tls_inLoadingLoop = false;
            if (neutralize) {
                ::SetThreadPriority(::GetCurrentThread(), savedPriority);
            }

            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            logger::info("Loading screen done: {:.2f}s, {} render iterations{}",
                ms / 1000.0, tls_iterations, tls_frozen ? " (frozen)" : "");
        }

        bool HookedStateCheck(std::int32_t a_state)
        {
            const bool result = s_origStateCheck(a_state);

            // Only intervene for the loading-screen loop condition, evaluated
            // on the serving thread inside DisplayLoadingScreen.
            if (!result || a_state != 2 || !tls_inLoadingLoop) {
                return result;
            }

            ++tls_iterations;
            const auto& cfg = Config::Get();

            // With SSE Display Tweaks present (flat): mode 1 promotes the throttle to
            // freeze; mode 2 (neutralize) keeps the throttle but instead deprioritizes
            // this thread (in HookedDisplayLoadingScreen) so DT's busy-wait yields to
            // the loader. Only mode 1 changes the effective loop mode here.
            int mode = cfg.loopMode;
            if (mode == 1 && !s_isVR && cfg.displayTweaksMode == 1 &&
                s_dtPresent.load(std::memory_order_relaxed)) {
                mode = 2;
            }

            switch (mode) {
            case 1:  // throttle
                ::Sleep(static_cast<DWORD>(s_isVR ? cfg.throttleMsVR : cfg.throttleMs));
                break;
            case 2:  // freeze (flat only — VR compositor needs frames)
                if (!s_isVR) {
                    if (tls_iterations > static_cast<std::uint32_t>(cfg.freezeAfterFrames)) {
                        // Pretend loading already finished: the cosmetic loop
                        // exits, the screen keeps the last rendered frame, and
                        // the actual load (other threads) gets everything.
                        tls_frozen = true;
                        return false;
                    }
                } else {
                    ::Sleep(static_cast<DWORD>(cfg.throttleMsVR));
                }
                break;
            default:
                break;
            }
            return result;
        }
    }

    bool LoadingLoopHook::IsLoadingScreenActive()
    {
        return s_loadingActive.load(std::memory_order_relaxed);
    }

    bool LoadingLoopHook::WasLoadingScreenSeen()
    {
        return s_loadingScreenSeen.load(std::memory_order_relaxed);
    }

    void LoadingLoopHook::ClearLoadingScreenSeen()
    {
        s_loadingScreenSeen.store(false, std::memory_order_relaxed);
    }

    bool LoadingLoopHook::Install()
    {
        const auto& cfg = Config::Get();
        s_isVR = REL::Module::IsVR();
        const auto ver = REL::Module::get().version();

        // Baseline (benchmark) mode: hook DisplayLoadingScreen for TIMING only,
        // and never install the stateCheck throttle — the loop runs vanilla so
        // the logged times are a true "before" reference.
        const bool baseline = (cfg.benchmarkMode == 0);
        const bool wantThrottle = !baseline && cfg.loopMode != 0;

        // Ghidra-verified: on VR the loading-render loop calls OpenVR every
        // iteration (it feeds the SteamVR compositor) and is already gated to
        // the headset frame rate, so it is NOT the CPU-hog competitor it is on
        // flat. Throttling it mostly causes loading-screen judder for little
        // load-time gain. iLoopMode=0 is recommended on VR; honour an explicit
        // opt-in but warn.
        if (wantThrottle && s_isVR) {
            logger::warn("VR: loop throttle enabled (iLoopMode={}) — may cause loading-screen "
                         "judder; the VR loop feeds the compositor each frame. iLoopMode=0 advised.",
                cfg.loopMode);
        }

        std::uint32_t dlsRva = 0;
        std::uint32_t checkRva = 0;
        for (const auto& k : KNOWN) {
            if (ver[0] == k.major && ver[1] == k.minor && ver[2] == k.patch) {
                dlsRva = k.dls;
                checkRva = k.stateCheck;
                break;
            }
        }

        const auto dlsPattern = scan::Parse(s_isVR ? SIG_DLS_VR : SIG_DLS_FLAT);
        const auto dlsAddr = Resolve(dlsPattern, dlsRva, "DisplayLoadingScreen");
        if (!dlsAddr) {
            return false;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("MinHook init failed ({})", static_cast<int>(initStatus));
            return false;
        }

        auto hook = [](std::uintptr_t target, void* detour, void** original, const char* tag) {
            const MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
            if (create != MH_OK) {
                logger::error("{}: MH_CreateHook failed ({})", tag, static_cast<int>(create));
                return false;
            }
            const MH_STATUS enable = MH_EnableHook(reinterpret_cast<void*>(target));
            if (enable != MH_OK) {
                logger::error("{}: MH_EnableHook failed ({})", tag, static_cast<int>(enable));
                MH_RemoveHook(reinterpret_cast<void*>(target));
                return false;
            }
            return true;
        };

        // Timing wrapper — always installed (both baseline and full).
        if (!hook(dlsAddr, reinterpret_cast<void*>(&HookedDisplayLoadingScreen),
                reinterpret_cast<void**>(&s_origDLS), "DisplayLoadingScreen")) {
            return false;
        }

        // Throttle/freeze — full mode only.
        if (wantThrottle) {
            const auto checkPattern = scan::Parse(s_isVR ? SIG_STATECHECK_VR : SIG_STATECHECK_FLAT);
            const auto checkAddr = Resolve(checkPattern, checkRva, "ServingThread::stateCheck");
            if (checkAddr) {
                hook(checkAddr, reinterpret_cast<void*>(&HookedStateCheck),
                    reinterpret_cast<void**>(&s_origStateCheck), "ServingThread::stateCheck");
            }
        }

        logger::info("LoadingLoopHook installed ({}, mode={}, VR={})",
            baseline ? "BASELINE/timing-only" : "full", cfg.loopMode, s_isVR);
        return true;
    }
}
