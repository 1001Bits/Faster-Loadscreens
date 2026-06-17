#pragma once

#include <atomic>

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
        bool IsEnabled() const { return m_enabled; }
        bool IsInitialized() const { return m_initialized; }
        void SetMenuEventsActive(bool active) { m_menuEventsActive = active; }

        // Compositing mode
        void SetMode(CompositeMode mode) { m_mode = mode; }
        CompositeMode GetMode() const { return m_mode; }

        // Flat loading screen mode: 0=blank, 1=background only, 2=background+tips
        void SetFlatMode(int mode) { m_flatMode = mode; }
        int GetFlatMode() const { return m_flatMode; }
        void KillAdvanceMovie();  // Block loading screen rendering immediately

        // Whether to capture and show the tip+level overlay
        void SetShowCapturedOverlay(bool show) { m_showCapturedOverlay = show; }

        // Per-frame callback (flat mode: called from Present hook for per-frame updates)
        using FrameCallback = void(*)();
        void SetFrameCallback(FrameCallback cb) { m_frameCallback = cb; }

        // Deferred NOP: applied inside Submit hook after right eye completes
        // (guarantees both eyes have valid frames before the loop breaks)
        void RequestDeferredNOP(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size);
        bool IsDeferredNOPApplied() const { return m_deferredNOPApplied.load(); }
        void ResetDeferredState();
        void ReleaseCapturedTextures();  // Call after overlays are hidden

        // Called from Update() to process captured frame outside the Submit hook
        // (avoids Nvidia driver crashes from D3D11/OpenVR calls during Submit)
        void ProcessPendingCapture();


    private:
        D3D11Compositor() = default;

        // Shader compilation
        bool CompileShaders();

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

        // Process captured frame: render through alpha key shader (black → transparent)
        void ProcessCapturedFrame();

        // Clear eye texture to solid black (hides controllers after deferred NOP)
        void ClearEyeToBlack(void* eyeTexture2D);

        // Ensure temp texture matches eye texture size and format
        void EnsureTempTexture(unsigned int width, unsigned int height, unsigned int format);

        // State
        bool m_initialized = false;
        bool m_enabled = false;
        bool m_showCapturedOverlay = true;
        CompositeMode m_mode = CompositeMode::LuminanceKey;

        // D3D11 objects (stored as void* to avoid d3d11.h in header)
        void* m_device = nullptr;
        void* m_context = nullptr;

        // Shaders
        void* m_vsFullscreen = nullptr;    // ID3D11VertexShader*
        void* m_psLuminanceKey = nullptr;  // ID3D11PixelShader*
        void* m_psBackground = nullptr;    // ID3D11PixelShader*
        void* m_psAlphaKey = nullptr;      // ID3D11PixelShader*

        // Resources
        void* m_sampler = nullptr;         // ID3D11SamplerState*
        void* m_constantBuffer = nullptr;  // ID3D11Buffer*
        void* m_blendState = nullptr;      // ID3D11BlendState*
        void* m_rasterState = nullptr;     // ID3D11RasterizerState*
        void* m_depthState = nullptr;      // ID3D11DepthStencilState*

        // Background texture SRV
        void* m_bgTexture = nullptr;       // ID3D11Texture2D* (not owned)
        void* m_bgSRV = nullptr;           // ID3D11ShaderResourceView*

        // Temp texture for compositing (copy game texture here as input)
        void* m_tempTexture = nullptr;     // ID3D11Texture2D*
        void* m_tempSRV = nullptr;         // ID3D11ShaderResourceView*
        unsigned int m_tempWidth = 0;
        unsigned int m_tempHeight = 0;
        unsigned int m_tempFormat = 0;

        // Eye texture tracking from Submit
        void* m_lastLeftEye = nullptr;
        void* m_lastRightEye = nullptr;

        int m_clearMatchCount = 0;
        int m_submitCompositeCount = 0;

        // Deferred NOP — applied inside Submit hook after right eye completes
        std::atomic<bool> m_deferredNOPPending{ false };
        std::atomic<bool> m_deferredNOPApplied{ false };
        std::uintptr_t m_deferredNOPAddress = 0;
        std::uint8_t m_deferredNOPBytes[16] = {};
        std::size_t m_deferredNOPSize = 0;

        // Frozen eye textures — snapshot taken when deferred NOP fires,
        // submitted in place of live textures so post-NOP code can't corrupt them
        std::atomic<bool> m_frozen{ false };
        std::atomic<bool> m_pendingCaptureProcess{ false };  // Set in Submit, processed in Update
        void* m_frozenLeftTex = nullptr;   // ID3D11Texture2D*

        // Alpha-keyed captured frame (black background → transparent)
        void* m_processedTex = nullptr;    // ID3D11Texture2D* (RGBA with alpha)
        void* m_processedRTV = nullptr;    // ID3D11RenderTargetView*
        void* m_frozenSRV = nullptr;       // ID3D11ShaderResourceView*

        // GPU constant buffer layout (must match HLSL cbuffer)
        struct CompositeParams
        {
            float threshold;
            float bgUvScaleX;   // Aspect-correct UV scale for background texture
            float bgUvScaleY;
            float pad;
        };
        CompositeParams m_compositeParams = {};

        // Background texture dimensions (for aspect ratio correction)
        unsigned int m_bgWidth = 0;
        unsigned int m_bgHeight = 0;

        // Flat mode state
        void* m_swapChain = nullptr;       // IDXGISwapChain*
        bool m_isVR = false;
        bool m_flatLazyInit = false;       // True when using dummy device fallback
        FrameCallback m_frameCallback = nullptr;
        std::uint32_t m_flatPresentCount = 0;
        bool m_skipPresent = false;
        bool m_flatBackgroundOnly = false;  // true = draw background only (no luminance key)
        int m_flatMode = 2;                 // 0=blank, 1=bg only, 2=bg+tips
        bool m_advanceMovieKilled = false;
        std::uint8_t m_advanceMovieOrigByte = 0;
        std::uintptr_t m_advanceMovieAddr = 0;

        // Lazy init helper — complete initialization on first Present call
        bool CompleteFlatInit(void* swapChain);
        bool TryGetDeviceFromRendererData();

        // NG loading state
        bool m_menuEventsActive = false;
        std::chrono::steady_clock::time_point m_ngLoadStartTime{};
        int m_ngLoadNumber = 0;

        // Original function pointers
        static inline decltype(&HookedSubmit) s_originalSubmit = nullptr;
        static inline decltype(&HookedClearRTV) s_originalClearRTV = nullptr;
        static inline decltype(&HookedPresentFlat) s_originalPresentFlat = nullptr;
        static inline decltype(&HookedPresentFlat) s_originalPresentFlip = nullptr;
        static inline D3D11Compositor* s_instance = nullptr;
    };
}
