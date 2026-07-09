#include "PCH.h"
#include "ScriptSettle.h"
#include "Config.h"

#include <atomic>
#include <thread>

namespace FasterLoadscreens
{
    namespace
    {
        RE::Setting* FindSetting(const char* a_name)
        {
            if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                if (auto* setting = ini->GetSetting(a_name)) {
                    return setting;
                }
            }
            if (auto* pref = RE::INIPrefSettingCollection::GetSingleton()) {
                if (auto* setting = pref->GetSetting(a_name)) {
                    return setting;
                }
            }
            return nullptr;
        }

        // Boost generation: a restore only fires if no newer boost superseded
        // it (e.g. save load during an active settle window re-arms the timer).
        std::atomic<int>  s_boostGen{ 0 };
        std::atomic<bool> s_shutdown{ false };
        bool  s_captured = false;
        float s_savedUpdateBudget  = 1.2f;
        float s_savedTaskletBudget = 1.2f;

        void BoostOnGameThread()
        {
            auto* upd = FindSetting("fUpdateBudgetMS:Papyrus");
            auto* tsk = FindSetting("fExtraTaskletBudgetMS:Papyrus");
            if (!upd || !tsk) {
                logger::warn("ScriptSettle: Papyrus budget settings not found — boost skipped");
                return;
            }
            // Capture the user's baseline ONCE (first boost) so overlapping
            // events never capture our own boosted value as "the baseline".
            if (!s_captured) {
                s_savedUpdateBudget  = upd->data.f;
                s_savedTaskletBudget = tsk->data.f;
                s_captured = true;
            }
            const float v = Config::Get().scriptBoostBudgetMS;
            upd->data.f = v;
            tsk->data.f = v;
            logger::info("ScriptSettle: Papyrus budgets boosted {:.1f}/{:.1f} -> {:.1f} ms for {}s "
                         "(mod init scripts settle now instead of trickling)",
                s_savedUpdateBudget, s_savedTaskletBudget, v, Config::Get().scriptBoostSecs);
        }

        void RestoreOnGameThread()
        {
            if (!s_captured) {
                return;
            }
            auto* upd = FindSetting("fUpdateBudgetMS:Papyrus");
            auto* tsk = FindSetting("fExtraTaskletBudgetMS:Papyrus");
            if (upd) upd->data.f = s_savedUpdateBudget;
            if (tsk) tsk->data.f = s_savedTaskletBudget;
            logger::info("ScriptSettle: Papyrus budgets restored to {:.1f}/{:.1f} ms",
                s_savedUpdateBudget, s_savedTaskletBudget);
        }

        void NudgeMcmOnGameThread()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh || !dh->LookupModByName("SkyUI_SE.esp")) {
                logger::info("ScriptSettle: SkyUI_SE.esp not loaded — MCM registration nudge skipped");
                return;
            }
            const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
            auto* script = factory ? factory->Create() : nullptr;
            if (!script) {
                logger::warn("ScriptSettle: could not create console script for MCM nudge");
                return;
            }
            script->SetCommand("setstage SKI_ConfigManagerInstance 1");
            script->CompileAndRun(nullptr);
            delete script;
            logger::info("ScriptSettle: forced SkyUI MCM registration sweep "
                         "(setstage SKI_ConfigManagerInstance 1)");
        }
    }

    void ScriptSettle::OnGameLoaded(bool a_newGame)
    {
        const auto& cfg = Config::Get();
        if (cfg.benchmarkMode == 0 || !cfg.scriptSettleEnable) {
            return;
        }

        static bool s_atexitRegistered = []() {
            std::atexit([]() { s_shutdown.store(true, std::memory_order_relaxed); });
            return true;
        }();
        (void)s_atexitRegistered;

        if (cfg.scriptBoostBudgetMS > 0.0f && cfg.scriptBoostSecs > 0) {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() { BoostOnGameThread(); });
            }
            const int gen  = s_boostGen.fetch_add(1, std::memory_order_relaxed) + 1;
            const int secs = cfg.scriptBoostSecs;
            std::thread([gen, secs]() {
                for (int i = 0; i < secs; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (s_shutdown.load(std::memory_order_relaxed)) return;
                }
                if (s_boostGen.load(std::memory_order_relaxed) != gen) {
                    return;  // superseded by a newer boost window
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { RestoreOnGameThread(); });
                }
            }).detach();
        }

        if (a_newGame && cfg.forceMcmRegistration) {
            const int delay = cfg.mcmNudgeDelaySecs;
            std::thread([delay]() {
                for (int i = 0; i < delay; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (s_shutdown.load(std::memory_order_relaxed)) return;
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { NudgeMcmOnGameThread(); });
                }
            }).detach();
        }
    }
}
