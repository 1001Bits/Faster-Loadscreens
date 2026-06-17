#pragma once

#include <atomic>
#include <thread>

namespace VRLoadingScreens
{
    class LoadingScreenManager
    {
    public:
        static LoadingScreenManager& GetSingleton()
        {
            static LoadingScreenManager instance;
            return instance;
        }

        // overlayMode: 0=HMD-relative, 1=World-locked, 2=Cinema, 3=Submit composite, 4=ClearRTV
        void Init(bool enableBackgrounds = true, float renderDelay = 0.5f, int overlayMode = 0,
                  float overlayAlpha = 0.5f);
        void OnLoadingMenuOpen();
        void OnLoadingMenuClose();
        void Update();

        void SetGameSessionLoaded()
        {
            m_gameSessionLoaded = true;
            m_needsDesyncFix = true;
        }
        bool IsInLoadingScreen() const { return m_inLoadingScreen.load(); }
        void SetBackgroundsEnabled(bool enabled) { m_backgroundsEnabled = enabled; }
        void SetRenderDelay(float seconds) { m_renderDelaySeconds = seconds; }
        void SetOverlayMode(int mode) { m_overlayMode = mode; }
        void SetOverlayAlpha(float alpha) { m_overlayAlpha = alpha; }
        void SetTimingOnly(bool timingOnly) { m_timingOnly = timingOnly; }
        bool IsEnabled() const { return m_enabled; }

        // Retry texture loading after device becomes available (NG deferred init)
        void RetryTextureLoad() { PrepareNextBackground(); }
        void* GetCurrentBgTexture() const { return m_currentBgTexture; }

    private:
        LoadingScreenManager() = default;

        void ScanForTextures();
        std::string PickRandomTexture();
        void PrepareNextBackground();
        void ShowOverlays();

        std::vector<std::string> m_texturePaths;
        std::string m_currentTexturePath;
        void* m_currentBgTexture = nullptr;  // ID3D11Texture2D* for skybox
        int m_lastIndex = -1;
        bool m_enabled = true;
        bool m_backgroundsEnabled = true;
        bool m_isVR = false;
        int m_overlayMode = 0; // 0=HMD, 1=World-locked, 2=Cinema, 3=Submit, 4=ClearRTV
        float m_overlayAlpha = 0.5f;
        bool m_timingOnly = false;
        bool m_gameSessionLoaded = false;
        float m_renderDelaySeconds = 0.5f;

        std::mt19937 m_rng{ std::random_device{}() };
        std::chrono::steady_clock::time_point m_loadStartTime;
        std::chrono::steady_clock::time_point m_lastCloseTime;
        bool m_pendingDesyncFix = false;
        bool m_needsDesyncFix = false;
        bool m_needsOverlayHide = false;
        std::chrono::steady_clock::time_point m_overlayHideTime;
        long long m_sinceLastClose = 0;

        // Animation loop NOP — applied from delay thread, restored on close
        std::uintptr_t m_loopAddress = 0;
        std::uint8_t m_originalLoopBytes[10] = {};
        bool m_originalBytesSaved = false;
        int m_loadCount = 0;
        std::atomic<bool> m_inLoadingScreen{ false };
        std::atomic<bool> m_loopNOPApplied{ false };
    };
}
