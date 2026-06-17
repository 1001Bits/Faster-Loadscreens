#include "PCH.h"
#include "GameSettingTweaks.h"
#include "Config.h"

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

        void SetFloat(const char* a_name, float a_value)
        {
            if (a_value < 0.0f) {
                return;  // configured to leave vanilla
            }
            if (auto* setting = FindSetting(a_name)) {
                const float old = setting->data.f;
                setting->data.f = a_value;
                logger::info("  {} : {:.4f} -> {:.4f}", a_name, old, a_value);
            } else {
                logger::warn("  {} : setting not found", a_name);
            }
        }

        void SetInt(const char* a_name, int a_value)
        {
            if (a_value < 0) {
                return;
            }
            if (auto* setting = FindSetting(a_name)) {
                const auto old = setting->data.i;
                setting->data.i = a_value;
                logger::info("  {} : {} -> {}", a_name, old, a_value);
            } else {
                logger::warn("  {} : setting not found", a_name);
            }
        }
    }

    void GameSettingTweaks::Apply()
    {
        const auto& cfg = Config::Get();
        if (cfg.benchmarkMode == 0) {
            logger::info("Baseline mode: game setting tweaks NOT applied");
            return;
        }
        logger::info("Applying game setting tweaks:");

        SetFloat("fMinSecondsForLoadFadeIn:Interface", cfg.minSecondsForLoadFadeIn);
        SetFloat("fLoadGameFadeSecs:General", cfg.loadGameFadeSecs);
        SetFloat("fFadeToBlackFadeSeconds:Interface", cfg.fadeToBlackFadeSeconds);
        SetFloat("fFastTravelFadeSecs:General", cfg.fastTravelFadeSecs);
        SetFloat("fAutoDoorFadeSecs:General", cfg.autoDoorFadeSecs);
        SetFloat("fNormalDoorFadeSecs:General", cfg.normalDoorFadeSecs);
        SetFloat("fNormalDoorFadeWait:General", cfg.normalDoorFadeWait);

        SetInt("iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
            cfg.loadingQueuedPriorityBudgetMs);
        SetInt("iPostProcessMilliseconds:BackgroundLoad", cfg.backgroundBudgetMs);

        SetFloat("fPostLoadUpdateTimeMS:Papyrus", cfg.postLoadUpdateTimeMS);
    }
}
