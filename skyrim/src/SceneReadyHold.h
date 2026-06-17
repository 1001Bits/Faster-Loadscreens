#pragma once

namespace FasterLoadscreens
{
    // Holds the loading screen open until the destination scene is actually
    // populated, instead of letting it clear the instant the engine flags the
    // load "done". Our fade trims (fMinSecondsForLoadFadeIn ~ 0) make that
    // instant clear expose pop-in: interior NPCs (the Warmaiden's shopkeeper)
    // and exterior grass finish their 3D a beat after the curtain drops.
    //
    // Mechanism: we hook the engine's show/hide-loading-menu orchestrator
    // (the function that posts the LoadingMenu kHide when a load completes).
    // On the CLOSE call, if the scene isn't ready yet, we DEFER the call
    // (return without invoking the original — the kHide is never posted, the
    // menu stays up) and let a main-thread tick re-check readiness. When the
    // scene is ready (nearby actors' 3D attached) or a hard timeout elapses,
    // the tick invokes the real close. The deferral is non-blocking: the engine
    // stays in its loading state and keeps promoting queued references every
    // frame, so the work we wait on actually finishes — no deadlock.
    //
    // Adaptive: when the scene is already ready at close time it closes
    // instantly (zero added time); it only waits when there'd otherwise be
    // pop-in, and never past iMaxHoldMs.
    class SceneReadyHold
    {
    public:
        // Call once at kDataLoaded (after the MenuWatcher is registered).
        static bool Install();

        // Release any in-flight hold immediately (run the deferred close).
        // Called on kPreLoadGame / kNewGame so a hold can't straddle loads.
        static void CancelPendingHold();
    };
}
