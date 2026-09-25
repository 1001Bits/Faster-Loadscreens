#pragma once

#include <atomic>
#include <mutex>

namespace VRLoadingScreens
{
    class VRCompositorHelper
    {
    public:
        static bool Initialize();
        static void SuspendRendering(bool suspend);
        static void SetBlackSkybox();
        static void ClearSkybox();
        static bool IsInitialized() { return s_initialized.load(std::memory_order_acquire); }
        static bool IsVR() { return s_isVR; }
        static void* GetCompositor() { return s_compositor; }
        static void* GetD3D11Device() { return s_device.load(std::memory_order_acquire); }
        static void* GetVRSystem() { return s_vrSystem; }
        static void* GetBlackTexture() { return s_blackTexture; }
        // Publishes a renderer device and tears down every helper-owned object
        // tied to a replaced device. Returns true only for a real replacement
        // (an initial null->device publication is not a loss event).
        static bool SetDevice(REX::W32::ID3D11Device* dev);

        // DDS texture loading. Flat mode preserves BC1/BC3 compression and all
        // complete declared mips. VR normalizes the top mip to bounded,
        // overlay-safe RGBA8 on the below-normal background worker.
        // shouldDefer is checked before opening/reading the file and immediately
        // before GPU resource creation, and is also polled during VR decode.
        // Returning true aborts this attempt without beginning the next expensive
        // phase, allowing LoadingMenu to take priority without waiting for it.
        using DDSWorkShouldDefer = bool(*)(void*);
        using DDSUploadTryBegin = bool(*)(void*);
        using DDSUploadEnd = void(*)(void*);
        static void* LoadDDSTexture(const std::string& filePath,
            DDSWorkShouldDefer shouldDefer = nullptr, void* deferContext = nullptr,
            DDSUploadTryBegin tryBeginUpload = nullptr,
            DDSUploadEnd endUpload = nullptr);
        static void ReleaseTexture(void* texture);

        // IVROverlay for background image + tip text
        static bool InitializeOverlay();
        // Opens the only interval in which Show* and SetBlackSkybox may publish
        // custom loading pixels. Native CLOSE closes this latch under the same
        // mutex as every publish, so a stale render update cannot re-show art.
        static void BeginLoadingPresentation();
        static void SetImageSkybox(void* d3dTexture);
        static bool ShowBlockerOverlay();  // Immediate black overlay to hide game content
        // overlayMode: 0=HMD-relative, 1=World-locked, 2=Cinema
        static bool ShowBackgroundOverlay(void* d3dTexture, int overlayMode = 0, float alpha = 0.5f);
        // Replace only the texture/bounds of an already-visible background.
        // The accepted transform, width, alpha and visibility remain untouched.
        // forcePublish resends even an unchanged texture handle. OpenVR can
        // drop the submitted image across Fallout's synthetic self-MoveTo
        // LoadingMenu while our COM ownership remains valid.
        static bool UpdateBackgroundOverlayTexture(
            void* d3dTexture, bool forcePublish = false);
        static bool HideBackgroundOverlay();
        static bool HideBlockerOverlay();
        // Synchronously remove every custom overlay and skybox. Any failed Hide
        // is followed by Destroy and forgotten so custom pixels can never be
        // intentionally retained past Bethesda's boundary. Abort/fallback paths
        // use an instant scene-fade clear. The March-compatible normal reveal
        // also calls this once from the game thread after native rendering has
        // advanced underneath the retained artwork.
        static void EndLoadingPresentationNow(
            float a_sceneFadeSeconds = 0.0f,
            bool a_reassertSceneFade = true);
        // Seal the publication latch (no further Show*/skybox publishes) while
        // leaving the currently shown overlays untouched. First half of the
        // bounded post-close hold below.
        static void SealLoadingPresentation();
        // Bounded post-close hold: keep the already-visible custom overlays up
        // for at least a_maxMs after native CLOSE so the loading art hands off
        // into the engine's own fade instead of dropping to the native menu's
        // tail frames. Publications are sealed first; the actual removal is the
        // same EndLoadingPresentationNow() path, driven by TickPostCloseHold()
        // and superseded by the next BeginLoadingPresentation/CLOSE boundary.
        // Successful save loads additionally gate Tick on manager-owned world
        // readiness. Once authorized, the scene-fade latch proves that opaque
        // and clear commands span later accepted compositor work; it does not
        // classify the submitted pixels.
        static void BeginTimedPostCloseHold(
            int a_maxMs, bool a_requireStereo = false);
        // Out-of-line so other TUs never ODR-use the inline-static atomics
        // directly (MSVC/LTCG emits spurious C4744 alignment-metadata warnings
        // for cross-TU references to inline static atomics).
        static void SetPostCloseStereo(bool a_stereoActive);
        static bool IsPostCloseStereoGatePending();
        // Legacy render-tick driver retained for compatibility with old call
        // sites. The March production path owns its deadline in the manager and
        // never uses this accepted-pair/fade-latch mechanism.
        static bool TickPostCloseHold();
        // SteamVR's grid-room alpha, or -1 if unreadable. Non-zero at a release
        // boundary means the compositor is still showing its app-hang
        // environment and whatever we uncover will be that, not the world.
        static float CurrentGridAlpha();

        // Compositor-level SCENE blackout (IVRCompositor::FadeToColor,
        // vtable[12]). This masks only the app-submitted scene layer; VR
        // overlays composite ABOVE it and stay fully visible - the original VR
        // build held this at alpha 1.0 for entire loads with its loading art on
        // screen.
        //
        // Restored from the original/committed v1.5 path after the later 2.x
        // refactor lost its effective use. It is the reason that path never
        // showed the end-of-load stale frame, and its absence is why fixes that
        // only moved overlay timing/ordering/sort-order could not work: hiding
        // an overlay is a binary reveal, and nothing about WHEN you flip it
        // changes WHAT is underneath at the instant you do.
        static bool ApplySceneFade(bool a_log = true);
        static void ClearSceneFade(float a_seconds);
        // Legacy lifecycle query retained for diagnostic callers. Eye buffers
        // are deliberately scrubbed only during the load, never post-close.
        static bool IsPostCloseCoverActive() {
            return s_postCloseCoverDeadlineTicks.load(
                std::memory_order_acquire) != 0;
        }
        // Legacy fallback entry point. It now performs the same synchronous
        // removal as EndLoadingPresentationNow; no retained-eye handoff remains.
        static void BeginPostLoadingHandoff();
        static void CancelPostLoadingHandoff();
        // Snapshot at HookedSubmit entry, before OpenVR can block. This keeps a
        // pre-arm/in-flight Submit from being credited to a newly armed latch.
        static std::uint64_t CurrentPostCloseFadeLatchGeneration();
        // Discard pair/timing evidence without releasing the artwork. Used
        // when manager readiness is revoked; the next authorized tick issues a
        // fresh opaque command and owns a new arm serial.
        static bool InvalidatePostCloseFadeLatch();
        static void OnGameEyeSubmitResult(
            int eye, bool accepted,
            std::uint64_t a_fadeLatchGenerationAtEntry);

        // Pixel-truth diagnostic for post-close release boundaries. The fade
        // latch's submitted-pair count proves only that the opaque command was
        // separated from its clear by later accepted compositor work; manager
        // transition state remains the authority for world readiness.
        // Requested here, consumed and sampled in the Submit hook before the
        // scrub runs. Single-slot: a request that is never consumed (the game
        // stopped submitting) is overwritten by the next one and costs nothing.
        // Overlay sort orders. Relative order (blocker behind bg behind tips)
        // is what our own compositing needs; the absolute magnitude is what
        // keeps the game's own title-screen overlays from drawing over the
        // black cover once the art is gone. Keep all three far above any
        // plausible game value.
        static constexpr std::uint32_t kBlockerSortOrder = 10000;
        static constexpr std::uint32_t kBgSortOrder = 10010;
        static constexpr std::uint32_t kTipsSortOrder = 10020;

        static constexpr int kEyeSampleArtRelease = 1;
        static constexpr int kEyeSampleCoverRelease = 2;
        // Frames sampled from the reveal onward. Covers a 1-3 frame artifact
        // plus the settled frame to compare it against.
        static constexpr int kEyeSampleBurstFrames = 5;
        // How far native-menu suppression is carried PAST the cover release.
        // The engine's LoadingMenu fade-out is what draws Fallout's own grid
        // backdrop; this keeps it suppressed until well after the cover is gone.
        static constexpr int kPostCoverReleaseSuppressMarginMs = 500;
        static void RequestEyeSample(int a_tag);
        static int ConsumeEyeSampleRequest();
        // Non-consuming read, for the LEFT eye: the right eye completes the
        // pair and consumes, so both halves of the same frame carry the same
        // tag and can be compared against each other.
        static int PeekEyeSampleRequest();
        static void UpdateBackgroundOverlay(); // per-frame world-lock update
        // Re-sample only the pose. Unlike ShowBackgroundOverlay this never
        // replaces/hides an already accepted texture when the new pose fails.
        static bool RelockBackgroundOverlayTransform();

        // Tips overlay — world-locked fallback on top of bg. Mode 3 supplies only
        // an owned, exact LoadingMenu before/after delta; submitted eye textures
        // are never accepted as tip content.
        static bool ShowTipsOverlay(void* d3dTexture, bool stereoSideBySide);
        static bool HideTipsOverlay();
        static bool UpdateTipsOverlayTransform();  // re-apply bg overlay's world pose
        static bool IsTipsOverlayActive() { return s_tipsOverlayActive.load(std::memory_order_acquire); }
        static bool IsBackgroundOverlayActive() { return s_bgOverlayActive.load(std::memory_order_acquire); }
        static void UpdateLastKnownPose();    // cache HMD pose each frame (before loading)
        static bool GetCurrentPose(float outPose[3][4]); // get freshest HMD pose for Submit
        static bool IsOverlayInitialized() { return s_overlayInitialized.load(std::memory_order_acquire); }

        // MCM-configurable overlay settings
        static void SetBackgroundWidth(float width)
        {
            s_bgWidthSetting.store(width, std::memory_order_release);
        }

        // ----- World-locked in-eye background (Option B) -----
        // Capture anchor HMD orientation (call when LoadingMenu opens). Caches
        // per-eye projection raw + eye-to-head transforms on first call.
        static void CaptureWorldLockAnchor();
        static void ClearWorldLockAnchor()
        {
            std::lock_guard poseLock(s_poseMutex);
            s_worldLockAnchorValid.store(false, std::memory_order_release);
        }
        static bool HasWorldLockAnchor()
        {
            return s_worldLockAnchorValid.load(std::memory_order_acquire);
        }

        // Raw anchor HMD pose (device-to-world matrix at capture time).
        // Written to outPose[3][4] if the anchor is valid; returns true on
        // success for callers that need the captured transform.
        static bool GetWorldLockAnchorPose(float outPose[3][4]);

        // Per-frame world-lock state for one eye, fed to the in-eye shader.
        struct WorldLockEyeData
        {
            float compositeRot[9];   // 3x3 row-major: eye-space → anchor-space
            float projLeft;          // tan half-angle (negative — see openvr GetProjectionRaw)
            float projRight;         // tan half-angle (positive)
            float projTop;           // tan half-angle (negative — y-up)
            float projBottom;        // tan half-angle (positive)
        };
        // Compute composite = inv(anchorRot) * currentRot * eyeToHeadRot for the given eye
        // and return per-eye projection params. Returns false if anchor not set.
        static bool GetWorldLockData(int eye, WorldLockEyeData& outData);

    private:
        // OpenVR Texture_t (matches openvr.h layout)
        struct VRTexture
        {
            void* handle;
            int   eType;       // 0 = TextureType_DirectX
            int   eColorSpace; // 1 = ColorSpace_Gamma
        };

        // OpenVR HmdMatrix34_t
        struct HmdMatrix34
        {
            float m[3][4];
        };

        // OpenVR TrackedDevicePose_t (matches openvr.h layout)
        struct TrackedDevicePose
        {
            HmdMatrix34 mDeviceToAbsoluteTracking;
            float vVelocity[3];
            float vAngularVelocity[3];
            int eTrackingResult;
            bool bPoseIsValid;
            bool bDeviceIsConnected;
        };

        // IVRCompositor vtable function pointer types (x64)
        using GetTrackingSpaceFn = int(*)(void*);
        using GetLastPosesFn = int(*)(void*, TrackedDevicePose*, std::uint32_t, TrackedDevicePose*, std::uint32_t);
        using SuspendRenderingFn = void(*)(void*, bool);
        using SetSkyboxOverrideFn = int(*)(void*, const VRTexture*, unsigned int);
        using ClearSkyboxOverrideFn = void(*)(void*);
        // IVRCompositor::GetCurrentGridAlpha, vtable[15]. Read-only.
        using GetGridAlphaFn = float(*)(void*);
        // IVRCompositor::FadeToColor, vtable[12]. Signature taken verbatim from
        // the original VR release (924a5f79 src/VRCompositorHelper.h:71).
        using FadeToColorFn =
            void(*)(void*, float, float, float, float, float, bool);

        // IVRSystem vtable function pointer type (x64)
        // void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float, TrackedDevicePose*, uint32_t)
        using GetDeviceToAbsTrackingPoseFn = void(*)(void*, int, float, TrackedDevicePose*, std::uint32_t);

        // IVRSystem vtable[2]: GetProjectionRaw(EVREye, float*, float*, float*, float*)
        using GetProjectionRawFn = void(*)(void*, int, float*, float*, float*, float*);

        // IVRSystem vtable[4]: HmdMatrix34_t GetEyeToHeadTransform(EVREye)
        // x64 MSVC ABI: struct >16 bytes returned via hidden first arg.
        // Effective signature: void(HmdMatrix34* retPtr, void* this, int eye)
        using GetEyeToHeadTransformFn = void(*)(HmdMatrix34*, void*, int);

        // IVROverlay_018 vtable function pointer types (x64)
        using OVR_CreateOverlayFn = int(*)(void*, const char*, const char*, std::uint64_t*);
        using OVR_DestroyOverlayFn = int(*)(void*, std::uint64_t);
        using OVR_SetAlphaFn = int(*)(void*, std::uint64_t, float);
        using OVR_SetSortOrderFn = int(*)(void*, std::uint64_t, std::uint32_t);
        using OVR_SetWidthFn = int(*)(void*, std::uint64_t, float);
        using OVR_SetTransformAbsFn = int(*)(void*, std::uint64_t, int, const HmdMatrix34*);
        using OVR_SetTransformDevRelFn = int(*)(void*, std::uint64_t, std::uint32_t, const HmdMatrix34*);
        using OVR_ShowFn = int(*)(void*, std::uint64_t);
        using OVR_HideFn = int(*)(void*, std::uint64_t);
        using OVR_SetTextureFn = int(*)(void*, std::uint64_t, const VRTexture*);
        using OVR_SetTextureBoundsFn = int(*)(void*, std::uint64_t, const float*);  // float[4]: uMin,vMin,uMax,vMax
        using OVR_SetOverlayFlagFn = int(*)(void*, std::uint64_t, int, bool);       // (handle, VROverlayFlags, enabled)
        using OVR_SetTexelAspectFn = int(*)(void*, std::uint64_t, float);           // (handle, aspect) — width/height ratio per texel

        // HMD pose helper (prefers IVRSystem non-blocking, falls back to IVRCompositor)
        static bool GetHMDPose(HmdMatrix34& outPose);
        static bool CreateBlackTextureLocked();
        static void DestroyOverlayHandlesLocked(bool destroy);

        // Runtime detection
        static inline bool s_isVR = false;

        // Compositor state
        static inline std::atomic<bool> s_initialized{ false };
        static inline std::mutex s_initializeMutex;
        static inline void* s_compositor = nullptr;
        static inline GetTrackingSpaceFn s_getTrackingSpace = nullptr;
        static inline GetLastPosesFn s_getLastPoses = nullptr;
        static inline SuspendRenderingFn s_suspendRendering = nullptr;
        static inline SetSkyboxOverrideFn s_setSkyboxOverride = nullptr;
        static inline ClearSkyboxOverrideFn s_clearSkyboxOverride = nullptr;
        static inline GetGridAlphaFn s_getGridAlpha = nullptr;
        static inline FadeToColorFn s_fadeToColor = nullptr;
        // Set only by ApplySceneFade, cleared only by ClearSceneFade. Its one
        // job is to guarantee the scene can never be left permanently black.
        static inline std::atomic<bool> s_sceneFadeHeld{ false };
        // The original used an instant clear because by then its eye buffers
        // held live world frames. A short ramp additionally multiplies any
        // residual stale frame toward zero instead of switching it on - the one
        // thing no overlay hide can ever do, because hiding an overlay is a
        // binary reveal of whatever is underneath.
        static constexpr float kSceneFadeUpSeconds = 0.35f;
        // Deferred skybox restore. EndLoadingPresentationNow used to hide the
        // blocker AND clear the skybox override in the same instant, i.e. two
        // visual state flips at the exact reveal boundary. The skybox is only
        // ever visible while the app is not submitting, so restoring it later
        // costs nothing and removes one variable from that boundary.
        static inline std::atomic<std::int64_t> s_skyboxClearAtTicks{ 0 };
        static constexpr int kSkyboxClearDelayMs = 3000;

        // IVRSystem state (for non-blocking pose queries)
        // NOTE: we deliberately do NOT cache s_getProjectionRaw / s_getEyeToHeadTransform
        // as persistent members — those are pulled once at Initialize() via local
        // variables to avoid conflicts with other plugins that hook the IVRSystem
        // vtable (e.g. Heisenberg/HIGGS).
        static inline void* s_vrSystem = nullptr;
        static inline GetDeviceToAbsTrackingPoseFn s_getDeviceToAbsTrackingPose = nullptr;

        // World-lock anchor state (captured at LoadingMenu open)
        static inline std::atomic<bool> s_worldLockAnchorValid{ false };
        static inline HmdMatrix34 s_anchorPose = {};   // raw HMD pose at capture
        static inline float s_anchorRotInv[9] = {};   // inv(anchor HMD rotation), row-major 3x3
        static inline float s_eyeToHeadRot[2][9] = {}; // per-eye eye→head rotation (cached)
        static inline float s_eyeProjLeft[2]  = {};    // per-eye projection raw: left tangent
        static inline float s_eyeProjRight[2] = {};    // per-eye projection raw: right tangent
        static inline float s_eyeProjTop[2]   = {};    // per-eye projection raw: top tangent
        static inline float s_eyeProjBottom[2] = {};   // per-eye projection raw: bottom tangent
        static inline std::atomic<bool> s_eyeDataCached{ false }; // projection + eye-to-head cached on first anchor
        static inline std::mutex s_poseMutex;           // anchor, projections, and cached HMD poses

        // D3D11 device (stored during Initialize for texture creation)
        // The renderer owns the device. Atomic publication makes the lazy flat-mode
        // handoff race-free; LoadDDSTexture takes a temporary COM reference before
        // doing asynchronous file I/O / resource creation.
        static inline std::atomic<REX::W32::ID3D11Device*> s_device{ nullptr };
        static inline std::mutex s_deviceMutex;

        // Black D3D11 texture for skybox fallback
        static inline void* s_blackTexture = nullptr;
        // Magenta twin of s_blackTexture, published on the blocker only while
        // the diagnostic probe is enabled. Makes "is the cover on screen"
        // answerable by eye, which black never can be.
        static inline void* s_debugCoverTexture = nullptr;

        // IVROverlay state
        static inline std::atomic<bool> s_overlayInitialized{ false };
        static inline std::recursive_mutex s_overlayMutex;
        static inline bool s_loadingPresentationOpen = false; // under s_overlayMutex
        // Every BeginLoadingPresentation and timed CLOSE arm owns a distinct
        // epoch. TickPostCloseHold captures the epoch armed by CLOSE and
        // verifies it again under s_overlayMutex before teardown, so an old
        // release tick can never hide a newer/re-armed presentation.
        static inline std::uint64_t s_presentationGeneration = 0; // under mutex
        static inline std::atomic<std::uint64_t>
            s_postCloseHoldGeneration{ 0 };
        static inline void* s_overlay = nullptr;
        static inline std::uint64_t s_bgOverlayHandle = 0;
        static inline std::uint64_t s_blockerOverlayHandle = 0;
        static inline std::uint64_t s_tipsOverlayHandle = 0;
        static inline std::atomic<bool> s_blockerOverlayActive{ false };
        static inline std::atomic<bool> s_handoffPending{ false };
        static inline std::atomic<bool> s_handoffSawLeft{ false };
        static inline std::atomic<int> s_handoffCompletePairs{ 0 };
        // steady_clock deadline (ticks) for the bounded post-close hold;
        // 0 = no hold pending. Cleared by every presentation boundary
        // (BeginLoadingPresentation, EndLoadingPresentationNow, Cancel).
        static inline std::atomic<std::int64_t> s_postCloseHoldDeadlineTicks{ 0 };
        static inline std::atomic<std::int64_t> s_postCloseCoverDeadlineTicks{ 0 };
        // Two-phase scene-fade latch. The atomic generation is sampled at
        // HookedSubmit entry; all pairing state is protected by its own mutex
        // so an old in-flight callback cannot increment a newly reset counter.
        // Boundary code takes overlay -> fadeLatch; Submit only takes fadeLatch.
        static inline std::mutex s_postCloseFadeLatchMutex;
        static inline std::uint64_t s_postCloseFadeLatchArmSerial = 0;
        static inline std::atomic<std::uint64_t>
            s_postCloseFadeLatchGeneration{ 0 };
        static inline std::uint64_t s_postCloseFadeLatchStateGeneration = 0;
        static inline std::uint64_t s_postCloseFadeLatchHoldGeneration = 0;
        static inline std::int64_t s_postCloseFadeLatchAtTicks = 0;
        static inline bool s_postCloseFadeLatchSawLeft = false;
        static inline int s_postCloseFadeLatchPairs = 0;
        static void ResetPostCloseFadeLatch();
        // Pending pre-scrub eye samples (see RequestEyeSample). A burst, not a
        // single shot: the first attempt sampled one frame and landed 41 ms
        // (roughly four frames) after the release, because the request is only
        // consumed on the next right-eye submit. A flash is one to three
        // frames, so a single late sample can report the settled world and
        // miss the artifact entirely.
        static inline std::atomic<int> s_eyeSampleRequest{ 0 };
        static inline std::atomic<int> s_eyeSampleRemaining{ 0 };
        static constexpr int kPostCloseCoverMaxMs = 1000;
        // Stereo gate: both stages additionally wait for the engine to resume
        // stereo rendering, bounded by an absolute backstop so nothing can ever
        // stick (also cleared by every presentation boundary).
        static inline std::atomic<bool> s_postCloseRequireStereo{ false };
        static inline std::atomic<bool> s_postCloseStereoActive{ false };
        static inline std::atomic<std::int64_t> s_postCloseStereoBackstopTicks{ 0 };
        // Bounds the case where the stereo signal never arrives. Kept short on
        // purpose: field logs show the signal currently NEVER fires, so this
        // backstop is the effective hold length, and a long one reproduces the
        // "background outstays the load" complaint. Until the signal is
        // identified this must stay close to the plain art hold.
        static constexpr int kPostCloseStereoBackstopMs = 600;
        // Displayed-texture ownership (both under s_overlayMutex). OpenVR takes
        // no reference on a texture handed to SetOverlayTexture, and the
        // post-close hold keeps an overlay visible after the manager has already
        // retired its own copy, so the helper holds one reference on whatever it
        // has published until that overlay is actually hidden.
        static inline void* s_bgShownTexture = nullptr;
        static inline void* s_tipsShownTexture = nullptr;
        static void RetainShownTexture(void*& slot, void* texture);
        // Legacy diagnostic helper from the retired blocker stage.
        static void ReassertBlockerCoverLocked();
        static inline std::atomic<bool> s_tipsOverlayActive{ false };

        // World-locked overlay state
        static inline std::atomic<bool> s_bgOverlayActive{ false };
        static inline std::atomic<bool> s_bgWorldPoseValid{ false };
        static inline std::atomic<int> s_overlayMode{ 0 };
        static inline int s_updateLogCounter = 0;
        static inline HmdMatrix34 s_bgWorldPose = {};       // background world-space pose

        // Cached pre-loading HMD pose (updated every frame, used when loading starts)
        static inline HmdMatrix34 s_lastKnownPose = {};
        static inline std::atomic<bool> s_hasLastKnownPose{ false };

        // MCM-configurable overlay settings
        static inline std::atomic<float> s_bgWidthSetting{ 10.0f };

        // World-lock mono-projection approximations (used by CaptureWorldLockAnchor
        // as defaults when skipping IVRSystem vtable[2]/[4] calls for plugin compat).
        // ~1.19 ≈ tan(50°) for ~100° horizontal FOV typical of consumer HMDs.
        static inline float s_bgProjTanHoriz = 1.19f;
        static inline float s_bgProjTanVert  = 1.19f;

        // IVROverlay_018 function pointers
        static inline OVR_CreateOverlayFn s_ovrCreateOverlay = nullptr;
        static inline OVR_DestroyOverlayFn s_ovrDestroyOverlay = nullptr;
        static inline OVR_SetAlphaFn s_ovrSetAlpha = nullptr;
        static inline OVR_SetSortOrderFn s_ovrSetSortOrder = nullptr;
        static inline OVR_SetWidthFn s_ovrSetWidth = nullptr;
        static inline OVR_SetTransformAbsFn s_ovrSetTransformAbs = nullptr;
        static inline OVR_SetTransformDevRelFn s_ovrSetTransformDevRel = nullptr;
        static inline OVR_ShowFn s_ovrShow = nullptr;
        static inline OVR_HideFn s_ovrHide = nullptr;
        static inline OVR_SetTextureFn s_ovrSetTexture = nullptr;
        static inline OVR_SetTextureBoundsFn s_ovrSetTextureBounds = nullptr;
        static inline OVR_SetOverlayFlagFn s_ovrSetOverlayFlag = nullptr;
        static inline OVR_SetTexelAspectFn s_ovrSetTexelAspect = nullptr;
    };
}
