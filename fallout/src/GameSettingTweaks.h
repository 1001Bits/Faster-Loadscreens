#pragma once

namespace VRLoadingScreens
{
    // Engine game-setting tweaks, ported from the Skyrim "Faster Loadscreens"
    // mod. Skyrim measured an ~11% save-load improvement from its fade/budget
    // arm, but that is evidence for Skyrim only; the Fallout benchmark treats
    // these settings as part of the plugin core and does not assume the same
    // result. Two levers complement the custom-background compositor:
    //
    //   1. Optionally trim the fixed fade / minimum-display taxes the engine
    //      charges around a load. The shipped default restores the values
    //      captured from the running executable/INI before any custom write.
    //   2. Raise iPostProcessMillisecondsLoadingQueuedPriority (vanilla 20ms) so
    //      the engine drains queued background-loaded data in far fewer pumps.
    //      No gameplay to protect during a load, so the budget can be large.
    //
    // Every targeted setting name was Ghidra-verified to exist in Fallout 4
    // OG 1.10.163 / NG 1.11.221 and 1.11.240 / VR 1.2.72. Persistent values are re-applied
    // after native CLOSE, outside the measured/load-I/O interval.
    class GameSettingTweaks
    {
    public:
        // Apply persistent user-facing fade settings and the fixed linked-area
        // safety policy without raising background-load budgets outside a load.
        static void Apply();
        // Temporarily raise the two background-load budgets for an active load,
        // snapshotting their engine values on entry and restoring them on close.
        static void BeginLoad();
        // Returns true once both saved values have been restored. A failed
        // engine-setting lookup leaves the corresponding snapshot live so the
        // caller can retry on a later game-thread task instead of silently
        // stranding the temporary 500 ms value in gameplay.
        static bool EndLoad();

        // All user-tunable fade / settle durations, surfaced in the MCM and
        // settings.ini. Before any custom write, Apply() captures all six values
        // from the running engine; vanillaFades restores those runtime/INI-specific
        // values. The configurable values below are used only in custom mode.
        struct FadeConfig
        {
            bool  vanillaFades            = true;   // restore captured startup values
            float minSecondsForLoadFadeIn = 1.5f;   // custom-mode scene-settle buffer
            float loadGameFadeSecs        = 1.0f;   // custom-mode release default
            float fadeToBlackFadeSeconds  = 1.0f;
            float autoDoorFadeSecs        = 0.5f;
            float normalDoorFadeSecs      = 0.4f;
            float normalDoorFadeWait      = 0.01f;
        };
        // Push the current fade configuration (read from settings.ini / MCM). The
        // values are stored and re-applied on every load by Apply().
        static void SetFades(const FadeConfig& a_cfg);

        // Full mode always forces the engine's bPreloadLinkedAreas setting off.
        // That safety policy is intentionally not configurable: speculative
        // interior preloading is retired, and the plugin no longer changes its
        // teleport radius or uInterior Cell Buffer.
    };
}
