#pragma once

namespace FasterLoadscreens
{
    // Speculative cell preloading: when a load door sits under the crosshair,
    // ask the engine to start background-loading the cell on the other side, so
    // that by the time the player activates the door much of the destination is
    // already in memory and the visible load is shorter.
    //
    // This drives the engine's OWN native interior-preload path — the same
    // function the engine calls for bPreloadLinkedInteriors — so it's a
    // supported operation, not a synthetic cell load. The difference is the
    // trigger: we fire it precisely on crosshair line-of-sight, not on the
    // global flag.
    //
    // Trigger: a ~10 Hz poll of RE::CrosshairPickData (the result the engine's
    // viewcaster writes each frame), dispatched onto the game thread via the
    // SKSE task interface. Polling the pick result rather than hooking the
    // picker means zero fragile per-version signatures for the trigger — only
    // the preload function itself is resolved from the binary, by signature.
    class DoorPrefetchHook
    {
    public:
        // Resolve the engine preload function and start the poller. Call once
        // after kDataLoaded. Returns true if the preload function resolved.
        static bool Install();

        // Drain any cells we queued via the engine's tracked ExteriorCellLoader
        // (QueueCellLoad) to completion. Called at the start of a load-screen
        // transition so our speculative grid finishes before the engine's own
        // transition grid-load runs (prevents an in-flight load conflict).
        // No-op when the tracked loader didn't resolve (SE/VR) or isn't yet live.
        static void FlushQueuedLoads();
    };
}
