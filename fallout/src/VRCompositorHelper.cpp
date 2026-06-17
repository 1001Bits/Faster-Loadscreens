#include "PCH.h"
#include "VRCompositorHelper.h"

#include <fstream>

namespace VRLoadingScreens
{
    static constexpr std::uintptr_t BSGraphics_RendererData_Offset_VR = 0x060f3ce8;

    bool VRCompositorHelper::Initialize()
    {
        if (s_initialized) return true;

        s_isVR = REL::Module::IsVR();

        // --- D3D11 device (needed for both VR and flat) ---
        if (s_isVR) {
            REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset_VR) };
            void* rendererData = *rendererDataPtr;
            if (rendererData) {
                s_device = *reinterpret_cast<REX::W32::ID3D11Device**>(
                    reinterpret_cast<std::uintptr_t>(rendererData) + 0x48);
            }
        } else {
            // Flat mode: device will be acquired lazily via Present hook
            // (RendererData address library IDs may not work for all NG versions)
            logger::info("VRCompositorHelper: flat mode — device deferred to Present hook");
        }

        if (s_device) {
            // Create 1x1 black texture (VR: skybox fallback, flat: unused but harmless)
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

            REX::W32::ID3D11Texture2D* tex = nullptr;
            HRESULT hr = s_device->CreateTexture2D(&desc, &initData, &tex);
            if (SUCCEEDED(hr) && tex) {
                s_blackTexture = tex;
            }
        }

        // --- VR-only: OpenVR compositor and system ---
        if (s_isVR) {
            HMODULE openvrDll = GetModuleHandleA("openvr_api.dll");
            if (!openvrDll) {
                logger::warn("VRCompositorHelper: openvr_api.dll not loaded");
                // Still mark initialized for D3D device access
                s_initialized = (s_device != nullptr);
                return s_initialized;
            }

            using VR_GetGenericInterfaceFn = void*(*)(const char*, int*);
            auto VR_GetGenericInterface = reinterpret_cast<VR_GetGenericInterfaceFn>(
                GetProcAddress(openvrDll, "VR_GetGenericInterface"));
            if (!VR_GetGenericInterface) {
                s_initialized = (s_device != nullptr);
                return s_initialized;
            }

            int error = 0;
            s_compositor = VR_GetGenericInterface("IVRCompositor_022", &error);
            if (!s_compositor) {
                s_compositor = VR_GetGenericInterface("IVRCompositor_021", &error);
            }
            if (!s_compositor) {
                logger::error("VRCompositorHelper: could not get IVRCompositor (error {})", error);
                s_initialized = (s_device != nullptr);
                return s_initialized;
            }

            void** vtable = *reinterpret_cast<void***>(s_compositor);
            s_getTrackingSpace = reinterpret_cast<GetTrackingSpaceFn>(vtable[1]);
            s_getLastPoses = reinterpret_cast<GetLastPosesFn>(vtable[3]);
            s_fadeToColor = reinterpret_cast<FadeToColorFn>(vtable[12]);
            s_fadeGrid = reinterpret_cast<FadeGridFn>(vtable[14]);
            s_setSkyboxOverride = reinterpret_cast<SetSkyboxOverrideFn>(vtable[16]);
            s_clearSkyboxOverride = reinterpret_cast<ClearSkyboxOverrideFn>(vtable[17]);
            s_suspendRendering = reinterpret_cast<SuspendRenderingFn>(vtable[32]);

            int sysError = 0;
            s_vrSystem = VR_GetGenericInterface("IVRSystem_019", &sysError);
            if (!s_vrSystem) {
                s_vrSystem = VR_GetGenericInterface("IVRSystem_020", &sysError);
            }
            if (s_vrSystem) {
                void** sysVtable = *reinterpret_cast<void***>(s_vrSystem);
                s_getDeviceToAbsTrackingPose = reinterpret_cast<GetDeviceToAbsTrackingPoseFn>(sysVtable[11]);
                logger::info("VRCompositorHelper: IVRSystem initialized");
            }
        }

        s_initialized = (s_device != nullptr);
        logger::info("VRCompositorHelper: initialized (VR={}, device={}, compositor={})",
            s_isVR, s_device != nullptr, s_compositor != nullptr);
        return s_initialized;
    }

    // ========================================================================
    // IVROverlay initialization and management
    // ========================================================================

    bool VRCompositorHelper::InitializeOverlay()
    {
        if (s_overlayInitialized) return true;
        if (!s_device) {
            logger::warn("VROverlay: no D3D11 device");
            return false;
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

        // Create background overlay
        int err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.bg", "VR Loading BG", &s_bgOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (bg) failed (error {})", err);
            return false;
        }

        s_ovrSetWidth(s_overlay, s_bgOverlayHandle, 5.0f);
        s_ovrSetAlpha(s_overlay, s_bgOverlayHandle, 1.0f);
        s_ovrSetSortOrder(s_overlay, s_bgOverlayHandle, 100);
        logger::info("VROverlay: background overlay created (handle={})", s_bgOverlayHandle);

        // Create captured frame overlay (tip text, shown below background)
        err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.captured", "VR Loading Captured", &s_capturedOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (captured) failed (error {})", err);
        } else {
            s_ovrSetWidth(s_overlay, s_capturedOverlayHandle, 5.0f);
            s_ovrSetAlpha(s_overlay, s_capturedOverlayHandle, 1.0f);
            s_ovrSetSortOrder(s_overlay, s_capturedOverlayHandle, 101);
            logger::info("VROverlay: captured frame overlay created (handle={})", s_capturedOverlayHandle);
        }

        // Create black blocker overlay (covers full FOV to hide game's stereo frames)
        err = s_ovrCreateOverlay(s_overlay, "vrloadingscreens.blocker", "VR Loading Blocker", &s_blockerOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: CreateOverlay (blocker) failed (error {})", err);
        } else {
            s_ovrSetWidth(s_overlay, s_blockerOverlayHandle, 30.0f);
            s_ovrSetAlpha(s_overlay, s_blockerOverlayHandle, 1.0f);
            s_ovrSetSortOrder(s_overlay, s_blockerOverlayHandle, 98);  // Behind everything
            logger::info("VROverlay: blocker overlay created (handle={})", s_blockerOverlayHandle);
        }

        s_overlayInitialized = true;
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

    void VRCompositorHelper::SetImageSkybox(void* d3dTexture)
    {
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

    void VRCompositorHelper::ShowBlockerOverlay()
    {
        if (!s_overlayInitialized || !s_overlay || !s_blockerOverlayHandle || !s_blackTexture) return;

        VRTexture blockerTex;
        blockerTex.handle = s_blackTexture;
        blockerTex.eType = 0;
        blockerTex.eColorSpace = 1;
        s_ovrSetTexture(s_overlay, s_blockerOverlayHandle, &blockerTex);
        s_ovrSetAlpha(s_overlay, s_blockerOverlayHandle, 1.0f);

        HmdMatrix34 blockerRel = {};
        blockerRel.m[0][0] = 1.0f;
        blockerRel.m[1][1] = 1.0f;
        blockerRel.m[2][2] = 1.0f;
        blockerRel.m[2][3] = -3.0f;
        s_ovrSetTransformDevRel(s_overlay, s_blockerOverlayHandle, 0, &blockerRel);
        s_ovrShow(s_overlay, s_blockerOverlayHandle);
        logger::info("VROverlay: blocker shown (immediate)");
    }

    void VRCompositorHelper::ShowBackgroundOverlay(void* d3dTexture, int overlayMode, float alpha)
    {
        if (!s_overlayInitialized || !s_overlay || !d3dTexture) return;

        s_overlayMode = overlayMode;

        VRTexture tex;
        tex.handle = d3dTexture;
        tex.eType = 0;
        tex.eColorSpace = 1;

        int err = s_ovrSetTexture(s_overlay, s_bgOverlayHandle, &tex);
        if (err != 0) {
            logger::warn("VROverlay: SetOverlayTexture (bg) failed (error {})", err);
            return;
        }

        // Mode 0: HMD-relative (locked to headset like a cinema screen)
        // Wide enough to cover full FOV (~120°) so no game content peeks through.
        // At 3m distance, 10m width gives 2*atan(5/3) = ~118° horizontal coverage.
        if (overlayMode == 0) {
            s_ovrSetWidth(s_overlay, s_bgOverlayHandle, s_bgWidthSetting);
            s_ovrSetAlpha(s_overlay, s_bgOverlayHandle, 1.0f);  // Fully opaque to hide game's stereo frames
            HmdMatrix34 devRel = {};
            devRel.m[0][0] = 1.0f;
            devRel.m[1][1] = 1.0f;
            devRel.m[2][2] = 1.0f;
            devRel.m[2][3] = -3.0f; // 3m in front of HMD
            int relErr = s_ovrSetTransformDevRel(s_overlay, s_bgOverlayHandle, 0, &devRel);
            logger::info("VROverlay: mode=HMD-relative (10m wide, 3m forward) relErr={}", relErr);
        }
        // Modes 1 & 2: World-locked via SetOverlayTransformAbsolute.
        // Placed in front of HMD at time of loading, stays fixed in world space.
        else {
            float distance = 7.0f;
            s_ovrSetWidth(s_overlay, s_bgOverlayHandle, s_bgWidthSetting);
            s_ovrSetAlpha(s_overlay, s_bgOverlayHandle, 1.0f);  // Fully opaque to hide game's stereo frames

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
            if (!gotPose && s_hasLastKnownPose) {
                hmd = s_lastKnownPose;
                gotPose = true;
                poseSource = "cached";
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

                // Store for per-frame re-application during loading
                s_bgWorldPose = transform;
                s_bgOverlayActive = true;

                int absErr = s_ovrSetTransformAbs(s_overlay, s_bgOverlayHandle, 1, &transform);
                logger::info("VROverlay: mode={} poseSource={} pos=[{:.2f},{:.2f},{:.2f}] fwd=[{:.2f},{:.2f}] err={}",
                    overlayMode == 2 ? "Cinema" : "World-locked", poseSource,
                    transform.m[0][3], transform.m[1][3], transform.m[2][3], fwdX, fwdZ, absErr);
            } else {
                // Fallback: device-relative at large distance (barely perceptible head tracking)
                s_bgOverlayActive = false;
                HmdMatrix34 devRel = {};
                devRel.m[0][0] = 1.0f;
                devRel.m[1][1] = 1.0f;
                devRel.m[2][2] = 1.0f;
                devRel.m[2][3] = -distance;
                s_ovrSetTransformDevRel(s_overlay, s_bgOverlayHandle, 0, &devRel);
                logger::info("VROverlay: no HMD pose available, device-relative fallback at {}m", distance);
            }
        }

        err = s_ovrShow(s_overlay, s_bgOverlayHandle);
        if (err != 0) {
            logger::warn("VROverlay: ShowOverlay (bg) failed (error {})", err);
        } else {
            logger::info("VROverlay: background shown");
        }

        ShowBlockerOverlay();
    }

    void VRCompositorHelper::UpdateBackgroundOverlayTransform(const HmdMatrix34& hmd)
    {
        // Transform world-space pose to device-relative:
        // P_device = R_hmd^T * (P_world - P_hmd)
        // R_device = R_hmd^T * R_world
        float dx = s_bgWorldPose.m[0][3] - hmd.m[0][3];
        float dy = s_bgWorldPose.m[1][3] - hmd.m[1][3];
        float dz = s_bgWorldPose.m[2][3] - hmd.m[2][3];

        float px = hmd.m[0][0] * dx + hmd.m[1][0] * dy + hmd.m[2][0] * dz;
        float py = hmd.m[0][1] * dx + hmd.m[1][1] * dy + hmd.m[2][1] * dz;
        float pz = hmd.m[0][2] * dx + hmd.m[1][2] * dy + hmd.m[2][2] * dz;

        HmdMatrix34 devRel = {};
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                float sum = 0.0f;
                for (int k = 0; k < 3; k++) {
                    sum += hmd.m[k][i] * s_bgWorldPose.m[k][j];
                }
                devRel.m[i][j] = sum;
            }
        }
        devRel.m[0][3] = px;
        devRel.m[1][3] = py;
        devRel.m[2][3] = pz;

        s_ovrSetTransformDevRel(s_overlay, s_bgOverlayHandle, 0, &devRel);
    }

    void VRCompositorHelper::UpdateBackgroundOverlay()
    {
        if (!s_bgOverlayActive || !s_overlayInitialized || !s_overlay) return;
        if (s_overlayMode == 0) return; // mode 0 is device-relative, no update needed

        // Re-apply stored absolute transform every frame to ensure it persists
        s_ovrSetTransformAbs(s_overlay, s_bgOverlayHandle, 1, &s_bgWorldPose);

        // Keep captured frame overlay in sync (using its own stored pose with forward offset)
        if (s_capturedOverlayHandle && s_capturedOverlayActive) {
            s_ovrSetTransformAbs(s_overlay, s_capturedOverlayHandle, 1, &s_capturedWorldPose);
        }
    }

    void VRCompositorHelper::UpdateLastKnownPose()
    {
        if (!s_initialized || !s_vrSystem || !s_getDeviceToAbsTrackingPose) return;

        TrackedDevicePose poses[1] = {};
        s_getDeviceToAbsTrackingPose(s_vrSystem, 1, 0.0f, poses, 1);
        if (poses[0].bPoseIsValid) {
            s_lastKnownPose = poses[0].mDeviceToAbsoluteTracking;
            s_hasLastKnownPose = true;
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
        if (s_hasLastKnownPose) {
            std::memcpy(outPose, s_lastKnownPose.m, sizeof(float) * 12);
            return true;
        }
        return false;
    }

    void VRCompositorHelper::HideBackgroundOverlay()
    {
        if (!s_overlayInitialized || !s_overlay) return;
        s_bgOverlayActive = false;
        s_capturedOverlayActive = false;
        s_updateLogCounter = 0;

        // Hide all overlays FIRST, then clear the fade.
        // Order matters: the tip overlay is alpha-keyed (transparent areas).
        // If the fade clears before overlays hide, game world bleeds through
        // the tip's transparent areas making it look like it disappeared early.
        s_ovrHide(s_overlay, s_bgOverlayHandle);
        if (s_capturedOverlayHandle) {
            s_ovrHide(s_overlay, s_capturedOverlayHandle);
        }
        if (s_blockerOverlayHandle) {
            s_ovrHide(s_overlay, s_blockerOverlayHandle);
        }
        // Now clear the black fade — overlays are already gone
        FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
    }

    void VRCompositorHelper::ShowCapturedFrameOverlay(void* d3dTexture, int overlayMode)
    {
        if (!s_overlayInitialized || !s_overlay || !d3dTexture || !s_capturedOverlayHandle) return;

        VRTexture tex;
        tex.handle = d3dTexture;
        tex.eType = 0;
        tex.eColorSpace = 1;

        int err = s_ovrSetTexture(s_overlay, s_capturedOverlayHandle, &tex);
        if (err != 0) {
            logger::warn("VROverlay: SetOverlayTexture (captured) failed (error {})", err);
            return;
        }

        // Use the same world pose as the background overlay so they always align.
        // Sort order (101 > 100) ensures the captured frame renders on top.
        float width = s_bgWidthSetting;
        s_ovrSetWidth(s_overlay, s_capturedOverlayHandle, width);
        s_ovrSetAlpha(s_overlay, s_capturedOverlayHandle, 1.0f);
        s_ovrSetSortOrder(s_overlay, s_capturedOverlayHandle, 101);

        float bounds[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        s_ovrSetTextureBounds(s_overlay, s_capturedOverlayHandle, bounds);

        if (s_bgOverlayActive) {
            // Start from background overlay's world pose, then apply offsets and rotation
            s_capturedWorldPose = s_bgWorldPose;

            // Offset along overlay's local axes: X (right) and Y (up)
            for (int row = 0; row < 3; row++) {
                s_capturedWorldPose.m[row][3] += s_tipOffsetX * s_bgWorldPose.m[row][0]
                                               + s_tipOffsetY * s_bgWorldPose.m[row][1];
            }

            // Apply yaw (around local Y) then pitch (around local X) if non-zero
            if (std::abs(s_tipYawDeg) > 0.01f) {
                float yawRad = s_tipYawDeg * 3.14159265f / 180.0f;
                float cosY = std::cos(yawRad);
                float sinY = std::sin(yawRad);
                for (int row = 0; row < 3; row++) {
                    float rt = s_capturedWorldPose.m[row][0];
                    float zn = s_capturedWorldPose.m[row][2];
                    s_capturedWorldPose.m[row][0] = cosY * rt - sinY * zn;
                    s_capturedWorldPose.m[row][2] = sinY * rt + cosY * zn;
                }
            }
            if (std::abs(s_tipPitchDeg) > 0.01f) {
                float pitchRad = s_tipPitchDeg * 3.14159265f / 180.0f;
                float cosP = std::cos(pitchRad);
                float sinP = std::sin(pitchRad);
                for (int row = 0; row < 3; row++) {
                    float up = s_capturedWorldPose.m[row][1];
                    float zn = s_capturedWorldPose.m[row][2];
                    s_capturedWorldPose.m[row][1] = cosP * up + sinP * zn;
                    s_capturedWorldPose.m[row][2] = -sinP * up + cosP * zn;
                }
            }

            s_capturedOverlayActive = true;
            s_ovrSetTransformAbs(s_overlay, s_capturedOverlayHandle, 1, &s_capturedWorldPose);
        } else {
            // Fallback: device-relative
            s_capturedOverlayActive = false;
            HmdMatrix34 devRel = {};
            devRel.m[0][0] = 1.0f;
            devRel.m[1][1] = 1.0f;
            devRel.m[2][2] = 1.0f;
            devRel.m[2][3] = -6.0f;
            s_ovrSetTransformDevRel(s_overlay, s_capturedOverlayHandle, 0, &devRel);
        }

        s_ovrShow(s_overlay, s_capturedOverlayHandle);
        logger::info("VROverlay: captured overlay shown ({}m wide, aligned to bg)", width);
    }

    void VRCompositorHelper::HideCapturedFrameOverlay()
    {
        if (!s_overlayInitialized || !s_overlay || !s_capturedOverlayHandle) return;
        s_capturedOverlayActive = false;
        s_ovrHide(s_overlay, s_capturedOverlayHandle);
    }

    // ========================================================================
    // DDS loading with BC3/BC1 decompression to R8G8B8A8
    // ========================================================================

    void VRCompositorHelper::DecompressBC1Block(const std::uint8_t* block, std::uint8_t* output, int stride)
    {
        std::uint16_t c0 = block[0] | (block[1] << 8);
        std::uint16_t c1 = block[2] | (block[3] << 8);

        std::uint8_t colors[4][4]; // RGBA
        colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
        colors[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;
        colors[0][2] = (c0 & 0x1F) * 255 / 31;
        colors[0][3] = 255;
        colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
        colors[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;
        colors[1][2] = (c1 & 0x1F) * 255 / 31;
        colors[1][3] = 255;

        if (c0 > c1) {
            for (int i = 0; i < 3; i++) {
                colors[2][i] = (2 * colors[0][i] + colors[1][i]) / 3;
                colors[3][i] = (colors[0][i] + 2 * colors[1][i]) / 3;
            }
            colors[2][3] = 255;
            colors[3][3] = 255;
        } else {
            for (int i = 0; i < 3; i++) {
                colors[2][i] = (colors[0][i] + colors[1][i]) / 2;
                colors[3][i] = 0;
            }
            colors[2][3] = 255;
            colors[3][3] = 0;
        }

        std::uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int idx = (bits >> ((y * 4 + x) * 2)) & 0x3;
                std::uint8_t* pixel = output + y * stride + x * 4;
                pixel[0] = colors[idx][0];
                pixel[1] = colors[idx][1];
                pixel[2] = colors[idx][2];
                pixel[3] = colors[idx][3];
            }
        }
    }

    void VRCompositorHelper::DecompressBC3Block(const std::uint8_t* block, std::uint8_t* output, int stride)
    {
        std::uint8_t a0 = block[0];
        std::uint8_t a1 = block[1];
        std::uint8_t alphas[8];
        alphas[0] = a0;
        alphas[1] = a1;
        if (a0 > a1) {
            alphas[2] = (6 * a0 + 1 * a1) / 7;
            alphas[3] = (5 * a0 + 2 * a1) / 7;
            alphas[4] = (4 * a0 + 3 * a1) / 7;
            alphas[5] = (3 * a0 + 4 * a1) / 7;
            alphas[6] = (2 * a0 + 5 * a1) / 7;
            alphas[7] = (1 * a0 + 6 * a1) / 7;
        } else {
            alphas[2] = (4 * a0 + 1 * a1) / 5;
            alphas[3] = (3 * a0 + 2 * a1) / 5;
            alphas[4] = (2 * a0 + 3 * a1) / 5;
            alphas[5] = (1 * a0 + 4 * a1) / 5;
            alphas[6] = 0;
            alphas[7] = 255;
        }

        std::uint64_t alphaBits = 0;
        for (int i = 0; i < 6; i++) {
            alphaBits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
        }

        DecompressBC1Block(block + 8, output, stride);

        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int pixelIdx = y * 4 + x;
                int alphaIdx = static_cast<int>((alphaBits >> (pixelIdx * 3)) & 0x7);
                std::uint8_t* pixel = output + y * stride + x * 4;
                pixel[3] = alphas[alphaIdx];
            }
        }
    }

    void* VRCompositorHelper::LoadDDSTexture(const std::string& filePath)
    {
        if (!s_device) {
            logger::warn("LoadDDS: no D3D11 device");
            return nullptr;
        }

        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            logger::warn("LoadDDS: failed to open {}", filePath);
            return nullptr;
        }

        auto fileSize = static_cast<std::size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> data(fileSize);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        file.close();

        if (data.size() < 128) return nullptr;
        if (data[0] != 'D' || data[1] != 'D' || data[2] != 'S' || data[3] != ' ')
            return nullptr;

        auto readU32 = [&](std::size_t off) -> std::uint32_t {
            return *reinterpret_cast<const std::uint32_t*>(&data[off]);
        };

        std::uint32_t height = readU32(12);
        std::uint32_t width = readU32(16);
        std::uint32_t pfFlags = readU32(80);

        bool isBC1 = false, isBC3 = false;

        if (pfFlags & 0x4) { // DDPF_FOURCC
            char fourCC[5] = { static_cast<char>(data[84]), static_cast<char>(data[85]),
                               static_cast<char>(data[86]), static_cast<char>(data[87]), 0 };

            if (std::memcmp(fourCC, "DXT1", 4) == 0) {
                isBC1 = true;
            } else if (std::memcmp(fourCC, "DXT5", 4) == 0) {
                isBC3 = true;
            } else {
                logger::warn("LoadDDS: unsupported format '{}'", fourCC);
                return nullptr;
            }
        } else {
            logger::warn("LoadDDS: not FOURCC compressed");
            return nullptr;
        }

        std::uint32_t blockSize = isBC1 ? 8 : 16;
        std::uint32_t blocksWide = std::max(1u, (width + 3) / 4);
        std::uint32_t blocksHigh = std::max(1u, (height + 3) / 4);
        std::size_t compressedSize = static_cast<std::size_t>(blocksWide) * blocksHigh * blockSize;

        if (128 + compressedSize > data.size()) {
            logger::warn("LoadDDS: file too small");
            return nullptr;
        }

        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 255);
        int rowPitch = width * 4;
        const std::uint8_t* srcBlock = &data[128];

        for (std::uint32_t by = 0; by < blocksHigh; by++) {
            for (std::uint32_t bx = 0; bx < blocksWide; bx++) {
                std::uint8_t blockPixels[4 * 16];

                if (isBC3) {
                    DecompressBC3Block(srcBlock, blockPixels, 16);
                } else {
                    DecompressBC1Block(srcBlock, blockPixels, 16);
                }
                srcBlock += blockSize;

                for (int y = 0; y < 4 && (by * 4 + y) < height; y++) {
                    for (int x = 0; x < 4 && (bx * 4 + x) < width; x++) {
                        std::uint8_t* dst = &rgba[((by * 4 + y) * width + bx * 4 + x) * 4];
                        std::uint8_t* src = &blockPixels[y * 16 + x * 4];
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = src[3];
                    }
                }
            }
        }

        REX::W32::D3D11_TEXTURE2D_DESC desc = {};
        desc.width = width;
        desc.height = height;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.sampleDesc.count = 1;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;

        REX::W32::D3D11_SUBRESOURCE_DATA initData = {};
        initData.sysMem = rgba.data();
        initData.sysMemPitch = rowPitch;

        REX::W32::ID3D11Texture2D* texture = nullptr;
        HRESULT hr = s_device->CreateTexture2D(&desc, &initData, &texture);
        if (FAILED(hr) || !texture) {
            logger::warn("LoadDDS: CreateTexture2D failed (hr={:x}, {}x{})",
                static_cast<unsigned>(hr), width, height);
            return nullptr;
        }

        logger::info("LoadDDS: {}x{} {} decompressed to RGBA", width, height, isBC3 ? "BC3" : "BC1");
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

    void VRCompositorHelper::FadeToColor(float seconds, float r, float g, float b, float a, bool background)
    {
        if (s_fadeToColor && s_compositor) {
            s_fadeToColor(s_compositor, seconds, r, g, b, a, background);
        }
    }

    void VRCompositorHelper::FadeGrid(float seconds, bool fadeIn)
    {
        if (s_fadeGrid && s_compositor) {
            s_fadeGrid(s_compositor, seconds, fadeIn);
        }
    }

    void VRCompositorHelper::SuspendRendering(bool suspend)
    {
        if (s_suspendRendering && s_compositor) {
            s_suspendRendering(s_compositor, suspend);
        }
    }

    void VRCompositorHelper::SetBlackSkybox()
    {
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
        if (s_clearSkyboxOverride && s_compositor) {
            s_clearSkyboxOverride(s_compositor);
        }
    }
}
