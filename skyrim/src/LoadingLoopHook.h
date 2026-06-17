#pragma once

namespace FasterLoadscreens
{
    // Hooks the engine's loading-screen render loop.
    //
    // During every loading screen, JobListManager::ServingThread runs
    // DisplayLoadingScreen(): a dedicated-thread loop that re-renders the
    // loading screen as fast as vsync allows. Each iteration locks
    // BSGraphics::Renderer — the same lock the main thread needs to create
    // resources for the load — and burns CPU/GPU the loader could use.
    //
    // We hook two functions:
    //   1. DisplayLoadingScreen — marks the serving thread (TLS flag) and
    //      measures the loading-screen window.
    //   2. ServingThread state-check (the loop condition) — when called from
    //      inside DisplayLoadingScreen, throttles the loop (Sleep per
    //      iteration) or breaks out of it entirely after a few warm-up frames
    //      (freeze mode), freeing the renderer lock + CPU for the actual load.
    //
    // Both targets are resolved by byte signature against the running module
    // (verified unique on SE 1.5.97, AE 1.6.1170 and VR 1.4.15), with known-RVA
    // fast paths. No Address Library IDs are used for these sites, so unknown
    // runtime versions degrade to "feature off" instead of crashing.
    class LoadingLoopHook
    {
    public:
        // Call once at kDataLoaded. Returns true if both hooks installed.
        static bool Install();

        // True while the serving thread is inside DisplayLoadingScreen.
        static bool IsLoadingScreenActive();

        // Latch: set when a DisplayLoadingScreen runs, so the close hook
        // (SceneReadyHold) can tell a real load-ending close from an incidental
        // one. Consumed (cleared) by SceneReadyHold once it acts on a close.
        static bool WasLoadingScreenSeen();
        static void ClearLoadingScreenSeen();
    };
}
