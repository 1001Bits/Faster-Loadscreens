#include "PCH.h"
#include "DoorPrefetchHook.h"
#include "Config.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace FasterLoadscreens
{
    namespace
    {
        // The engine interior-preload forwarder:
        //   void preload(void* /*unused*/, TESObjectCELL* a_cell)
        //   -> CellLoaderManager::QueuePreload(g_manager, a_cell, 1)
        // A 21-byte tail-call forwarder; addresses hardcoded per verified runtime
        // (no scan — an unrecognized build leaves s_preloadCell null and prefetch
        // is simply off, fail-safe).
        struct KnownRVA
        {
            std::uint16_t major, minor, patch;
            std::uint32_t forwarder;
        };
        constexpr KnownRVA KNOWN[] = {
            { 1, 5, 97,   0x1584c0 },  // SE 1.5.97
            { 1, 6, 1170, 0x1a1fd0 },  // AE 1.6.1170 (Steam)
            { 1, 6, 1179, 0x1a1e00 },  // AE 1.6.1179 (GOG) — Ghidra-verified
            { 1, 4, 15,   0x168f70 },  // VR 1.4.15
        };

        using PreloadCellFn = void (*)(void*, RE::TESObjectCELL*);
        PreloadCellFn s_preloadCell = nullptr;

        // Skyrim's OWN tracked exterior-cell loader (ExteriorCellLoader). Unlike
        // the untracked interior CellLoaderTask, the engine accounts for cells
        // queued here and drains them safely at the transition. We mirror
        // WallSoGB Better Transitions: queue the arrival grid here, then drain
        // it (FinishLoadingAllQueuedCells) when the load screen opens.
        //
        //   QueueCellLoad(loader, worldspace, cellX, cellY, flag=false)
        //   FinishLoadingAllQueuedCells(loader)
        //   loader = *(void**)(base + 0x30fdb38)  — lazily inited, null-check it.
        //
        // Resolved AE-1.6.1170-only (signature-validated). On other runtimes
        // these stay null and the exterior path falls back to single-cell.
        using QueueCellLoadFn = void (*)(void*, RE::TESWorldSpace*, std::int32_t, std::int32_t, bool);
        using FinishQueuedFn = void (*)(void*);

        // WallSoGB-Better-Transitions exterior Finish() lifecycle, ported to
        // Skyrim AE's ExteriorCellLoader. All three are thin methods that take the
        // loader as `this`:
        //   PostProcessCompletedTasks(loader) — integrate completed loads (drains
        //     the loader's task table: tasks in state 2 -> 3, integrated via the
        //     task vtable +0x30). Matched to FO4 ExteriorCellLoader::
        //     PostProcessCompletedTasks (FO4 rva 0x392af0): LockForWrite, iterate
        //     the hash table, state 2->3 + vfunc(+0x30), no IOManager call.
        //   TryCancelTasks(loader) -> count — cancel in-flight/queued tasks
        //     (state 0 -> 6 via IOManager::TryCancelTask), returns a count of how
        //     many were cancellable. Matched to FO4 TryCancelTasks (0x3922a0):
        //     state 0->6, IOManager::TryCancelTask, returns an int counter.
        //   WaitForTasks(loader) — block until in-flight tasks reach a terminal
        //     state (per-task IOManager::WaitForTask spin), then integrate. Matched
        //     to FO4 WaitForTasks (0x392500): the distinctive per-task
        //     IOManager::WaitForTask call that spins on state 3/4.
        // Resolved AE-1.6.1170-only, prologue-validated; null on any other runtime.
        using PPFn = void (*)(void*);
        using TryCancelFn = std::uint32_t (*)(void*);
        using WaitFn = void (*)(void*);

        QueueCellLoadFn s_queueCellLoad = nullptr;
        FinishQueuedFn s_finishQueued = nullptr;
        PPFn s_postProcess = nullptr;
        TryCancelFn s_tryCancel = nullptr;
        WaitFn s_waitTasks = nullptr;
        void** s_extLoaderPtr = nullptr;

        bool s_isVR = false;

        std::atomic<bool> s_running{ false };
        // Set when a poll task is queued, cleared when it runs — so a stalled
        // game thread (e.g. mid-load) doesn't accumulate a backlog of polls.
        std::atomic<bool> s_pollQueued{ false };

        // Set when we queue exterior cells via the tracked loader; consumed by
        // FlushQueuedLoads so it only reconciles the shared loader on transitions
        // where WE actually queued (never touches the engine's own transition tasks).
        std::atomic<bool> s_queuedExterior{ false };

        // Recently-prefetched cells, by FormID -> last-fire time. A cell isn't
        // re-fired until the cooldown elapses (so staring at a door doesn't spam
        // the loader), but the cooldown also lets an evicted cell be re-warmed.
        std::mutex s_seenMutex;
        std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> s_seen;

        bool ResolveForwarder()
        {
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
            const auto ver = REL::Module::get().version();
            for (const auto& k : KNOWN) {
                if (ver[0] == k.major && ver[1] == k.minor && ver[2] == k.patch) {
                    s_preloadCell = reinterpret_cast<PreloadCellFn>(base + k.forwarder);
                    logger::info("DoorPrefetch: preload fn via known RVA {:x}", k.forwarder);
                    return true;
                }
            }
            logger::warn("DoorPrefetch: no known preload-fn RVA for runtime {}.{}.{} — prefetch disabled",
                ver[0], ver[1], ver[2]);
            return false;
        }

        // Resolve the engine's tracked ExteriorCellLoader for WallSoGB's Better-
        // Transitions pattern: QueueCellLoad queues an exterior cell into the engine's
        // OWN tracked loader (so the transition integrates it — no double-load), and
        // Finish() = PostProcessCompletedTasks + TryCancelTasks + WaitForTasks
        // reconciles it. All five RE-verified per runtime (GhidrAssist, cross-confirmed
        // in the shared cell-load orchestrator) on SE 1.5.97 / AE 1.6.1170 / VR 1.4.15.
        // QueueCellLoad's prologue is validated so a wrong/patched binary leaves the
        // pointers null and the exterior path is skipped (interior preload still works).
        // PostProcess and WaitForTasks are near-clones with identical prologues — the
        // RVA selects which.
        void ResolveExteriorLoader()
        {
            struct ExtLoader
            {
                std::uint16_t major, minor, patch;
                std::uint32_t queueCellLoad, tryCancel, waitTasks, postProcess, loaderGlobal;
            };
            static constexpr ExtLoader KNOWN[] = {
                { 1, 5, 97,   0x24ad30, 0x24afc0, 0x24b150, 0x24b2f0, 0x2ec5cf8 },   // SE 1.5.97
                { 1, 6, 1170, 0x29c530, 0x29c7b0, 0x29c990, 0x29cb80, 0x30fdb38 },   // AE 1.6.1170 (Steam)
                { 1, 4, 15,   0x25c420, 0x25c6b0, 0x25c840, 0x25c9e0, 0x2f8abc8 },   // VR 1.4.15
            };
            const auto ver = REL::Module::get().version();
            const ExtLoader* k = nullptr;
            for (const auto& e : KNOWN) {
                if (ver[0] == e.major && ver[1] == e.minor && ver[2] == e.patch) {
                    k = &e;
                    break;
                }
            }
            if (!k) {
                logger::info("DoorPrefetch: tracked ExteriorCellLoader unknown on this runtime — exterior preload disabled (interior preload still active)");
                return;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
            s_queueCellLoad = reinterpret_cast<QueueCellLoadFn>(base + k->queueCellLoad);
            s_tryCancel = reinterpret_cast<TryCancelFn>(base + k->tryCancel);
            s_waitTasks = reinterpret_cast<WaitFn>(base + k->waitTasks);
            s_postProcess = reinterpret_cast<PPFn>(base + k->postProcess);
            s_extLoaderPtr = reinterpret_cast<void**>(base + k->loaderGlobal);
            logger::info("DoorPrefetch: tracked ExteriorCellLoader resolved ({}.{}.{}): QueueCellLoad={:x} TryCancel={:x} WaitForTasks={:x} PostProcess={:x} loader*={:x}",
                k->major, k->minor, k->patch, k->queueCellLoad, k->tryCancel, k->waitTasks, k->postProcess, k->loaderGlobal);
        }

        // Resolve the destination cell of a load door, or null.
        RE::TESObjectCELL* GetDoorDestinationCell(RE::TESObjectREFR* a_door)
        {
            if (!a_door) {
                return nullptr;
            }
            const auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
            if (!teleport || !teleport->teleportData) {
                return nullptr;
            }
            const auto linkedDoor = teleport->teleportData->linkedDoor.get();
            if (!linkedDoor) {
                return nullptr;
            }
            return linkedDoor->GetParentCell();
        }

        // Fire the engine preload for one targeted refr, if it's a load door to
        // a not-yet-loaded cell that's off cooldown. Safe to call with any refr.
        // Fire the engine background-preload for one cell, cooldown-gated. Returns
        // true if it actually fired (off cooldown + not already loaded).
        bool PreloadCell(RE::TESObjectCELL* a_cell, const char* a_source)
        {
            // Skip an already-RESIDENT cell. IsAttached() alone is NOT enough: a cell can
            // be loaded (loadedData != null) without being attached, and re-issuing the
            // engine preload on an already-loaded cell CORRUPTS it — the engine then
            // fast-faults LATER while walking that cell (pure-engine/HDT-SMP stack, no mod
            // DLL). CONFIRMED on VR/Enderal: repeatable CTD that disappears with
            // bPrefetchCellOnCrosshairDoor=0. This is the same resident guard Wall's
            // Better-Transitions (TES::IsCellLoaded), our FO4 LoadingScreenPlugin
            // (cell->loadedData), and PreloadExteriorGrid all use — the single-cell path
            // was the one place missing it. The cooldown only tracks OUR fires, so a cell
            // the game loaded itself (bPreloadLinkedInteriors, a connected interior) would
            // otherwise slip straight through.
            if (!a_cell || a_cell->IsAttached() ||
                a_cell->GetRuntimeData().loadedData != nullptr) {
                return false;
            }
            const auto formID = a_cell->GetFormID();
            const auto now = std::chrono::steady_clock::now();
            const auto cooldown = std::chrono::milliseconds(Config::Get().prefetchDoorCooldownMs);
            {
                std::lock_guard lock(s_seenMutex);
                const auto it = s_seen.find(formID);
                if (it != s_seen.end() && (now - it->second) < cooldown) {
                    return false;
                }
                s_seen[formID] = now;
            }
            s_preloadCell(nullptr, a_cell);
            logger::info("DoorPrefetch: preloading cell {:08X} ({}) via {}", formID, a_cell->GetFormEditorID(), a_source);
            return true;
        }

        // Preload the NxN arrival grid (radius cells each way) around an exterior
        // destination cell — the engine's uGridsToLoad window. Warming only the one
        // door cell barely helps a dense city; the engine loads the whole grid at the
        // transition.
        //
        // WALL'S PATTERN: queue each cold cell through the engine's OWN tracked
        // ExteriorCellLoader via QueueCellLoad(loader, worldspace, x, y). The engine
        // ACCOUNTS for these — at the transition our FlushQueuedLoads() runs the same
        // Finish() the engine uses (PostProcess + cancel + wait), so the grid is
        // integrated/cancelled cleanly, no double-load. This replaces the old untracked
        // interior-forwarder (s_preloadCell) approach the engine didn't know about,
        // which crashed (resident re-load), then raced, then HUNG the cold worldspace
        // transition. Gated on the s_seen cooldown (CENTER cell) so we don't re-queue
        // the whole grid every ~10 Hz poll. Caller guarantees the tracked loader
        // resolved + destWS != playerWS (so these cells are never in the player's
        // active grid). radius 0 = just the destination cell, via the same tracked path.
        void PreloadExteriorGrid(RE::TESObjectCELL* a_center, RE::TESWorldSpace* a_ws, const char* a_source)
        {
            if (!s_queueCellLoad || !s_extLoaderPtr || !*s_extLoaderPtr || !a_ws) {
                return;  // tracked loader unavailable (caller already gated — belt-and-braces)
            }
            void* loader = *s_extLoaderPtr;
            const int radius = Config::Get().prefetchGridRadius;
            const auto* coords = a_center->GetCoordinates();
            if (!coords) {
                return;
            }

            // Re-scan gate: queue the grid at most once per cooldown for this center
            // (avoids the cellMap lookups + re-queue on every ~10 Hz poll).
            const auto centerID = a_center->GetFormID();
            const auto now = std::chrono::steady_clock::now();
            const auto cooldown = std::chrono::milliseconds(Config::Get().prefetchDoorCooldownMs);
            {
                std::lock_guard lock(s_seenMutex);
                const auto it = s_seen.find(centerID);
                if (it != s_seen.end() && (now - it->second) < cooldown) {
                    return;
                }
                s_seen[centerID] = now;
            }

            // COLD-ONLY: queue each cell NOT already resident. Skipping resident cells
            // (a revisit) avoids QueueCellLoad's resident early-out (which pokes the
            // live attach) and is pointless anyway. Cold first-visit cells are the slow
            // (~4.7s) loads this speeds up.
            int fired = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const std::int32_t cx = coords->cellX + dx;
                    const std::int32_t cy = coords->cellY + dy;
                    const RE::CellID id(static_cast<std::int16_t>(cy), static_cast<std::int16_t>(cx));
                    const auto it = a_ws->cellMap.find(id);
                    if (it == a_ws->cellMap.end() || !it->second) {
                        continue;  // no cell record at these coords
                    }
                    auto* c = it->second;
                    if (c->IsAttached() || c->GetRuntimeData().loadedData != nullptr) {
                        continue;  // already resident — skip
                    }
                    s_queueCellLoad(loader, a_ws, cx, cy, false);  // tracked queue (Wall)
                    ++fired;
                }
            }
            if (fired > 0) {
                s_queuedExterior.store(true, std::memory_order_relaxed);
                logger::info("DoorPrefetch: queued {} cold cells (grid r={}) via tracked ExteriorCellLoader around ({},{}) via {}",
                    fired, radius, coords->cellX, coords->cellY, a_source);
            }
        }

        // Fire the engine preload for one targeted refr, if it's a load door to a
        // not-yet-loaded cell that's off cooldown. Safe to call with any refr.
        void TryPreloadFromRef(RE::TESObjectREFR* a_refr, const char* a_source)
        {
            auto* cell = GetDoorDestinationCell(a_refr);
            if (!cell || cell->IsAttached() || cell->GetRuntimeData().loadedData != nullptr) {
                return;  // not a load door, or already RESIDENT (attached OR loadedData!=null).
                         // Re-preloading a resident cell re-runs QueuePreload's grid-shove/attach
                         // on a live cell -> vtable corruption -> CTD (the FO4 satellite-cell crash).
                         // IsAttached alone misses loaded-but-not-attached cells, so check both.
            }
            // Single-cell engine preload (QueuePreload) of the destination cell. This is
            // the door-prefetch that produced the documented -55% door speedup — fast and
            // self-contained. Interiors always; exterior destinations only when enabled AND
            // the destination is a DIFFERENT worldspace than the player. Rationale: a
            // foreign-worldspace cell can never be in the player's active grid, so
            // QueuePreload takes its safe background-load branch; an in-grid / same-WS cell
            // would be SHOVED into the live grid (TES+0xd0, FUN_1401a61d0) before its
            // loadedData is ready -> other engine code derefs it -> CTD, so we skip those.
            // The IsAttached check above already skips anything resident (no re-shove).
            // (The tracked ExteriorCellLoader grid was tried and reverted: slower per
            //  transition and hung on cold caches; this single-cell path is the fast one.)
            if (cell->IsExteriorCell()) {
                if (!Config::Get().prefetchExteriorCells) {
                    return;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* playerWS = player ? player->GetWorldspace() : nullptr;
                auto* destWS = cell->GetRuntimeData().worldSpace;
                if (!destWS || destWS == playerWS) {
                    return;  // same/unknown worldspace -> would risk the live-grid shove
                }
                // Exterior: route through the engine's OWN tracked ExteriorCellLoader
                // (QueueCellLoad) and reconcile it at the transition via FlushQueuedLoads,
                // instead of the untracked single-cell QueuePreload that left an orphaned
                // task for the engine's transition to double-integrate (vanishing refs /
                // falling havok). radius from iPrefetchGridRadius (0 = just the dest cell).
                PreloadExteriorGrid(cell, destWS, a_source);
                return;
            }
            PreloadCell(cell, a_source);  // interior single-cell (safe, self-contained)
        }

        // The game's activation pick ray length (Skyrim.ini [Interface]
        // fActivatePickLength). Cached; vanilla default 180.0 if unreadable.
        float ActivatePickLength()
        {
            static float cached = 0.0f;
            if (cached > 0.0f) {
                return cached;
            }
            if (auto* col = RE::INISettingCollection::GetSingleton()) {
                if (auto* s = col->GetSetting("fActivatePickLength:Interface")) {
                    cached = s->GetFloat();
                }
            }
            if (cached <= 0.0f) {
                cached = 180.0f;
            }
            return cached;
        }

        // Extended door DETECTION (preload-only). An independent camera pick at
        // fActivatePickLength * mult, using the engine's own crosshair-forward
        // convention (camera node world-rotation Y column). It never writes
        // CrosshairPickData and never changes fActivatePickLength, so the player's
        // activation reach is unchanged — we just preload the destination cell
        // earlier. Runs on the game thread (caller is an SKSE task).
        void TryExtendedDoorPick()
        {
            if (!Config::Get().prefetchExtendedRay) {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            auto* cell = player->GetParentCell();
            if (!cell) {
                return;
            }
            auto* world = cell->GetbhkWorld();
            if (!world) {
                return;
            }
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->cameraRoot) {
                return;
            }
            const auto& xf = cam->cameraRoot->world;
            const RE::NiPoint3 origin = xf.translate;
            // Engine derives the crosshair forward from the camera node's world
            // rotation Y column (verified in PickCrosshairReference).
            RE::NiPoint3 fwd{ xf.rotate.entry[0][1], xf.rotate.entry[1][1], xf.rotate.entry[2][1] };
            const float mag = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
            if (mag < 1.0e-4f) {
                return;
            }
            fwd *= (1.0f / mag);

            const float dist = ActivatePickLength() * Config::Get().prefetchRangeMult;
            const float scale = RE::bhkWorld::GetWorldScale();
            const RE::NiPoint3 end = origin + fwd * dist;

            RE::bhkPickData pick;
            pick.rayInput.from = RE::hkVector4(origin.x * scale, origin.y * scale, origin.z * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(end.x * scale, end.y * scale, end.z * scale, 0.0f);
            player->GetCollisionFilterInfo(pick.rayInput.filterInfo);  // ignore the player's own body

            world->PickObject(pick);
            if (!pick.rayOutput.HasHit() || !pick.rayOutput.rootCollidable) {
                return;
            }
            if (auto* refr = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable)) {
                TryPreloadFromRef(refr, "extray");  // reuses the load-door / exterior-gate / cooldown gating
            }
        }

        // Runs on the game thread (SKSE task). Checks every pick handle the
        // crosshair data carries, so whichever caster is aimed at a load door
        // triggers the preload.
        //
        // Flat Skyrim's CrosshairPickData has 3 RefHandles (target / targetActor
        // / secondary) at +0x04/+0x08/+0x0c, then a NiPoint3 at +0x10. VR's
        // struct is larger: its constructor inits NINE RefHandle slots from
        // +0x04 to +0x24 — a separate pick target per source (main crosshair,
        // primary wand, secondary wand, ...). CommonLibSSE-NG only types the
        // first three, so on VR we read the extra slots by raw offset.
        void PollOnGameThread()
        {
            s_pollQueued.store(false, std::memory_order_relaxed);
            if (!s_preloadCell) {
                return;
            }
            auto* pick = RE::CrosshairPickData::GetSingleton();
            if (!pick) {
                return;
            }
            // Typed handles present on every runtime.
            const RE::ObjectRefHandle base[] = { pick->target, pick->targetActor, pick->unk0C };
            for (const auto& h : base) {
                if (auto refr = h.get()) {
                    TryPreloadFromRef(refr.get(), "crosshair");
                }
            }
            // VR-only extra pick sources (primary + secondary wand + gaze) live
            // at +0x10..+0x24 as raw RefHandles. On flat, +0x10 is float data —
            // never read there.
            if (s_isVR) {
                const auto pickBase = reinterpret_cast<std::uintptr_t>(pick);
                for (const std::uintptr_t off : { 0x10u, 0x14u, 0x18u, 0x1cu, 0x20u, 0x24u }) {
                    const auto handle = *reinterpret_cast<RE::ObjectRefHandle*>(pickBase + off);
                    if (auto refr = handle.get()) {
                        TryPreloadFromRef(refr.get(), "crosshair");
                    }
                }
            }

            // Extended-range detection beyond the crosshair pick (preload-only).
            TryExtendedDoorPick();
        }

        // ~10 Hz cadence thread. It only ever calls SKSE's AddTask (thread-safe);
        // all engine access happens inside the task on the game thread.
        void PollerThread()
        {
            while (s_running.load(std::memory_order_relaxed)) {
                const int interval = Config::Get().prefetchPollMs;
                std::this_thread::sleep_for(std::chrono::milliseconds(interval > 0 ? interval : 100));
                if (!Config::Get().prefetchCellOnCrosshairDoor) {
                    continue;
                }
                // Skip if a poll is still queued (game thread stalled) — avoids
                // a backlog draining all at once when it resumes.
                if (s_pollQueued.exchange(true, std::memory_order_relaxed)) {
                    continue;
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { PollOnGameThread(); });
                } else {
                    s_pollQueued.store(false, std::memory_order_relaxed);
                }
            }
        }
    }

    bool DoorPrefetchHook::Install()
    {
        if (Config::Get().benchmarkMode == 0) {
            logger::info("DoorPrefetch: baseline mode — not installed");
            return false;
        }
        if (!Config::Get().prefetchCellOnCrosshairDoor) {
            logger::info("DoorPrefetch: disabled by config");
            return false;
        }
        if (!ResolveForwarder()) {
            return false;
        }
        ResolveExteriorLoader();  // AE-1.6.1170-only tracked loader; null elsewhere
        s_isVR = REL::Module::IsVR();
        s_running.store(true);
        // Stop the detached poller at CRT teardown (crash-on-exit guard).
        std::atexit([]() { s_running.store(false, std::memory_order_relaxed); });
        std::thread(PollerThread).detach();
        logger::info("DoorPrefetch: installed (crosshair cell preload active)");
        return true;
    }

    // was crashing like a bitch before, checked Walls code, that fixed it.
    //
    // Reconcile our tracked exterior-cell preloads to a clean terminal state at the
    // start of a load-screen transition — WallSoGB Better-Transitions' Finish(),
    // RE-verified 1:1 against the engine's own cell-load orchestrator on all runtimes:
    //   PostProcessCompletedTasks(loader);                  // integrate done loads (2->3)
    //   if (TryCancelTasks(loader)) WaitForTasks(loader);   // cancel queued, wait in-flight
    // CANCELLING the not-yet-started backlog and waiting only for the few already-
    // in-flight is exactly why this does NOT hang on a cold cache — unlike the
    // WaitForQueue force-drain (0x29ce80), which spun on the whole cold backlog and
    // hung the worldspace transition. No-op when the tracked loader didn't resolve
    // (unknown runtime) or isn't live yet. Runs at the DisplayLoadingScreen hook.
    void DoorPrefetchHook::FlushQueuedLoads()
    {
        // Only reconcile when WE queued exterior cells this transition — otherwise we'd
        // run the shared cell-loader's Finish on the engine's OWN transition tasks.
        if (!s_queuedExterior.exchange(false)) {
            return;
        }
        if (!s_extLoaderPtr || !*s_extLoaderPtr || !s_postProcess || !s_tryCancel) {
            return;
        }
        void* loader = *s_extLoaderPtr;
        s_postProcess(loader);  // integrate already-completed loads (state 2->3, non-blocking)
        s_tryCancel(loader);    // cancel queued-but-not-started (non-blocking; count ignored)
        // WaitForTasks REMOVED. RE (FUN_140dfdd00) shows it is an UNBOUNDED spin-wait:
        //   while (task.state in {3,4}) { pump_io(); Sleep(0/1); }   // no timeout, no exit
        // If an in-flight task can't complete (starved IO worker — which Display Tweaks
        // aggravates but does NOT cause), it spins forever -> infinite loadscreen. Dropping
        // it makes this deadlock-proof by construction: integrate what finished, cancel the
        // rest, never block. The transition loads anything we cancelled, normally.
        logger::info("DoorPrefetch: FlushQueuedLoads — postProcess + tryCancel (no wait)");
    }
}
