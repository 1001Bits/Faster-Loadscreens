#include "PCH.h"
#include "VRCompositorHelper.h"
#include "D3D11Compositor.h"
#include "DDSOverlayCodec.h"
#include "LoadingScreenManager.h"
#include "RuntimePolicy.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

#include <d3d11.h>

namespace VRLoadingScreens
{
    static constexpr std::uintptr_t BSGraphics_RendererData_Offset_VR = 0x060f3ce8;
    static constexpr float kVRLoadingScreenAspect = 16.0f / 9.0f;
    // IVROverlay_018 uses the historical ordinal enum, not the bit-mask values
    // introduced by newer OpenVR interfaces.
    static constexpr int kOverlayFlagSideBySideParallel018 = 10;

    // Forward declarations — defined further down (Option B helpers).
    static void HmdRotToArr(const float m[3][4], float out[9]);
    static void MatTranspose3(const float in[9], float out[9]);
    static void MatMul3(const float A[9], const float B[9], float out[9]);

    static constexpr const char* OverlayErrorName(int error)
    {
        switch (error) {
        case 0: return "None";
        case 10: return "UnknownOverlay";
        case 11: return "InvalidHandle";
        case 12: return "PermissionDenied";
        case 13: return "OverlayLimitExceeded";
        case 18: return "WrongTransformType";
        case 19: return "InvalidTrackedDevice";
        case 20: return "InvalidParameter";
        case 23: return "RequestFailed";
        case 24: return "InvalidTexture";
        case 31: return "TextureAlreadyLocked";
        case 32: return "TextureLockCapacityReached";
        case 33: return "TextureNotLocked";
        case 34: return "TimedOut";
        default: return "UnknownError";
        }
    }

    bool VRCompositorHelper::CreateBlackTextureLocked()
    {
        std::lock_guard deviceLock(s_deviceMutex);
        if (s_blackTexture) return true;
        auto* device = s_device.load(std::memory_order_acquire);
        if (!device) return false;

        REX::W32::D3D11_TEXTURE2D_DESC desc = {};
        desc.width = 1;
        desc.height = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.sampleDesc.count = 1;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;

        std::uint32_t blackPixel = 0xFF000000;
        REX::W32::D3D11_SUBRESOURCE_DATA initData = {};
        initData.sysMem = &blackPixel;
        initData.sysMemPitch = 4;

        REX::W32::ID3D11Texture2D* texture = nullptr;
        const HRESULT hr =
            device->CreateTexture2D(&desc, &initData, &texture);
        if (FAILED(hr) || !texture) {
            logger::warn(
                "VRCompositorHelper: black texture creation failed (hr={:08X})",
                static_cast<std::uint32_t>(hr));
            return false;
        }
        s_blackTexture = texture;

        // Diagnostic twin: identical in every way except colour. The whole
        // question that repeated log analysis could not settle is whether the
        // black cover is on screen at all during stage 1. Black is
        // indistinguishable from "nothing rendered", from a faded compositor,
        // and from a dark game frame, so its presence is unfalsifiable by
        // eye. A saturated colour is not. Only ever published when the probe
        // flag is on.
        std::uint32_t magentaPixel = 0xFFFF00FF;  // ABGR: opaque magenta
        initData.sysMem = &magentaPixel;
        REX::W32::ID3D11Texture2D* debugTexture = nullptr;
        if (SUCCEEDED(
                device->CreateTexture2D(&desc, &initData, &debugTexture)) &&
            debugTexture) {
            s_debugCoverTexture = debugTexture;
        }
        return true;
    }

    void VRCompositorHelper::DestroyOverlayHandlesLocked(bool destroy)
    {
        const auto clearHandle =
            [destroy](std::uint64_t& handle, std::atomic<bool>& active) {
                if (handle && s_overlay) {
                    if (active.load(std::memory_order_acquire) && s_ovrHide) {
                        const int hideError = s_ovrHide(s_overlay, handle);
                        if (hideError != 0) {
                            logger::warn(
                                "VROverlay: fail-open hide returned error {} "
                                "for handle {}",
                                hideError, handle);
                        }
                    }
                    if (destroy && s_ovrDestroyOverlay) {
                        const int destroyError =
                            s_ovrDestroyOverlay(s_overlay, handle);
                        if (destroyError != 0) {
                            logger::warn(
                                "VROverlay: fail-open destroy returned error {} "
                                "for handle {}",
                                destroyError, handle);
                        }
                    }
                }
                active.store(false, std::memory_order_release);
                if (destroy) handle = 0;
            };

        clearHandle(s_tipsOverlayHandle, s_tipsOverlayActive);
        clearHandle(s_bgOverlayHandle, s_bgOverlayActive);
        clearHandle(s_blockerOverlayHandle, s_blockerOverlayActive);
        s_bgWorldPoseValid.store(false, std::memory_order_release);
        s_handoffPending.store(false, std::memory_order_release);
        s_handoffSawLeft.store(false, std::memory_order_release);
        s_handoffCompletePairs.store(0, std::memory_order_release);
        if (destroy) {
            s_overlayInitialized.store(false, std::memory_order_release);
        }
    }

    bool VRCompositorHelper::SetDevice(REX::W32::ID3D11Device* dev)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!dev) {
            s_loadingPresentationOpen = false;
        }

        REX::W32::ID3D11Device* previous = nullptr;
        {
            // LoadDDSTexture takes its temporary COM reference under this same
            // mutex, so it can never AddRef a pointer concurrently retired here.
            std::lock_guard deviceLock(s_deviceMutex);
            previous = s_device.load(std::memory_order_acquire);
            if (previous == dev) {
                if (dev && !s_blackTexture) {
                    // Create outside this nested lock below.
                } else {
                    return false;
                }
            }
            s_device.store(dev, std::memory_order_release);
        }

        const bool replacement = previous && previous != dev;
        if (replacement) {
            s_loadingPresentationOpen = false;
        }
        if (replacement || (!dev && previous)) {
            // Device loss is an authoritative visual boundary. No old hold or
            // fade arm may survive into resources created for another device.
            s_postCloseHoldGeneration.store(0, std::memory_order_release);
            s_postCloseHoldDeadlineTicks.store(0, std::memory_order_release);
            s_postCloseCoverDeadlineTicks.store(0, std::memory_order_release);
            ResetPostCloseFadeLatch();
            s_postCloseRequireStereo.store(false, std::memory_order_release);
            s_postCloseStereoActive.store(false, std::memory_order_release);
            s_postCloseStereoBackstopTicks.store(0, std::memory_order_release);
            if (s_sceneFadeHeld.load(std::memory_order_acquire)) {
                ClearSceneFade(0.0f);
            }
            if (s_clearSkyboxOverride && s_compositor) {
                s_clearSkyboxOverride(s_compositor);
            }
            DestroyOverlayHandlesLocked(true);
            if (s_blackTexture) {
                static_cast<REX::W32::ID3D11Texture2D*>(
                    s_blackTexture)->Release();
                s_blackTexture = nullptr;
            }
            if (s_debugCoverTexture) {
                static_cast<REX::W32::ID3D11Texture2D*>(
                    s_debugCoverTexture)->Release();
                s_debugCoverTexture = nullptr;
            }
        }

        if (dev) {
            CreateBlackTextureLocked();
        }
        s_initialized.store(
            dev && (!s_isVR || s_compositor),
            std::memory_order_release);
        return replacement;
    }

    bool VRCompositorHelper::Initialize()
    {
        std::lock_guard initLock(s_initializeMutex);
        if (s_initialized.load(std::memory_order_acquire)) return true;

        s_isVR = REL::Module::IsVR();

        // --- D3D11 device (needed for both VR and flat) ---
        if (s_isVR) {
            REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset_VR) };
            void* rendererData = *rendererDataPtr;
            if (rendererData) {
                SetDevice(*reinterpret_cast<REX::W32::ID3D11Device**>(
                    reinterpret_cast<std::uintptr_t>(rendererData) + 0x48));
            }
        } else {
            // Flat mode: device will be acquired lazily via Present hook
            // (RendererData address library IDs may not work for all NG versions)
            logger::info("VRCompositorHelper: flat mode — device deferred to Present hook");
        }

        CreateBlackTextureLocked();

        // --- VR-only: OpenVR compositor and system ---
        if (s_isVR) {
            HMODULE openvrDll = GetModuleHandleA("openvr_api.dll");
            if (!openvrDll) {
                logger::warn("VRCompositorHelper: openvr_api.dll not loaded");
                s_initialized.store(false, std::memory_order_release);
                return false;
            }

            using VR_GetGenericInterfaceFn = void*(*)(const char*, int*);
            auto VR_GetGenericInterface = reinterpret_cast<VR_GetGenericInterfaceFn>(
                GetProcAddress(openvrDll, "VR_GetGenericInterface"));
            if (!VR_GetGenericInterface) {
                s_initialized.store(false, std::memory_order_release);
                return false;
            }

            // Acquire and hook the compositor exactly once. Full initialization
            // may be retried while D3D is still unavailable; re-reading our own
            // detours from the vtable as originals would recurse indefinitely.
            if (!s_compositor) {
                int error = 0;
                s_compositor = VR_GetGenericInterface("IVRCompositor_022", &error);
                if (!s_compositor) {
                    s_compositor = VR_GetGenericInterface("IVRCompositor_021", &error);
                }
                if (!s_compositor) {
                    logger::error("VRCompositorHelper: could not get IVRCompositor (error {})", error);
                    s_initialized.store(false, std::memory_order_release);
                    return false;
                }

                void** vtable = *reinterpret_cast<void***>(s_compositor);
                s_getTrackingSpace = reinterpret_cast<GetTrackingSpaceFn>(vtable[1]);
                s_getLastPoses = reinterpret_cast<GetLastPosesFn>(vtable[3]);
                s_setSkyboxOverride = reinterpret_cast<SetSkyboxOverrideFn>(vtable[16]);
                s_clearSkyboxOverride = reinterpret_cast<ClearSkyboxOverrideFn>(vtable[17]);
                s_suspendRendering = reinterpret_cast<SuspendRenderingFn>(vtable[32]);
                // GetCurrentGridAlpha is bound READ-ONLY. SteamVR fades its own
                // grid room in when a scene application stops submitting - it
                // has a developer setting named "Do not fade to grid when app
                // hangs" - and this mod's animation-loop NOP makes the game
                // submit ZERO frames for an entire load (heartbeat-measured:
                // 28 consecutive samples at eyePairs/s=0.0 across 56 s). The
                // grid is a normal alpha-blended layer, and SetSkyboxOverride
                // does NOT remove it: per Valve, changing the skybox swaps the
                // room but "the grid remains". So the black skybox only darkens
                // the backdrop behind the grid lines. Reading the alpha at the
                // release boundaries is what turns that from theory into fact.
                // FadeGrid (14) is deliberately NOT bound: whether an app-side
                // call overrides the compositor's own hang fade is unverified,
                // and Bethesda's fade calls stay untouched.
                s_getGridAlpha = reinterpret_cast<GetGridAlphaFn>(vtable[15]);
                // FadeToColor[12]. Same interface this build already reads
                // 15/16/17/32 from, and the index the original bound.
                s_fadeToColor = reinterpret_cast<FadeToColorFn>(vtable[12]);
                logger::info(
                    "VRCompositorHelper: OpenVR compositor fns bound "
                    "(gridAlpha={}, sceneFade={})",
                    s_getGridAlpha != nullptr, s_fadeToColor != nullptr);
            }

            if (!s_vrSystem) {
                int sysError = 0;
                s_vrSystem = VR_GetGenericInterface("IVRSystem_019", &sysError);
                if (!s_vrSystem) {
                    s_vrSystem = VR_GetGenericInterface("IVRSystem_020", &sysError);
                }
            }
            if (s_vrSystem) {
                void** sysVtable = *reinterpret_cast<void***>(s_vrSystem);
                s_getDeviceToAbsTrackingPose = reinterpret_cast<GetDeviceToAbsTrackingPoseFn>(sysVtable[11]);

                // NOTE: We deliberately do NOT call sysVtable[2] (GetProjectionRaw) or
                // sysVtable[4] (GetEyeToHeadTransform) here. In testing, those calls
                // corrupted the IVRSystem singleton's vtable pointer in vrclient_x64
                // (value ended up as 1.0f bit-pattern 0x3F800000) — reliably crashing
                // other VR plugins (FO4VRTools, Heisenberg/HIGGS, VirtualHolsters)
                // that hold the same IVRSystem pointer. Root cause unconfirmed; may
                // be an OpenVR / vrclient_x64 interaction triggered by calls from a
                // non-game-module client. Option B's world-lock path falls back to
                // the non-world-locked shader when s_eyeDataCached is false.
                s_eyeDataCached.store(false, std::memory_order_release);
                logger::info("VRCompositorHelper: IVRSystem initialized "
                             "(eye data capture disabled for plugin-compat safety)");
            }
            if (!s_device.load(std::memory_order_acquire)) {
                logger::info("VRCompositorHelper: OpenVR ready; renderer device initialization deferred");
            }
        }

        const bool complete = s_device.load(std::memory_order_acquire) != nullptr &&
            (!s_isVR || s_compositor != nullptr);
        s_initialized.store(complete, std::memory_order_release);
        logger::info("VRCompositorHelper: initialized (VR={}, device={}, compositor={})",
            s_isVR, s_device.load(std::memory_order_acquire) != nullptr, s_compositor != nullptr);
        return complete;
    }

    // ========================================================================
    // IVROverlay initialization and management
    // ========================================================================

    bool VRCompositorHelper::InitializeOverlay()
    {
        std::lock_guard initLock(s_initializeMutex);
        std::lock_guard overlayLock(s_overlayMutex);
        if (s_overlayInitialized.load(std::memory_order_acquire) &&
            s_bgOverlayHandle && s_blockerOverlayHandle) {
            // Tips are optional because the compositor can bake them into the
            // background. Retry a prior optional creation failure without
            // disturbing the already-valid mandatory stack.
            if (!s_tipsOverlayHandle && s_overlay && s_ovrCreateOverlay) {
                const int tipsError = s_ovrCreateOverlay(
                    s_overlay, "vrloadingscreens.tips", "VR Loading Tips",
                    &s_tipsOverlayHandle);
                if (tipsError == 0) {
                    s_ovrSetAlpha(
                        s_overlay, s_tipsOverlayHandle, 1.0f);
                    s_ovrSetSortOrder(
                        s_overlay, s_tipsOverlayHandle, 210);
                    logger::info(
                        "VROverlay: optional tips overlay recovered "
                        "(handle={})",
                        s_tipsOverlayHandle);
                }
            }
            return true;
        }
        if (!s_device.load(std::memory_order_acquire)) {
            logger::warn("VROverlay: no D3D11 device");
            return false;
        }
        if (!CreateBlackTextureLocked()) {
            logger::warn("VROverlay: mandatory blocker texture unavailable");
            return false;
        }

        // A previous partial attempt may have created one or two handles.
        // Destroy them before retrying so readiness is all-or-nothing for the
        // mandatory background+blocker pair.
        if (s_bgOverlayHandle || s_blockerOverlayHandle ||
            s_tipsOverlayHandle) {
            DestroyOverlayHandlesLocked(true);
        }

        HMODULE openvrDll = GetModuleHandleA("openvr_api.dll");
        if (!openvrDll) return false;

        using VR_GetGenericInterfaceFn = void*(*)(const char*, int*);
        auto VR_GetGenericInterface = reinterpret_cast<VR_GetGenericInterfaceFn>(
            GetProcAddress(openvrDll, "VR_GetGenericInterface"));
        if (!VR_GetGenericInterface) return false;

        // Request IVROverlay_018 specifically — our vtable indices match this version
        int error = 0;
        s_overlay = VR_GetGenericInterface("IVROverlay_018", &error);
        if (!s_overlay) {
            logger::warn("VROverlay: IVROverlay_018 not available (error {})", error);
            return false;
        }
        logger::info("VROverlay: got IVROverlay_018 interface");

        // IVROverlay_018 vtable indices
        void** vtable = *reinterpret_cast<void***>(s_overlay);
        s_ovrCreateOverlay    = reinterpret_cast<OVR_CreateOverlayFn>(vtable[1]);
        s_ovrDestroyOverlay   = reinterpret_cast<OVR_DestroyOverlayFn>(vtable[2]);
        s_ovrSetAlpha         = reinterpret_cast<OVR_SetAlphaFn>(vtable[16]);
        s_ovrSetSortOrder     = reinterpret_cast<OVR_SetSortOrderFn>(vtable[20]);
        s_ovrSetWidth         = reinterpret_cast<OVR_SetWidthFn>(vtable[22]);
        s_ovrSetTransformAbs    = reinterpret_cast<OVR_SetTransformAbsFn>(vtable[33]);
        s_ovrSetTransformDevRel = reinterpret_cast<OVR_SetTransformDevRelFn>(vtable[35]);
        s_ovrShow             = reinterpret_cast<OVR_ShowFn>(vtable[41]);
        s_ovrHide             = reinterpret_cast<OVR_HideFn>(vtable[42]);
        s_ovrSetTexture       = reinterpret_cast<OVR_SetTextureFn>(vtable[58]);
        s_ovrSetTextureBounds = reinterpret_cast<OVR_SetTextureBoundsFn>(vtable[28]);
        s_ovrSetOverlayFlag   = reinterpret_cast<OVR_SetOverlayFlagFn>(vtable[12]);
        s_ovrSetTexelAspect   = reinterpret_cast<OVR_SetTexelAspectFn>(vtable[18]);

        // Create background overlay
        int err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.bg", "VR Loading BG", &s_bgOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (bg) failed (error {})", err);
            return false;
        }

        s_ovrSetWidth(s_overlay, s_bgOverlayHandle, 5.0f);
        s_ovrSetAlpha(s_overlay, s_bgOverlayHandle, 1.0f);
        s_ovrSetSortOrder(s_overlay, s_bgOverlayHandle, kBgSortOrder);
        logger::info("VROverlay: background overlay created (handle={})", s_bgOverlayHandle);

        // Create black blocker overlay (covers full FOV to hide game's stereo frames)
        err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.blocker", "VR Loading Blocker", &s_blockerOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (blocker) failed (error {})", err);
            DestroyOverlayHandlesLocked(true);
            return false;
        } else {
            s_ovrSetWidth(s_overlay, s_blockerOverlayHandle, 30.0f);
            s_ovrSetAlpha(s_overlay, s_blockerOverlayHandle, 1.0f);
            // Sort order must beat every game-created overlay: the FO4VR title
            // screen submits its own UI overlays, and they peek through a low
            // blocker. 150 was NOT high enough — it only ever looked correct
            // because bg (200) and tips (210) sat in front of the game's
            // overlays for most of a load. The moment stage 1 of the post-close
            // hold drops the art, the blocker becomes the sole cover, and any
            // game overlay sorted above 150 draws over it. That is exactly the
            // art-release-to-cover-release window where the title screen was
            // reported, with no black phase, because the cover was never on
            // top. The whole stack is now lifted far above any plausible game
            // value while keeping our internal order blocker < bg < tips.
            s_ovrSetSortOrder(
                s_overlay, s_blockerOverlayHandle, kBlockerSortOrder);
            logger::info("VROverlay: blocker overlay created (handle={})", s_blockerOverlayHandle);
        }

        // Create the optional tips fallback overlay. Active mode 3 submits only
        // the owned LoadingMenu before/after delta, never a captured eye image.
        // It is world-locked to the background and sorted in front.
        err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.tips", "VR Loading Tips", &s_tipsOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (tips) failed (error {})", err);
            s_tipsOverlayHandle = 0;
        } else {
            s_ovrSetAlpha(s_overlay, s_tipsOverlayHandle, 1.0f);
            s_ovrSetSortOrder(s_overlay, s_tipsOverlayHandle, kTipsSortOrder);
            logger::info("VROverlay: tips overlay created (handle={})", s_tipsOverlayHandle);
        }

        s_overlayInitialized.store(
            s_bgOverlayHandle != 0 && s_blockerOverlayHandle != 0,
            std::memory_order_release);
        return true;
    }

    bool VRCompositorHelper::GetHMDPose(HmdMatrix34& outPose)
    {
        if (!s_getLastPoses || !s_compositor) return false;

        TrackedDevicePose poses[1] = {};
        int err = s_getLastPoses(s_compositor, poses, 1, nullptr, 0);
        if (err != 0 || !poses[0].bPoseIsValid) return false;

        outPose = poses[0].mDeviceToAbsoluteTracking;
        return true;
    }

    void VRCompositorHelper::ResetPostCloseFadeLatch()
    {
        std::lock_guard latchLock(s_postCloseFadeLatchMutex);
        // Disable entry snapshots before clearing the mutex-owned state. A
        // Submit that already captured the old generation must revalidate it
        // under this same mutex before it can touch the pair state.
        s_postCloseFadeLatchGeneration.store(0, std::memory_order_release);
        s_postCloseFadeLatchStateGeneration = 0;
        s_postCloseFadeLatchHoldGeneration = 0;
        s_postCloseFadeLatchAtTicks = 0;
        s_postCloseFadeLatchSawLeft = false;
        s_postCloseFadeLatchPairs = 0;
    }

    void VRCompositorHelper::BeginLoadingPresentation()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (++s_presentationGeneration == 0) {
            ++s_presentationGeneration;
        }
        // A chained native OPEN supersedes a pending post-close release. If a
        // stale hold timer survived into this load it would hide the new
        // load's overlays mid-screen.
        s_postCloseHoldGeneration.store(0, std::memory_order_release);
        s_postCloseHoldDeadlineTicks.store(0, std::memory_order_release);
        s_postCloseCoverDeadlineTicks.store(0, std::memory_order_release);
        ResetPostCloseFadeLatch();
        s_postCloseRequireStereo.store(false, std::memory_order_release);
        s_postCloseStereoActive.store(false, std::memory_order_release);
        s_postCloseStereoBackstopTicks.store(0, std::memory_order_release);
        s_loadingPresentationOpen = true;
    }

    void VRCompositorHelper::SetImageSkybox(void* d3dTexture)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen) return;
        if (!s_setSkyboxOverride || !s_compositor || !d3dTexture) return;

        VRTexture tex;
        tex.handle = d3dTexture;
        tex.eType = 0;
        tex.eColorSpace = 1;

        int err = s_setSkyboxOverride(s_compositor, &tex, 1);
        if (err != 0) {
            logger::warn("SetSkyboxOverride (image) failed (error {})", err);
        } else {
            logger::info("SetSkyboxOverride: image skybox set");
        }
    }

    bool VRCompositorHelper::ShowBlockerOverlay()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen) return false;
        if (!s_overlayInitialized.load(std::memory_order_acquire) || !s_overlay ||
            !s_blockerOverlayHandle || !s_blackTexture) return false;

        VRTexture blockerTex;
        blockerTex.handle = s_blackTexture;
        blockerTex.eType = 0;
        blockerTex.eColorSpace = 1;
        const int textureErr =
            s_ovrSetTexture(s_overlay, s_blockerOverlayHandle, &blockerTex);
        const int alphaErr =
            s_ovrSetAlpha(s_overlay, s_blockerOverlayHandle, 1.0f);

        HmdMatrix34 blockerRel = {};
        blockerRel.m[0][0] = 1.0f;
        blockerRel.m[1][1] = 1.0f;
        blockerRel.m[2][2] = 1.0f;
        blockerRel.m[2][3] = -3.0f;
        const int transformErr =
            s_ovrSetTransformDevRel(
                s_overlay, s_blockerOverlayHandle, 0, &blockerRel);
        const int showErr = s_ovrShow(s_overlay, s_blockerOverlayHandle);
        if (textureErr != 0 || alphaErr != 0 ||
            transformErr != 0 || showErr != 0) {
            const int hideErr =
                s_ovrHide(s_overlay, s_blockerOverlayHandle);
            // If OpenVR also rejects the cleanup call, keep the conservative
            // active state so close/handoff will retry instead of forgetting a
            // potentially visible overlay.
            s_blockerOverlayActive.store(
                hideErr != 0, std::memory_order_release);
            logger::warn(
                "VROverlay: blocker show failed "
                "(texture={}, alpha={}, transform={}, show={}, cleanup={})",
                textureErr, alphaErr, transformErr, showErr, hideErr);
            return false;
        }
        s_blockerOverlayActive.store(true, std::memory_order_release);
        logger::info("VROverlay: blocker shown (immediate)");
        return true;
    }

    // Caller must hold s_overlayMutex. Retains the newly published texture
    // before dropping the previous one so replacing an overlay's texture with
    // itself, or with a texture whose only other owner is the slot being
    // replaced, can never free it mid-swap.
    void VRCompositorHelper::RetainShownTexture(void*& slot, void* texture)
    {
        if (slot == texture) return;
        if (texture) {
            static_cast<REX::W32::ID3D11Texture2D*>(texture)->AddRef();
        }
        void* const previous = slot;
        slot = texture;
        if (previous) {
            static_cast<REX::W32::ID3D11Texture2D*>(previous)->Release();
        }
    }

    bool VRCompositorHelper::ShowBackgroundOverlay(void* d3dTexture, int overlayMode, float alpha)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen) return false;
        if (!s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !d3dTexture) return false;

        s_overlayMode.store(overlayMode, std::memory_order_release);
        s_bgWorldPoseValid.store(false, std::memory_order_release);

        VRTexture tex;
        tex.handle = d3dTexture;
        tex.eType = 0;
        tex.eColorSpace = 1;

        D3D11_TEXTURE2D_DESC textureDesc{};
        auto* texture2D = static_cast<ID3D11Texture2D*>(d3dTexture);
        texture2D->GetDesc(&textureDesc);
        ID3D11Device* textureDevice = nullptr;
        texture2D->GetDevice(&textureDevice);
        const bool deviceMatches =
            textureDevice ==
            reinterpret_cast<ID3D11Device*>(
                s_device.load(std::memory_order_acquire));
        if (textureDevice) textureDevice->Release();

        const int textureErr =
            s_ovrSetTexture(s_overlay, s_bgOverlayHandle, &tex);
        if (textureErr == 0) {
            // OpenVR now samples this texture. Own a reference for as long as
            // the overlay can present it.
            RetainShownTexture(s_bgShownTexture, d3dTexture);
        }
        if (textureErr != 0) {
            const int hideErr = s_ovrHide(s_overlay, s_bgOverlayHandle);
            s_bgOverlayActive.store(
                hideErr != 0, std::memory_order_release);
            if (hideErr == 0) {
                RetainShownTexture(s_bgShownTexture, nullptr);
            }
            logger::warn(
                "VROverlay: SetOverlayTexture (bg) failed "
                "(error={}({}), size={}x{}, format={}, mips={}, array={}, "
                "samples={}, usage={}, bind=0x{:X}, misc=0x{:X}, "
                "deviceMatches={}, cleanup={})",
                textureErr, OverlayErrorName(textureErr),
                textureDesc.Width, textureDesc.Height,
                static_cast<unsigned>(textureDesc.Format),
                textureDesc.MipLevels, textureDesc.ArraySize,
                textureDesc.SampleDesc.Count,
                static_cast<unsigned>(textureDesc.Usage),
                textureDesc.BindFlags, textureDesc.MiscFlags,
                deviceMatches, hideErr);
            return false;
        }

        // Always present VR loading art on a 16:9 landscape quad. OpenVR derives
        // overlay height from the submitted texture AND its texture bounds, so
        // crop a centered 16:9 region instead of stretching 4:3/square sources.
        // The mode-3 baked texture is already 16:9 and therefore uses full bounds.
        float textureBounds[4] = { 0.0f, 0.0f, 1.0f, 1.0f }; // uMin,vMin,uMax,vMax
        int boundsErr = -1;
        int texelAspectErr = -1;
        if (textureDesc.Width > 0 && textureDesc.Height > 0) {
            const float sourceAspect =
                static_cast<float>(textureDesc.Width) / static_cast<float>(textureDesc.Height);
            if (sourceAspect > kVRLoadingScreenAspect) {
                const float visibleU = kVRLoadingScreenAspect / sourceAspect;
                textureBounds[0] = (1.0f - visibleU) * 0.5f;
                textureBounds[2] = 1.0f - textureBounds[0];
            } else if (sourceAspect < kVRLoadingScreenAspect) {
                const float visibleV = sourceAspect / kVRLoadingScreenAspect;
                textureBounds[1] = (1.0f - visibleV) * 0.5f;
                textureBounds[3] = 1.0f - textureBounds[1];
            }
            boundsErr = s_ovrSetTextureBounds
                ? s_ovrSetTextureBounds(
                    s_overlay, s_bgOverlayHandle, textureBounds)
                : -1;
            texelAspectErr = s_ovrSetTexelAspect
                ? s_ovrSetTexelAspect(
                    s_overlay, s_bgOverlayHandle, 1.0f)
                : -1;
            logger::info(
                "VROverlay: background {}x{} center-cropped to 16:9 bounds [{:.3f},{:.3f},{:.3f},{:.3f}]",
                textureDesc.Width, textureDesc.Height,
                textureBounds[0], textureBounds[1], textureBounds[2], textureBounds[3]);
        }

        const int widthErr = s_ovrSetWidth(
            s_overlay, s_bgOverlayHandle,
            s_bgWidthSetting.load(std::memory_order_acquire));
        const int alphaErr =
            s_ovrSetAlpha(s_overlay, s_bgOverlayHandle, alpha);
        int transformErr = -1;

        // Mode 0: HMD-relative (locked to headset like a cinema screen)
        // Wide enough to cover full FOV (~120°) so no game content peeks through.
        // At 3m distance, 10m width gives 2*atan(5/3) = ~118° horizontal coverage.
        if (overlayMode == 0) {
            HmdMatrix34 devRel = {};
            devRel.m[0][0] = 1.0f;
            devRel.m[1][1] = 1.0f;
            devRel.m[2][2] = 1.0f;
            devRel.m[2][3] = -3.0f; // 3m in front of HMD
            transformErr =
                s_ovrSetTransformDevRel(
                    s_overlay, s_bgOverlayHandle, 0, &devRel);
            logger::info(
                "VROverlay: mode=HMD-relative (10m wide, 3m forward) relErr={}",
                transformErr);
        }
        // Modes 1 & 2: World-locked via SetOverlayTransformAbsolute.
        // Placed in front of HMD at time of loading, stays fixed in world space.
        else {
            float distance = 7.0f;

            // Get HMD pose — try live query first (freshest orientation),
            // then cached pre-loading pose, then GetLastPoses as last resort.
            HmdMatrix34 hmd = {};
            bool gotPose = false;
            const char* poseSource = "none";

            // 1. Live IVRSystem query (tracking should still be active when loading menu opens)
            if (s_vrSystem && s_getDeviceToAbsTrackingPose) {
                TrackedDevicePose poses[1] = {};
                s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
                if (poses[0].bPoseIsValid) {
                    hmd = poses[0].mDeviceToAbsoluteTracking;
                    gotPose = true;
                    poseSource = "live-IVRSystem";
                }
            }
            // 2. Cached pose from last frame (guaranteed valid if onFrameUpdate ran)
            if (!gotPose && s_hasLastKnownPose.load(std::memory_order_acquire)) {
                std::lock_guard poseLock(s_poseMutex);
                if (s_hasLastKnownPose.load(std::memory_order_relaxed)) {
                    hmd = s_lastKnownPose;
                    gotPose = true;
                    poseSource = "cached";
                }
            }
            // 3. GetLastPoses (blocking, least preferred)
            if (!gotPose) {
                gotPose = GetHMDPose(hmd);
                if (gotPose) poseSource = "GetLastPoses";
            }

            if (gotPose) {
                float fwdX = -hmd.m[0][2];
                float fwdZ = -hmd.m[2][2];
                float len = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
                if (len > 0.001f) { fwdX /= len; fwdZ /= len; }

                // Rotation: overlay faces +Z in local space (OpenVR convention).
                // Z-axis must point FROM overlay TOWARDS user = -fwd direction.
                // Z = [-fwdX, 0, -fwdZ], right = cross(up, Z) = [-fwdZ, 0, fwdX]
                HmdMatrix34 transform = {};
                transform.m[0][0] = -fwdZ;  transform.m[0][1] = 0.0f; transform.m[0][2] = -fwdX;
                transform.m[1][0] = 0.0f;   transform.m[1][1] = 1.0f; transform.m[1][2] = 0.0f;
                transform.m[2][0] = fwdX;   transform.m[2][1] = 0.0f; transform.m[2][2] = -fwdZ;
                transform.m[0][3] = hmd.m[0][3] + fwdX * distance;
                transform.m[1][3] = hmd.m[1][3];
                transform.m[2][3] = hmd.m[2][3] + fwdZ * distance;

                transformErr =
                    s_ovrSetTransformAbs(
                        s_overlay, s_bgOverlayHandle, 1, &transform);
                if (transformErr == 0) {
                    // Store only a transform OpenVR accepted for per-frame
                    // re-application during loading.
                    s_bgWorldPose = transform;
                    s_bgWorldPoseValid.store(true, std::memory_order_release);
                }
                logger::info("VROverlay: mode={} poseSource={} pos=[{:.2f},{:.2f},{:.2f}] fwd=[{:.2f},{:.2f}] err={}",
                    overlayMode == 2 ? "Cinema" : "World-locked", poseSource,
                    transform.m[0][3], transform.m[1][3], transform.m[2][3], fwdX, fwdZ, transformErr);
            } else {
                // Fallback: device-relative at large distance (barely perceptible head tracking)
                s_bgWorldPoseValid.store(false, std::memory_order_release);
                HmdMatrix34 devRel = {};
                devRel.m[0][0] = 1.0f;
                devRel.m[1][1] = 1.0f;
                devRel.m[2][2] = 1.0f;
                devRel.m[2][3] = -distance;
                transformErr =
                    s_ovrSetTransformDevRel(
                        s_overlay, s_bgOverlayHandle, 0, &devRel);
                logger::info(
                    "VROverlay: no HMD pose available, device-relative fallback at {}m (err={})",
                    distance, transformErr);
            }
        }

        int showErr = -1;
        if (boundsErr == 0 && texelAspectErr == 0 &&
            widthErr == 0 && alphaErr == 0 && transformErr == 0) {
            showErr = s_ovrShow(s_overlay, s_bgOverlayHandle);
        }
        if (boundsErr != 0 || texelAspectErr != 0 ||
            widthErr != 0 || alphaErr != 0 ||
            transformErr != 0 || showErr != 0) {
            const int hideErr = s_ovrHide(s_overlay, s_bgOverlayHandle);
            s_bgOverlayActive.store(
                hideErr != 0, std::memory_order_release);
            if (hideErr == 0) {
                RetainShownTexture(s_bgShownTexture, nullptr);
            }
            logger::warn(
                "VROverlay: background attach failed "
                "(bounds={}, texelAspect={}, width={}, alpha={}, transform={}, "
                "show={}, cleanup={})",
                boundsErr, texelAspectErr, widthErr, alphaErr, transformErr,
                showErr, hideErr);
            return false;
        }
        s_bgOverlayActive.store(true, std::memory_order_release);
        logger::info("VROverlay: background shown (alpha={:.2f})", alpha);

        // Blocker overlay no longer drawn here — IMenu::DisplayMenu hook in
        // D3D11Compositor kills LoadingMenu's eye-buffer compositor at the
        // source, so there's nothing leaking through to cover.
        return true;
    }

    bool VRCompositorHelper::UpdateBackgroundOverlayTexture(
        void* d3dTexture, bool forcePublish)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen ||
            !s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_bgOverlayHandle || !d3dTexture ||
            !s_bgOverlayActive.load(std::memory_order_acquire)) {
            return false;
        }
        if (!forcePublish && s_bgShownTexture == d3dTexture) {
            return true;
        }

        auto describeBounds = [](void* texture, float (&bounds)[4],
                                  D3D11_TEXTURE2D_DESC& desc) {
            auto* texture2D = static_cast<ID3D11Texture2D*>(texture);
            texture2D->GetDesc(&desc);
            bounds[0] = 0.0f;
            bounds[1] = 0.0f;
            bounds[2] = 1.0f;
            bounds[3] = 1.0f;
            if (desc.Width == 0 || desc.Height == 0) {
                return false;
            }
            const float sourceAspect =
                static_cast<float>(desc.Width) /
                static_cast<float>(desc.Height);
            if (sourceAspect > kVRLoadingScreenAspect) {
                const float visibleU =
                    kVRLoadingScreenAspect / sourceAspect;
                bounds[0] = (1.0f - visibleU) * 0.5f;
                bounds[2] = 1.0f - bounds[0];
            } else if (sourceAspect < kVRLoadingScreenAspect) {
                const float visibleV =
                    sourceAspect / kVRLoadingScreenAspect;
                bounds[1] = (1.0f - visibleV) * 0.5f;
                bounds[3] = 1.0f - bounds[1];
            }
            return true;
        };

        float newBounds[4]{};
        D3D11_TEXTURE2D_DESC newDesc{};
        if (!describeBounds(d3dTexture, newBounds, newDesc)) {
            logger::warn(
                "VROverlay: in-place background update rejected invalid "
                "texture dimensions");
            return false;
        }

        VRTexture newTexture{};
        newTexture.handle = d3dTexture;
        newTexture.eType = 0;
        newTexture.eColorSpace = 1;
        const int textureErr =
            s_ovrSetTexture(s_overlay, s_bgOverlayHandle, &newTexture);
        const int boundsErr = textureErr == 0 && s_ovrSetTextureBounds
            ? s_ovrSetTextureBounds(
                  s_overlay, s_bgOverlayHandle, newBounds)
            : -1;
        if (textureErr == 0 && boundsErr == 0) {
            RetainShownTexture(s_bgShownTexture, d3dTexture);
            logger::info(
                "VROverlay: visible background texture {} in place "
                "({}x{}, pose preserved)",
                forcePublish ? "re-published" : "updated",
                newDesc.Width, newDesc.Height);
            return true;
        }

        // SetOverlayTexture and SetOverlayTextureBounds are separate OpenVR
        // calls. If the second one fails, put back the still-retained previous
        // texture and its bounds so a best-effort tip update cannot destroy the
        // already-correct plain custom background.
        int rollbackTextureErr = -1;
        int rollbackBoundsErr = -1;
        if (s_bgShownTexture) {
            float oldBounds[4]{};
            D3D11_TEXTURE2D_DESC oldDesc{};
            if (describeBounds(s_bgShownTexture, oldBounds, oldDesc)) {
                VRTexture oldTexture{};
                oldTexture.handle = s_bgShownTexture;
                oldTexture.eType = 0;
                oldTexture.eColorSpace = 1;
                rollbackTextureErr = s_ovrSetTexture(
                    s_overlay, s_bgOverlayHandle, &oldTexture);
                if (rollbackTextureErr == 0 && s_ovrSetTextureBounds) {
                    rollbackBoundsErr = s_ovrSetTextureBounds(
                        s_overlay, s_bgOverlayHandle, oldBounds);
                }
            }
        }
        logger::warn(
            "VROverlay: in-place background texture update failed "
            "(texture={}, bounds={}, rollbackTexture={}, rollbackBounds={}); "
            "plain custom background retained",
            textureErr, boundsErr, rollbackTextureErr, rollbackBoundsErr);
        return false;
    }

    void VRCompositorHelper::UpdateBackgroundOverlay()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_bgOverlayActive.load(std::memory_order_acquire) ||
            !s_bgWorldPoseValid.load(std::memory_order_acquire) ||
            !s_overlayInitialized.load(std::memory_order_acquire) || !s_overlay) return;
        if (s_overlayMode.load(std::memory_order_acquire) == 0) return;

        // Re-apply stored absolute transform every frame to ensure it persists
        s_ovrSetTransformAbs(s_overlay, s_bgOverlayHandle, 1, &s_bgWorldPose);
    }

    bool VRCompositorHelper::RelockBackgroundOverlayTransform()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_bgOverlayActive.load(std::memory_order_acquire) ||
            !s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_bgOverlayHandle) {
            return false;
        }

        const int overlayMode =
            s_overlayMode.load(std::memory_order_acquire);
        if (overlayMode == 0) {
            HmdMatrix34 devRel{};
            devRel.m[0][0] = 1.0f;
            devRel.m[1][1] = 1.0f;
            devRel.m[2][2] = 1.0f;
            devRel.m[2][3] = -3.0f;
            const int err = s_ovrSetTransformDevRel(
                s_overlay, s_bgOverlayHandle, 0, &devRel);
            if (err != 0) {
                logger::warn(
                    "VROverlay: HMD-relative pose re-lock failed "
                    "(error {}); prior pose retained",
                    err);
                return false;
            }
            s_bgWorldPoseValid.store(false, std::memory_order_release);
            return true;
        }

        HmdMatrix34 hmd{};
        bool gotPose = false;
        const char* poseSource = "none";
        if (s_vrSystem && s_getDeviceToAbsTrackingPose) {
            TrackedDevicePose poses[1]{};
            s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
            if (poses[0].bPoseIsValid) {
                hmd = poses[0].mDeviceToAbsoluteTracking;
                gotPose = true;
                poseSource = "live-IVRSystem";
            }
        }
        if (!gotPose && s_hasLastKnownPose.load(std::memory_order_acquire)) {
            std::lock_guard poseLock(s_poseMutex);
            if (s_hasLastKnownPose.load(std::memory_order_relaxed)) {
                hmd = s_lastKnownPose;
                gotPose = true;
                poseSource = "cached";
            }
        }
        if (!gotPose) {
            gotPose = GetHMDPose(hmd);
            if (gotPose) {
                poseSource = "GetLastPoses";
            }
        }
        if (!gotPose) {
            logger::warn(
                "VROverlay: world pose re-lock had no valid HMD pose; "
                "prior pose retained");
            return false;
        }

        float fwdX = -hmd.m[0][2];
        float fwdZ = -hmd.m[2][2];
        const float len = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
        if (len > 0.001f) {
            fwdX /= len;
            fwdZ /= len;
        }

        constexpr float distance = 7.0f;
        HmdMatrix34 transform{};
        transform.m[0][0] = -fwdZ;
        transform.m[0][2] = -fwdX;
        transform.m[1][1] = 1.0f;
        transform.m[2][0] = fwdX;
        transform.m[2][2] = -fwdZ;
        transform.m[0][3] = hmd.m[0][3] + fwdX * distance;
        transform.m[1][3] = hmd.m[1][3];
        transform.m[2][3] = hmd.m[2][3] + fwdZ * distance;

        const int err = s_ovrSetTransformAbs(
            s_overlay, s_bgOverlayHandle, 1, &transform);
        if (err != 0) {
            logger::warn(
                "VROverlay: world pose re-lock failed (error {}); "
                "prior pose retained",
                err);
            return false;
        }
        s_bgWorldPose = transform;
        s_bgWorldPoseValid.store(true, std::memory_order_release);
        logger::info(
            "VROverlay: world pose re-locked (mode={}, source={})",
            overlayMode, poseSource);
        return true;
    }

    void VRCompositorHelper::UpdateLastKnownPose()
    {
        if (!s_initialized.load(std::memory_order_acquire) ||
            !s_vrSystem || !s_getDeviceToAbsTrackingPose) return;

        TrackedDevicePose poses[1] = {};
        s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
        if (poses[0].bPoseIsValid) {
            std::lock_guard poseLock(s_poseMutex);
            s_lastKnownPose = poses[0].mDeviceToAbsoluteTracking;
            s_hasLastKnownPose.store(true, std::memory_order_release);
        }
    }

    bool VRCompositorHelper::GetCurrentPose(float outPose[3][4])
    {
        // Try live IVRSystem query first (freshest)
        if (s_vrSystem && s_getDeviceToAbsTrackingPose) {
            TrackedDevicePose poses[1] = {};
            s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
            if (poses[0].bPoseIsValid) {
                std::memcpy(outPose, poses[0].mDeviceToAbsoluteTracking.m, sizeof(float) * 12);
                return true;
            }
        }
        // Fall back to cached pose
        if (s_hasLastKnownPose.load(std::memory_order_acquire)) {
            std::lock_guard poseLock(s_poseMutex);
            if (s_hasLastKnownPose.load(std::memory_order_relaxed)) {
                std::memcpy(outPose, s_lastKnownPose.m, sizeof(float) * 12);
                return true;
            }
        }
        return false;
    }

    bool VRCompositorHelper::HideBackgroundOverlay()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_overlayInitialized.load(std::memory_order_acquire) || !s_overlay) {
            s_bgOverlayActive.store(false, std::memory_order_release);
            s_tipsOverlayActive.store(false, std::memory_order_release);
            RetainShownTexture(s_bgShownTexture, nullptr);
            RetainShownTexture(s_tipsShownTexture, nullptr);
            return true;
        }
        s_bgWorldPoseValid.store(false, std::memory_order_release);
        s_updateLogCounter = 0;

        bool hidden = true;
        if (s_bgOverlayHandle &&
            s_bgOverlayActive.load(std::memory_order_acquire)) {
            const int err = s_ovrHide(s_overlay, s_bgOverlayHandle);
            if (err == 0) {
                s_bgOverlayActive.store(false, std::memory_order_release);
            } else {
                hidden = false;
                logger::warn(
                    "VROverlay: background hide failed (error {})", err);
            }
        }
        // Release only what is provably off screen: a failed hide leaves the
        // overlay able to sample its texture, so keep that reference.
        if (!s_bgOverlayActive.load(std::memory_order_acquire)) {
            RetainShownTexture(s_bgShownTexture, nullptr);
        }
        if (s_tipsOverlayHandle && s_tipsOverlayActive.load(std::memory_order_acquire)) {
            const int err = s_ovrHide(s_overlay, s_tipsOverlayHandle);
            if (err == 0) {
                s_tipsOverlayActive.store(false, std::memory_order_release);
            } else {
                hidden = false;
                logger::warn("VROverlay: tips hide failed (error {})", err);
            }
        }
        if (!s_tipsOverlayActive.load(std::memory_order_acquire)) {
            RetainShownTexture(s_tipsShownTexture, nullptr);
        }
        return hidden;
    }

    bool VRCompositorHelper::HideBlockerOverlay()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_blockerOverlayHandle) {
            s_blockerOverlayActive.store(false, std::memory_order_release);
            return true;
        }
        const int err = s_ovrHide(s_overlay, s_blockerOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: blocker hide failed (error {})", err);
            return false;
        }
        s_blockerOverlayActive.store(false, std::memory_order_release);
        logger::info("VROverlay: blocker hidden");
        return true;
    }

    void VRCompositorHelper::EndLoadingPresentationNow(
        float a_sceneFadeSeconds,
        bool a_reassertSceneFade)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        // Bethesda continues issuing its own FadeToColor calls throughout a
        // long load, so s_sceneFadeHeld records only our intent, not the
        // compositor's current alpha. Reassert opaque black while the custom
        // artwork is still above the scene; the subsequent clear is then a
        // real black-to-world ramp rather than a possible alpha-0 no-op.
        if (a_sceneFadeSeconds > 0.0f && a_reassertSceneFade) {
            ApplySceneFade();
        }
        // Close before hiding. Every Show* call checks this value while holding
        // the same mutex, so no stale Update can publish after this boundary.
        s_loadingPresentationOpen = false;
        s_postCloseHoldGeneration.store(0, std::memory_order_release);
        s_postCloseHoldDeadlineTicks.store(0, std::memory_order_release);
        s_postCloseCoverDeadlineTicks.store(0, std::memory_order_release);
        ResetPostCloseFadeLatch();
        s_postCloseRequireStereo.store(false, std::memory_order_release);
        s_postCloseStereoActive.store(false, std::memory_order_release);
        s_postCloseStereoBackstopTicks.store(0, std::memory_order_release);
        bool destroyedHandle = false;
        // Returns true when the overlay is provably off screen, i.e. OpenVR
        // accepted either the hide or the fail-open destroy.
        const auto hideFailOpen =
            [&destroyedHandle](std::uint64_t& handle,
                std::atomic<bool>& active, const char* label) {
                if (!handle || !s_overlay) {
                    active.store(false, std::memory_order_release);
                    return true;
                }

                // Hide every extant handle, even if our cached active bit says
                // false. The native CLOSE boundary is authoritative and must
                // also clean up any state drift after an earlier OpenVR error.
                const int hideError =
                    s_ovrHide ? s_ovrHide(s_overlay, handle) : -1;
                bool retired = hideError == 0;
                if (hideError != 0) {
                    const int destroyError =
                        s_ovrDestroyOverlay
                            ? s_ovrDestroyOverlay(s_overlay, handle)
                            : -1;
                    logger::warn(
                        "VROverlay: native-close {} hide failed ({}); "
                        "fail-open destroy={} handle={}",
                        label, hideError, destroyError, handle);
                    // Whether OpenVR accepted Destroy or not, forget the custom
                    // object locally. Never intentionally retry/show it after
                    // Bethesda's native CLOSE boundary.
                    handle = 0;
                    destroyedHandle = true;
                    retired = destroyError == 0;
                }
                active.store(false, std::memory_order_release);
                return retired;
            };

        const bool tipsRetired = hideFailOpen(
            s_tipsOverlayHandle, s_tipsOverlayActive, "tips");
        const bool backgroundRetired = hideFailOpen(
            s_bgOverlayHandle, s_bgOverlayActive, "background");
        hideFailOpen(
            s_blockerOverlayHandle, s_blockerOverlayActive, "blocker");
        // Drop the displayed-texture references only for overlays OpenVR agreed
        // to retire. If both the hide and the destroy failed the overlay may
        // still be sampling its texture, so keeping the reference leaks at most
        // one image rather than presenting freed memory.
        if (tipsRetired) {
            RetainShownTexture(s_tipsShownTexture, nullptr);
        }
        if (backgroundRetired) {
            RetainShownTexture(s_bgShownTexture, nullptr);
        }
        // Fail-open: whatever brought us here, the scene blackout must not
        // survive it. Abort/fallback callers keep the default instant clear;
        // the normal post-close release passes its reveal duration here so the
        // blackout is cleared exactly once, after the overlays are retired.
        ClearSceneFade(a_sceneFadeSeconds);
        // March had no persistent skybox presentation owner. Clear any fallback
        // override in the same one-step transaction so it cannot outlive the
        // artwork and produce a second black-to-world transition later.
        s_skyboxClearAtTicks.store(0, std::memory_order_release);
        if (s_clearSkyboxOverride && s_compositor) {
            s_clearSkyboxOverride(s_compositor);
        }
        s_bgWorldPoseValid.store(false, std::memory_order_release);
        s_handoffPending.store(false, std::memory_order_release);
        s_handoffSawLeft.store(false, std::memory_order_release);
        s_handoffCompletePairs.store(0, std::memory_order_release);
        if (destroyedHandle) {
            // The next OPEN retries a clean all-or-nothing overlay init.
            s_overlayInitialized.store(false, std::memory_order_release);
        }
    }

    void VRCompositorHelper::BeginPostLoadingHandoff()
    {
        // Legacy callers use this for a mid-load fallback. Fail open
        // immediately as well; no custom blocker/skybox is allowed to depend on
        // future successful eye submissions for removal.
        EndLoadingPresentationNow();
    }

    void VRCompositorHelper::CancelPostLoadingHandoff()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        s_postCloseHoldGeneration.store(0, std::memory_order_release);
        s_postCloseHoldDeadlineTicks.store(0, std::memory_order_release);
        s_postCloseCoverDeadlineTicks.store(0, std::memory_order_release);
        ResetPostCloseFadeLatch();
        s_postCloseRequireStereo.store(false, std::memory_order_release);
        s_postCloseStereoActive.store(false, std::memory_order_release);
        s_postCloseStereoBackstopTicks.store(0, std::memory_order_release);
        s_handoffPending.store(false, std::memory_order_release);
        s_handoffSawLeft.store(false, std::memory_order_release);
        s_handoffCompletePairs.store(0, std::memory_order_release);
        // A superseding native/fallback OPEN will not necessarily call
        // SetBlackSkybox or EndLoadingPresentationNow. Clear any opaque latch
        // inherited from the previous CLOSE before handing it native ownership.
        if (s_sceneFadeHeld.load(std::memory_order_acquire)) {
            ClearSceneFade(0.0f);
        }
    }

    // Caller must hold s_overlayMutex. Deliberately bypasses
    // s_loadingPresentationOpen: the seal exists to stop NEW publications after
    // CLOSE, not to stop us re-asserting the cover we already own and are still
    // showing. Fail-open and diagnostic — it can only ever make the blocker
    // more correct.
    void VRCompositorHelper::ReassertBlockerCoverLocked()
    {
        if (!s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_blockerOverlayHandle || !s_blackTexture) {
            logger::warn(
                "VROverlay: blocker cover unavailable at art release "
                "(init={}, overlay={}, handle={}, texture={})",
                s_overlayInitialized.load(std::memory_order_acquire),
                s_overlay != nullptr, s_blockerOverlayHandle != 0,
                s_blackTexture != nullptr);
            return;
        }

        // Magenta while diagnosing: if the viewer sees magenta in the stage-1
        // window then this cover IS on top and the artifact is elsewhere; if
        // they see the title screen instead, the cover is provably not on top
        // and the search moves to the overlay layer. One bit, unambiguous.
        const bool debugCover =
            LoadingScreenManager::IsVRPresentationProbeEnabled() &&
            s_debugCoverTexture != nullptr;
        VRTexture blockerTex;
        blockerTex.handle = debugCover ? s_debugCoverTexture : s_blackTexture;
        blockerTex.eType = 0;
        blockerTex.eColorSpace = 1;
        const int textureErr =
            s_ovrSetTexture(s_overlay, s_blockerOverlayHandle, &blockerTex);
        const int alphaErr =
            s_ovrSetAlpha(s_overlay, s_blockerOverlayHandle, 1.0f);
        const int sortErr = s_ovrSetSortOrder(
            s_overlay, s_blockerOverlayHandle, kBlockerSortOrder);
        HmdMatrix34 blockerRel = {};
        blockerRel.m[0][0] = 1.0f;
        blockerRel.m[1][1] = 1.0f;
        blockerRel.m[2][2] = 1.0f;
        blockerRel.m[2][3] = -3.0f;
        const int transformErr = s_ovrSetTransformDevRel(
            s_overlay, s_blockerOverlayHandle, 0, &blockerRel);
        const int showErr = s_ovrShow(s_overlay, s_blockerOverlayHandle);
        if (showErr == 0) {
            s_blockerOverlayActive.store(true, std::memory_order_release);
        }
        if (textureErr || alphaErr || sortErr || transformErr || showErr) {
            logger::warn(
                "VROverlay: blocker cover re-assert had errors "
                "(texture={}, alpha={}, sort={}, transform={}, show={})",
                textureErr, alphaErr, sortErr, transformErr, showErr);
        } else {
            logger::info(
                "VROverlay: blocker cover re-asserted (sort={}, colour={})",
                static_cast<int>(kBlockerSortOrder),
                debugCover ? "MAGENTA-DIAGNOSTIC" : "black");
        }
    }

    void VRCompositorHelper::SealLoadingPresentation()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        // Native CLOSE first stops every new publication; the pixels already
        // on screen stay owned by the bounded hold decision that follows.
        s_loadingPresentationOpen = false;
    }

    void VRCompositorHelper::BeginTimedPostCloseHold(
        int a_maxMs, bool a_requireStereo)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        s_loadingPresentationOpen = false;
        if (a_maxMs <= 0) {
            // Degenerate request — behave exactly like the synchronous close.
            s_postCloseHoldGeneration.store(0, std::memory_order_release);
            s_postCloseHoldDeadlineTicks.store(0, std::memory_order_release);
            s_postCloseCoverDeadlineTicks.store(0, std::memory_order_release);
            ResetPostCloseFadeLatch();
            s_postCloseRequireStereo.store(false, std::memory_order_release);
            s_postCloseStereoActive.store(false, std::memory_order_release);
            s_postCloseStereoBackstopTicks.store(0, std::memory_order_release);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        // A hold arm is its own ownership boundary as well as OPEN. This also
        // makes a duplicate/re-armed CLOSE distinct, so a tick that claimed an
        // older deadline cannot tear down the newer hold merely because no new
        // BeginLoadingPresentation occurred between them.
        if (++s_presentationGeneration == 0) {
            ++s_presentationGeneration;
        }
        // Publish the epoch before the deadline. A tick which acquires the new
        // deadline therefore also observes the generation it must validate
        // under s_overlayMutex before touching any overlay.
        s_postCloseHoldGeneration.store(
            s_presentationGeneration, std::memory_order_release);
        // A duplicate/re-armed CLOSE owns a new epoch. Retire any latch from
        // the superseded arm before publishing this arm's deadline.
        ResetPostCloseFadeLatch();
        // Publish the stereo-gate state before the deadline that gates it.
        s_postCloseStereoActive.store(false, std::memory_order_release);
        s_postCloseRequireStereo.store(
            a_requireStereo, std::memory_order_release);
        s_postCloseStereoBackstopTicks.store(
            a_requireStereo
                ? (now + std::chrono::milliseconds(kPostCloseStereoBackstopMs))
                      .time_since_epoch().count()
                : 0,
            std::memory_order_release);
        const auto deadline = now + std::chrono::milliseconds(a_maxMs);
        s_postCloseHoldDeadlineTicks.store(
            deadline.time_since_epoch().count(), std::memory_order_release);
        // Nonzero marks the cover as pending; the release window is re-based
        // when stage 1 actually removes the art, so include the art hold here
        // only as a coarse backstop for a stage 1 that never ticks.
        const auto coverDeadline = now + std::chrono::milliseconds(
            a_maxMs + kPostCloseCoverMaxMs);
        s_postCloseCoverDeadlineTicks.store(
            coverDeadline.time_since_epoch().count(),
            std::memory_order_release);
        logger::info(
            "VROverlay: post-close hold armed (art >= {} ms{}, then scene "
            "fade latch across {} post-arm accepted eye pairs and >= {} ms "
            "({} ms backstop), publications sealed, "
            "gridAlpha={:.3f})",
            a_maxMs,
            a_requireStereo ? " + engine stereo resume" : "",
            Policy::kVRSceneFadeLatchRequiredPairs,
            Policy::kVRSceneFadeLatchMinimumMs,
            Policy::kVRSceneFadeLatchBackstopMs,
            CurrentGridAlpha());
    }

    void VRCompositorHelper::SetPostCloseStereo(bool a_stereoActive)
    {
        s_postCloseStereoActive.store(
            a_stereoActive, std::memory_order_release);
    }

    bool VRCompositorHelper::IsPostCloseStereoGatePending()
    {
        // Include the cover phase: stage 1's claim zeroes the hold deadline,
        // and without the cover term the stereo feed would stop there. On the
        // backstop path (stereo not seen within stage 1's window) that froze
        // stereoActive at false, so stage 2 could never release on the stereo
        // signal and always sat out its full timeout — releasing blind onto
        // possibly-mono frames, which is exactly what it exists to prevent.
        return s_postCloseRequireStereo.load(std::memory_order_acquire) &&
            (s_postCloseHoldDeadlineTicks.load(std::memory_order_acquire) !=
                 0 ||
             s_postCloseCoverDeadlineTicks.load(std::memory_order_acquire) !=
                 0);
    }

    bool VRCompositorHelper::ApplySceneFade(bool a_log)
    {
        if (!s_fadeToColor || !s_compositor) {
            if (a_log) {
                logger::warn(
                    "VRCompositor: scene fade unavailable; cannot latch "
                    "opaque scene black");
            }
            return false;
        }
        // seconds=0 (instant), opaque black, background=false so it applies to
        // the scene layer rather than the compositor background.
        s_fadeToColor(s_compositor, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false);
        s_sceneFadeHeld.store(true, std::memory_order_release);
        if (a_log) {
            logger::info(
                "VRCompositor: scene fade APPLIED (alpha 1.0, instant)");
        }
        return true;
    }

    void VRCompositorHelper::ClearSceneFade(float a_seconds)
    {
        if (!s_fadeToColor || !s_compositor) return;
        // Idempotent and unconditional: this must run even if the fade was
        // never applied, so a leaked blackout can never survive into gameplay.
        s_fadeToColor(
            s_compositor, a_seconds, 0.0f, 0.0f, 0.0f, 0.0f, false);
        if (s_sceneFadeHeld.exchange(false, std::memory_order_acq_rel)) {
            logger::info(
                "VRCompositor: scene fade CLEARED over {:.2f}s", a_seconds);
        }
    }

    float VRCompositorHelper::CurrentGridAlpha()
    {
        if (!s_getGridAlpha || !s_compositor) return -1.0f;
        return s_getGridAlpha(s_compositor);
    }

    bool VRCompositorHelper::TickPostCloseHold()
    {
        // Deferred skybox restore, claimed by exactly one tick. Only ever
        // restores when no presentation is open, so a new load can never be
        // un-blackened by the previous load's timer.
        const auto skyboxAt =
            s_skyboxClearAtTicks.load(std::memory_order_acquire);
        auto expectedSkyboxAt = skyboxAt;
        if (skyboxAt != 0 &&
            std::chrono::steady_clock::now().time_since_epoch().count() >=
                skyboxAt &&
            s_skyboxClearAtTicks.compare_exchange_strong(
                expectedSkyboxAt, 0,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::lock_guard overlayLock(s_overlayMutex);
            if (!s_loadingPresentationOpen && s_clearSkyboxOverride &&
                s_compositor) {
                s_clearSkyboxOverride(s_compositor);
            }
        }

        // The manager decides when the world transition is ready. This helper
        // then performs a two-CALLBACK visual handoff: first establish opaque
        // scene black underneath the still-visible custom artwork; only after
        // later accepted L->R submissions may the artwork be retired and the
        // fade cleared. Applying alpha 1 and immediately targeting alpha 0 in
        // one callback lets OpenVR coalesce the first command, which is exactly
        // what the v2.3.2 field log exposed.
        const auto releaseAt =
            s_postCloseHoldDeadlineTicks.load(std::memory_order_acquire);
        if (releaseAt == 0) {
            return false;
        }
        const auto holdGeneration =
            s_postCloseHoldGeneration.load(std::memory_order_acquire);
        if (holdGeneration == 0) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now.time_since_epoch().count() < releaseAt) {
            return false;
        }

        // Finish every ownership check and visual state change under the
        // recursive overlay lock. Boundary code uses the same lock, so an old
        // callback can never arm or release against a newer presentation.
        std::lock_guard overlayLock(s_overlayMutex);
        if (s_loadingPresentationOpen ||
            s_postCloseHoldGeneration.load(std::memory_order_acquire) !=
                holdGeneration ||
            s_postCloseHoldDeadlineTicks.load(std::memory_order_acquire) !=
                releaseAt) {
            return false;
        }

        auto fadeLatchGeneration =
            s_postCloseFadeLatchGeneration.load(std::memory_order_acquire);
        if (fadeLatchGeneration == 0) {
            // Keep every overlay up while making the scene beneath it opaque.
            // Publish the generation only AFTER FadeToColor returns, so a
            // Submit already in flight when this command was issued cannot be
            // credited as post-fade compositor work.
            if (!ApplySceneFade()) {
                // Fail open if the required compositor primitive disappeared.
                // The helper cannot honestly call this a latched reveal, and
                // retaining custom overlays indefinitely would be worse.
                auto expectedRelease = releaseAt;
                if (!s_postCloseHoldDeadlineTicks.compare_exchange_strong(
                        expectedRelease, 0,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return false;
                }
                logger::error(
                    "VRCompositor: post-close fade latch unavailable; "
                    "releasing custom presentation fail-open");
                EndLoadingPresentationNow();
                return true;
            }
            const auto fadeAppliedAt = std::chrono::steady_clock::now();
            {
                std::lock_guard latchLock(s_postCloseFadeLatchMutex);
                if (++s_postCloseFadeLatchArmSerial == 0) {
                    ++s_postCloseFadeLatchArmSerial;
                }
                fadeLatchGeneration = s_postCloseFadeLatchArmSerial;
                s_postCloseFadeLatchStateGeneration = fadeLatchGeneration;
                s_postCloseFadeLatchHoldGeneration = holdGeneration;
                s_postCloseFadeLatchAtTicks =
                    fadeAppliedAt.time_since_epoch().count();
                s_postCloseFadeLatchSawLeft = false;
                s_postCloseFadeLatchPairs = 0;
                s_postCloseFadeLatchGeneration.store(
                    fadeLatchGeneration, std::memory_order_release);
            }
            logger::info(
                "VRCompositor: post-close scene fade latch ARMED "
                "(arm={}, holdGeneration={}, artwork retained, pairs={}, "
                "floor={} ms, backstop={} ms)",
                fadeLatchGeneration, holdGeneration,
                Policy::kVRSceneFadeLatchRequiredPairs,
                Policy::kVRSceneFadeLatchMinimumMs,
                Policy::kVRSceneFadeLatchBackstopMs);
            return false;
        }

        int acceptedPairs = 0;
        std::int64_t latchElapsedMs = 0;
        Policy::VRSceneFadeLatchDecision latchDecision =
            Policy::VRSceneFadeLatchDecision::kWait;
        {
            std::lock_guard latchLock(s_postCloseFadeLatchMutex);
            if (s_postCloseFadeLatchStateGeneration !=
                    fadeLatchGeneration ||
                s_postCloseFadeLatchHoldGeneration != holdGeneration ||
                s_postCloseFadeLatchGeneration.load(
                    std::memory_order_acquire) != fadeLatchGeneration ||
                s_postCloseFadeLatchAtTicks == 0) {
                return false;
            }
            acceptedPairs = s_postCloseFadeLatchPairs;
            const auto latchAt = std::chrono::steady_clock::time_point{
                std::chrono::steady_clock::duration{
                    s_postCloseFadeLatchAtTicks} };
            latchElapsedMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - latchAt).count();
            latchDecision = Policy::DecideVRSceneFadeLatch(
                true, latchElapsedMs, acceptedPairs);
            if (latchDecision !=
                Policy::VRSceneFadeLatchDecision::kWait) {
                // Single final-release owner. Keep the latch mutex held while
                // claiming the deadline so no accepted callback can mutate
                // the state between the decision and disarm.
                auto expectedRelease = releaseAt;
                if (!s_postCloseHoldDeadlineTicks.compare_exchange_strong(
                        expectedRelease, 0,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return false;
                }
                s_postCloseFadeLatchGeneration.store(
                    0, std::memory_order_release);
                s_postCloseFadeLatchStateGeneration = 0;
                s_postCloseFadeLatchHoldGeneration = 0;
                s_postCloseFadeLatchAtTicks = 0;
                s_postCloseFadeLatchSawLeft = false;
                s_postCloseFadeLatchPairs = 0;
            }
        }

        if (latchDecision == Policy::VRSceneFadeLatchDecision::kWait) {
            return false;
        }

        // Carry native-menu suppression a little past the reveal, so the
        // engine's own LoadingMenu fade-out cannot draw into the frame the
        // moment the overlays leave.
        D3D11Compositor::GetSingleton()
            .ExtendPostCloseLoadingMenuSuppression(
                kPostCoverReleaseSuppressMarginMs);
        RequestEyeSample(kEyeSampleCoverRelease);

        const bool tipsWereVisible =
            s_tipsOverlayActive.load(std::memory_order_acquire);
        const bool artWasVisible =
            s_bgOverlayActive.load(std::memory_order_acquire);
        // Opaque black has now survived later accepted compositor work. Do not
        // reapply it here: the entire point of the latch is to keep this clear
        // in a later callback than the final alpha-1 command.
        EndLoadingPresentationNow(kSceneFadeUpSeconds, false);
        const char* releaseReason =
            latchDecision == Policy::VRSceneFadeLatchDecision::kFreshPairs
                ? "post-arm-accepted-pairs"
                : "fade-latch-backstop";
        logger::info(
            "VROverlay: post-close release (latched scene fade, tips={}, "
            "background={}, pairs={}, latchElapsed={} ms, reason={}, "
            "sceneFadeReassertRequested=false, sceneFadeUp={:.2f}s, "
            "gridAlpha={:.3f})",
            tipsWereVisible, artWasVisible,
            acceptedPairs, latchElapsedMs, releaseReason,
            kSceneFadeUpSeconds, CurrentGridAlpha());
        return true;
    }

    void VRCompositorHelper::RequestEyeSample(int a_tag)
    {
        // Opt-in: the sample is a blocking Map on the submit thread at the
        // exact release boundary. That is the same class of render-thread work
        // that reintroduced the flash in 2.1.7, so it must never run unless a
        // diagnostic session explicitly asked for it.
        if (!LoadingScreenManager::IsVRPresentationProbeEnabled()) {
            return;
        }
        s_eyeSampleRequest.store(a_tag, std::memory_order_release);
        s_eyeSampleRemaining.store(
            a_tag == kEyeSampleCoverRelease ? kEyeSampleBurstFrames : 1,
            std::memory_order_release);
    }

    int VRCompositorHelper::PeekEyeSampleRequest()
    {
        if (s_eyeSampleRemaining.load(std::memory_order_acquire) <= 0) {
            return 0;
        }
        const int tag = s_eyeSampleRequest.load(std::memory_order_acquire);
        if (tag == 0) return 0;
        const int left = s_eyeSampleRemaining.load(std::memory_order_acquire);
        return tag | ((left - 1) << 8);
    }

    int VRCompositorHelper::ConsumeEyeSampleRequest()
    {
        if (s_eyeSampleRemaining.load(std::memory_order_acquire) <= 0) {
            return 0;
        }
        const int left =
            s_eyeSampleRemaining.fetch_sub(1, std::memory_order_acq_rel);
        if (left <= 0) {
            s_eyeSampleRemaining.store(0, std::memory_order_release);
            return 0;
        }
        const int tag = s_eyeSampleRequest.load(std::memory_order_acquire);
        // Encode which frame of the burst this is, so the log shows the
        // sequence the headset actually displayed rather than one snapshot.
        return tag | ((left - 1) << 8);
    }

    std::uint64_t
    VRCompositorHelper::CurrentPostCloseFadeLatchGeneration()
    {
        return s_postCloseFadeLatchGeneration.load(
            std::memory_order_acquire);
    }

    bool VRCompositorHelper::InvalidatePostCloseFadeLatch()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        const auto invalidatedArm =
            s_postCloseFadeLatchGeneration.load(std::memory_order_acquire);
        if (invalidatedArm == 0) {
            return false;
        }
        ResetPostCloseFadeLatch();
        logger::info(
            "VRCompositor: post-close scene fade latch invalidated "
            "(arm={}, artwork retained; readiness must re-arm)",
            invalidatedArm);
        return true;
    }

    void VRCompositorHelper::OnGameEyeSubmitResult(
        int eye, bool accepted,
        std::uint64_t a_fadeLatchGenerationAtEntry)
    {
        if ((eye != 0 && eye != 1) ||
            a_fadeLatchGenerationAtEntry == 0) {
            return;
        }

        std::lock_guard latchLock(s_postCloseFadeLatchMutex);
        if (s_postCloseFadeLatchStateGeneration !=
                a_fadeLatchGenerationAtEntry ||
            s_postCloseFadeLatchGeneration.load(
                std::memory_order_acquire) !=
                a_fadeLatchGenerationAtEntry) {
            return;
        }

        const auto next = Policy::AdvanceVRSceneFadePair(
            Policy::VRSceneFadePairState{
                s_postCloseFadeLatchSawLeft,
                s_postCloseFadeLatchPairs },
            eye, accepted);
        s_postCloseFadeLatchSawLeft = next.sawAcceptedLeft;
        s_postCloseFadeLatchPairs = next.acceptedPairs;
    }

    bool VRCompositorHelper::ShowTipsOverlay(
        void* d3dTexture, bool stereoSideBySide)
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen) return false;
        if (!s_overlayInitialized.load(std::memory_order_acquire) || !s_overlay ||
            !s_tipsOverlayHandle || !d3dTexture) return false;

        VRTexture tex;
        tex.handle = d3dTexture;
        tex.eType = 0;
        tex.eColorSpace = 1;

        const int textureErr =
            s_ovrSetTexture(s_overlay, s_tipsOverlayHandle, &tex);
        if (textureErr == 0) {
            RetainShownTexture(s_tipsShownTexture, d3dTexture);
        }

        // Historical callers could supply left/right halves. Active mode 3 is
        // always monoscopic, but explicitly disable the persistent flag so an
        // older overlay state cannot crop its LoadingMenu delta.
        const int stereoFlagErr = s_ovrSetOverlayFlag
            ? s_ovrSetOverlayFlag(
                s_overlay, s_tipsOverlayHandle,
                kOverlayFlagSideBySideParallel018, stereoSideBySide)
            : -1;
        const float fullBounds[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        const int boundsErr = s_ovrSetTextureBounds
            ? s_ovrSetTextureBounds(
                s_overlay, s_tipsOverlayHandle, fullBounds)
            : -1;
        const int texelAspectErr = s_ovrSetTexelAspect
            ? s_ovrSetTexelAspect(
                s_overlay, s_tipsOverlayHandle, 1.0f)
            : -1;

        D3D11_TEXTURE2D_DESC textureDesc{};
        static_cast<ID3D11Texture2D*>(d3dTexture)->GetDesc(&textureDesc);
        float sampledAspect = 0.0f;
        if (textureDesc.Width > 0 && textureDesc.Height > 0) {
            const float sampledWidth = stereoSideBySide
                ? static_cast<float>(textureDesc.Width) * 0.5f
                : static_cast<float>(textureDesc.Width);
            sampledAspect =
                sampledWidth / static_cast<float>(textureDesc.Height);
        }

        // Preserve the source UI's geometry and contain it inside the 16:9
        // background. A square Scaleform layer therefore occupies the same
        // centered-square region used by CompositeTipsIntoBg.
        const float bgWidth =
            s_bgWidthSetting.load(std::memory_order_acquire);
        const float tipsWidth = sampledAspect > 0.0f &&
            sampledAspect < kVRLoadingScreenAspect
            ? bgWidth * sampledAspect / kVRLoadingScreenAspect
            : bgWidth;
        const int widthErr = sampledAspect > 0.0f
            ? s_ovrSetWidth(
                s_overlay, s_tipsOverlayHandle, tipsWidth)
            : -1;
        const int alphaErr =
            s_ovrSetAlpha(s_overlay, s_tipsOverlayHandle, 1.0f);
        const int transformErr =
            UpdateTipsOverlayTransform() ? 0 : -1;

        int showErr = -1;
        if (textureErr == 0 && stereoFlagErr == 0 &&
            boundsErr == 0 && texelAspectErr == 0 &&
            widthErr == 0 && alphaErr == 0 && transformErr == 0) {
            showErr = s_ovrShow(s_overlay, s_tipsOverlayHandle);
        }
        if (textureErr != 0 || stereoFlagErr != 0 ||
            boundsErr != 0 || texelAspectErr != 0 ||
            widthErr != 0 || alphaErr != 0 ||
            transformErr != 0 || showErr != 0) {
            const int hideErr = s_ovrHide(s_overlay, s_tipsOverlayHandle);
            s_tipsOverlayActive.store(
                hideErr != 0, std::memory_order_release);
            if (hideErr == 0) {
                RetainShownTexture(s_tipsShownTexture, nullptr);
            }
            logger::warn(
                "VROverlay: tips attach failed "
                "(texture={}, stereoFlag={}, bounds={}, texelAspect={}, "
                "width={}, alpha={}, transform={}, show={}, cleanup={})",
                textureErr, stereoFlagErr, boundsErr, texelAspectErr,
                widthErr, alphaErr, transformErr, showErr, hideErr);
            return false;
        }
        s_tipsOverlayActive.store(true, std::memory_order_release);
        logger::info(
            "VROverlay: tips shown ({}x{}, sampledAspect={:.3f}, width={:.2f}m, stereo={})",
            textureDesc.Width, textureDesc.Height, sampledAspect, tipsWidth,
            stereoSideBySide);
        return true;
    }

    bool VRCompositorHelper::HideTipsOverlay()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_tipsOverlayHandle) {
            s_tipsOverlayActive.store(false, std::memory_order_release);
            RetainShownTexture(s_tipsShownTexture, nullptr);
            return true;
        }
        if (!s_tipsOverlayActive.load(std::memory_order_acquire)) {
            RetainShownTexture(s_tipsShownTexture, nullptr);
            return true;
        }
        const int err = s_ovrHide(s_overlay, s_tipsOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: tips hide failed (error {})", err);
            return false;
        }
        s_tipsOverlayActive.store(false, std::memory_order_release);
        RetainShownTexture(s_tipsShownTexture, nullptr);
        return true;
    }

    bool VRCompositorHelper::UpdateTipsOverlayTransform()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        // Match v1.5.0's working ShowCapturedFrameOverlay behaviour: tips
        // overlay uses the EXACT bg world pose with no Z offset. Bg sort 200,
        // tips sort 210 — sort order alone puts tips on top of bg in the same
        // plane, exactly as if the pixels were baked into the bg texture.
        //
        // The previous −2cm "forward" offset pushed tips behind bg in 3D
        // space (overlay normal direction conflict), causing the engine to
        // depth-occlude them despite the higher sort. That's where the
        // "half hidden by background" symptom came from.
        if (!s_overlayInitialized.load(std::memory_order_acquire) ||
            !s_overlay || !s_tipsOverlayHandle) return false;
        if (!s_bgOverlayActive.load(std::memory_order_acquire)) return false;

        int err = -1;
        if (s_bgWorldPoseValid.load(std::memory_order_acquire)) {
            err = s_ovrSetTransformAbs(
                s_overlay, s_tipsOverlayHandle, 1, &s_bgWorldPose);
        } else {
            HmdMatrix34 devRel = {};
            devRel.m[0][0] = 1.0f;
            devRel.m[1][1] = 1.0f;
            devRel.m[2][2] = 1.0f;
            devRel.m[2][3] = s_overlayMode.load(std::memory_order_acquire) == 0 ? -3.0f : -7.0f;
            err = s_ovrSetTransformDevRel(
                s_overlay, s_tipsOverlayHandle, 0, &devRel);
        }
        if (err != 0) {
            logger::warn(
                "VROverlay: tips transform update failed (error {})", err);
        }
        return err == 0;
    }

    // ========================================================================
    // DDS loading. Flat mode maps legacy DXT1/DXT5 payloads directly to BC1/BC3
    // resources. SteamVR rejects Fallout's BGRA8 and BC3 DDS resources with
    // VROverlayError_InvalidTexture, so VR normalizes only the top mip to the
    // conservative RGBA8/one-mip overlay shape on this low-priority worker.
    // ========================================================================

    namespace
    {
        constexpr std::size_t kDDSHeaderBytes = 128;
        constexpr std::uint32_t kDDSHeaderSize = 124;
        constexpr std::uint32_t kDDSPixelFormatSize = 32;
        constexpr std::uint32_t kDDSRequiredFlags = 0x00001007;  // CAPS|HEIGHT|WIDTH|PIXELFORMAT
        constexpr std::uint32_t kDDPFourCC = 0x00000004;
        constexpr std::uint32_t kDDPRGB = 0x00000040;
        constexpr std::uint32_t kDDSCaps2CubeMask = 0x0000FE00;
        constexpr std::uint32_t kDDSCaps2Volume = 0x00200000;
        constexpr std::uint32_t kMaxTextureDimension = 16384;
        constexpr std::uint64_t kMaxDDSFileBytes = 512ull * 1024ull * 1024ull;

        constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
        {
            return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
        }

        bool ReadDDSU32(const std::vector<std::uint8_t>& a_data, std::size_t a_offset,
            std::uint32_t& a_value)
        {
            if (a_offset > a_data.size() || sizeof(a_value) > a_data.size() - a_offset) {
                return false;
            }
            std::memcpy(&a_value, a_data.data() + a_offset, sizeof(a_value));
            return true;
        }

        bool CheckedMul(std::size_t a_left, std::size_t a_right, std::size_t& a_result)
        {
            if (a_left != 0 && a_right > std::numeric_limits<std::size_t>::max() / a_left) {
                return false;
            }
            a_result = a_left * a_right;
            return true;
        }

        struct DeviceReference
        {
            explicit DeviceReference(REX::W32::ID3D11Device* a_device) : device(a_device)
            {}
            ~DeviceReference()
            {
                if (device) {
                    device->Release();
                }
            }
            DeviceReference(const DeviceReference&) = delete;
            DeviceReference& operator=(const DeviceReference&) = delete;

            REX::W32::ID3D11Device* device;
        };
    }

    void* VRCompositorHelper::LoadDDSTexture(const std::string& filePath,
        DDSWorkShouldDefer shouldDefer, void* deferContext,
        DDSUploadTryBegin tryBeginUpload, DDSUploadEnd endUpload)
    {
        const auto deferRequested = [shouldDefer, deferContext]() {
            return shouldDefer && shouldDefer(deferContext);
        };

        // LoadingMenu wins every phase boundary. In particular, do not even
        // open the file when a timed load is already active.
        if (deferRequested()) {
            return nullptr;
        }

        // ID3D11Device resource creation is free-threaded unless the device was
        // explicitly created SINGLETHREADED. Publish/load the renderer pointer
        // atomically and keep it alive across the asynchronous disk read.
        REX::W32::ID3D11Device* device = nullptr;
        {
            // Pair the load and AddRef with SetDevice's retirement mutex. An
            // atomic pointer alone cannot keep the old renderer device alive
            // between load and AddRef during a concurrent device replacement.
            std::lock_guard deviceLock(s_deviceMutex);
            device = s_device.load(std::memory_order_acquire);
            if (device) {
                device->AddRef();
            }
        }
        if (!device) {
            logger::warn("LoadDDS: no D3D11 device");
            return nullptr;
        }
        DeviceReference deviceRef(device);
        if ((device->GetCreationFlags() & REX::W32::D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
            logger::warn("LoadDDS: renderer device is SINGLETHREADED; worker-side texture creation is unsafe");
            return nullptr;
        }

        if (deferRequested()) {
            return nullptr;
        }
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            logger::warn("LoadDDS: failed to open {}", filePath);
            return nullptr;
        }

        const std::streampos endPosition = file.tellg();
        if (endPosition == std::streampos(-1)) {
            logger::warn("LoadDDS: tellg failed for {}", filePath);
            return nullptr;
        }
        const std::streamoff endOffset = static_cast<std::streamoff>(endPosition);
        if (endOffset < static_cast<std::streamoff>(kDDSHeaderBytes) ||
            static_cast<std::uint64_t>(endOffset) > kMaxDDSFileBytes) {
            logger::warn("LoadDDS: invalid file size {} for {}", endOffset, filePath);
            return nullptr;
        }
        const auto fileSize = static_cast<std::size_t>(endOffset);
        file.seekg(0, std::ios::beg);
        if (!file) {
            logger::warn("LoadDDS: seek failed for {}", filePath);
            return nullptr;
        }

        std::vector<std::uint8_t> data;
        try {
            data.resize(fileSize);
        } catch (const std::bad_alloc&) {
            logger::warn("LoadDDS: allocation failed for {} bytes ({})", fileSize, filePath);
            return nullptr;
        }

        // Read in bounded chunks so a load that opens mid-read can stop the
        // worker after the current OS request instead of letting a large DDS
        // continue competing with save/cell IO.
        constexpr std::size_t kReadChunkBytes = 1024 * 1024;
        std::size_t totalBytesRead = 0;
        while (totalBytesRead < fileSize) {
            if (deferRequested()) {
                return nullptr;
            }
            const std::size_t requested = std::min(
                kReadChunkBytes, fileSize - totalBytesRead);
            file.read(
                reinterpret_cast<char*>(data.data() + totalBytesRead),
                static_cast<std::streamsize>(requested));
            const auto bytesRead = file.gcount();
            if (bytesRead != static_cast<std::streamsize>(requested)) {
                logger::warn("LoadDDS: read failed/short ({} of {} bytes) from {}",
                    totalBytesRead + static_cast<std::size_t>(std::max<std::streamsize>(0, bytesRead)),
                    fileSize, filePath);
                return nullptr;
            }
            totalBytesRead += requested;
        }
        if (deferRequested()) {
            return nullptr;
        }

        if (std::memcmp(data.data(), "DDS ", 4) != 0) {
            logger::warn("LoadDDS: invalid magic in {}", filePath);
            return nullptr;
        }

        std::uint32_t headerSize = 0, headerFlags = 0, height = 0, width = 0;
        std::uint32_t depth = 0, declaredMipLevels = 0, pfSize = 0, pfFlags = 0;
        std::uint32_t fourCC = 0, bpp = 0, rMask = 0, gMask = 0, bMask = 0, aMask = 0;
        std::uint32_t caps2 = 0;
        if (!ReadDDSU32(data, 4, headerSize) || !ReadDDSU32(data, 8, headerFlags) ||
            !ReadDDSU32(data, 12, height) || !ReadDDSU32(data, 16, width) ||
            !ReadDDSU32(data, 24, depth) || !ReadDDSU32(data, 28, declaredMipLevels) ||
            !ReadDDSU32(data, 76, pfSize) || !ReadDDSU32(data, 80, pfFlags) ||
            !ReadDDSU32(data, 84, fourCC) || !ReadDDSU32(data, 88, bpp) ||
            !ReadDDSU32(data, 92, rMask) || !ReadDDSU32(data, 96, gMask) ||
            !ReadDDSU32(data, 100, bMask) || !ReadDDSU32(data, 104, aMask) ||
            !ReadDDSU32(data, 112, caps2)) {
            logger::warn("LoadDDS: incomplete header in {}", filePath);
            return nullptr;
        }
        if (headerSize != kDDSHeaderSize || pfSize != kDDSPixelFormatSize ||
            (headerFlags & kDDSRequiredFlags) != kDDSRequiredFlags) {
            logger::warn("LoadDDS: malformed header (size={}, pfSize={}, flags={:x}) in {}",
                headerSize, pfSize, headerFlags, filePath);
            return nullptr;
        }
        if (width == 0 || height == 0 || width > kMaxTextureDimension ||
            height > kMaxTextureDimension || depth > 1 ||
            (caps2 & (kDDSCaps2CubeMask | kDDSCaps2Volume)) != 0) {
            logger::warn("LoadDDS: unsupported dimensions/resource ({}x{}, depth={}, caps2={:x}) in {}",
                width, height, depth, caps2, filePath);
            return nullptr;
        }

        std::uint32_t maxMipLevels = 1;
        for (std::uint32_t dim = std::max(width, height); dim > 1; dim >>= 1) {
            ++maxMipLevels;
        }
        const std::uint32_t mipLevels = declaredMipLevels == 0 ? 1 : declaredMipLevels;
        if (mipLevels > maxMipLevels) {
            logger::warn("LoadDDS: invalid mip count {} (max {} for {}x{}) in {}",
                mipLevels, maxMipLevels, width, height, filePath);
            return nullptr;
        }

        bool blockCompressed = false;
        bool forceOpaqueAlpha = false;
        std::uint32_t bytesPerBlock = 0;
        REX::W32::DXGI_FORMAT format = REX::W32::DXGI_FORMAT_UNKNOWN;
        const char* formatTag = nullptr;

        if ((pfFlags & kDDPFourCC) != 0) {
            if (fourCC == MakeFourCC('D', 'X', 'T', '1')) {
                format = REX::W32::DXGI_FORMAT_BC1_UNORM;
                formatTag = "BC1";
                bytesPerBlock = 8;
                blockCompressed = true;
            } else if (fourCC == MakeFourCC('D', 'X', 'T', '5')) {
                format = REX::W32::DXGI_FORMAT_BC3_UNORM;
                formatTag = "BC3";
                bytesPerBlock = 16;
                blockCompressed = true;
            } else {
                logger::warn("LoadDDS: unsupported FOURCC {:08X} in {}", fourCC, filePath);
                return nullptr;
            }
        } else if ((pfFlags & kDDPRGB) != 0) {
            if (aMask != 0 && aMask != 0xff000000) {
                logger::warn("LoadDDS: unsupported alpha mask {:x} in {}", aMask, filePath);
                return nullptr;
            }
            if (bpp == 32 && rMask == 0x000000ff && gMask == 0x0000ff00
                          && bMask == 0x00ff0000) {
                format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
                formatTag = (aMask == 0xff000000) ? "RGBA8" : "RGBX8";
                forceOpaqueAlpha = (aMask == 0);
            } else if (bpp == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00
                                 && bMask == 0x000000ff) {
                format = aMask == 0xff000000 ? REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM :
                    REX::W32::DXGI_FORMAT_B8G8R8X8_UNORM;
                formatTag = (aMask == 0xff000000) ? "BGRA8" : "BGRX8";
                forceOpaqueAlpha = (aMask == 0);
            } else {
                logger::warn("LoadDDS: unsupported uncompressed format (bpp={} R={:x} G={:x} B={:x} A={:x})",
                    bpp, rMask, gMask, bMask, aMask);
                return nullptr;
            }
        } else {
            logger::warn("LoadDDS: pixel format flags={:x} not supported in {}", pfFlags, filePath);
            return nullptr;
        }

        std::vector<REX::W32::D3D11_SUBRESOURCE_DATA> initialData;
        initialData.reserve(mipLevels);
        std::size_t cursor = kDDSHeaderBytes;
        std::uint32_t mipWidth = width;
        std::uint32_t mipHeight = height;
        for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
            if (deferRequested()) {
                return nullptr;
            }
            const std::size_t rowUnits = blockCompressed ?
                std::max<std::size_t>(1, (static_cast<std::size_t>(mipWidth) + 3) / 4) : mipWidth;
            const std::size_t rowCount = blockCompressed ?
                std::max<std::size_t>(1, (static_cast<std::size_t>(mipHeight) + 3) / 4) : mipHeight;
            std::size_t rowPitch = 0;
            std::size_t slicePitch = 0;
            if (!CheckedMul(rowUnits, blockCompressed ? bytesPerBlock : 4u, rowPitch) ||
                !CheckedMul(rowPitch, rowCount, slicePitch) ||
                rowPitch > std::numeric_limits<std::uint32_t>::max() ||
                slicePitch > std::numeric_limits<std::uint32_t>::max() ||
                cursor > data.size() || slicePitch > data.size() - cursor) {
                logger::warn("LoadDDS: truncated/overflowing mip {} ({}x{}) in {}",
                    mip, mipWidth, mipHeight, filePath);
                return nullptr;
            }

            REX::W32::D3D11_SUBRESOURCE_DATA subresource{};
            if (forceOpaqueAlpha) {
                // R8G8B8X8 has no DXGI X-channel format. We expose it as RGBA,
                // so normalize the undefined X byte or SteamVR may interpret a
                // zero-filled X channel as transparent overlay alpha.
                for (std::size_t row = 0; row < rowCount; ++row) {
                    auto* rowData = data.data() + cursor + row * rowPitch;
                    for (std::size_t x = 0; x < mipWidth; ++x) {
                        rowData[x * 4 + 3] = 0xff;
                    }
                }
            }
            subresource.sysMem = data.data() + cursor;
            subresource.sysMemPitch = static_cast<std::uint32_t>(rowPitch);
            subresource.sysMemSlicePitch = static_cast<std::uint32_t>(slicePitch);
            initialData.push_back(subresource);
            cursor += slicePitch;
            mipWidth = std::max(1u, mipWidth >> 1);
            mipHeight = std::max(1u, mipHeight >> 1);
        }

        std::vector<std::uint8_t> overlayPixels;
        if (s_isVR) {
            using DDSOverlayCodec::SourceFormat;
            SourceFormat overlaySourceFormat = SourceFormat::kRGBA8;
            if (blockCompressed) {
                overlaySourceFormat = bytesPerBlock == 8
                    ? SourceFormat::kBC1
                    : SourceFormat::kBC3;
            } else if (format == REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM ||
                       format == REX::W32::DXGI_FORMAT_B8G8R8X8_UNORM) {
                overlaySourceFormat = SourceFormat::kBGRA8;
            }

            const auto* topMip = static_cast<const std::uint8_t*>(
                initialData.front().sysMem);
            const std::span<const std::uint8_t> topMipPayload(
                topMip, initialData.front().sysMemSlicePitch);
            if (!DDSOverlayCodec::DecodeTopMipToRGBA8(
                    topMipPayload, width, height, overlaySourceFormat,
                    forceOpaqueAlpha, overlayPixels,
                    shouldDefer, deferContext)) {
                if (deferRequested()) {
                    return nullptr;
                }
                logger::warn(
                    "LoadDDS: VR overlay normalization failed "
                    "({}x{} {}, {} top-mip bytes)",
                    width, height, formatTag,
                    initialData.front().sysMemSlicePitch);
                return nullptr;
            }

            REX::W32::D3D11_SUBRESOURCE_DATA overlayData{};
            overlayData.sysMem = overlayPixels.data();
            overlayData.sysMemPitch = width * 4;
            overlayData.sysMemSlicePitch =
                static_cast<std::uint32_t>(overlayPixels.size());
            initialData.assign(1, overlayData);
            format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
        }

        REX::W32::D3D11_TEXTURE2D_DESC desc = {};
        desc.width = width;
        desc.height = height;
        desc.mipLevels = s_isVR ? 1 : mipLevels;
        desc.arraySize = 1;
        desc.format = format;
        desc.sampleDesc.count = 1;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE |
            (s_isVR ? REX::W32::D3D11_BIND_RENDER_TARGET : 0);

        // Creating a DEFAULT texture with initial data performs the GPU upload.
        // This is the final gate: never begin it while a timed load is active.
        if (deferRequested()) {
            return nullptr;
        }
        if (tryBeginUpload && !tryBeginUpload(deferContext)) {
            return nullptr;
        }
        struct UploadAdmissionGuard
        {
            DDSUploadEnd end = nullptr;
            void* context = nullptr;
            ~UploadAdmissionGuard()
            {
                if (end) end(context);
            }
        } uploadAdmission{
            tryBeginUpload ? endUpload : nullptr,
            deferContext
        };
        REX::W32::ID3D11Texture2D* texture = nullptr;
        const HRESULT hr = device->CreateTexture2D(&desc, initialData.data(), &texture);
        if (FAILED(hr) || !texture) {
            if (texture) {
                texture->Release();
            }
            logger::warn("LoadDDS: CreateTexture2D failed (hr={:08X}, {}x{} {}, {} mip(s))",
                static_cast<std::uint32_t>(hr), width, height, formatTag, mipLevels);
            return nullptr;
        }

        if (s_isVR) {
            logger::info(
                "LoadDDS: {}x{} {} normalized to overlay-safe RGBA8 "
                "(1 mip, {} source bytes -> {} upload bytes)",
                width, height, formatTag, cursor - kDDSHeaderBytes,
                overlayPixels.size());
        } else {
            logger::info(
                "LoadDDS: {}x{} {} uploaded directly ({} mip(s), {} bytes)",
                width, height, formatTag, mipLevels,
                cursor - kDDSHeaderBytes);
        }
        return texture;
    }

    void VRCompositorHelper::ReleaseTexture(void* texture)
    {
        if (texture) {
            static_cast<REX::W32::ID3D11Texture2D*>(texture)->Release();
        }
    }

    // ========================================================================
    // Compositor methods
    // ========================================================================

    void VRCompositorHelper::SuspendRendering(bool suspend)
    {
        if (s_suspendRendering && s_compositor) {
            s_suspendRendering(s_compositor, suspend);
        }
    }

    void VRCompositorHelper::SetBlackSkybox()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        if (!s_loadingPresentationOpen) return;
        // A new load owns the backdrop again: cancel any pending restore from
        // the previous load before publishing black.
        s_skyboxClearAtTicks.store(0, std::memory_order_release);
        // Also drop any scene fade left over from a previous load. The fade is
        // re-applied when this load's NOP lands; carrying one in would blacken
        // the world before the overlays are even up.
        ClearSceneFade(0.0f);
        if (s_setSkyboxOverride && s_compositor && s_blackTexture) {
            VRTexture tex;
            tex.handle = s_blackTexture;
            tex.eType = 0;
            tex.eColorSpace = 1;
            int err = s_setSkyboxOverride(s_compositor, &tex, 1);
            if (err != 0) {
                logger::warn("SetSkyboxOverride (black) failed (error {})", err);
            }
        }
    }

    void VRCompositorHelper::ClearSkybox()
    {
        std::lock_guard overlayLock(s_overlayMutex);
        // Explicit caller (native-mode paths, teardown): restore immediately
        // and cancel any deferred restore so the two cannot race.
        s_skyboxClearAtTicks.store(0, std::memory_order_release);
        if (s_clearSkyboxOverride && s_compositor) {
            s_clearSkyboxOverride(s_compositor);
        }
    }

    // ========================================================================
    // World-locked in-eye background (Option B)
    // ========================================================================

    // Extract 3x3 rotation (first 3 cols of 3x4) into row-major float[9]
    static void HmdRotToArr(const float m[3][4], float out[9])
    {
        out[0] = m[0][0]; out[1] = m[0][1]; out[2] = m[0][2];
        out[3] = m[1][0]; out[4] = m[1][1]; out[5] = m[1][2];
        out[6] = m[2][0]; out[7] = m[2][1]; out[8] = m[2][2];
    }

    // Transpose of a rotation = its inverse (assuming orthonormal)
    static void MatTranspose3(const float in[9], float out[9])
    {
        out[0] = in[0]; out[1] = in[3]; out[2] = in[6];
        out[3] = in[1]; out[4] = in[4]; out[5] = in[7];
        out[6] = in[2]; out[7] = in[5]; out[8] = in[8];
    }

    // out = A * B  (row-major 3x3)
    static void MatMul3(const float A[9], const float B[9], float out[9])
    {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                out[r * 3 + c] = A[r * 3 + 0] * B[0 * 3 + c]
                               + A[r * 3 + 1] * B[1 * 3 + c]
                               + A[r * 3 + 2] * B[2 * 3 + c];
            }
        }
    }

    void VRCompositorHelper::CaptureWorldLockAnchor()
    {
        if (!s_vrSystem || !s_getDeviceToAbsTrackingPose) {
            logger::warn("WorldLock: no IVRSystem, cannot capture anchor");
            return;
        }

        HmdMatrix34 hmd = {};
        bool gotPose = false;

        TrackedDevicePose poses[1] = {};
        s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
        if (poses[0].bPoseIsValid) {
            hmd = poses[0].mDeviceToAbsoluteTracking;
            gotPose = true;
        } else if (s_hasLastKnownPose.load(std::memory_order_acquire)) {
            std::lock_guard poseLock(s_poseMutex);
            if (s_hasLastKnownPose.load(std::memory_order_relaxed)) {
                hmd = s_lastKnownPose;
                gotPose = true;
            }
        }

        if (!gotPose) {
            logger::warn("WorldLock: no valid HMD pose, anchor not captured");
            return;
        }

        std::lock_guard poseLock(s_poseMutex);

        // Retain the raw anchor pose for callers that need an anchored
        // device-to-world transform.
        s_anchorPose = hmd;

        float anchorRot[9];
        HmdRotToArr(hmd.m, anchorRot);

        // Full anchor rotation (yaw + pitch + roll). When the user hasn't moved,
        // composite = inv(anchor) * current = identity → bg samples at the same uv
        // and appears centered in the eye texture. Earlier yaw-only variant caused
        // vertical offset ("bg somewhere to the side") when head had any pitch.
        MatTranspose3(anchorRot, s_anchorRotInv);

        // Populate projection tangents + eye-to-head rotation from SAFE DEFAULTS
        // instead of calling IVRSystem::GetProjectionRaw (vtable[2]) or
        // IVRSystem::GetEyeToHeadTransform (vtable[4]) — those calls reliably
        // corrupted vrclient_x64's IVRSystem vtable pointer in testing, crashing
        // every other VR plugin (FO4VRTools, Heisenberg, VirtualHolsters).
        //
        // For a MONO world-locked background at ~3m distance these defaults are
        // indistinguishable from true per-eye values:
        //   - Eye-to-head rotation: identity (<2° cant on consumer HMDs).
        //   - Projection tangents: symmetric ±tan(half-FOV). Horiz FOV ~100° →
        //     tangent ~1.19. IPD parallax for a mono image at 3m is negligible
        //     (~1° angular error between eyes), so same tangents for both eyes.
        if (!s_eyeDataCached.load(std::memory_order_relaxed)) {
            const float hTan = s_bgProjTanHoriz;
            const float vTan = s_bgProjTanVert;
            for (int eye = 0; eye < 2; eye++) {
                s_eyeProjLeft[eye]   = -hTan;
                s_eyeProjRight[eye]  =  hTan;
                s_eyeProjTop[eye]    = -vTan;
                s_eyeProjBottom[eye] =  vTan;
                // Identity 3x3 row-major
                s_eyeToHeadRot[eye][0] = 1.0f; s_eyeToHeadRot[eye][1] = 0.0f; s_eyeToHeadRot[eye][2] = 0.0f;
                s_eyeToHeadRot[eye][3] = 0.0f; s_eyeToHeadRot[eye][4] = 1.0f; s_eyeToHeadRot[eye][5] = 0.0f;
                s_eyeToHeadRot[eye][6] = 0.0f; s_eyeToHeadRot[eye][7] = 0.0f; s_eyeToHeadRot[eye][8] = 1.0f;
            }
            s_eyeDataCached.store(true, std::memory_order_release);
            logger::info("WorldLock: eye data filled from defaults (hTan={:.2f} vTan={:.2f}, eye-to-head=identity)",
                hTan, vTan);
        }

        s_worldLockAnchorValid.store(true, std::memory_order_release);
        logger::info("WorldLock: anchor captured");
    }

    bool VRCompositorHelper::GetWorldLockAnchorPose(float outPose[3][4])
    {
        if (!s_worldLockAnchorValid.load(std::memory_order_acquire)) return false;
        std::lock_guard poseLock(s_poseMutex);
        if (!s_worldLockAnchorValid.load(std::memory_order_relaxed)) return false;
        std::memcpy(outPose, s_anchorPose.m, sizeof(float) * 12);
        return true;
    }

    bool VRCompositorHelper::GetWorldLockData(int eye, WorldLockEyeData& outData)
    {
        if (!s_worldLockAnchorValid.load(std::memory_order_acquire) ||
            !s_eyeDataCached.load(std::memory_order_acquire)) return false;
        if (eye < 0 || eye > 1) return false;
        if (!s_vrSystem || !s_getDeviceToAbsTrackingPose) return false;

        HmdMatrix34 hmd = {};
        TrackedDevicePose poses[1] = {};
        s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
        if (poses[0].bPoseIsValid) {
            hmd = poses[0].mDeviceToAbsoluteTracking;
        } else if (s_hasLastKnownPose.load(std::memory_order_acquire)) {
            std::lock_guard poseLock(s_poseMutex);
            if (!s_hasLastKnownPose.load(std::memory_order_relaxed)) {
                return false;
            }
            hmd = s_lastKnownPose;
        } else {
            return false;
        }

        float currentRot[9];
        HmdRotToArr(hmd.m, currentRot);

        // composite = inv(eyeToHead) * inv(anchor) * current * eyeToHead
        // Rotates an eye-space ray into anchor-eye-space.
        float anchorRotInv[9];
        float eyeToHead[9];
        float projLeft = 0.0f, projRight = 0.0f, projTop = 0.0f, projBottom = 0.0f;
        {
            std::lock_guard poseLock(s_poseMutex);
            if (!s_worldLockAnchorValid.load(std::memory_order_relaxed) ||
                !s_eyeDataCached.load(std::memory_order_relaxed)) {
                return false;
            }
            std::memcpy(anchorRotInv, s_anchorRotInv, sizeof(anchorRotInv));
            std::memcpy(eyeToHead, s_eyeToHeadRot[eye], sizeof(eyeToHead));
            projLeft = s_eyeProjLeft[eye];
            projRight = s_eyeProjRight[eye];
            projTop = s_eyeProjTop[eye];
            projBottom = s_eyeProjBottom[eye];
        }

        float invEyeToHead[9];
        MatTranspose3(eyeToHead, invEyeToHead);

        float tmp1[9], tmp2[9];
        MatMul3(anchorRotInv, currentRot, tmp1);         // inv(anchor) * current
        MatMul3(tmp1, eyeToHead, tmp2);                  // * eyeToHead
        MatMul3(invEyeToHead, tmp2, outData.compositeRot); // inv(eyeToHead) * ...

        outData.projLeft   = projLeft;
        outData.projRight  = projRight;
        outData.projTop    = projTop;
        outData.projBottom = projBottom;
        return true;
    }
}
