#pragma once

namespace RE { class TESWorldSpace; }

namespace VRLoadingScreens::WorldspacePreload
{
    // Exact-runtime contracts must install successfully before any speculative
    // exterior submission. Called once at game-data-ready.
    [[nodiscard]] bool Install();

    using PreloadWorldFn = void (*)(void*, RE::TESWorldSpace*, int, int, bool);

    // Under DoorPrefetch's final submission/lifecycle barrier. Preparation and
    // the queue-only call share the native teardown lock. The graph reference
    // survives the call and is retired only by native form teardown.
    [[nodiscard]] bool Submit(void* a_tes, RE::TESWorldSpace* a_world,
        int a_x, int a_y, PreloadWorldFn a_preload);
}
