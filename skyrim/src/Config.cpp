#include "PCH.h"
#include "Config.h"

#include <Windows.h>

namespace FasterLoadscreens
{
    namespace
    {
        constexpr const char* INI_REL_PATH = "Data\\SKSE\\Plugins\\FasterLoadscreens.ini";
        // MCM Helper writes user changes here (ModSetting sources). Values in
        // this file OVERRIDE the base INI — without reading it, the MCM menu
        // silently did nothing in earlier builds.
        constexpr const char* MCM_REL_PATH = "Data\\MCM\\Settings\\FasterLoadscreens.ini";

        std::string AbsPath(const char* a_rel)
        {
            // GetPrivateProfile* resolves relative paths against the Windows
            // directory, not the CWD — build an absolute path from the CWD
            // (SKSE plugins start with CWD = game root).
            char cwd[MAX_PATH]{};
            ::GetCurrentDirectoryA(MAX_PATH, cwd);
            return std::string(cwd) + "\\" + a_rel;
        }

        // Two-layer read: MCM settings override the base INI; the base INI
        // overrides compiled defaults. "Key absent" falls through a layer.
        std::string s_basePath;
        std::string s_mcmPath;
        bool s_hasMcm = false;

        bool HasKey(const char* path, const char* section, const char* key)
        {
            char buf[8]{};
            ::GetPrivateProfileStringA(section, key, "\x01", buf, sizeof(buf), path);
            return buf[0] != '\x01';
        }

        int ReadInt(const char* section, const char* key, int def)
        {
            int v = static_cast<int>(::GetPrivateProfileIntA(section, key, def, s_basePath.c_str()));
            if (s_hasMcm && HasKey(s_mcmPath.c_str(), section, key)) {
                v = static_cast<int>(::GetPrivateProfileIntA(section, key, v, s_mcmPath.c_str()));
            }
            return v;
        }

        bool ReadBool(const char* section, const char* key, bool def)
        {
            return ReadInt(section, key, def ? 1 : 0) != 0;
        }

        float ReadFloatFrom(const char* path, const char* section, const char* key, float def)
        {
            char buf[64]{};
            ::GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
            if (!buf[0]) {
                return def;
            }
            char* end = nullptr;
            const float v = std::strtof(buf, &end);
            return end != buf ? v : def;
        }

        float ReadFloat(const char* section, const char* key, float def)
        {
            float v = ReadFloatFrom(s_basePath.c_str(), section, key, def);
            if (s_hasMcm) {
                v = ReadFloatFrom(s_mcmPath.c_str(), section, key, v);
            }
            return v;
        }
    }

    void Config::Load()
    {
        s_basePath = AbsPath(INI_REL_PATH);
        s_mcmPath  = AbsPath(MCM_REL_PATH);
        s_hasMcm   = ::GetFileAttributesA(s_mcmPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (s_hasMcm) {
            logger::info("Config: MCM settings overlay found at {}", s_mcmPath);
        }

        benchmarkMode = ReadInt("Benchmark", "iMode", benchmarkMode);
        if (benchmarkMode != 0) benchmarkMode = 1;

        loopMode = ReadInt("LoadingScreen", "iLoopMode", -1);  // -1 = auto (resolved below)
        throttleMs = ReadInt("LoadingScreen", "iThrottleMs", throttleMs);
        throttleMsVR = ReadInt("LoadingScreen", "iThrottleMsVR", throttleMsVR);
        freezeAfterFrames = ReadInt("LoadingScreen", "iFreezeAfterFrames", freezeAfterFrames);
        displayTweaksMode = ReadInt("LoadingScreen", "iDisplayTweaksMode", displayTweaksMode);

        boostPriority = ReadBool("Boost", "bBoostPriority", boostPriority);
        loadingQueuedPriorityBudgetMs = ReadInt("Boost", "iLoadingQueuedPriorityBudgetMs",
            loadingQueuedPriorityBudgetMs);
        backgroundBudgetMs = ReadInt("Boost", "iBackgroundBudgetMs", backgroundBudgetMs);
        postLoadFinalizeBudgetMs = ReadInt("Boost", "iPostLoadFinalizeBudgetMs", postLoadFinalizeBudgetMs);

        scriptSettleEnable = ReadBool("ScriptSettle", "bEnable", scriptSettleEnable);
        scriptBoostBudgetMS = ReadFloat("ScriptSettle", "fScriptBoostBudgetMS", scriptBoostBudgetMS);
        scriptBoostSecs = ReadInt("ScriptSettle", "iScriptBoostSecs", scriptBoostSecs);
        forceMcmRegistration = ReadBool("ScriptSettle", "bForceMcmRegistration", forceMcmRegistration);
        mcmNudgeDelaySecs = ReadInt("ScriptSettle", "iMcmNudgeDelaySecs", mcmNudgeDelaySecs);

        minSecondsForLoadFadeIn = ReadFloat("Fades", "fMinSecondsForLoadFadeIn", minSecondsForLoadFadeIn);
        loadGameFadeSecs = ReadFloat("Fades", "fLoadGameFadeSecs", loadGameFadeSecs);
        fadeToBlackFadeSeconds = ReadFloat("Fades", "fFadeToBlackFadeSeconds", fadeToBlackFadeSeconds);
        fastTravelFadeSecs = ReadFloat("Fades", "fFastTravelFadeSecs", fastTravelFadeSecs);
        autoDoorFadeSecs = ReadFloat("Fades", "fAutoDoorFadeSecs", autoDoorFadeSecs);
        normalDoorFadeSecs = ReadFloat("Fades", "fNormalDoorFadeSecs", normalDoorFadeSecs);
        normalDoorFadeWait = ReadFloat("Fades", "fNormalDoorFadeWait", normalDoorFadeWait);

        postLoadUpdateTimeMS = ReadFloat("Papyrus", "fPostLoadUpdateTimeMS", postLoadUpdateTimeMS);

        prefetchCellOnCrosshairDoor = ReadBool("Prefetch", "bPrefetchCellOnCrosshairDoor", prefetchCellOnCrosshairDoor);
        prefetchPollMs = ReadInt("Prefetch", "iPrefetchPollMs", prefetchPollMs);
        prefetchDoorCooldownMs = ReadInt("Prefetch", "iPrefetchDoorCooldownMs", prefetchDoorCooldownMs);
        prefetchExtendedRay = ReadBool("Prefetch", "bPrefetchExtendedRay", prefetchExtendedRay);
        prefetchRangeMult = ReadFloat("Prefetch", "fPrefetchRangeMult", prefetchRangeMult);
        prefetchExteriorCells = ReadBool("Prefetch", "bPrefetchExteriorCells", prefetchExteriorCells);
        prefetchGridRadius = ReadInt("Prefetch", "iPrefetchGridRadius", prefetchGridRadius);

        sceneReadyHoldEnable = ReadBool("SceneReadyHold", "bEnable", sceneReadyHoldEnable);
        sceneReadyMaxHoldMs = ReadInt("SceneReadyHold", "iMaxHoldMs", sceneReadyMaxHoldMs);
        sceneReadyTickMs = ReadInt("SceneReadyHold", "iTickMs", sceneReadyTickMs);
        sceneReadyStreak = ReadInt("SceneReadyHold", "iReadyStreak", sceneReadyStreak);
        sceneReadyWaitActors = ReadBool("SceneReadyHold", "bWaitActors", sceneReadyWaitActors);
        sceneReadyRadius = ReadFloat("SceneReadyHold", "fActorRadius", sceneReadyRadius);
        sceneReadyMaxActors = ReadInt("SceneReadyHold", "iMaxActors", sceneReadyMaxActors);
        sceneReadyActorPct = ReadInt("SceneReadyHold", "iActorPct", sceneReadyActorPct);
        sceneReadyWaitGrass = ReadBool("SceneReadyHold", "bWaitGrass", sceneReadyWaitGrass);
        sceneReadyGrassBudgetMs = ReadInt("SceneReadyHold", "iGrassBudgetMs", sceneReadyGrassBudgetMs);

        // Clamp to sane ranges
        // -1 (or absent) = auto: throttle on flat, OFF on VR — the VR loading loop
        // feeds the compositor every frame, so throttling it only judders the headset.
        if (loopMode < 0) loopMode = REL::Module::IsVR() ? 0 : 1;
        else if (loopMode > 2) loopMode = 1;
        if (throttleMs < 1) throttleMs = 1;
        if (throttleMs > 1000) throttleMs = 1000;
        if (throttleMsVR < 0) throttleMsVR = 0;
        if (throttleMsVR > 100) throttleMsVR = 100;
        if (freezeAfterFrames < 1) freezeAfterFrames = 1;
        if (displayTweaksMode < 0 || displayTweaksMode > 2) displayTweaksMode = 2;
        if (prefetchPollMs < 16) prefetchPollMs = 16;
        if (prefetchPollMs > 2000) prefetchPollMs = 2000;
        if (prefetchDoorCooldownMs < 0) prefetchDoorCooldownMs = 0;
        if (prefetchRangeMult <= 0.0f) {
            prefetchRangeMult = REL::Module::IsVR() ? 4.0f : 30.0f;  // 0 = auto: short on VR (re-stream visible in-headset), long on flat
        } else if (prefetchRangeMult < 1.0f) {
            prefetchRangeMult = 1.0f;                                // never shorter than vanilla reach
        } else if (prefetchRangeMult > 64.0f) {
            prefetchRangeMult = 64.0f;
        }
        if (prefetchGridRadius < 0) prefetchGridRadius = 0;
        if (prefetchGridRadius > 4) prefetchGridRadius = 4;       // 9x9 cap

        if (postLoadFinalizeBudgetMs > 1000) postLoadFinalizeBudgetMs = 1000;
        if (scriptBoostBudgetMS < 0.0f) scriptBoostBudgetMS = 0.0f;   // 0/neg = disabled boost
        if (scriptBoostBudgetMS > 16.0f) scriptBoostBudgetMS = 16.0f; // never eat a whole frame
        if (scriptBoostSecs < 0) scriptBoostSecs = 0;
        if (scriptBoostSecs > 120) scriptBoostSecs = 120;
        if (mcmNudgeDelaySecs < 5) mcmNudgeDelaySecs = 5;
        if (mcmNudgeDelaySecs > 120) mcmNudgeDelaySecs = 120;

        if (sceneReadyMaxHoldMs < 0) sceneReadyMaxHoldMs = 0;
        if (sceneReadyMaxHoldMs > 15000) sceneReadyMaxHoldMs = 15000;
        if (sceneReadyTickMs < 8) sceneReadyTickMs = 8;
        if (sceneReadyTickMs > 200) sceneReadyTickMs = 200;
        if (sceneReadyStreak < 1) sceneReadyStreak = 1;
        if (sceneReadyStreak > 10) sceneReadyStreak = 10;
        if (sceneReadyRadius < 0.0f) sceneReadyRadius = 0.0f;
        if (sceneReadyMaxActors < 0) sceneReadyMaxActors = 0;
        if (sceneReadyMaxActors > 4096) sceneReadyMaxActors = 4096;
        if (sceneReadyActorPct < 0) sceneReadyActorPct = 0;
        if (sceneReadyActorPct > 100) sceneReadyActorPct = 100;
        if (sceneReadyGrassBudgetMs < 0) sceneReadyGrassBudgetMs = 0;
        if (sceneReadyGrassBudgetMs > sceneReadyMaxHoldMs) sceneReadyGrassBudgetMs = sceneReadyMaxHoldMs;

        logger::info("Config: benchmarkMode={} ({})", benchmarkMode,
            benchmarkMode == 0 ? "BASELINE — no speedups" : "full");
        logger::info("Config: loopMode={} throttleMs={} throttleMsVR={} freezeAfter={} "
                     "dtMode={} boostPriority={} loadingBudget={} bgBudget={}",
            loopMode, throttleMs, throttleMsVR, freezeAfterFrames,
            displayTweaksMode, boostPriority, loadingQueuedPriorityBudgetMs, backgroundBudgetMs);
        logger::info("Config fades: minLoadFadeIn={:.4f} loadGame={:.2f} fadeToBlack={:.2f} "
                     "fastTravel={:.2f} autoDoor={:.2f} door={:.2f} doorWait={:.2f} postLoadMS={:.1f}",
            minSecondsForLoadFadeIn, loadGameFadeSecs, fadeToBlackFadeSeconds,
            fastTravelFadeSecs, autoDoorFadeSecs, normalDoorFadeSecs, normalDoorFadeWait,
            postLoadUpdateTimeMS);
        logger::info("Config prefetch: crosshairCell={} pollMs={} cooldownMs={} extendedRay={} rangeMult={:.2f}",
            prefetchCellOnCrosshairDoor, prefetchPollMs, prefetchDoorCooldownMs,
            prefetchExtendedRay, prefetchRangeMult);
        logger::info("Config prefetch: exteriorCells={} gridRadius={}", prefetchExteriorCells, prefetchGridRadius);
        logger::info("Config sceneReadyHold: enable={} maxHoldMs={} tickMs={} streak={} "
                     "waitActors={} radius={:.0f} maxActors={} actorPct={} waitGrass={} grassBudgetMs={}",
            sceneReadyHoldEnable, sceneReadyMaxHoldMs, sceneReadyTickMs, sceneReadyStreak,
            sceneReadyWaitActors, sceneReadyRadius, sceneReadyMaxActors, sceneReadyActorPct,
            sceneReadyWaitGrass, sceneReadyGrassBudgetMs);
    }
}
