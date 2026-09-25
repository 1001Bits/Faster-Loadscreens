#pragma once

namespace VRLoadingScreens
{
    // Exterior-only door destination preloader. Speculative interior cell work is
    // retired; an interior destination is observed and rejected without an engine
    // submission.
    //
    // Three independently configured detection sources feed the same
    // destination, suppression, and cooldown checks:
    //   1. Event-driven pick (cheap): a BSTEventSink on the engine's activate-
    //      pick-ref event — flat crosshair or either VR wand/center pick — and
    //      fires only when that target changes.
    //   2. Extended rays (lead time): optional camera-forward and horizontal
    //      movement Havok rays, polled at low rate with a 180-unit base reach
    //      multiplied by the configured 2x..10x value. They do not change the
    //      player's activation reach.
    //   3. Exterior-gate proximity: scans only already-attached exterior-cell
    //      door references near the player and requires no Havok ray.
    //
    // Guarded exterior-to-exterior destinations use TES::PreloadWorld only in its
    // single-arrival-cell queue mode. The unsafe live-grid/uGrids mode is retired. The
    // plugin never reads TESWorldSpace::cellMap and never calls ExteriorCellLoader
    // directly. Exterior requests are keyed by exact world/grid/mode, accept a live
    // linked destination cell as resident/pending evidence, and permit at most one
    // distance-qualified revalidation of a void-return PreloadWorld submission.
    // Exterior submissions stop across transition barriers. Destination graphs,
    // multibound maps and nodes are initialized before submission; extra graph
    // references last until native form teardown, not merely a plugin flush.
    class DoorPrefetch
    {
    public:
        using SessionLostCallback = void (*)() noexcept;

        // Resolve the engine preload function (call once at game-data-ready).
        static void Install();
        // Optional main-lifecycle bridge for AE, where CommonLib's menu event
        // singleton is unavailable. The engine-owned player/cell validity check
        // invokes this only outside transition suppression after it has already
        // parked the poller.
        static void SetSessionLostCallback(
            SessionLostCallback a_callback) noexcept;
        // Publish the real gameplay lifecycle to the shared poller. Install leaves
        // it parked so game-data-ready/main-menu time never queues periodic game-
        // thread work. Set true only after kPostLoadGame/kNewGame has produced a
        // live session; set false when MainMenu opens. Session changes clear only
        // plugin-owned destination/throttle generations, invalidate cached gate
        // references, and register the pick sink if configured. Fallout's resident
        // cells and loader work are never cancelled or discarded.
        static void SetGameSessionActive(bool a_active);
        // Queue crosshair-pick event-sink registration on the game thread.
        // Idempotent; call when the player should exist (e.g. each menu close).
        static void EnsureRegistered();
        // Enable only the event-driven crosshair / VR-wand pick source. Production
        // policy currently leaves it disabled; retained callers can discover only
        // exterior destinations because interior submission no longer exists.
        static void SetEnabled(bool a_enabled);
        // Retained internal hard-off control for the retired extended-ray path.
        static void SetExtendedRay(bool a_enabled, float a_rangeMult);
        // Authorize every plugin-issued TES::PreloadWorld request and configure the
        // cross-worldspace gate-proximity detection source. When disabled, exterior
        // candidates from picks and rays also fail closed. Proximity does not require
        // Havok. The legacy grid selector is accepted for ABI/config compatibility
        // but every value is clamped to the engine's single-arrival-cell mode.
        static void SetExteriorGateProximity(
            bool a_enabled, float a_distanceUnits, int a_gridSelector);
        // Publish whether this exact executable has a byte-validated
        // PositionPlayerJob -> ShowLoadingMenu correction contract. Only then
        // may successful PreloadWorld submissions arm presentation provenance.
        static void SetExteriorPresentationCorrectionReady(
            bool a_ready) noexcept;
        // One-shot evidence used immediately before PositionPlayerJob opens a
        // loading menu. True only when this plugin actually submitted the exact
        // exterior world/grid destination. Interior round trips preserve pending
        // proof; the exact exterior arrival consumes it and reconciles all other
        // records against the newly arrived source world. A shown-menu call may
        // update source context but cannot match/consume evidence.
        static bool ConsumeExteriorSubmissionEvidence(
            void* a_world,
            float a_x,
            float a_y,
            bool a_allowConsume) noexcept;
        // Explicitly discard presentation provenance for a lifecycle replacement
        // which does not pass through the normal session-deactivation edge.
        static void ClearExteriorPresentationEvidence(
            const char* a_reason) noexcept;
        // Enter the transition barrier and drain only a plugin call already inside
        // an engine preload entry. The engine loader is global, so this never
        // cancels, post-processes, or waits on it; engine-owned work may finish or
        // deduplicate normally. Save replacement may additionally clear all
        // presentation provenance inside the same barrier/drain transaction.
        // Main-thread only.
        static void FlushQueuedLoads(
            bool a_clearPresentationEvidence = false);
        // Leave the transition barrier after the load closes, enter a five-second
        // quiet period, then wake the poller and rebuild both gate caches on a later
        // game-thread poll rather than reusing pre-transition references. Entering
        // the boundary also starts a fresh plugin cooldown/throttle generation.
        static void EndTransition();
        // Stop and join the owned poller. Event callbacks become inert via their
        // source switches (F4SE plugins/event sinks otherwise live for the process).
        static void Shutdown();
    };
}
