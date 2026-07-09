#pragma once

namespace FasterLoadscreens
{
    struct Config
    {
        // [Benchmark] 0 = baseline (measure load times, apply NO speedups),
        //             1 = full (all features on). Default 1.
        int benchmarkMode = 1;

        // [LoadingScreen]
        // 0 = off, 1 = throttle render loop, 2 = freeze render loop (flat only)
        int loopMode = 1;
        int throttleMs = 50;
        int throttleMsVR = 15;
        int freezeAfterFrames = 3;
        // SSE Display Tweaks coexistence mode (flat only). DT's loading-screen FPS
        // limiter busy-waits to hold its target FPS, re-occupying the CPU our
        // throttle-sleep frees for the loader.
        //   0 = off        run iLoopMode as-is (DT will fight the throttle)
        //   1 = freeze     when DT is present, promote throttle→freeze (loop exits,
        //                  DT's limiter has no frame to spin on; screen freezes)
        //   2 = neutralize keep the throttle (screen stays ANIMATED) but drop the
        //                  loading serving-thread to LOWEST priority, so DT's
        //                  busy-wait yields to the loader instead of re-stealing the
        //                  CPU. DT is never touched — its in-game uncapped-FPS
        //                  benefit is fully intact. (DEFAULT)
        int displayTweaksMode = 2;

        // [Boost]
        bool boostPriority = true;
        int loadingQueuedPriorityBudgetMs = 500;  // vanilla 20, -1 = leave; applied ONLY while the loading menu is open
        int backgroundBudgetMs = -1;              // vanilla 5, -1 = leave
        // Finalize budget after the loading menu CLOSES. The raised loading
        // budget must not persist into gameplay: the engine keeps draining
        // leftover queued refs on the main thread each frame, and at 500 ms
        // per pump that reads as "FPS tanks for ~10s after every load".
        // -1 = restore the vanilla value captured at startup (20).
        int postLoadFinalizeBudgetMs = -1;

        // [ScriptSettle] — give mod scripts room to initialize cleanly.
        // On new game / save load the Papyrus VM gets a mass of OnInit /
        // OnPlayerLoadGame work; at the vanilla 1.2 ms/frame budget a big
        // modlist takes minutes to settle and some mods appear "not
        // initialized". Temporarily raising the VM budgets right after the
        // event lets initialization finish during the first seconds.
        // Only the Papyrus VM budget boost runs by default — it writes two
        // existing game Setting floats on the game thread, nothing more, so it
        // cannot destabilize a load. This is what actually helps mods finish
        // OnInit / OnPlayerLoadGame quickly on a big list.
        bool  scriptSettleEnable = true;
        float scriptBoostBudgetMS = 4.0f;   // fUpdateBudgetMS + fExtraTaskletBudgetMS during settle (vanilla 1.2)
        int   scriptBoostSecs = 20;         // settle window length
        // New-game only: force a SkyUI MCM registration sweep shortly after
        // game start (setstage SKI_ConfigManagerInstance 1). EXPERIMENTAL and
        // OFF by default — it executes a console command from a background
        // timer, which is fragile on a still-initializing new game and was a
        // suspected CTD source in v1.7.0. The VM budget boost above already
        // gives SkyUI's own registration the cycles it needs; only enable this
        // if MCMs still fail to appear. Only fires if SkyUI is installed.
        bool  forceMcmRegistration = false;
        int   mcmNudgeDelaySecs = 15;

        // [Fades] — negative = leave vanilla
        float minSecondsForLoadFadeIn = 0.0001f;  // vanilla 1.5
        float loadGameFadeSecs = 0.1f;            // vanilla 1.0
        float fadeToBlackFadeSeconds = 0.1f;      // vanilla 1.0
        float fastTravelFadeSecs = 0.1f;          // vanilla 0.5
        float autoDoorFadeSecs = 0.1f;
        float normalDoorFadeSecs = 0.1f;
        float normalDoorFadeWait = 0.05f;

        // [Papyrus] — negative = leave vanilla
        float postLoadUpdateTimeMS = -1.0f;       // vanilla 500

        // Crosshair door -> destination-cell preload (drives the engine's own
        // interior-preload path on line-of-sight).
        bool prefetchCellOnCrosshairDoor = true;
        int prefetchPollMs = 100;          // crosshair poll cadence
        int prefetchDoorCooldownMs = 8000; // don't re-fire same cell within this

        // Extended door detection: an independent camera pick (preload-only) at a
        // multiple of the game's activation pick length, so doors get preloaded
        // before they're within reach. Does NOT change activation reach — the
        // player still can't open the door until the normal crosshair reaches it.
        bool prefetchExtendedRay = false;
        // Detection distance = fActivatePickLength * this. 0 = AUTO per runtime (30 on
        // flat for long lead, 4 on VR — firing far out makes the texture re-stream
        // visible in the headset). Set 1-64 to force a specific value.
        float prefetchRangeMult = 0.0f;

        // Preload EXTERIOR/worldspace destinations (city gates). Safe again via the
        // worldspace guard in DoorPrefetchHook: only cells in a DIFFERENT worldspace
        // than the player are preloaded (those can't be in the active grid, so the
        // engine background-loads them without the crash-prone grid insertion).
        bool prefetchExteriorCells = false;

        // Exterior arrival-grid radius. 0 = single destination cell; 2 = the engine's
        // 5x5 uGridsToLoad window. >0 enables the COLD-ONLY arrival-grid preload, which
        // delivers the big city-GATE wins (single-cell barely dents a multi-cell city
        // load). RUNTIME-AGNOSTIC: PreloadExteriorGrid drives the interior forwarder
        // s_preloadCell (resolved on SE/AE/VR), NOT the dormant AE-only tracked
        // ExteriorCellLoader — so it runs on OG/VR too. Safe via the cold-only skip
        // (never re-loads a resident cell) + the worldspace guard. VALIDATED on AE;
        // VR/SE still need in-game verification (VR is the most load-fragile) — drop
        // back to 0 there if unstable. Range 0-4.
        int prefetchGridRadius = 0;

        // [SceneReadyHold] — hold the loading screen open until the destination
        // scene is actually populated (nearby actors' 3D attached), instead of
        // letting it clear the instant the engine says "load done". Kills the
        // shopkeeper / object pop-in our fade trims expose. Adaptive: closes
        // instantly when the scene is already ready, waits only when it isn't,
        // and never past iMaxHoldMs. We suppress only the loading-menu kHide
        // message; all other end-of-load engine work runs normally.
        // EXPERIMENTAL, OFF by default. This defers the engine's loading-menu
        // close via a hardcoded-RVA hook and fires on EVERY load — a suspected
        // CTD source in v1.7.0 on heavy modlists, and the reason the original
        // author shipped it off. The load-scoped finalize budget + SceneReady
        // are NOT needed for the FPS-tank fix; that fix lives entirely in the
        // GameSettingTweaks open/close budget restore. Enable only to test.
        bool sceneReadyHoldEnable = false;
        int sceneReadyMaxHoldMs = 4000;     // hard cap on the deferral (safety)
        int sceneReadyTickMs = 33;          // readiness re-check cadence (~30 Hz)
        int sceneReadyStreak = 2;           // consecutive ready ticks before close
        bool sceneReadyWaitActors = true;   // wait for nearby actors' 3D
        float sceneReadyRadius = 4000.0f;   // actor scan radius (game units)
        int sceneReadyMaxActors = 64;       // hard cap on actors scanned / tick
        int sceneReadyActorPct = 90;        // % of eligible actors that must be 3D
        bool sceneReadyWaitGrass = false;   // soft-wait exterior grass (best-effort)
        int sceneReadyGrassBudgetMs = 750;  // extra grass budget (within iMaxHoldMs)

        void Load();

        static Config& Get()
        {
            static Config instance;
            return instance;
        }
    };
}
