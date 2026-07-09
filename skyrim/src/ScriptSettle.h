#pragma once

namespace FasterLoadscreens
{
    // Script initialization guard for fresh saves and save loads.
    //
    // Problem: on a new game (and to a lesser degree every save load) the
    // Papyrus VM receives a burst of OnInit / OnPlayerLoadGame work from every
    // installed mod. The VM's per-frame budget (fUpdateBudgetMS, vanilla
    // 1.2 ms) was tuned for the vanilla game; on a large modlist the backlog
    // takes minutes to drain and mods appear "not initialized" — quests not
    // started, MCM menus missing.
    //
    // Fix: temporarily raise the VM budgets right after the event so the
    // initialization burst completes in the first seconds, then restore the
    // user's baseline. Additionally (new game only, optional) force a SkyUI
    // MCM registration sweep — the standard remedy for MCMs that fail to
    // register on fresh saves.
    class ScriptSettle
    {
    public:
        // Call from kNewGame (a_newGame = true) / kPostLoadGame (false).
        static void OnGameLoaded(bool a_newGame);
    };
}
