#pragma once

namespace FasterLoadscreens
{
    // Applies the INI-setting levers that shorten every load by a fixed
    // amount, independent of hardware:
    //
    //   fMinSecondsForLoadFadeIn:Interface  (vanilla 1.5s!) — forced minimum
    //       loading-screen display time
    //   fLoadGameFadeSecs / fFadeToBlackFadeSeconds / fFastTravelFadeSecs /
    //       door fades — fixed fade tax around every transition (~1-2s total)
    //   iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad
    //       (vanilla 20) — per-pump ms budget for finalizing queued loads
    //       while a loading screen is up; raising it lets each pump finish
    //       far more work
    //   fPostLoadUpdateTimeMS:Papyrus (vanilla 500) — post-load script
    //       catch-up time appended to the visible load (optional, off by
    //       default: script-heavy modlists want the catch-up)
    //
    // All values are read live by the engine from the Setting objects, so
    // writing them at runtime takes effect on the next load. Values < 0 in
    // the config leave the vanilla setting untouched.
    class GameSettingTweaks
    {
    public:
        // Call at kDataLoaded (collections exist) and again after loads to
        // re-assert against other mods resetting values.
        static void Apply();
    };
}
