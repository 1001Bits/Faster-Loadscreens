#include "PCH.h"
#include "LoadingScreenManager.h"
#include "VRCompositorHelper.h"
#include "D3D11Compositor.h"

namespace VRLoadingScreens
{
    // Loading loop exit: CMP [RSI+0x68],2 (4 bytes) + JZ back-to-top (6 bytes) = 10 bytes
    // Same RVA in VR and OG 1.10.163 (confirmed via Ghidra)
    static constexpr std::uintptr_t AnimationLoop_CMP_Offset = 0xd07573;

    static constexpr std::uint8_t NOP10[] = {
        0x0F, 0x1F, 0x44, 0x00, 0x00,
        0x0F, 0x1F, 0x44, 0x00, 0x00
    };

    void LoadingScreenManager::Init(bool enableBackgrounds, float renderDelay, int overlayMode,
                                     float overlayAlpha)
    {
        m_backgroundsEnabled = enableBackgrounds;
        m_renderDelaySeconds = renderDelay;
        m_overlayMode = overlayMode;
        m_overlayAlpha = overlayAlpha;
        m_isVR = REL::Module::IsVR();

        ScanForTextures();

        if (m_texturePaths.empty()) {
            logger::warn("No loading screen DDS textures found in Data/Textures/LoadingScreens/");
        } else {
            logger::info("Found {} loading screen textures", m_texturePaths.size());
        }

        // Animation loop NOP (breaks loading render loop for massive speedup)
        // Dynamically NOP the CMP+JE that controls the animation loop during loading
        if (m_isVR) {
            REL::Relocation<std::uintptr_t> animCmp{ REL::Offset(AnimationLoop_CMP_Offset) };
            m_loopAddress = animCmp.address();
        } else if (!REL::Module::IsNG()) {
            // OG flat: CMP [RDI+0x68],2; JE back (10 bytes) at RVA 0xCBFFCD
            // Found via pattern scan — only match for this loop condition in the binary
            m_loopAddress = REL::Module::get().base() + 0xCBFFCD;
        }

        if (m_loopAddress) {
            // Verify bytes before saving (CMP opcode = 0x83)
            auto* b = reinterpret_cast<const std::uint8_t*>(m_loopAddress);
            if (b[0] == 0x83 && b[2] == 0x68 && b[3] == 0x02) {
                std::memcpy(m_originalLoopBytes, reinterpret_cast<void*>(m_loopAddress), 10);
                m_originalBytesSaved = true;
                DWORD oldProtect;
                VirtualProtect(reinterpret_cast<void*>(m_loopAddress), 10, PAGE_EXECUTE_READWRITE, &oldProtect);
                logger::info("Animation loop saved at {:x}", m_loopAddress);
            } else if (m_isVR) {
                // VR offset is trusted, save without verification
                std::memcpy(m_originalLoopBytes, reinterpret_cast<void*>(m_loopAddress), 10);
                m_originalBytesSaved = true;
                DWORD oldProtect;
                VirtualProtect(reinterpret_cast<void*>(m_loopAddress), 10, PAGE_EXECUTE_READWRITE, &oldProtect);
                logger::info("VR: Animation loop saved at {:x}", m_loopAddress);
            } else {
                logger::warn("OG: Animation loop bytes mismatch at {:x} ({:02x})", m_loopAddress, b[0]);
                m_loopAddress = 0;
            }
        }

        // Initialize compositor (VR: OpenVR + overlays, flat: D3D device only)
        logger::info("Init: calling VRCompositorHelper::Initialize()...");
        VRCompositorHelper::Initialize();
        logger::info("Init: VRCompositorHelper initialized OK");
        if (m_isVR) {
            VRCompositorHelper::InitializeOverlay();
        }

        if (m_backgroundsEnabled) {
            logger::info("Init: calling PrepareNextBackground()...");
            PrepareNextBackground();
            logger::info("Init: PrepareNextBackground OK");
        }
    }

    void LoadingScreenManager::ScanForTextures()
    {
        m_texturePaths.clear();
        std::filesystem::path searchDir = "Data/Textures/LoadingScreens";
        if (!std::filesystem::exists(searchDir)) return;

        for (const auto& entry : std::filesystem::directory_iterator(searchDir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".dds") {
                m_texturePaths.push_back("LoadingScreens/" + entry.path().filename().string());
            }
        }
        std::sort(m_texturePaths.begin(), m_texturePaths.end());
    }

    std::string LoadingScreenManager::PickRandomTexture()
    {
        if (m_texturePaths.empty()) return "";
        if (m_texturePaths.size() == 1) return m_texturePaths[0];

        int idx;
        do {
            std::uniform_int_distribution<int> dist(0, static_cast<int>(m_texturePaths.size()) - 1);
            idx = dist(m_rng);
        } while (idx == m_lastIndex && m_texturePaths.size() > 1);

        m_lastIndex = idx;
        return m_texturePaths[idx];
    }

    void LoadingScreenManager::PrepareNextBackground()
    {
        if (m_texturePaths.empty()) return;

        // Release previous D3D11 texture
        if (m_currentBgTexture) {
            VRCompositorHelper::ReleaseTexture(m_currentBgTexture);
            m_currentBgTexture = nullptr;
        }

        m_currentTexturePath = PickRandomTexture();
        logger::info("Next background: {}", m_currentTexturePath);

        // Load DDS as D3D11 texture for skybox
        if (m_backgroundsEnabled) {
            std::string fullPath = "Data/Textures/" + m_currentTexturePath;
            m_currentBgTexture = VRCompositorHelper::LoadDDSTexture(fullPath);
            if (m_currentBgTexture) {
                logger::info("Loaded background texture: {}", fullPath);
            }
        }

    }

    void LoadingScreenManager::OnLoadingMenuOpen()
    {
        auto now = std::chrono::steady_clock::now();
        auto sinceLastClose = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastCloseTime).count();
        m_loadStartTime = now;
        m_inLoadingScreen = true;
        m_loopNOPApplied = false;
        m_loadCount++;

        // Cancel any pending overlay hide from a previous load close
        // (prevents mid-load overlay disappearance)
        m_needsOverlayHide = false;

        logger::info("LoadingMenu opened (load #{}, VR={}, sinceLastClose={}ms)",
            m_loadCount, m_isVR, sinceLastClose);

        if (m_timingOnly) return;

        m_sinceLastClose = sinceLastClose;

        if (m_isVR && m_originalBytesSaved) {
            // Immediately hide game's native loading screen with black blocker + fade
            VRCompositorHelper::FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false);
            VRCompositorHelper::ShowBlockerOverlay();

            // VR: delay, then deferred NOP + overlays
            static constexpr float kMainMenuFadeSeconds = 0.5f;
            float effectiveDelay = m_renderDelaySeconds;
            if (!m_gameSessionLoaded) {
                effectiveDelay = std::max(effectiveDelay, kMainMenuFadeSeconds);
            }
            int delayMs = static_cast<int>(effectiveDelay * 1000.0f);
            std::thread([this, delayMs]() {
                if (delayMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
                if (!m_inLoadingScreen.load()) return;
                // Show overlays FIRST so bg overlay is world-locked before deferred NOP fires.
                // RequestDeferredNOP sets the pending flag, and the next Submit for eye 1
                // will capture + show the tip overlay — s_bgOverlayActive must be true by then.
                ShowOverlays();
                D3D11Compositor::GetSingleton().RequestDeferredNOP(m_loopAddress, NOP10, 10);
                logger::info("VR: overlay + NOP after {}ms delay", delayMs);
            }).detach();
        } else if (!m_isVR && !REL::Module::IsNG() && m_originalBytesSaved) {
            // OG flat: apply NOP immediately (no VR compositor concerns)
            std::memcpy(reinterpret_cast<void*>(m_loopAddress), NOP10, 10);
            m_loopNOPApplied = true;
            logger::info("OG: Animation loop NOP applied at {:x}", m_loopAddress);
        }

        if (!m_isVR) {
            auto& comp = D3D11Compositor::GetSingleton();
            // Mode 0: kill AdvanceMovie early → black screen, no GPU rendering
            // Mode 1: native loading screen (no 3D model, tips still render)
            // Mode 2/3: our compositor handles rendering
            if (comp.GetFlatMode() == 0) {
                comp.KillAdvanceMovie();
            }
            if (m_backgroundsEnabled && m_currentBgTexture) {
                comp.SetBackgroundTexture(m_currentBgTexture);
            }
            comp.SetEnabled(true);
            logger::info("Flat: compositor enabled (flatMode={})", comp.GetFlatMode());
        }
    }

    void LoadingScreenManager::ShowOverlays()
    {
        bool isQuickReload = (m_sinceLastClose < 1000);

        if (VRCompositorHelper::IsOverlayInitialized()) {
            if (!isQuickReload && m_backgroundsEnabled && m_currentBgTexture) {
                VRCompositorHelper::ShowBackgroundOverlay(m_currentBgTexture, m_overlayMode, m_overlayAlpha);
            }
        }
    }

    void LoadingScreenManager::OnLoadingMenuClose()
    {
        m_lastCloseTime = std::chrono::steady_clock::now();
        auto elapsed = m_lastCloseTime - m_loadStartTime;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        m_inLoadingScreen = false;

        logger::info("Loading screen #{} closed — duration: {:.2f}s", m_loadCount, ms / 1000.0);

        if (m_timingOnly) return;

        auto& compositor = D3D11Compositor::GetSingleton();

        // Restore animation loop bytes (VR deferred NOP or OG flat direct NOP)
        // VR path: m_loopNOPApplied isn't set by the delay thread — the NOP is applied
        // inside the Submit hook via D3D11Compositor. Check IsDeferredNOPApplied() too.
        bool needRestore = m_loopNOPApplied ||
            (m_isVR && D3D11Compositor::GetSingleton().IsDeferredNOPApplied());
        if (m_originalBytesSaved && needRestore) {
            std::memcpy(reinterpret_cast<void*>(m_loopAddress), m_originalLoopBytes, 10);
            m_loopNOPApplied = false;
            logger::info("Animation loop restored at {:x}", m_loopAddress);
        }

        if (m_isVR) {
            // Match reference: restore NOP, reset compositor, check desync, hide overlays
            compositor.ResetDeferredState();

            bool desyncReloadPending = m_needsDesyncFix;
            if (m_needsDesyncFix) {
                m_needsDesyncFix = false;
                m_pendingDesyncFix = true;
            }

            if (!desyncReloadPending) {
                // Normal close: schedule delayed overlay hide (matches reference 200ms)
                m_needsOverlayHide = true;
                m_overlayHideTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                logger::info("VR: overlay hide scheduled in 200ms");
            } else {
                logger::info("VR: keeping overlays visible (desync fix pending)");
            }

            // Always prepare next background (matches reference)
            if (m_backgroundsEnabled) {
                PrepareNextBackground();
            }
        } else {
            // Flat: disable compositing
            compositor.SetEnabled(false);
            if (m_backgroundsEnabled) {
                PrepareNextBackground();
            }
        }
    }

    void LoadingScreenManager::Update()
    {
        if (m_timingOnly) return;

        if (m_isVR) {
            // VR: re-apply overlay transform every frame during loading
            if (m_inLoadingScreen.load()) {
                VRCompositorHelper::UpdateBackgroundOverlay();
                D3D11Compositor::GetSingleton().ProcessPendingCapture();
            }

            // Delayed overlay hide (matches reference — 200ms after close)
            if (m_needsOverlayHide && std::chrono::steady_clock::now() >= m_overlayHideTime) {
                m_needsOverlayHide = false;
                VRCompositorHelper::HideBackgroundOverlay();
                VRCompositorHelper::ClearSkybox();
                D3D11Compositor::GetSingleton().ReleaseCapturedTextures();
                logger::info("VR: overlays hidden (delayed)");
            }

            // Desync fix (matches reference — runs from Update after close)
            if (m_pendingDesyncFix && !m_inLoadingScreen.load()) {
                m_pendingDesyncFix = false;
                RE::Console::ExecuteCommand("player.moveto player");
                logger::info("VR: executed desync fix");
            }
        }
    }
}
