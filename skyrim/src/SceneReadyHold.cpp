#include "PCH.h"
#include "SceneReadyHold.h"
#include "Config.h"
#include "LoadingLoopHook.h"

#include "RE/E/ExtraCellGrassData.h"

#include <Windows.h>
#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace FasterLoadscreens
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The engine's show/hide-loading-menu orchestrator:
        //   undefined8 ShowHideLoadingMenu(char a_show, void* a_ctx,
        //                                  uint8_t a_flag, char a_alsoHide)
        //   a_show != 0 -> OPEN (posts LoadingMenu kShow)
        //   a_show == 0 -> CLOSE (posts LoadingMenu kHide, restores HUD/controls,
        //                 clears the engine loading guards, pumps save-load events)
        // We intercept only the CLOSE. Deferring the whole call leaves the engine
        // in its loading state (guards stay set) so the cell loader keeps
        // promoting queued references — exactly the work that finishes the scene.
        using ShowHideFn = std::uint64_t (*)(char, void*, std::uint8_t, char);

        // ShowHideLoadingMenu — hardcoded RVAs per verified runtime (no scan, no
        // Address Library). Unrecognized build → feature off (this is the
        // experimental, off-by-default scene-ready hold anyway).
        struct KnownRVA
        {
            std::uint16_t major, minor, patch;
            std::uint32_t rva;
        };
        constexpr KnownRVA KNOWN_FLAT[] = {
            { 1, 5, 97,   0x157710 },  // SE 1.5.97
            { 1, 6, 1170, 0x1A1150 },  // AE 1.6.1170
        };

        struct HeldArgs
        {
            void* ctx;
            std::uint8_t flag;
            char alsoHide;
        };

        ShowHideFn s_origShowHide = nullptr;

        std::atomic<bool> s_running{ false };
        std::atomic<bool> s_tickQueued{ false };
        std::atomic<bool> s_holdPending{ false };
        // True while WE re-issue the close, so our own detour passes it straight
        // through instead of re-deferring (defense-in-depth; MinHook trampolines
        // don't re-enter the detour, but the original could tail into hooked code).
        std::atomic<bool> s_inReplay{ false };

        // Touched on the main thread only (close detour + the AddTask'd tick,
        // which never run concurrently — same thread).
        Clock::time_point s_holdStart{};
        HeldArgs s_heldArgs{};
        int s_readyStreak = 0;

        // ============================================================
        // Readiness predicate (main thread, bounded cost)
        // ============================================================

        bool HasInGameScene()
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            return pc && pc->GetParentCell() != nullptr;
        }

        // Best-effort, version-fragile grass proxy. Exterior cells only; never a
        // hard gate (the tick time-boxes it). True = grass task consumed and
        // handles populated, or not applicable.
        bool IsGrassReady(RE::TESObjectCELL* a_cell)
        {
            if (!a_cell || !a_cell->IsExteriorCell()) {
                return true;
            }
            auto* g = a_cell->extraList.GetByType<RE::ExtraCellGrassData>();
            if (!g) {
                return false;  // grass data not created yet
            }
            return g->addGrassTask == nullptr && !g->grassHandles.empty();
        }

        bool SceneReady()
        {
            const auto& cfg = Config::Get();

            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc || !pc->Is3DLoaded()) {
                return false;  // player itself not attached yet
            }
            auto* cell = pc->GetParentCell();
            if (!cell || !cell->IsAttached()) {
                return false;  // destination cell not attached
            }
            auto* lcd = cell->GetRuntimeData().loadedData;
            if (!lcd) {
                return false;
            }
            // Cheap pre-gate: wait only for the engine's CRITICAL (near-player)
            // ref queue to drain — that's what causes visible pop-in. We do NOT
            // wait on queuedRefCount (the full queue incl. distant, low-priority
            // refs that finish later and never pop in view), so the hold releases
            // as soon as the near scene is ready instead of the whole cell.
            if (lcd->criticalQueuedRefCount != 0) {
                return false;
            }

            if (cfg.sceneReadyWaitActors) {
                int total = 0;
                int ready = 0;
                int scanned = 0;
                const auto origin = pc->GetPosition();
                cell->ForEachReferenceInRange(origin, cfg.sceneReadyRadius,
                    [&](RE::TESObjectREFR& a_ref) {
                        if (cfg.sceneReadyMaxActors > 0 && ++scanned > cfg.sceneReadyMaxActors) {
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                        auto* actor = a_ref.As<RE::Actor>();
                        if (!actor) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        // Skip refs that legitimately have no 3D, else they'd
                        // stall readiness forever.
                        if (a_ref.IsDisabled() || a_ref.IsInitiallyDisabled() || actor->IsDead()) {
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        ++total;
                        if (actor->Is3DLoaded()) {
                            ++ready;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                if (total > 0 && (ready * 100) < (total * cfg.sceneReadyActorPct)) {
                    return false;
                }
            }
            return true;
        }

        // ============================================================
        // Deferral plumbing (all on the main thread)
        // ============================================================

        // Run the deferred close exactly once. Main thread only.
        void ReleaseHeldClose(const char* a_reason, long long a_elapsedMs)
        {
            if (!s_holdPending.exchange(false, std::memory_order_relaxed)) {
                return;
            }
            const HeldArgs a = s_heldArgs;
            s_inReplay.store(true, std::memory_order_relaxed);
            if (s_origShowHide) {
                s_origShowHide(0, a.ctx, a.flag, a.alsoHide);
            }
            s_inReplay.store(false, std::memory_order_relaxed);
            LoadingLoopHook::ClearLoadingScreenSeen();
            if (a_elapsedMs >= 0) {
                logger::info("SceneReadyHold: released after {}ms ({})", a_elapsedMs, a_reason);
            } else {
                logger::info("SceneReadyHold: released ({})", a_reason);
            }
        }

        // Runs on the game thread (SKSE task) while a hold is pending.
        void TickSceneReadyHold()
        {
            s_tickQueued.store(false, std::memory_order_relaxed);
            if (!s_holdPending.load(std::memory_order_relaxed)) {
                return;
            }
            const auto& cfg = Config::Get();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - s_holdStart).count();

            bool ready = SceneReady();
            // Optional, time-boxed, best-effort grass wait (exterior only): once
            // actors are ready, give grass a short extra budget to populate.
            if (ready && cfg.sceneReadyWaitGrass &&
                elapsed < cfg.sceneReadyGrassBudgetMs) {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                auto* cell = pc ? pc->GetParentCell() : nullptr;
                if (cell && cell->IsExteriorCell() && !IsGrassReady(cell)) {
                    ready = false;
                }
            }

            s_readyStreak = ready ? (s_readyStreak + 1) : 0;

            const bool fire = (s_readyStreak >= cfg.sceneReadyStreak) ||
                              (elapsed >= cfg.sceneReadyMaxHoldMs);
            if (!fire) {
                return;
            }
            ReleaseHeldClose(ready ? "scene ready" : "timeout", elapsed);
        }

        // ~30 Hz cadence thread. Only ever calls SKSE AddTask (thread-safe); all
        // engine access happens inside the task on the game thread. Idle-cheap:
        // wakes only to check the atomic, dispatches a tick only while holding.
        void PollerThread()
        {
            while (s_running.load(std::memory_order_relaxed)) {
                int interval = Config::Get().sceneReadyTickMs;
                if (interval < 8) interval = 8;
                if (interval > 200) interval = 200;
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));

                if (!s_holdPending.load(std::memory_order_relaxed)) {
                    continue;
                }
                if (s_tickQueued.exchange(true, std::memory_order_relaxed)) {
                    continue;  // a tick is already queued; don't pile up
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { TickSceneReadyHold(); });
                } else {
                    s_tickQueued.store(false, std::memory_order_relaxed);
                }
            }
        }

        // ============================================================
        // The hook
        // ============================================================

        std::uint64_t HookedShowHide(char a_show, void* a_ctx, std::uint8_t a_flag, char a_alsoHide)
        {
            if (a_show != 0) {
                // OPEN — a new load is showing the menu. Release any stale hold
                // FIRST: the real close clears the engine's loading re-entry guard,
                // without which this open would early-out and never show.
                ReleaseHeldClose("new load incoming", -1);
                return s_origShowHide(a_show, a_ctx, a_flag, a_alsoHide);
            }

            // CLOSE.
            const auto& cfg = Config::Get();
            if (s_inReplay.load(std::memory_order_relaxed) || !cfg.sceneReadyHoldEnable) {
                return s_origShowHide(0, a_ctx, a_flag, a_alsoHide);
            }

            // Only hold a close that ends a real loading screen (latch set by
            // DisplayLoadingScreen); consume the latch so one screen = one hold.
            const bool seen = LoadingLoopHook::WasLoadingScreenSeen();
            if (seen) {
                LoadingLoopHook::ClearLoadingScreenSeen();
            }
            // Pass through when: not a load close, no in-game scene to wait on
            // (main menu / startup), or the scene is already populated (instant).
            if (!seen || !HasInGameScene() || SceneReady()) {
                return s_origShowHide(0, a_ctx, a_flag, a_alsoHide);
            }

            // Defer: stash the close args, start the hold, return WITHOUT closing.
            // The kHide is never posted, so the menu stays up; the tick will
            // re-issue this close once the scene is ready or the timeout fires.
            s_heldArgs = { a_ctx, a_flag, a_alsoHide };
            s_holdStart = Clock::now();
            s_readyStreak = 0;
            s_holdPending.store(true, std::memory_order_relaxed);
            logger::info("SceneReadyHold: holding loading screen (scene not ready)");
            return 0;
        }

        // ============================================================
        // Target resolution
        // ============================================================

        std::uintptr_t ResolveShowHide()
        {
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
            const auto ver = REL::Module::get().version();
            for (const auto& k : KNOWN_FLAT) {
                if (ver[0] == k.major && ver[1] == k.minor && ver[2] == k.patch) {
                    logger::info("SceneReadyHold: ShowHideLoadingMenu via known RVA {:x}", k.rva);
                    return base + k.rva;
                }
            }
            logger::warn("SceneReadyHold: no known ShowHideLoadingMenu RVA for runtime {}.{}.{} — feature off",
                ver[0], ver[1], ver[2]);
            return 0;
        }
    }

    bool SceneReadyHold::Install()
    {
        const auto& cfg = Config::Get();
        if (cfg.benchmarkMode == 0) {
            logger::info("SceneReadyHold: baseline mode — not installed");
            return false;
        }
        if (!cfg.sceneReadyHoldEnable) {
            logger::info("SceneReadyHold: disabled by config");
            return false;
        }

        const auto target = ResolveShowHide();
        if (!target) {
            logger::warn("SceneReadyHold: ShowHideLoadingMenu not resolved — feature off (loads close as before)");
            return false;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("SceneReadyHold: MinHook init failed ({})", static_cast<int>(initStatus));
            return false;
        }
        const MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(&HookedShowHide), reinterpret_cast<void**>(&s_origShowHide));
        if (create != MH_OK) {
            logger::error("SceneReadyHold: MH_CreateHook failed ({})", static_cast<int>(create));
            return false;
        }
        if (MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
            logger::error("SceneReadyHold: MH_EnableHook failed");
            MH_RemoveHook(reinterpret_cast<void*>(target));
            return false;
        }

        s_running.store(true);
        std::thread(PollerThread).detach();
        logger::info("SceneReadyHold: installed (maxHold={}ms tick={}ms streak={} waitActors={} "
                     "radius={:.0f} maxActors={} actorPct={} waitGrass={} grassBudget={}ms)",
            cfg.sceneReadyMaxHoldMs, cfg.sceneReadyTickMs, cfg.sceneReadyStreak,
            cfg.sceneReadyWaitActors, cfg.sceneReadyRadius, cfg.sceneReadyMaxActors,
            cfg.sceneReadyActorPct, cfg.sceneReadyWaitGrass, cfg.sceneReadyGrassBudgetMs);
        return true;
    }

    void SceneReadyHold::CancelPendingHold()
    {
        // Main thread (SKSE message handler). Run the deferred close so a hold
        // can't straddle into the next load.
        ReleaseHeldClose("load/new-game incoming", -1);
    }
}
