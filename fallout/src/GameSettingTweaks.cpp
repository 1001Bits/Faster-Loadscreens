#include "PCH.h"
#include "GameSettingTweaks.h"

#include <mutex>

namespace VRLoadingScreens
{
    namespace
    {
        std::mutex s_settingsMutex;

        // Current fade configuration, pushed from settings.ini / MCM via SetFades
        // and re-applied on every load by Apply(). Defaults match FadeConfig.
        GameSettingTweaks::FadeConfig s_fades;

        // Capture the engine/user values before this DLL ever applies custom
        // fades. That makes "vanilla" restore the actual value for the running
        // OG, AE, or VR executable (including its INI), instead of writing one
        // runtime's assumed constants into all three.
        struct FadeDefaults
        {
            bool minSecondsValid = false;
            bool loadGameValid = false;
            bool fadeToBlackValid = false;
            bool autoDoorValid = false;
            bool normalDoorValid = false;
            bool normalDoorWaitValid = false;
            float minSeconds = 0.0f;
            float loadGame = 0.0f;
            float fadeToBlack = 0.0f;
            float autoDoor = 0.0f;
            float normalDoor = 0.0f;
            float normalDoorWait = 0.0f;
        } s_fadeDefaults;

        bool s_loadBudgetsActive = false;
        bool s_oldQueuedBudgetValid = false;
        bool s_oldGeneralBudgetValid = false;
        int  s_oldQueuedBudget = 0;
        int  s_oldGeneralBudget = 0;
        RE::Setting* s_queuedBudgetSetting = nullptr;
        RE::Setting* s_generalBudgetSetting = nullptr;

        // SEH-guarded setting access. RE::GetINISetting is documented to crash
        // on NG 1.11.x (internal -1 sentinel before the collection is built),
        // so every access is wrapped. Only POD locals live inside __try (no
        // C++ unwinding objects) so this compiles without C2712. Returns true
        // and fills outOld on success.
        bool SehSetFloat(const char* name, float val, float* outOld)
        {
            bool ok = false;
            __try {
                if (auto* s = RE::GetINISetting(name)) {
                    if (outOld) *outOld = s->GetFloat();
                    s->SetFloat(val);
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        bool SehGetFloat(const char* name, float* outValue)
        {
            bool ok = false;
            __try {
                if (auto* s = RE::GetINISetting(name)) {
                    if (outValue) *outValue = s->GetFloat();
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        bool SehSetInt(
            const char* name,
            int val,
            int* outOld,
            RE::Setting** outSetting = nullptr)
        {
            bool ok = false;
            if (outSetting) *outSetting = nullptr;
            __try {
                if (auto* s = RE::GetINISetting(name)) {
                    if (outOld) *outOld = s->GetInt();
                    if (outSetting) *outSetting = s;
                    s->SetInt(val);
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        bool SehSetInt(RE::Setting* setting, int val)
        {
            bool ok = false;
            __try {
                if (setting) {
                    setting->SetInt(val);
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        // Binary ('b' prefix) INI setting. TES::GridArrayLoad checks this exact
        // byte only to gate BGSCombinedCellGeometryDB::PreloadGrid; normal cell
        // loading is unaffected. SEH-guarded like the others.
        bool SehSetBinary(const char* name, bool val, bool* outOld)
        {
            bool ok = false;
            __try {
                if (auto* s = RE::GetINISetting(name)) {
                    if (outOld) *outOld = s->GetBinary();
                    s->SetBinary(val);
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        void TweakFloat(const char* name, float val)
        {
            float old = 0.0f;
            if (SehSetFloat(name, val, &old)) {
                logger::info("  {} : {:.4f} -> {:.4f}", name, old, val);
            } else {
                logger::warn("  {} : not found / unavailable", name);
            }
        }

        void RestoreCapturedFloat(
            const char* name, bool valid, float value)
        {
            if (valid) {
                TweakFloat(name, value);
            } else {
                logger::warn(
                    "  {} : startup default unavailable; left unchanged",
                    name);
            }
        }

        void ApplyCustomFloat(
            const char* name, bool defaultCaptured, float value)
        {
            if (defaultCaptured) {
                TweakFloat(name, value);
            } else {
                // Never make a value irreversible for this process. If the
                // startup snapshot was unavailable, leave the engine value
                // untouched rather than apply custom data we cannot restore.
                logger::warn(
                    "  {} : startup default unavailable; custom write skipped",
                    name);
            }
        }

        void CaptureFadeDefaults()
        {
            // Retry only fields that have never been captured. A missing field
            // is never custom-written, so a later successful read still observes
            // its pristine engine/INI value rather than one of our own values.
            if (!s_fadeDefaults.minSecondsValid) {
                s_fadeDefaults.minSecondsValid = SehGetFloat(
                    "fMinSecondsForLoadFadeIn:Interface",
                    &s_fadeDefaults.minSeconds);
            }
            if (!s_fadeDefaults.loadGameValid) {
                s_fadeDefaults.loadGameValid = SehGetFloat(
                    "fLoadGameFadeSecs:General",
                    &s_fadeDefaults.loadGame);
            }
            if (!s_fadeDefaults.fadeToBlackValid) {
                s_fadeDefaults.fadeToBlackValid = SehGetFloat(
                    "fFadeToBlackFadeSeconds:Interface",
                    &s_fadeDefaults.fadeToBlack);
            }
            if (!s_fadeDefaults.autoDoorValid) {
                s_fadeDefaults.autoDoorValid = SehGetFloat(
                    "fAutoDoorFadeSecs:General",
                    &s_fadeDefaults.autoDoor);
            }
            if (!s_fadeDefaults.normalDoorValid) {
                s_fadeDefaults.normalDoorValid = SehGetFloat(
                    "fNormalDoorFadeSecs:General",
                    &s_fadeDefaults.normalDoor);
            }
            if (!s_fadeDefaults.normalDoorWaitValid) {
                s_fadeDefaults.normalDoorWaitValid = SehGetFloat(
                    "fNormalDoorFadeWait:General",
                    &s_fadeDefaults.normalDoorWait);
            }
        }

        void ApplyPersistentSettings()
        {
            CaptureFadeDefaults();
            logger::info("GameSettingTweaks: applying {} tweaks",
                s_fades.vanillaFades
                    ? "captured engine-default fade + settle"
                    : "custom fade + settle");

            if (s_fades.vanillaFades) {
                RestoreCapturedFloat(
                    "fMinSecondsForLoadFadeIn:Interface",
                    s_fadeDefaults.minSecondsValid,
                    s_fadeDefaults.minSeconds);
                RestoreCapturedFloat(
                    "fLoadGameFadeSecs:General",
                    s_fadeDefaults.loadGameValid,
                    s_fadeDefaults.loadGame);
                RestoreCapturedFloat(
                    "fFadeToBlackFadeSeconds:Interface",
                    s_fadeDefaults.fadeToBlackValid,
                    s_fadeDefaults.fadeToBlack);
                RestoreCapturedFloat(
                    "fAutoDoorFadeSecs:General",
                    s_fadeDefaults.autoDoorValid,
                    s_fadeDefaults.autoDoor);
                RestoreCapturedFloat(
                    "fNormalDoorFadeSecs:General",
                    s_fadeDefaults.normalDoorValid,
                    s_fadeDefaults.normalDoor);
                RestoreCapturedFloat(
                    "fNormalDoorFadeWait:General",
                    s_fadeDefaults.normalDoorWaitValid,
                    s_fadeDefaults.normalDoorWait);
            } else {
                ApplyCustomFloat(
                    "fMinSecondsForLoadFadeIn:Interface",
                    s_fadeDefaults.minSecondsValid,
                    s_fades.minSecondsForLoadFadeIn);
                ApplyCustomFloat(
                    "fLoadGameFadeSecs:General",
                    s_fadeDefaults.loadGameValid,
                    s_fades.loadGameFadeSecs);
                ApplyCustomFloat(
                    "fFadeToBlackFadeSeconds:Interface",
                    s_fadeDefaults.fadeToBlackValid,
                    s_fades.fadeToBlackFadeSeconds);
                ApplyCustomFloat(
                    "fAutoDoorFadeSecs:General",
                    s_fadeDefaults.autoDoorValid,
                    s_fades.autoDoorFadeSecs);
                ApplyCustomFloat(
                    "fNormalDoorFadeSecs:General",
                    s_fadeDefaults.normalDoorValid,
                    s_fades.normalDoorFadeSecs);
                ApplyCustomFloat(
                    "fNormalDoorFadeWait:General",
                    s_fadeDefaults.normalDoorWaitValid,
                    s_fades.normalDoorFadeWait);
            }

            // Hard safety policy: Fallout's native linked-door system can queue
            // speculative interior cells. The same structural freeze reproduced
            // with Skyrim's native-only linked-interior preloader, so full mode
            // always disables this engine setting. Legacy MCM/user INI keys are
            // ignored, and neither the teleport radius nor interior buffer is
            // modified anymore.
            bool oldB = false;
            if (SehSetBinary("bPreloadLinkedAreas:General", false, &oldB)) {
                logger::info(
                    "  bPreloadLinkedAreas : {} -> false "
                    "(interior preloading retired)",
                    oldB);
            } else {
                logger::warn("  bPreloadLinkedAreas : not found / unavailable");
            }
        }
    }

    void GameSettingTweaks::SetFades(const FadeConfig& a_cfg)
    {
        std::lock_guard lock(s_settingsMutex);
        s_fades = a_cfg;
    }

    void GameSettingTweaks::Apply()
    {
        std::lock_guard lock(s_settingsMutex);
        ApplyPersistentSettings();
    }

    void GameSettingTweaks::BeginLoad()
    {
        std::lock_guard lock(s_settingsMutex);
        if (s_oldQueuedBudgetValid) {
            // A chained load may begin after a partial restore: preserve the
            // original snapshot and reassert only the active-load value.
            const bool reasserted =
                SehSetInt(s_queuedBudgetSetting, 500) ||
                SehSetInt(
                    "iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
                    500, nullptr, &s_queuedBudgetSetting);
            if (!reasserted) {
                logger::warn(
                    "GameSettingTweaks: chained queued budget reassert unavailable");
            }
        } else {
            s_oldQueuedBudgetValid = SehSetInt(
                "iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
                500, &s_oldQueuedBudget, &s_queuedBudgetSetting);
        }
        if (s_oldGeneralBudgetValid) {
            const bool reasserted =
                SehSetInt(s_generalBudgetSetting, 500) ||
                SehSetInt(
                    "iPostProcessMilliseconds:BackgroundLoad",
                    500, nullptr, &s_generalBudgetSetting);
            if (!reasserted) {
                logger::warn(
                    "GameSettingTweaks: chained general budget reassert unavailable");
            }
        } else {
            s_oldGeneralBudgetValid = SehSetInt(
                "iPostProcessMilliseconds:BackgroundLoad", 500,
                &s_oldGeneralBudget, &s_generalBudgetSetting);
        }
        s_loadBudgetsActive = s_oldQueuedBudgetValid || s_oldGeneralBudgetValid;
        // Report what 500 REPLACES, not just that it was written. This is the
        // only engine setting this plugin changes that is live inside the
        // measured load window, and its sign has never actually been checked:
        // a bigger per-frame drain budget means fewer pumps (faster), but it
        // also lets the main thread sit in one drain for up to 500 ms, which
        // delays the frame boundary where load-completion is next evaluated
        // (slower). Every other tweak here logs old -> new; this one did not,
        // so the comparison was impossible to make from a log.
        logger::info(
            "GameSettingTweaks: load budgets enabled "
            "(queued {} -> 500 [ok={}], general {} -> 500 [ok={}])",
            s_oldQueuedBudget, s_oldQueuedBudgetValid,
            s_oldGeneralBudget, s_oldGeneralBudgetValid);
    }

    bool GameSettingTweaks::EndLoad()
    {
        std::lock_guard lock(s_settingsMutex);
        if (!s_loadBudgetsActive) {
            return true;
        }
        if (s_oldQueuedBudgetValid) {
            if (SehSetInt(s_queuedBudgetSetting, s_oldQueuedBudget) ||
                SehSetInt(
                    "iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
                    s_oldQueuedBudget,
                    nullptr,
                    &s_queuedBudgetSetting)) {
                s_oldQueuedBudgetValid = false;
                s_queuedBudgetSetting = nullptr;
            } else {
                logger::warn(
                    "GameSettingTweaks: queued load-budget restore unavailable; "
                    "snapshot retained for retry");
            }
        }
        if (s_oldGeneralBudgetValid) {
            if (SehSetInt(s_generalBudgetSetting, s_oldGeneralBudget) ||
                SehSetInt(
                    "iPostProcessMilliseconds:BackgroundLoad",
                    s_oldGeneralBudget,
                    nullptr,
                    &s_generalBudgetSetting)) {
                s_oldGeneralBudgetValid = false;
                s_generalBudgetSetting = nullptr;
            } else {
                logger::warn(
                    "GameSettingTweaks: general load-budget restore unavailable; "
                    "snapshot retained for retry");
            }
        }
        s_loadBudgetsActive =
            s_oldQueuedBudgetValid || s_oldGeneralBudgetValid;
        if (s_loadBudgetsActive) {
            return false;
        }
        logger::info("GameSettingTweaks: load budgets restored");
        return true;
    }
}
