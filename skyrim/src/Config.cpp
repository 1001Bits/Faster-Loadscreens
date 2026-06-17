#include "PCH.h"
#include "Config.h"

#include <Windows.h>

namespace FasterLoadscreens
{
    namespace
    {
        constexpr const char* INI_REL_PATH = "Data\\SKSE\\Plugins\\FasterLoadscreens.ini";

        std::string IniPath()
        {
            // GetPrivateProfile* resolves relative paths against the Windows
            // directory, not the CWD — build an absolute path from the CWD
            // (SKSE plugins start with CWD = game root).
            char cwd[MAX_PATH]{};
            ::GetCurrentDirectoryA(MAX_PATH, cwd);
            return std::string(cwd) + "\\" + INI_REL_PATH;
        }

        int ReadInt(const char* path, const char* section, const char* key, int def)
        {
            return static_cast<int>(::GetPrivateProfileIntA(section, key, def, path));
        }

        bool ReadBool(const char* path, const char* section, const char* key, bool def)
        {
            return ReadInt(path, section, key, def ? 1 : 0) != 0;
        }

        float ReadFloat(const char* path, const char* section, const char* key, float def)
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
    }

    void Config::Load()
    {
        const std::string pathStr = IniPath();
        const char* path = pathStr.c_str();

        benchmarkMode = ReadInt(path, "Benchmark", "iMode", benchmarkMode);
        if (benchmarkMode != 0) benchmarkMode = 1;

        loopMode = ReadInt(path, "LoadingScreen", "iLoopMode", -1);  // -1 = auto (resolved below)
        throttleMs = ReadInt(path, "LoadingScreen", "iThrottleMs", throttleMs);
        throttleMsVR = ReadInt(path, "LoadingScreen", "iThrottleMsVR", throttleMsVR);
        freezeAfterFrames = ReadInt(path, "LoadingScreen", "iFreezeAfterFrames", freezeAfterFrames);
        displayTweaksMode = ReadInt(path, "LoadingScreen", "iDisplayTweaksMode", displayTweaksMode);

        boostPriority = ReadBool(path, "Boost", "bBoostPriority", boostPriority);
        loadingQueuedPriorityBudgetMs = ReadInt(path, "Boost", "iLoadingQueuedPriorityBudgetMs",
            loadingQueuedPriorityBudgetMs);
        backgroundBudgetMs = ReadInt(path, "Boost", "iBackgroundBudgetMs", backgroundBudgetMs);

        minSecondsForLoadFadeIn = ReadFloat(path, "Fades", "fMinSecondsForLoadFadeIn", minSecondsForLoadFadeIn);
        loadGameFadeSecs = ReadFloat(path, "Fades", "fLoadGameFadeSecs", loadGameFadeSecs);
        fadeToBlackFadeSeconds = ReadFloat(path, "Fades", "fFadeToBlackFadeSeconds", fadeToBlackFadeSeconds);
        fastTravelFadeSecs = ReadFloat(path, "Fades", "fFastTravelFadeSecs", fastTravelFadeSecs);
        autoDoorFadeSecs = ReadFloat(path, "Fades", "fAutoDoorFadeSecs", autoDoorFadeSecs);
        normalDoorFadeSecs = ReadFloat(path, "Fades", "fNormalDoorFadeSecs", normalDoorFadeSecs);
        normalDoorFadeWait = ReadFloat(path, "Fades", "fNormalDoorFadeWait", normalDoorFadeWait);

        postLoadUpdateTimeMS = ReadFloat(path, "Papyrus", "fPostLoadUpdateTimeMS", postLoadUpdateTimeMS);

        prefetchCellOnCrosshairDoor = ReadBool(path, "Prefetch", "bPrefetchCellOnCrosshairDoor", prefetchCellOnCrosshairDoor);
        prefetchPollMs = ReadInt(path, "Prefetch", "iPrefetchPollMs", prefetchPollMs);
        prefetchDoorCooldownMs = ReadInt(path, "Prefetch", "iPrefetchDoorCooldownMs", prefetchDoorCooldownMs);
        prefetchExtendedRay = ReadBool(path, "Prefetch", "bPrefetchExtendedRay", prefetchExtendedRay);
        prefetchRangeMult = ReadFloat(path, "Prefetch", "fPrefetchRangeMult", prefetchRangeMult);
        prefetchExteriorCells = ReadBool(path, "Prefetch", "bPrefetchExteriorCells", prefetchExteriorCells);
        prefetchGridRadius = ReadInt(path, "Prefetch", "iPrefetchGridRadius", prefetchGridRadius);

        sceneReadyHoldEnable = ReadBool(path, "SceneReadyHold", "bEnable", sceneReadyHoldEnable);
        sceneReadyMaxHoldMs = ReadInt(path, "SceneReadyHold", "iMaxHoldMs", sceneReadyMaxHoldMs);
        sceneReadyTickMs = ReadInt(path, "SceneReadyHold", "iTickMs", sceneReadyTickMs);
        sceneReadyStreak = ReadInt(path, "SceneReadyHold", "iReadyStreak", sceneReadyStreak);
        sceneReadyWaitActors = ReadBool(path, "SceneReadyHold", "bWaitActors", sceneReadyWaitActors);
        sceneReadyRadius = ReadFloat(path, "SceneReadyHold", "fActorRadius", sceneReadyRadius);
        sceneReadyMaxActors = ReadInt(path, "SceneReadyHold", "iMaxActors", sceneReadyMaxActors);
        sceneReadyActorPct = ReadInt(path, "SceneReadyHold", "iActorPct", sceneReadyActorPct);
        sceneReadyWaitGrass = ReadBool(path, "SceneReadyHold", "bWaitGrass", sceneReadyWaitGrass);
        sceneReadyGrassBudgetMs = ReadInt(path, "SceneReadyHold", "iGrassBudgetMs", sceneReadyGrassBudgetMs);

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
