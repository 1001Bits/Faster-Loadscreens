#pragma once

namespace VRLoadingScreens
{
    class VRCompositorHelper
    {
    public:
        static bool Initialize();
        static void FadeToColor(float seconds, float r, float g, float b, float a, bool background = false);
        static void FadeGrid(float seconds, bool fadeIn);
        static void SuspendRendering(bool suspend);
        static void SetBlackSkybox();
        static void ClearSkybox();
        static bool IsInitialized() { return s_initialized; }
        static bool IsVR() { return s_isVR; }
        static void* GetCompositor() { return s_compositor; }
        static void* GetD3D11Device() { return s_device; }
        static void* GetVRSystem() { return s_vrSystem; }
        static void* GetBlackTexture() { return s_blackTexture; }
        static void SetDevice(REX::W32::ID3D11Device* dev) { s_device = dev; }

        // DDS texture loading (returns uncompressed R8G8B8A8 D3D11 texture)
        static void* LoadDDSTexture(const std::string& filePath);
        static void ReleaseTexture(void* texture);

        // IVROverlay for background image + tip text
        static bool InitializeOverlay();
        static void SetImageSkybox(void* d3dTexture);
        static void ShowBlockerOverlay();  // Immediate black overlay to hide game content
        // overlayMode: 0=HMD-relative, 1=World-locked, 2=Cinema
        static void ShowBackgroundOverlay(void* d3dTexture, int overlayMode = 0, float alpha = 0.5f);
        static void HideBackgroundOverlay();
        static void ShowCapturedFrameOverlay(void* d3dTexture, int overlayMode);
        static void HideCapturedFrameOverlay();
        static void UpdateBackgroundOverlay(); // per-frame world-lock update
        static void UpdateLastKnownPose();    // cache HMD pose each frame (before loading)
        static bool GetCurrentPose(float outPose[3][4]); // get freshest HMD pose for Submit
        static bool IsOverlayInitialized() { return s_overlayInitialized; }

        // MCM-configurable overlay settings
        static void SetBackgroundWidth(float width) { s_bgWidthSetting = width; }
        static void SetTipDisplayOffset(float x, float y) { s_tipOffsetX = x; s_tipOffsetY = y; }
        static void SetTipDisplayRotation(float yawDeg, float pitchDeg) { s_tipYawDeg = yawDeg; s_tipPitchDeg = pitchDeg; }

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
        using FadeToColorFn = void(*)(void*, float, float, float, float, float, bool);
        using FadeGridFn = void(*)(void*, float, bool);
        using SuspendRenderingFn = void(*)(void*, bool);
        using SetSkyboxOverrideFn = int(*)(void*, const VRTexture*, unsigned int);
        using ClearSkyboxOverrideFn = void(*)(void*);

        // IVRSystem vtable function pointer type (x64)
        // void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float, TrackedDevicePose*, uint32_t)
        using GetDeviceToAbsTrackingPoseFn = void(*)(void*, int, float, TrackedDevicePose*, std::uint32_t);

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

        // DDS decompression
        static void DecompressBC3Block(const std::uint8_t* block, std::uint8_t* output, int stride);
        static void DecompressBC1Block(const std::uint8_t* block, std::uint8_t* output, int stride);

        // HMD pose helper (prefers IVRSystem non-blocking, falls back to IVRCompositor)
        static bool GetHMDPose(HmdMatrix34& outPose);
        // Compute and apply device-relative transform from world pose + current HMD pose
        static void UpdateBackgroundOverlayTransform(const HmdMatrix34& hmd);

        // Runtime detection
        static inline bool s_isVR = false;

        // Compositor state
        static inline bool s_initialized = false;
        static inline void* s_compositor = nullptr;
        static inline GetTrackingSpaceFn s_getTrackingSpace = nullptr;
        static inline GetLastPosesFn s_getLastPoses = nullptr;
        static inline FadeToColorFn s_fadeToColor = nullptr;
        static inline FadeGridFn s_fadeGrid = nullptr;
        static inline SuspendRenderingFn s_suspendRendering = nullptr;
        static inline SetSkyboxOverrideFn s_setSkyboxOverride = nullptr;
        static inline ClearSkyboxOverrideFn s_clearSkyboxOverride = nullptr;

        // IVRSystem state (for non-blocking pose queries)
        static inline void* s_vrSystem = nullptr;
        static inline GetDeviceToAbsTrackingPoseFn s_getDeviceToAbsTrackingPose = nullptr;

        // D3D11 device (stored during Initialize for texture creation)
        static inline REX::W32::ID3D11Device* s_device = nullptr;

        // Black D3D11 texture for skybox fallback
        static inline void* s_blackTexture = nullptr;

        // IVROverlay state
        static inline bool s_overlayInitialized = false;
        static inline void* s_overlay = nullptr;
        static inline std::uint64_t s_bgOverlayHandle = 0;
        static inline std::uint64_t s_capturedOverlayHandle = 0;
        static inline std::uint64_t s_blockerOverlayHandle = 0;

        // World-locked overlay state
        static inline bool s_bgOverlayActive = false;
        static inline bool s_capturedOverlayActive = false;
        static inline int s_overlayMode = 0;
        static inline int s_updateLogCounter = 0;
        static inline HmdMatrix34 s_bgWorldPose = {};       // background world-space pose
        static inline HmdMatrix34 s_capturedWorldPose = {};  // captured overlay world-space pose

        // Cached pre-loading HMD pose (updated every frame, used when loading starts)
        static inline HmdMatrix34 s_lastKnownPose = {};
        static inline bool s_hasLastKnownPose = false;

        // MCM-configurable overlay settings
        static inline float s_bgWidthSetting = 10.0f;
        static inline float s_tipOffsetX = -1.0f;
        static inline float s_tipOffsetY = -3.0f;
        static inline float s_tipYawDeg = 0.0f;
        static inline float s_tipPitchDeg = 0.0f;

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
    };
}
