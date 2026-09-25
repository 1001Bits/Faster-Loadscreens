#pragma once

namespace VRLoadingScreens
{
    // Keeps Fallout's native bPreloadLinkedAreas selection/cancellation loop but
    // optionally changes its exterior-destination TES::PreloadWorld request from
    // the full uGridsToLoad square to the engine's own single-arrival-cell mode.
    // In particular, this makes the wanted interior->exterior prediction cheap;
    // the same engine callsite can also be reached by an exterior-origin door.
    // Repeated identical single-cell requests from PlayerCharacter::Update are
    // rate-limited at the exact native caller; Fallout still owns the forwarded
    // request's resident/pending checks and all cell loading. Supported on OG
    // 1.10.163, VR 1.2.72, and AE/NG 1.11.221/1.11.240. OG/VR retain the exact
    // immediate toggle and install that caller's CALL detour once at startup.
    // AE/NG keeps its caller's register-store intact and filters a
    // TES::PreloadWorld hook by its exact verified return address, so unrelated
    // engine/plugin requests are unchanged.
    class NativePreloadPolicy
    {
    public:
        static bool Install() noexcept;
        static void SetExteriorArrivalCellOnly(bool a_enabled) noexcept;
        // Clear only the plugin-owned exact-destination throttle generation.
        // Fallout's resident/pending cells and loader work remain untouched.
        // DoorPrefetch calls this from its existing session/transition lifecycle
        // entry points so a new save cannot inherit an older world's cooldown.
        static void ResetForLoadBoundary() noexcept;
        static void Shutdown() noexcept;
    };
}
