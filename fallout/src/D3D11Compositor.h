#pragma once

#include "RuntimePolicy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace VRLoadingScreens
{
    enum class CompositeMode
    {
        LuminanceKey = 0,  // Hook Submit, composite bg behind game content
        ClearIntercept = 1 // Hook ClearRTV, draw bg before game renders
    };

    class D3D11Compositor
    {
    public:
        static D3D11Compositor& GetSingleton()
        {
            static D3D11Compositor instance;
            return instance;
        }

        // Initialize with IVRCompositor and D3D11 device (VR mode)
        bool Initialize(void* vrCompositor, void* d3dDevice);

        // Initialize for flat mode (hooks Present for background compositing)
        bool InitializeFlat();

        // Set the background texture (ID3D11Texture2D*)
        void SetBackgroundTexture(void* d3dTexture);

        // Enable/disable compositing (call on loading menu open/close)
        void SetEnabled(bool enabled);
        bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }
        bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }
        bool IsRenderReady() const { return m_renderReady.load(std::memory_order_acquire); }
        void SetMenuEventsActive(bool active) { m_menuEventsActive.store(active, std::memory_order_release); }

        // Preserve the caller's Present interval by default. Flat loading may
        // opt into interval 0 explicitly when the matching config is enabled.
        void SetDisableVSyncWhileLoading(bool disable) {
            m_disableVSyncWhileLoading.store(disable, std::memory_order_release);
        }

        // Compositing mode
        void SetMode(CompositeMode mode) { m_mode.store(mode, std::memory_order_release); }
        CompositeMode GetMode() const { return m_mode.load(std::memory_order_acquire); }

        // Flat loading screen mode: 0=blank, 1=native, 2=background, 3=background+tips.
        void SetFlatMode(int mode) { m_flatMode.store(mode, std::memory_order_release); }
        int GetFlatMode() const { return m_flatMode.load(std::memory_order_acquire); }
        struct FlatNativeLoadingSelectionSnapshot
        {
            Policy::NativeLoadingSelection selection{};
            void* owner = nullptr;
            std::uint64_t initialSerial = 0;
        };
        // Coherent initial native choice used by NG's exact Show boundary.
        // The model field is latched only from loadScreenShown=false; content
        // may retain a later same-owner tip refresh. Passing a required serial
        // prevents a chained Show from consuming the prior menu's publication.
        bool TryGetFlatNativeLoadingSelection(
            FlatNativeLoadingSelectionSnapshot& a_snapshot,
            std::uint64_t a_requiredInitialSerial = 0);
        // A chained native Hide+Show can complete between two Presents. The
        // custom CLOSE clears compositor state, so re-publish the already
        // verified new Show before its custom OPEN.
        bool RestoreFlatNativeLoadingSelectionForUpcomingOpen(
            const FlatNativeLoadingSelectionSnapshot& a_snapshot);
        void KillAdvanceMovie();  // Block loading screen rendering immediately

        // True once the per-load AdvanceMovie RET has fired. On every verified
        // flat mode-3 runtime this can happen only after Fallout's native
        // content choice and a later
        // successfully presented custom frame. Tip-bearing loads additionally
        // require an owner/serial-matched visible-pixel delta from DisplayMovie,
        // so the
        // deferred animation-loop NOP cannot freeze an empty or fade-start SWF
        // frame.
        bool IsAdvanceMovieKilled() const { return m_advanceMovieKilled.load(std::memory_order_acquire); }

        // Secondary frame floor before mode 3 may kill AdvanceMovie. Native
        // selection/draw/presentation proof is the authoritative readiness
        // gate; this count is never a fallback.
        void SetAdvanceMovieKillFrames(int n) {
            m_advanceMovieKillFrames.store(n, std::memory_order_release);
        }

        // Per-frame callback (flat mode: called from Present hook for per-frame updates)
        using FrameCallback = void(*)();
        void SetFrameCallback(FrameCallback cb) { m_frameCallback.store(cb, std::memory_order_release); }

        // March v1.0 deferred NOP: applied after the next right-eye Submit
        // returns. The manager preserves March's 500/100 ms floor and may hold
        // a verified tip-bearing mode-3 screen to the bounded 600/700 ms capture
        // deadlines; OpenVR's return code never gates the patch. Exact-byte
        // ownership still protects the executable site.
        void RequestDeferredNOP(std::uintptr_t address,
            const std::uint8_t* expectedBytes, const std::uint8_t* patchBytes,
            std::size_t size, std::uint64_t loadGeneration);
        bool IsDeferredNOPApplied() const { return m_deferredNOPApplied.load(); }
        void ResetDeferredState();

        // Mode-3 tips capture window. The only publishable source is the
        // LoadingMenu-owned Scaleform render target; eye-texture extraction is
        // deliberately not exposed because it can contain controllers/hands.
        void SetTipsExtractEnabled(bool e);
        // A verified flat native-minimal screen has no tip pixels to capture,
        // but its owner/content/serial must survive until one opaque black
        // Present succeeds and authorizes the background-only freeze.
        void DisableTipsCapturePreservingFlatNativeSelection();
        bool IsTipsExtractEnabled() const { return m_tipsExtractEnabled.load(std::memory_order_acquire); }
        bool IsTipsDeltaProofPending() const {
            return m_tipsDeltaQueryPending.load(std::memory_order_acquire);
        }
        // True once this load's capture is provably non-convergent. The mode-3
        // terminal policy treats it as an immediate background fallback at the
        // March floor instead of wasting the bounded 600 ms capture window.
        bool IsTipsCaptureHopeless() const {
            return m_tipsCaptureHopeless.load(std::memory_order_acquire);
        }
        struct VRNativeLoadingSelectionSnapshot
        {
            Policy::NativeLoadingContent content{
                Policy::NativeLoadingContent::kUnknown
            };
            void* owner = nullptr;
            std::uint64_t serial = 0;
            std::uint64_t epoch = 0;
        };
        bool TryGetVRNativeLoadingSelection(
            VRNativeLoadingSelectionSnapshot& a_snapshot) const {
            if (!m_vrNativeSelectionSeen.load(std::memory_order_acquire)) {
                return false;
            }
            const auto serial =
                m_vrNativeSelectionSerial.load(std::memory_order_acquire);
            const auto publicationEpoch =
                m_vrNativeSelectionEpoch.load(std::memory_order_acquire);
            const auto loadEpoch =
                m_vrTipsLoadEpoch.load(std::memory_order_acquire);
            const auto content = static_cast<Policy::NativeLoadingContent>(
                m_vrNativeLoadingContent.load(std::memory_order_acquire));
            void* const owner =
                m_loadingTextOwner.load(std::memory_order_acquire);
            const bool ready =
                m_loadingTextReady.load(std::memory_order_acquire);
            if (!m_vrNativeSelectionSeen.load(std::memory_order_acquire) ||
                serial == 0 ||
                m_vrNativeSelectionSerial.load(std::memory_order_acquire) !=
                    serial ||
                publicationEpoch != loadEpoch ||
                m_vrNativeSelectionEpoch.load(std::memory_order_acquire) !=
                    publicationEpoch ||
                !owner ||
                (content == Policy::NativeLoadingContent::kTipAndLevel &&
                 !ready)) {
                return false;
            }
            a_snapshot.content = content;
            a_snapshot.owner = owner;
            a_snapshot.serial = serial;
            a_snapshot.epoch = publicationEpoch;
            return true;
        }
        // SendLoadingText normally publishes immediately before the matching
        // native OPEN. At that point its selection is tagged for loadEpoch+1;
        // allow the game thread to choose blocker-only black for a verified
        // minimal screen before any custom artwork is attached. The ordinary
        // getter above remains strict to the already-active epoch.
        bool TryGetVRNativeLoadingSelectionForUpcomingOpen(
            VRNativeLoadingSelectionSnapshot& a_snapshot) const {
            if (m_inLoadingScreen.load(std::memory_order_acquire) ||
                !m_vrNativeSelectionSeen.load(std::memory_order_acquire)) {
                return false;
            }
            const auto serial =
                m_vrNativeSelectionSerial.load(std::memory_order_acquire);
            const auto publicationEpoch =
                m_vrNativeSelectionEpoch.load(std::memory_order_acquire);
            const auto expectedEpoch =
                m_vrTipsLoadEpoch.load(std::memory_order_acquire) + 1;
            const auto content = static_cast<Policy::NativeLoadingContent>(
                m_vrNativeLoadingContent.load(std::memory_order_acquire));
            void* const owner =
                m_loadingTextOwner.load(std::memory_order_acquire);
            const bool ready =
                m_loadingTextReady.load(std::memory_order_acquire);
            if (!m_vrNativeSelectionSeen.load(std::memory_order_acquire) ||
                serial == 0 ||
                m_vrNativeSelectionSerial.load(std::memory_order_acquire) !=
                    serial ||
                publicationEpoch != expectedEpoch ||
                m_vrNativeSelectionEpoch.load(std::memory_order_acquire) !=
                    publicationEpoch ||
                !owner ||
                content == Policy::NativeLoadingContent::kUnknown ||
                (content == Policy::NativeLoadingContent::kTipAndLevel &&
                 !ready)) {
                return false;
            }
            a_snapshot.content = content;
            a_snapshot.owner = owner;
            a_snapshot.serial = serial;
            a_snapshot.epoch = publicationEpoch;
            return true;
        }
        bool IsTipsTextureReady() {
            // Poll the asynchronous non-empty proof from the render/update
            // thread. This avoids a CPU/GPU stall inside DisplayMovie while
            // ensuring an all-transparent first delta is never published.
            std::lock_guard resourceLock(m_renderResourceMutex);
            TryFinalizeTipsDeltaQuery();
            return m_tipsCaptureComplete.load(std::memory_order_acquire);
        }
        // Return an AddRef'd owner-scoped LoadingMenu delta. The caller must
        // Release it after submitting to OpenVR. Never fall back to an eye
        // texture: that is how controller imagery became baked into mode 3.
        void* AcquireTipsTexture() const;
        bool IsScaleformTipsReady() {
            return IsTipsTextureReady();
        }

        // Composite bg + tips into a single owned RGBA RT, returning its
        // ID3D11Texture2D*. Used to submit a single IVROverlay containing
        // both bg and tip text baked into the same pixels — matches v1.0's
        // pre-overlay model and avoids the "tips behind bg" depth conflict.
        // Returns nullptr if either source isn't ready or D3D resources can't
        // be allocated. Idempotent: re-runs the composite each call.
        void* CompositeTipsIntoBg();

        // VR scaleform-RT capture. Hook BSGraphics::Renderer::GetScaleformSurfaceType
        // to learn the index of the render target that scaleform draws loading-menu
        // UI into. Acquire the texture from RenderTarget::texture when present,
        // otherwise from RenderTarget::rtView->GetResource (the authoritative
        // VR layout). The returned COM pointer owns one reference.
        bool InstallScaleformRTHook();
        bool InstallScreenSpaceRTHook();
        int IdentifyBoundRenderTargetIndex() const;
        void* AcquireScaleformRTTexture() const;
        int  GetScaleformRTIndex() const {
            return m_loadingMenuScaleformRtIndex.load(std::memory_order_acquire);
        }

        // Legacy diagnostic hooks. LoadingMenu suppression is owned solely by
        // the vtable-checked DisplayMenu hook so other menus remain untouched.
        bool InstallRenderMenusHook();

        // Hook IMenu::DisplayMenu — the menu-level dispatcher that calls
        // BSScaleformRenderer::DisplayMovie. Mode 3 snapshots the exact bound
        // RTV subresource before and after this verified LoadingMenu call; modes
        // 0/2 suppress it only while their custom blocker is active.
        bool InstallDisplayMenuHook();

        // Legacy diagnostic hook for the universal movie compositor. It never
        // suppresses content because this function has no menu identity.
        bool InstallDisplayMovieHook();

        // Silences the loading-model sound loops whenever the plugin is
        // showing a custom loading screen (no model exists to make them).
        bool InstallUpdateSoundsHook();

        // Install the complete VR mode-3 tips pipeline outside LoadingMenu's
        // timed path. Idempotent/single-flight; partial failures remain a clean
        // native-tips fallback and may be retried after a later config change.
        bool InstallVRMode3Hooks();
        bool AreVRMode3HooksReady() const {
            return m_vrMode3HooksReady.load(std::memory_order_acquire);
        }

        // "Currently inside a LoadingMenu" flag. Drives the RenderMenus hook
        // suppression for ALL modes (mode 2 additionally requires warmup
        // completion so the RT has time to populate for capture).
        void SetInLoadingScreen(bool v) {
            // Publish the per-load state BEFORE the flag that makes the render
            // thread act on it, so a hook observing "in loading screen" always
            // sees this load's timestamp and zeroed counters.
            if (v) {
                // A fresh OPEN supersedes any armed post-close suppression
                // window; per-load rules own LoadingMenu rendering again.
                m_lmPostCloseSuppressMainMenu.store(
                    false, std::memory_order_release);
                m_lmPostCloseSuppressUntilTicks.store(
                    0, std::memory_order_release);
                m_inLoadingScreenSinceTicks.store(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count(),
                    std::memory_order_release);
                // Per-load diagnostic — log the first 4 suppress decisions
                // regardless of mode so we can verify the hook is firing.
                m_renderMenusSkipCount.store(0, std::memory_order_relaxed);
                m_displayMenuSkipCount.store(0, std::memory_order_relaxed);
                m_displayMovieSkipCount.store(0, std::memory_order_relaxed);
                m_displayMovieFireCount.store(0, std::memory_order_relaxed);
                m_diagLoadingMenuDisplayCalls.store(0, std::memory_order_relaxed);
                m_diagLoadingMenuDisplayVisible.store(0, std::memory_order_relaxed);
                m_diagFirstVisibleDisplayMs.store(-1, std::memory_order_relaxed);
                m_diagDisplayMovieCaptureEligible.store(0, std::memory_order_relaxed);
                m_diagCaptureArms.store(0, std::memory_order_relaxed);
                m_diagCaptureBeginRejects.store(0, std::memory_order_relaxed);
                m_diagDisplayMovieResultZero.store(0, std::memory_order_relaxed);
                m_diagCaptureCompleteCancels.store(0, std::memory_order_relaxed);
                m_eyeScrubLogCount.store(0, std::memory_order_relaxed);
                m_diagRejectInsufficient.store(0, std::memory_order_relaxed);
                m_diagRejectFullSurface.store(0, std::memory_order_relaxed);
                m_tipsFullSurfaceRun.store(0, std::memory_order_relaxed);
                m_tipsCaptureHopeless.store(false, std::memory_order_relaxed);
                m_eyeScrubOkCount.store(0, std::memory_order_relaxed);
                m_eyeScrubFailCount.store(0, std::memory_order_relaxed);
                m_eyeScrubLastHr.store(0, std::memory_order_relaxed);
                if (m_isVR.load(std::memory_order_acquire)) {
                    const auto loadEpoch =
                        m_vrTipsLoadEpoch.fetch_add(
                            1, std::memory_order_acq_rel) + 1;
                    // SendLoadingText can publish immediately before the OPEN
                    // event. Retain only a publication explicitly tagged for
                    // the epoch that just became active.
                    if (m_vrNativeSelectionEpoch.load(
                            std::memory_order_acquire) != loadEpoch) {
                        m_vrNativeSelectionSeen.store(
                            false, std::memory_order_release);
                        m_vrNativeLoadingContent.store(
                            static_cast<std::uint8_t>(
                                Policy::NativeLoadingContent::kUnknown),
                            std::memory_order_release);
                        m_loadingTextReady.store(
                            false, std::memory_order_release);
                        m_loadingTextOwner.store(
                            nullptr, std::memory_order_release);
                    }
                }
            }
            if (!v && !m_isVR.load(std::memory_order_acquire)) {
                // Flat RET authorization uses this same lock. CLOSE cannot
                // cross the final ownership check and executable-byte write.
                std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
                m_inLoadingScreen.store(false, std::memory_order_release);
                return;
            }
            if (!v && m_isVR.load(std::memory_order_acquire)) {
                // Invalidate the current publication before a late render hook
                // can certify it for the next LoadingMenu.
                m_inLoadingScreen.store(false, std::memory_order_release);
                m_vrTipsLoadEpoch.fetch_add(1, std::memory_order_acq_rel);
                m_vrNativeSelectionSeen.store(
                    false, std::memory_order_release);
                m_vrNativeSelectionEpoch.store(0, std::memory_order_release);
                m_vrNativeLoadingContent.store(
                    static_cast<std::uint8_t>(
                        Policy::NativeLoadingContent::kUnknown),
                    std::memory_order_release);
                m_loadingTextReady.store(false, std::memory_order_release);
                m_loadingTextOwner.store(nullptr, std::memory_order_release);
                return;
            }
            m_inLoadingScreen.store(v, std::memory_order_release);
        }
        bool IsInLoadingScreen() const {
            return m_inLoadingScreen.load(std::memory_order_acquire);
        }
        // Total eye submits this session. The load heartbeat differentiates it
        // to get a frame rate, which is the one number that separates "engine
        // busy streaming" from "engine stalled" during a frozen load.
        int TotalEyeSubmits() const {
            return m_eyeSubmitTotal.load(std::memory_order_relaxed);
        }

        // Bounded post-close LoadingMenu render suppression. Armed at native
        // CLOSE only for loads whose presentation was custom-owned, so the
        // menu's tail frames (black + tip/level + spinner during its fade-out)
        // can never present after the custom overlays are released. A fresh
        // OPEN clears the window; it also self-expires after a_maxMs.
        // a_includeMainMenu additionally suppresses MainMenu renders in the
        // same window — the engine re-renders the not-yet-removed title menu
        // for ~0.5 s after a save load that started there. Pass it only for
        // closes that entered a game session, never for quit-to-menu.
        // Monotonically extend the armed post-close suppression window to at
        // least now + a_marginMs, and keep the observation window ahead of it.
        //
        // Needed because the two clocks disagree: suppression is armed on
        // wall-clock at CLOSE, but the black cover is released by a tick that
        // only runs from the frame callback after a right-eye Submit, and that
        // deadline is re-based when the art is released. A post-close submit
        // stall (a documented state in this build - the heartbeat has measured
        // 28 consecutive samples at zero submits) therefore pushes the cover
        // release past a suppression window that expired on schedule, leaving
        // native LoadingMenu tail frames drawing with nothing in front of them.
        //
        // Deliberately NOT BeginPostCloseLoadingMenuSuppression: that stores
        // unconditionally (so it could SHORTEN a longer window) and resets the
        // census arm-time and budget, which would corrupt the timeline the
        // census exists to record.
        void ExtendPostCloseLoadingMenuSuppression(int a_marginMs);

        void BeginPostCloseLoadingMenuSuppression(
            int a_maxMs, bool a_includeMainMenu = false);

        // One-line per-load summary of the mode-3 capture pipeline gates.
        // Cheap (reads atomics); call at most a couple of times per load.
        void LogTipsPipelineDiagnostics(const char* a_stage);

    private:
        D3D11Compositor() = default;
        ~D3D11Compositor();

        // Shader compilation
        bool CompileShaders();
        bool CreatePipelineResources();
        bool ResolveAdvanceMovie();
        bool TryRestoreAdvanceMovie();
        bool TryRestoreAdvanceMovieLocked();
        bool InstallFlatTipDrawHooks();
        bool RebuildDeviceResources(void* d3dDevice);
        void ReleaseDeviceResources();
        void HandleDeviceLoss(const char* reason);
        void ConfigureTipsExtraction(
            bool a_enabled, bool a_preserveFlatNativeSelection);

        // Submit hook (VR mode: LuminanceKey) — called once per eye
        static int __cdecl HookedSubmit(void* compositor, int eye,
            const void* texture, const void* bounds, int flags);
        void CompositeFrame(void* eyeTexture2D, int eye);

        // Present hook (flat mode) — composites background behind loading screen
        static HRESULT WINAPI HookedPresentFlat(void* swapChain, UINT syncInterval, UINT flags);
        void CompositeFlatFrame(void* backbufferTex);

        // ClearRTV hook (mode: ClearIntercept)
        static void __stdcall HookedClearRTV(void* context, void* rtv,
            const float color[4]);
        void DrawBackgroundOnRTV(void* context, void* rtv);

        // Ensure temp texture matches eye texture size and format
        bool EnsureTempTexture(unsigned int width, unsigned int height, unsigned int format);
        void* AcquireBackgroundSRV(unsigned int& width, unsigned int& height) const;
        bool HasBackgroundTexture() const;

        // State
        std::atomic<bool> m_initialized{ false };
        std::atomic<bool> m_renderReady{ false };
        std::atomic<bool> m_enabled{ false };
        std::atomic<CompositeMode> m_mode{ CompositeMode::LuminanceKey };

        // D3D11 objects (stored as void* to avoid d3d11.h in header)
        void* m_device = nullptr;
        void* m_context = nullptr;
        // Serializes exceptional device-rebuild teardown against compositor
        // resource users. Normal render calls are all on Fallout's render
        // thread; this mutex exists for renderer/device replacement callbacks.
        mutable std::recursive_mutex m_renderResourceMutex;
        std::atomic<bool> m_deviceLost{ false };

        // Shaders
        void* m_vsFullscreen = nullptr;    // ID3D11VertexShader*
        void* m_psLuminanceKey = nullptr;  // ID3D11PixelShader*
        void* m_psBackground = nullptr;    // ID3D11PixelShader*
        void* m_psBlit = nullptr;          // ID3D11PixelShader* (pass-through sampler for upscale)
        void* m_psTipsKey = nullptr;       // ID3D11PixelShader* (maskless lum->alpha key for composite)
        void* m_psTipsDelta = nullptr;     // ID3D11PixelShader* (LoadingMenu before/after delta)
        void* m_psFlatTipsProof = nullptr; // ID3D11PixelShader* (flat-visible tip delta proof)

        // Resources
        void* m_sampler = nullptr;         // ID3D11SamplerState*
        void* m_constantBuffer = nullptr;  // ID3D11Buffer*
        void* m_blendState = nullptr;      // ID3D11BlendState*
        void* m_alphaBlendState = nullptr; // straight-alpha RGB, destination alpha preserved
        void* m_rasterState = nullptr;     // ID3D11RasterizerState*
        void* m_depthState = nullptr;      // ID3D11DepthStencilState*

        // Background texture SRV
        void* m_bgSRV = nullptr;           // ID3D11ShaderResourceView*
        mutable std::mutex m_bgMutex;       // protects SRV replacement and dimensions

        // Temp texture for compositing (copy game texture here as input)
        void* m_tempTexture = nullptr;     // ID3D11Texture2D*
        void* m_tempSRV = nullptr;         // ID3D11ShaderResourceView*
        unsigned int m_tempWidth = 0;
        unsigned int m_tempHeight = 0;
        unsigned int m_tempFormat = 0;

        std::atomic<bool> m_tipsExtractEnabled{ false };

        // LoadingMenu capture path. HookedDisplayMovie snapshots the exact
        // currently bound RTV subresource immediately before and after only the
        // vtable-verified LoadingMenu movie draws. The delta shader can therefore
        // publish tip/level pixels without carrying pre-existing eye/controller
        // pixels into our overlay.
        struct LoadingMenuCaptureViewport
        {
            float topLeftX = 0.0f;
            float topLeftY = 0.0f;
            float width = 0.0f;
            float height = 0.0f;
            float minDepth = 0.0f;
            float maxDepth = 0.0f;
        };
        struct LoadingMenuCaptureScissor
        {
            std::int32_t left = 0;
            std::int32_t top = 0;
            std::int32_t right = 0;
            std::int32_t bottom = 0;
            bool enabled = false;
        };
        struct LoadingMenuCaptureRegion
        {
            unsigned int left = 0;
            unsigned int top = 0;
            unsigned int right = 0;
            unsigned int bottom = 0;
        };
        bool BeginLoadingMenuDeltaCapture();
        void CompleteLoadingMenuDeltaCapture();
        void CancelLoadingMenuDeltaCapture();
        void TryFinalizeTipsDeltaQuery();
        void CaptureScaleformRTToTips();
        void* m_tipsScaleformTex = nullptr; // ID3D11Texture2D* (our owned copy, upscaled)
        void* m_tipsScaleformRTV = nullptr; // ID3D11RenderTargetView*
        void* m_loadingMenuBeforeTex = nullptr; // ID3D11Texture2D*
        void* m_loadingMenuBeforeSRV = nullptr; // ID3D11ShaderResourceView*
        void* m_loadingMenuAfterTex = nullptr;  // ID3D11Texture2D*
        void* m_loadingMenuAfterSRV = nullptr;  // ID3D11ShaderResourceView*
        void* m_flatTipsProofSRV = nullptr;     // accepted flat delta, replayed every Present
        void* m_loadingMenuCaptureSource = nullptr; // ID3D11Texture2D*, owned ref while armed
        void* m_tipsDeltaQuery = nullptr; // ID3D11Query*, proves non-empty glyph output
        unsigned int m_loadingMenuCaptureSubresource = 0;
        unsigned int m_loadingMenuCaptureWidth = 0;
        unsigned int m_loadingMenuCaptureHeight = 0;
        unsigned int m_loadingMenuCaptureFormat = 0;
        unsigned int m_loadingMenuCaptureViewFormat = 0;
        unsigned int m_loadingMenuCaptureSrvFormat = 0;
        // The armed delta draw's own target size — the denominator for the
        // full-surface coverage test, in the SAME units as the occlusion-query
        // numerator. Deliberately separate from m_tipsScaleformWidth/Height,
        // which CaptureScaleformRTToTips can overwrite mid-query.
        unsigned int m_tipsDeltaQueryWidth = 0;
        unsigned int m_tipsDeltaQueryHeight = 0;
        LoadingMenuCaptureViewport m_loadingMenuCaptureViewport{};
        LoadingMenuCaptureScissor m_loadingMenuCaptureScissor{};
        LoadingMenuCaptureRegion m_loadingMenuCaptureRegion{};
        std::atomic<bool> m_loadingMenuDeltaArmed{ false };
        std::atomic<bool> m_tipsDeltaQueryPending{ false };
        // The flat proof tags belong to the exact DisplayMenu TLS publication
        // that armed this delta. They are read/written only while holding
        // m_renderResourceMutex; the final serial is atomic because
        // HookedPresentFlat consumes it outside that mutex.
        bool m_loadingMenuCaptureFlatProof = false;
        void* m_loadingMenuCaptureFlatOwner = nullptr;
        std::uint64_t m_loadingMenuCaptureFlatSerial = 0;
        std::uint64_t m_loadingMenuCaptureFlatLoadEpoch = 0;
        // VR uses the same owner-scoped delta, but its native selection can
        // refresh while the GPU query is pending. Revalidate all three tags
        // before publishing or compositing the captured texture.
        bool m_loadingMenuCaptureVRProof = false;
        void* m_loadingMenuCaptureVROwner = nullptr;
        std::uint64_t m_loadingMenuCaptureVRSerial = 0;
        std::uint64_t m_loadingMenuCaptureVREpoch = 0;
        // Shared native completion signal. VR consumes it in the exact-delta
        // capture path; flat consumes it in the post-ready Present freeze gate.
        std::atomic<bool> m_loadingTextReady{ false };
        std::atomic<void*> m_loadingTextOwner{ nullptr };
        // Fallout4VR 1.2.72 uses the same SendLoadingText selection fields and
        // branch as flat Fallout. This publication is separate from flat's
        // serial/draw proof: VR needs the native choice to decide whether its
        // 100 ms March floor may freeze immediately or must leave Scaleform live
        // for a bounded exact-delta capture. Epoch tags make pre-OPEN publication
        // legal while rejecting CLOSE/reopen leakage.
        std::atomic<std::uint8_t> m_vrNativeLoadingContent{
            static_cast<std::uint8_t>(
                Policy::NativeLoadingContent::kUnknown)
        };
        std::atomic<bool> m_vrNativeSelectionSeen{ false };
        std::atomic<std::uint64_t> m_vrNativeSelectionSerial{ 0 };
        std::atomic<std::uint64_t> m_vrNativeSelectionEpoch{ 0 };
        std::atomic<std::uint64_t> m_vrTipsLoadEpoch{ 0 };
        // bg+tips composite — owned RGBA RT sized to bg, holds bg with tips
        // alpha-keyed on top. Submitted as the bg overlay's only texture so
        // VR sees a single overlay with everything pre-baked.
        void* m_bgPlusTipsTex = nullptr;    // ID3D11Texture2D*
        void* m_bgPlusTipsRTV = nullptr;    // ID3D11RenderTargetView*
        // Pass-2 blend for the composite: SrcAlpha/InvSrcAlpha on COLOR but
        // dest-alpha PRESERVED (SrcBlendAlpha=ZERO, DestBlendAlpha=ONE).
        // This state uses premultiplied RGB (ONE/InvSrcAlpha); the generic
        // straight-alpha state also preserves destination alpha.
        void* m_bgPlusTipsBlend = nullptr;  // ID3D11BlendState*
        void* m_tipsSrcSRV    = nullptr;    // ID3D11ShaderResourceView* on m_tipsScaleformTex
        unsigned int m_bgPlusTipsWidth  = 0;
        unsigned int m_bgPlusTipsHeight = 0;

        // Captured tip/level glyph extent within m_tipsScaleformTex, in
        // normalized texture coordinates. The capture is the whole screen-space
        // UI surface (observed 1024x1024 on VR), and Fallout lays the loading
        // screen out inside a band of it, so fitting the WHOLE square into the
        // 16:9 composite left the text at roughly half width. Measuring the real
        // glyph extent once per capture lets the composite scale the text to the
        // background instead of to the surface it happened to be drawn on.
        bool  ComputeTipsContentBounds();
        void* m_tipsBoundsTex     = nullptr; // ID3D11Texture2D* (small, RT)
        void* m_tipsBoundsRTV     = nullptr; // ID3D11RenderTargetView*
        void* m_tipsBoundsStaging = nullptr; // ID3D11Texture2D* (CPU readable)
        static constexpr unsigned int kTipsBoundsRes = 128;
        float m_tipsContentU0 = 0.0f;
        float m_tipsContentV0 = 0.0f;
        float m_tipsContentU1 = 1.0f;
        float m_tipsContentV1 = 1.0f;
        bool  m_tipsContentBoundsValid = false;
        // Where the measured glyph block is anchored: left edge at the left
        // margin, bottom edge at the bottom line — matching where the native
        // flat loading screen keeps its tip text, per user preference. The
        // block is NOT rescaled to fill a share of the screen (that enlargement
        // read as far too big in the headset); it draws at the plain
        // contain-fit size, widened by the modest factor below.
        static constexpr float kTipsAnchorLeftMarginFraction = 0.04f;
        static constexpr float kTipsAnchorBottomFraction = 0.90f;
        // Horizontal-only widening of the drawn tip/level block, anchored at
        // the left margin so only the right edge extends.
        static constexpr float kTipsWidthScale = 1.05f;
        // Reject a measured block too narrow to be a line of tip text, so a
        // partial fade-in capture cannot mis-anchor the placement.
        static constexpr float kTipsMinContentWidth = 0.10f;
        void* m_tipsScaleformSrcSRV = nullptr;  // ID3D11ShaderResourceView* on game's RT
        void* m_tipsScaleformSrcCached = nullptr; // last-seen game tex pointer
        unsigned int m_tipsScaleformWidth = 0;
        unsigned int m_tipsScaleformHeight = 0;
        unsigned int m_tipsScaleformFormat = 0;
        std::atomic<int> m_tipsScaleformLogCount{ 0 };
        static constexpr unsigned int kTipsUpscale = 2;  // 2x → 2048x2048 from 1024 source

        // VR capture-once gate: HookedSendLoadingText identifies the owner;
        // the next non-empty DisplayMovie delta is frozen for this load.
        std::atomic<bool> m_tipsCaptureComplete{ false };

        // Eye texture tracking from Submit
        void* m_lastLeftEye = nullptr;
        void* m_lastRightEye = nullptr;

        std::atomic<int> m_clearMatchCount{ 0 };
        std::atomic<int> m_submitCompositeCount{ 0 };

        // Deferred NOP — applied inside Submit hook after right eye completes
        std::atomic<bool> m_deferredNOPPending{ false };
        std::atomic<bool> m_deferredNOPApplied{ false };
        std::uintptr_t m_deferredNOPAddress = 0;
        std::uint8_t m_deferredNOPBytes[16] = {};
        std::uint8_t m_deferredNOPOriginalBytes[16] = {};
        std::size_t m_deferredNOPSize = 0;
        bool m_deferredNOPOriginalValid = false;
        // Protected by m_deferredNOPMutex. The serial/generation are diagnostic
        // ownership tags only; March did not wait for a separately accepted
        // left-eye submission before freezing the animation loop.
        std::uint64_t m_deferredNOPArmSerial = 0;
        std::uint64_t m_deferredNOPLoadGeneration = 0;
        std::mutex m_deferredNOPMutex;

        // Frozen state — flag retained because the Submit hook still uses
        // m_deferredNOP* to gate post-NOP compositing behavior
        std::atomic<bool> m_frozen{ false };

        // GPU constant buffer layout (must match the active HLSL cbuffers).
        struct CompositeParams
        {
            float threshold;
            float bgUvScaleX;   // Aspect-correct UV scale for background texture
            float bgUvScaleY;
            float pad;
        };
        struct DeltaSourceParams
        {
            float uvOffsetX;
            float uvOffsetY;
            float uvScaleX;
            float uvScaleY;
        };
        static_assert(sizeof(DeltaSourceParams) == sizeof(CompositeParams));
        std::atomic<float> m_luminanceThreshold{ 0.25f };

        // Background texture dimensions (for aspect ratio correction)
        unsigned int m_bgWidth = 0;
        unsigned int m_bgHeight = 0;

        // Flat mode state
        void* m_swapChain = nullptr;       // IDXGISwapChain*
        std::mutex m_flatSwapChainMutex;
        // Load-only 350-FPS limiter state. This is deliberately separate from
        // Fallout's gameplay VSync and exists only while the compositor is enabled
        // for LoadingMenu.
        std::atomic<std::int64_t> m_flatLimiterDeadlineUs{ 0 };
        std::atomic<std::uint32_t> m_flatLimiterWaitCount{ 0 };
        std::atomic<std::uint64_t> m_flatLimiterWaitUs{ 0 };
        std::atomic<bool> m_flatSyncSampled{ false };
        std::atomic<UINT> m_flatIncomingSyncInterval{ 0 };
        std::atomic<UINT> m_flatOutgoingSyncInterval{ 0 };
        std::atomic<bool> m_isVR{ false };
        std::atomic<bool> m_flatLazyInit{ false }; // True when using dummy device fallback
        std::atomic<FrameCallback> m_frameCallback{ nullptr };
        std::atomic<std::uint32_t> m_flatPresentCount{ 0 };
        // Flat mode-3 readiness ownership. SendLoadingText can return just before
        // the MenuOpen event, so OPEN preserves native selection state while
        // CLOSE invalidates it. The publication serial ties the exact native
        // content choice to a later DisplayMovie and successful Present; the
        // load epoch prevents a CLOSE/reopen race from authorizing stale proof.
        std::atomic<bool> m_flatTipReadinessHookInstalled{ false };
        std::atomic<bool> m_flatTipDrawHooksInstalled{ false };
        std::atomic<std::uint8_t> m_flatNativeLoadingContent{ 0 };
        // AdvanceMovie consumes artScreen once to create the 3D model, while
        // later SendLoadingText calls can rotate text from validScreens. Latch
        // the initial model choice separately and tag the exact publication.
        std::atomic<std::uint8_t> m_flatNativeModelSelection{ 0 };
        std::atomic<std::uint64_t> m_flatNativeInitialSelectionSerial{ 0 };
        // Published only after the owner-tagged before/after delta passes its
        // non-empty, non-full-surface GPU coverage proof. A DisplayMovie return
        // value by itself is deliberately insufficient.
        std::atomic<std::uint64_t> m_flatTipsDrawSerial{ 0 };
        std::atomic<std::uint64_t> m_flatTipsPresentedSerial{ 0 };
        std::atomic<std::uint64_t> m_flatTipsReadySerial{ 0 };
        std::atomic<std::uint64_t> m_flatTipsLoadEpoch{ 0 };
        bool m_skipPresent = false;
        std::atomic<bool> m_flatBackgroundOnly{ false }; // draw background without luminance key
        std::atomic<int> m_flatMode{ 2 };   // 0=blank, 1=native, 2=bg, 3=bg+tips
        std::atomic<bool> m_advanceMovieKilled{ false };
        // Shared secondary frame floor. Every verified flat runtime additionally
        // requires exact native readiness plus a completed Present.
        std::atomic<int> m_advanceMovieKillFrames{ 60 };
        static constexpr std::size_t kAdvanceMovieVerifiedPrefixSize = 26;
        std::array<std::uint8_t, kAdvanceMovieVerifiedPrefixSize>
            m_advanceMovieOrigBytes{};
        std::uint8_t m_advanceMovieOrigByte = 0;
        std::uintptr_t m_advanceMovieAddr = 0;
        std::mutex m_advanceMovieMutex;
        std::atomic<bool> m_advanceMovieRestoreReadFailureLogged{ false };
        std::atomic<bool> m_disableVSyncWhileLoading{ false };

        // Lazy init helper — complete initialization on first Present call
        bool CompleteFlatInit(void* swapChain);
        bool TryGetDeviceFromRendererData();
        bool AcceptFlatSwapChain(void* swapChain);

        // NG loading state
        std::atomic<bool> m_menuEventsActive{ false };
        std::chrono::steady_clock::time_point m_ngLoadStartTime{};
        int m_ngLoadNumber = 0;

        // Original function pointers
        static inline decltype(&HookedSubmit) s_originalSubmit = nullptr;
        static inline decltype(&HookedClearRTV) s_originalClearRTV = nullptr;
        // Vtable-restore bookkeeping. We save the slot address and original
        // function pointer so a future shutdown path can unswap; today there's
        // no DLL-detach hook so these are documentation-grade. Without them
        // the DLL must remain pinned for the process lifetime — see audit.
        static inline void** s_submitVtableSlot = nullptr;
        static inline void*  s_originalSubmitForRestore = nullptr;
        static inline void** s_clearRTVVtableSlot = nullptr;
        static inline void*  s_originalClearRTVForRestore = nullptr;
        // Flat Strategy-1 game-swapchain vtable[8] restore bookkeeping (same caveat).
        static inline void** s_presentVtableSlot = nullptr;
        static inline void*  s_originalPresentFlatForRestore = nullptr;
        static inline decltype(&HookedPresentFlat) s_originalPresentFlat = nullptr;
        static inline decltype(&HookedPresentFlat) s_originalPresentFlip = nullptr;
        static inline D3D11Compositor* s_instance = nullptr;

        // Scaleform RT capture state
        using GetScaleformSurfaceType_t = void* (__fastcall*)(void*, unsigned int);
        static inline GetScaleformSurfaceType_t s_originalGetScaleformSurfaceType = nullptr;
        static void* __fastcall HookedGetScaleformSurfaceType(void* rendererThis, unsigned int index);
        std::atomic<int> m_scaleformRtIndex{ -1 };
        // Written only after the verified LoadingMenu DisplayMenu hook matches
        // the D3D11 render target currently bound by Fallout against the
        // RendererData table. This identifies the exact active movie surface
        // (observed idx 60), not a guessed "next Scaleform call" or submitted
        // VR eye.
        std::atomic<int> m_loadingMenuScaleformRtIndex{ -1 };
        // The RT is shared by the entire screen-space menu pass; LoadingMenu
        // ownership is proven separately by HookedDisplayMenu after the verified
        // LoadingMenu instance has rendered. Never infer ownership from “the
        // next Scaleform call after OPEN”.
        std::atomic<int> m_screenSpaceRtIndex{ -1 };
        std::atomic<bool> m_loadingMenuRenderFresh{ false };
        using ScreenSpaceSetRT_t = void (__fastcall*)(void*);
        static inline ScreenSpaceSetRT_t s_originalScreenSpaceSetRT = nullptr;
        static void __fastcall HookedScreenSpaceSetRT(void* uiSingleton);
        static inline thread_local bool s_insideScreenSpaceSetRT = false;
        bool m_screenSpaceRTHookInstalled = false;
        std::atomic<int> m_scaleformHookLogCount{ 0 };
        mutable std::atomic<int> m_scaleformRTLogCount{ 0 };
        bool m_scaleformRTHookInstalled = false;

        // RenderMenus hook — UI::ScreenSpace_RenderMenus(void* this).
        // Suppresses screen-space menu render during loading (post-warmup) so
        // LoadingMenu's tips/level don't leak into the eye buffer behind our
        // bg overlay. Gated on m_tipsExtractEnabled and a completed current-load
        // capture, so allocation/copy failures retain the native fallback and
        // HUD/Pip-Boy rendering is never affected outside the capture window.
        using RenderMenus_t = void (__fastcall*)(void*);
        static inline RenderMenus_t s_originalRenderMenus = nullptr;
        static void __fastcall HookedRenderMenus(void* uiSingleton);
        bool m_renderMenusHookInstalled = false;
        std::atomic<int> m_renderMenusSkipCount{ 0 };  // diagnostic — suppressed renders
        std::atomic<bool> m_inLoadingScreen{ false };
        // steady_clock deadline (ticks) for the post-close LoadingMenu render
        // suppression window; 0 = inactive. Written on the game thread at
        // CLOSE, cleared at OPEN, read on the render thread per DisplayMenu.
        std::atomic<std::int64_t> m_lmPostCloseSuppressUntilTicks{ 0 };
        // Whether the active post-close window also suppresses MainMenu (set
        // only for closes that entered a game session).
        std::atomic<bool> m_lmPostCloseSuppressMainMenu{ false };
        std::atomic<std::int64_t> m_inLoadingScreenSinceTicks{ 0 };

        // Per-load mode-3 capture pipeline gate counters (diagnostics only).
        std::atomic<int> m_diagLoadingMenuDisplayCalls{ 0 };
        std::atomic<int> m_diagLoadingMenuDisplayVisible{ 0 };
        std::atomic<int> m_diagFirstVisibleDisplayMs{ -1 };
        std::atomic<int> m_diagDisplayMovieCaptureEligible{ 0 };
        std::atomic<int> m_diagCaptureArms{ 0 };
        std::atomic<int> m_diagCaptureBeginRejects{ 0 };
        // Split deliberately: a single "cancelled" counter could not tell
        // "the engine reported no draw" apart from "our completion rejected the
        // draw", which is exactly what made the 100%-failure capture bug
        // unfalsifiable from the log alone.
        std::atomic<int> m_diagDisplayMovieResultZero{ 0 };
        std::atomic<int> m_diagCaptureCompleteCancels{ 0 };
        // First-scrub / first-failure one-shot log gate for the submitted
        // eye-texture clear (see HookedSubmit).
        std::atomic<int> m_eyeScrubLogCount{ 0 };
        // The one-shot gate above deliberately does NOT tell success from
        // failure: it is shared by both branches, so a scrub that works once
        // and then fails for the rest of the load reads exactly like a healthy
        // load. That made the stale-pixel flash unfalsifiable from the log, so
        // both outcomes are now counted for the whole load and reported at
        // CLOSE. Same lesson as the capture counters above.
        // Largest share of the LoadingMenu surface a genuine tips delta may
        // cover. Healthy captures are 2-3%; a full repaint is 99.9%. 40% sits
        // in the enormous empty gap between the two, so it needs no tuning.
        static constexpr unsigned kMaxTipCoveragePercent = 40;
        // Post-close DisplayMenu census — own budget, see HookedDisplayMenu.
        std::atomic<int> m_diagPostCloseMenuCalls{ 0 };
        // Window arm time, so census entries carry an elapsed-since-CLOSE
        // stamp and can be placed against the art/cover release boundaries.
        std::atomic<std::int64_t> m_lmPostCloseArmedTicks{ 0 };
        // Log-only observation deadline, deliberately LONGER than the
        // suppression deadline. Bounding the census by the suppression window
        // made it structurally unable to see native menu draws that land after
        // suppression stops - which is precisely where the reported artifact
        // sits, since the cover is released at 635-965 ms.
        std::atomic<std::int64_t> m_lmPostCloseObserveUntilTicks{ 0 };
        static constexpr int kPostCloseObserveExtraMs = 2500;
        // Per-load tips rejection counters, split by reason. A single shared
        // counter could not say whether a load failed because the delta was
        // empty or because it covered the whole surface, which is exactly the
        // distinction needed to know if the retry can ever converge.
        std::atomic<int> m_diagRejectInsufficient{ 0 };
        std::atomic<int> m_diagRejectFullSurface{ 0 };
        // Non-convergence bailout: after this many CONSECUTIVE full-surface
        // rejects the load's capture is declared hopeless and no further
        // attempts are armed (see BeginLoadingMenuDeltaCapture). Distinct from
        // m_tipsCaptureComplete on purpose - hopeless publishes nothing.
        static constexpr int kTipsHopelessConsecutiveRejects = 6;
        std::atomic<int> m_tipsFullSurfaceRun{ 0 };
        std::atomic<bool> m_tipsCaptureHopeless{ false };
        // Session-total, never reset per load: the heartbeat differentiates it.
        std::atomic<int> m_eyeSubmitTotal{ 0 };
        std::atomic<int> m_eyeScrubOkCount{ 0 };
        std::atomic<int> m_eyeScrubFailCount{ 0 };
        std::atomic<unsigned> m_eyeScrubLastHr{ 0 };

        // Samples what Fallout ACTUALLY submitted, before the during-load scrub
        // can black it. The post-close fade latch uses accepted pairs only as
        // command-order evidence; this diagnostic remains the only pixel-level
        // distinction between a world frame and retained title content.
        void SampleSubmittedEye(void* a_eyeTexture, int a_tag, int a_eye);

        // IMenu::DisplayMenu hook — kills LoadingMenu's BSScaleformRenderer::DisplayMovie
        // call (the actual eye-buffer compositor for scaleform menus).
        // void __fastcall(IMenu* this, bool param0)
        using DisplayMenu_t = void (__fastcall*)(void*, bool);
        static inline DisplayMenu_t s_originalDisplayMenu = nullptr;
        static void __fastcall HookedDisplayMenu(void* iMenu, bool param0);
        bool m_displayMenuHookInstalled = false;
        std::atomic<int> m_displayMenuSkipCount{ 0 };

        // BSScaleformRenderer::DisplayMovie hook — universal scaleform compositor.
        // char __fastcall(BSScaleformRenderer* this, DisplayHandle<TreeRoot>& handle, bool param)
        using DisplayMovie_t = char (__fastcall*)(void*, void*, bool);
        static inline DisplayMovie_t s_originalDisplayMovie = nullptr;
        static char __fastcall HookedDisplayMovie(void* renderer, void* handle, bool param);
        bool m_displayMovieHookInstalled = false;
        std::atomic<int> m_displayMovieSkipCount{ 0 };
        std::atomic<int> m_displayMovieFireCount{ 0 };
        std::mutex m_vrMode3HookInstallMutex;
        std::atomic<bool> m_vrMode3HooksReady{ false };
        static inline thread_local bool s_insideLoadingMenuDisplay = false;
        static inline thread_local void* s_loadingMenuDisplayOwner = nullptr;
        // Flat draw proof snapshots the complete publication/load identity at
        // DisplayMenu entry and revalidates it after DisplayMovie returns.
        static inline thread_local std::uint64_t
            s_loadingMenuDisplaySerial = 0;
        static inline thread_local std::uint64_t
            s_loadingMenuDisplayLoadEpoch = 0;

        using SendLoadingText_t = void (__fastcall*)(void*);
        static inline SendLoadingText_t s_originalSendLoadingText = nullptr;
        static void __fastcall HookedSendLoadingText(void* loadingMenu);
        bool InstallFlatSendLoadingTextHook();
        bool m_sendLoadingTextHookInstalled = false;

        // LoadingMenu::UpdateSounds — silences the loading-model sound loops
        // while the model itself is suppressed. See the offset comment in the
        // .cpp for why the engine would otherwise play them indefinitely.
        using UpdateSounds_t = void (__fastcall*)(void*, std::uint64_t, bool);
        static inline UpdateSounds_t s_originalUpdateSounds = nullptr;
        static void __fastcall HookedUpdateSounds(
            void* loadingMenu, std::uint64_t nowMs, bool autoSpinning);
        bool m_updateSoundsHookInstalled = false;
        std::atomic<int> m_loadingSoundSuppressLogCount{ 0 };
    };
}
