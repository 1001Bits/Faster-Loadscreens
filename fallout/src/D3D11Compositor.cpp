#include "PCH.h"
#include "D3D11Compositor.h"
#include "VRCompositorHelper.h"
#include "PerformancePatches.h"
#include "LoadingScreenManager.h"
#include "RuntimePolicy.h"

// Include D3D11 directly for full API access (REX::W32 only has subset)
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include <intrin.h>  // _ReturnAddress
#include <array>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace VRLoadingScreens
{
    // ========================================================================
    // HLSL shader source (compiled at runtime via d3dcompiler_47.dll)
    // ========================================================================

    static const char* VS_FULLSCREEN_SRC = R"(
struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

    // Plain bilinear blit — preserves RGBA from source. Used to upscale the
    // scaleform UI render target into a larger tips overlay texture.
    static const char* PS_BLIT_SRC = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return src.Sample(samp, uv);
}
)";

    // Flat luminance-key shader:
    // Dark pixels in the game's eye texture are replaced with the background
    // texture sampled at the same UV. Both eyes get identical background content,
    // placing the image at optical infinity (no stereo disparity, no parallax).
    // This matches how the FO4VR title screen renders its background.
    static const char* PS_LUMINANCE_KEY_SRC = R"(
cbuffer Params : register(b0) {
    float threshold;
    float bgUvScaleX;
    float bgUvScaleY;
    float _pad;
};

Texture2D gameTex : register(t0);
Texture2D bgTex   : register(t1);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 game = gameTex.Sample(samp, uv);
    // Aspect-correct UV for background (crop excess to fill screen)
    float2 bgUV = float2(0.5 + (uv.x - 0.5) * bgUvScaleX,
                         0.5 + (uv.y - 0.5) * bgUvScaleY);
    float4 bg = bgTex.Sample(samp, bgUV);
    float lum = dot(game.rgb, float3(0.299, 0.587, 0.114));
    // Sharp cutoff: only pixels with luminance above the threshold band are
    // preserved. Wide-soft `saturate(lum/threshold)` let mid-tone game bg
    // bleed through at 30-60% even with a high threshold; smoothstep with a
    // narrow band gives a clean text-only key.
    float alpha = smoothstep(threshold, threshold + 0.05, lum);
    // The exported vanilla frame places VaultTecLogo_mc at approximately
    // x=.877..923, y=.844..878. Use a hard, safely oversized exclusion so no
    // antialiased or animated spinner pixels survive the luminance key.
    if (uv.x > 0.80 && uv.y > 0.78)
        alpha = 0.0;
    return lerp(bg, game, alpha);
}
)";

    // Alpha-key for the bg+tips capture/composite. The scaleform RT is pure
    // LoadingMenu UI, so derive alpha from luminance, but explicitly remove the
    // bottom-right VaultTecLogo_mc region. That movie clip is Fallout's loading
    // spinner; the custom screen intentionally keeps only the tip and level UI.
    // Applying the mask while capturing also protects the separate-overlay
    // fallback, which otherwise bypasses the later composite pass.
    static const char* PS_TIPS_KEY_SRC = R"(
Texture2D srcTex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 c = srcTex.Sample(samp, uv);
    float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));
    float alpha = smoothstep(0.05, 0.20, lum);
    // LoadingMenu.as locks VaultTecLogo_mc to the bottom-right safe-rect.
    // Keep the left-side tip, level text and meter completely untouched.
    if (uv.x > 0.80 && uv.y > 0.78)
        return float4(0, 0, 0, 0);
    return float4(c.rgb, alpha);
}
)";

    static const char* PS_BACKGROUND_SRC = R"(
cbuffer Params : register(b0) {
    float threshold;
    float bgUvScaleX;
    float bgUvScaleY;
    float _pad;
};

Texture2D bgTex     : register(t0);
SamplerState samp   : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    // Aspect-correct UV for background (crop excess to fill screen)
    float2 bgUV = float2(0.5 + (uv.x - 0.5) * bgUvScaleX,
                         0.5 + (uv.y - 0.5) * bgUvScaleY);
    return float4(bgTex.Sample(samp, bgUV).rgb, 1.0);
}
)";

    // Exact LoadingMenu draw delta. `beforeTex` is copied from Fallout's bound
    // RTV immediately before BSScaleformRenderer::DisplayMovie and `afterTex`
    // immediately after that same verified LoadingMenu call. Existing world,
    // controllers, hands, and other eye pixels are identical in both snapshots
    // and receive zero alpha. Do not impose 1280x720 stage UVs here: Fallout VR
    // can render the movie through a transformed viewport, so a fixed lower-left
    // mask can cut the native tip and level text out of the captured delta.
    // VaultTecLogo_mc suppression is a hard prerequisite for enabling capture.
    static const char* PS_TIPS_DELTA_SRC = R"(
cbuffer DeltaSourceParams : register(b0) {
    float4 sourceUvTransform; // xy = offset, zw = scale within the full RTV
};

Texture2D afterTex  : register(t0);
Texture2D beforeTex : register(t1);
SamplerState samp   : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float2 sourceUv =
        sourceUvTransform.xy + uv * sourceUvTransform.zw;
    float4 afterColor = afterTex.Sample(samp, sourceUv);
    float4 beforeColor = beforeTex.Sample(samp, sourceUv);
    float4 difference = abs(afterColor - beforeColor);
    float changed = max(max(difference.r, difference.g),
                        max(difference.b, difference.a));

    // The delta is ONLY the ownership gate: pixels the verified LoadingMenu
    // draw did not touch stay fully transparent and black, so no controller
    // or pre-existing UI color can enter the published texture.
    float gate = smoothstep(0.004, 0.02, changed);

    // Native look: keep Scaleform's own premultiplied color and coverage
    // alpha. The previous luminance-ramp alpha replaced the game's
    // antialiased glyph coverage with a brightness key, which fattened and
    // brightened edges (user: "unnatural, too shiny"). If the surface carries
    // no alpha channel content, fall back to the old luminance key so tips
    // never silently disappear (the 16-sample occlusion proof would otherwise
    // reject the capture and drop mode 3 to background-only).
    float luminance = dot(afterColor.rgb, float3(0.299, 0.587, 0.114));
    float luminanceFallback =
        smoothstep(0.035, 0.16, luminance) * step(afterColor.a, 0.001);
    float alpha = max(afterColor.a, luminanceFallback) * gate;
    clip(alpha - 0.0001);
    // Output stays premultiplied (Scaleform renders premultiplied); the
    // composite pass blends it with SrcBlend=ONE accordingly.
    return float4(afterColor.rgb * gate, alpha);
}
)";

    // Flat-mode readiness proof and persistent replay. This is intentionally
    // separate from the VR overlay texture shader: change is only the ownership
    // gate, while luminance determines replay opacity. Multiplying alpha by a
    // smooth change ramp made clip(0) samples count in the occlusion query even
    // though their persistent replay was transparent. Every surviving sample
    // now has at least 0.5 alpha under the same 0.40..0.45 luminance key used by
    // CompositeFlatFrame. The spinner corner is excluded so flat remains safe
    // when GFx access is unavailable.
    static const char* PS_FLAT_TIPS_PROOF_SRC = R"(
cbuffer DeltaSourceParams : register(b0) {
    float4 sourceUvTransform;
};

Texture2D afterTex  : register(t0);
Texture2D beforeTex : register(t1);
SamplerState samp   : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    if (uv.x > 0.80 && uv.y > 0.78)
        discard;
    float2 sourceUv =
        sourceUvTransform.xy + uv * sourceUvTransform.zw;
    float4 afterColor = afterTex.Sample(samp, sourceUv);
    float4 beforeColor = beforeTex.Sample(samp, sourceUv);
    float3 difference = abs(afterColor.rgb - beforeColor.rgb);
    float changed = max(difference.r, max(difference.g, difference.b));
    float luminance = dot(afterColor.rgb, float3(0.299, 0.587, 0.114));
    float replayAlpha = smoothstep(0.40, 0.45, luminance);
    clip(changed - 0.004);
    clip(luminance - 0.425);
    // Straight-alpha output matches CompositeFlatFrame's replay blend.
    return float4(afterColor.rgb, replayAlpha);
}
)";


    // D3DCompile function pointer (loaded dynamically)
    using D3DCompileFn = HRESULT(WINAPI*)(
        const void*, SIZE_T, const char*, const void*, void*,
        const char*, const char*, UINT, UINT, void**, void**);
    static D3DCompileFn s_D3DCompile = nullptr;

    namespace
    {
        template <class T>
        void ReleaseCom(T*& object)
        {
            if (object) {
                object->Release();
                object = nullptr;
            }
        }

        template <class T>
        void ReleaseVoidCom(void*& object)
        {
            auto* typed = static_cast<T*>(object);
            ReleaseCom(typed);
            object = nullptr;
        }

        struct ComReleaseGuard
        {
            IUnknown* object = nullptr;
            ~ComReleaseGuard() { if (object) object->Release(); }
        };

        void BindFullscreenVertexPipeline(
            ID3D11DeviceContext* context, ID3D11VertexShader* shader)
        {
            context->VSSetShader(shader, nullptr, 0);
            // Fallout can leave tessellation/geometry stages bound. A
            // fullscreen triangle has no matching hull/domain/geometry inputs;
            // explicitly disable them and let ScopedPipelineState restore the
            // exact game state after our draw.
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
            context->GSSetShader(nullptr, nullptr, 0);
        }

        bool IsNativeSystemGraphicsRuntime(const wchar_t* modulePath)
        {
            if (!modulePath || !*modulePath) return false;

            const wchar_t* base = wcsrchr(modulePath, L'\\');
            base = base ? base + 1 : modulePath;
            if (_wcsicmp(base, L"dxgi.dll") != 0 &&
                _wcsicmp(base, L"d3d11.dll") != 0) {
                return false;
            }

            wchar_t systemDirectory[MAX_PATH] = {};
            const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return false;

            std::wstring expected(systemDirectory, length);
            if (!expected.empty() && expected.back() != L'\\') expected.push_back(L'\\');
            expected.append(base);
            return _wcsicmp(modulePath, expected.c_str()) == 0;
        }

        bool HasLoadedD3D11Proxy()
        {
            const HMODULE module = GetModuleHandleW(L"d3d11.dll");
            if (!module) return false;

            wchar_t modulePath[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return false;

            // ENB configuration files and its optional shader compiler can be
            // left behind after the actual d3d11 proxy is removed. They do not
            // alter the swapchain path and must not force the shared MinHook
            // strategy. Only a loaded, non-System32 d3d11 runtime is evidence
            // that an ENB-style forwarding proxy is active.
            return !IsNativeSystemGraphicsRuntime(modulePath);
        }

        struct MainWindowSearch
        {
            DWORD processId = 0;
            HWND window = nullptr;
            std::uint64_t area = 0;
        };

        BOOL CALLBACK FindLargestProcessWindow(HWND window, LPARAM parameter)
        {
            auto& search = *reinterpret_cast<MainWindowSearch*>(parameter);
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != search.processId || !IsWindowVisible(window) ||
                GetWindow(window, GW_OWNER) != nullptr) {
                return TRUE;
            }
            RECT rect{};
            if (!GetClientRect(window, &rect)) return TRUE;
            const auto width = std::max<LONG>(0, rect.right - rect.left);
            const auto height = std::max<LONG>(0, rect.bottom - rect.top);
            const auto area = static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height);
            if (area > search.area) {
                search.area = area;
                search.window = window;
            }
            return TRUE;
        }

        HWND GetLargestProcessWindow()
        {
            MainWindowSearch search{ GetCurrentProcessId() };
            EnumWindows(&FindLargestProcessWindow, reinterpret_cast<LPARAM>(&search));
            return search.window;
        }

        // Every compositor draw temporarily replaces these slots. The common path
        // saves only directly affected state; if we must create a new RTV for a
        // resource, CaptureHazardBindings additionally preserves SRVs that D3D
        // would auto-unbind across every shader stage.
        class ScopedPipelineState
        {
        public:
            explicit ScopedPipelineState(ID3D11DeviceContext* context) : m_context(context)
            {
                // Pixel UAV slots overlap the OM output namespace. Query RTV/DSV
                // first, then request only the legal UAV range beginning after
                // the active RTV span; asking for 8 RTVs and UAV slot 0 in one
                // call is invalid and can lose the game's OM UAV bindings.
                m_context->OMGetRenderTargets(
                    static_cast<UINT>(m_rtvs.size()), m_rtvs.data(), &m_dsv);
                for (std::size_t i = 0; i < m_rtvs.size(); ++i) {
                    if (m_rtvs[i]) m_rtvCount = static_cast<UINT>(i + 1);
                }
                if (m_rtvCount < m_omUavs.size()) {
                    m_context->OMGetRenderTargetsAndUnorderedAccessViews(
                        0, nullptr, nullptr, m_rtvCount,
                        static_cast<UINT>(m_omUavs.size()) - m_rtvCount,
                        m_omUavs.data() + m_rtvCount);
                }
                m_context->CSGetUnorderedAccessViews(
                    0, static_cast<UINT>(m_csUavs.size()), m_csUavs.data());
                m_viewportCount = static_cast<UINT>(m_viewports.size());
                m_context->RSGetViewports(&m_viewportCount, m_viewports.data());
                m_context->IAGetInputLayout(&m_inputLayout);
                m_context->IAGetPrimitiveTopology(&m_topology);
                m_vsClassCount = static_cast<UINT>(m_vsClasses.size());
                m_context->VSGetShader(&m_vs, m_vsClasses.data(), &m_vsClassCount);
                m_hsClassCount = static_cast<UINT>(m_hsClasses.size());
                m_context->HSGetShader(&m_hs, m_hsClasses.data(), &m_hsClassCount);
                m_dsClassCount = static_cast<UINT>(m_dsClasses.size());
                m_context->DSGetShader(&m_ds, m_dsClasses.data(), &m_dsClassCount);
                m_gsClassCount = static_cast<UINT>(m_gsClasses.size());
                m_context->GSGetShader(&m_gs, m_gsClasses.data(), &m_gsClassCount);
                m_psClassCount = static_cast<UINT>(m_psClasses.size());
                m_context->PSGetShader(&m_ps, m_psClasses.data(), &m_psClassCount);
                m_context->PSGetConstantBuffers(0, 1, &m_psConstantBuffer);
                m_context->PSGetShaderResources(
                    0, static_cast<UINT>(m_psSrvs.size()), m_psSrvs.data());
                m_context->PSGetSamplers(0, 1, &m_psSampler);
                m_context->OMGetBlendState(&m_blendState, m_blendFactor, &m_sampleMask);
                m_context->RSGetState(&m_rasterState);
                m_context->OMGetDepthStencilState(&m_depthState, &m_stencilRef);
            }

            ScopedPipelineState(const ScopedPipelineState&) = delete;
            ScopedPipelineState& operator=(const ScopedPipelineState&) = delete;

            ~ScopedPipelineState()
            {
                m_context->OMSetRenderTargetsAndUnorderedAccessViews(
                    m_rtvCount, m_rtvCount ? m_rtvs.data() : nullptr, m_dsv,
                    m_rtvCount,
                    static_cast<UINT>(m_omUavs.size()) - m_rtvCount,
                    m_rtvCount < m_omUavs.size() ? m_omUavs.data() + m_rtvCount : nullptr,
                    nullptr);
                m_context->CSSetUnorderedAccessViews(
                    0, static_cast<UINT>(m_csUavs.size()), m_csUavs.data(), nullptr);
                m_context->RSSetViewports(
                    m_viewportCount, m_viewportCount ? m_viewports.data() : nullptr);
                m_context->IASetInputLayout(m_inputLayout);
                m_context->IASetPrimitiveTopology(m_topology);
                m_context->VSSetShader(
                    m_vs, m_vsClassCount ? m_vsClasses.data() : nullptr, m_vsClassCount);
                m_context->HSSetShader(
                    m_hs, m_hsClassCount ? m_hsClasses.data() : nullptr, m_hsClassCount);
                m_context->DSSetShader(
                    m_ds, m_dsClassCount ? m_dsClasses.data() : nullptr, m_dsClassCount);
                m_context->GSSetShader(
                    m_gs, m_gsClassCount ? m_gsClasses.data() : nullptr, m_gsClassCount);
                m_context->PSSetShader(
                    m_ps, m_psClassCount ? m_psClasses.data() : nullptr, m_psClassCount);
                m_context->PSSetConstantBuffers(0, 1, &m_psConstantBuffer);
                m_context->PSSetShaderResources(
                    0, static_cast<UINT>(m_psSrvs.size()), m_psSrvs.data());
                m_context->PSSetSamplers(0, 1, &m_psSampler);
                m_context->OMSetBlendState(m_blendState, m_blendFactor, m_sampleMask);
                m_context->RSSetState(m_rasterState);
                m_context->OMSetDepthStencilState(m_depthState, m_stencilRef);

                for (const auto& binding : m_hazardSrvs) {
                    ID3D11ShaderResourceView* view = binding.view;
                    switch (binding.stage) {
                    case ShaderStage::VS: m_context->VSSetShaderResources(binding.slot, 1, &view); break;
                    case ShaderStage::HS: m_context->HSSetShaderResources(binding.slot, 1, &view); break;
                    case ShaderStage::DS: m_context->DSSetShaderResources(binding.slot, 1, &view); break;
                    case ShaderStage::GS: m_context->GSSetShaderResources(binding.slot, 1, &view); break;
                    case ShaderStage::PS: m_context->PSSetShaderResources(binding.slot, 1, &view); break;
                    case ShaderStage::CS: m_context->CSSetShaderResources(binding.slot, 1, &view); break;
                    }
                }

                for (auto*& rtv : m_rtvs) ReleaseCom(rtv);
                ReleaseCom(m_dsv);
                for (auto*& uav : m_omUavs) ReleaseCom(uav);
                for (auto*& uav : m_csUavs) ReleaseCom(uav);
                ReleaseCom(m_inputLayout);
                ReleaseCom(m_vs);
                ReleaseCom(m_hs);
                ReleaseCom(m_ds);
                ReleaseCom(m_gs);
                ReleaseCom(m_ps);
                for (UINT i = 0; i < m_vsClassCount; ++i) ReleaseCom(m_vsClasses[i]);
                for (UINT i = 0; i < m_hsClassCount; ++i) ReleaseCom(m_hsClasses[i]);
                for (UINT i = 0; i < m_dsClassCount; ++i) ReleaseCom(m_dsClasses[i]);
                for (UINT i = 0; i < m_gsClassCount; ++i) ReleaseCom(m_gsClasses[i]);
                for (UINT i = 0; i < m_psClassCount; ++i) ReleaseCom(m_psClasses[i]);
                ReleaseCom(m_psConstantBuffer);
                for (auto*& srv : m_psSrvs) ReleaseCom(srv);
                ReleaseCom(m_psSampler);
                ReleaseCom(m_blendState);
                ReleaseCom(m_rasterState);
                ReleaseCom(m_depthState);
                for (auto& binding : m_hazardSrvs) ReleaseCom(binding.view);
            }

            // Prefer a view the game already has bound for this backbuffer.
            // This removes the normal per-Present CreateRTV call without keeping
            // a reference across ResizeBuffers (which would make resize fail).
            ID3D11RenderTargetView* FindRenderTargetView(ID3D11Resource* resource) const
            {
                for (auto* rtv : m_rtvs) {
                    if (!rtv) continue;
                    ID3D11Resource* candidate = nullptr;
                    rtv->GetResource(&candidate);
                    const bool matches = candidate == resource;
                    ReleaseCom(candidate);
                    if (matches) return rtv;
                }
                return nullptr;
            }

            void CaptureHazardBindings(ID3D11Resource* resource)
            {
                if (!resource || !m_hazardSrvs.empty()) return;
                auto inspect = [&](ShaderStage stage, auto&& getViews) {
                    std::array<ID3D11ShaderResourceView*,
                        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views{};
                    getViews(static_cast<UINT>(views.size()), views.data());
                    for (UINT slot = 0; slot < static_cast<UINT>(views.size()); ++slot) {
                        auto*& view = views[slot];
                        if (!view) continue;
                        ID3D11Resource* candidate = nullptr;
                        view->GetResource(&candidate);
                        const bool conflicts = candidate == resource;
                        ReleaseCom(candidate);
                        // PS slots 0-1 are already part of the ordinary saved
                        // state and will be restored without a duplicate ref.
                        if (conflicts && !(stage == ShaderStage::PS && slot < 2)) {
                            m_hazardSrvs.push_back({ stage, slot, view });
                            view = nullptr;
                        }
                        ReleaseCom(view);
                    }
                };
                inspect(ShaderStage::VS, [&](UINT n, auto** p) { m_context->VSGetShaderResources(0, n, p); });
                inspect(ShaderStage::HS, [&](UINT n, auto** p) { m_context->HSGetShaderResources(0, n, p); });
                inspect(ShaderStage::DS, [&](UINT n, auto** p) { m_context->DSGetShaderResources(0, n, p); });
                inspect(ShaderStage::GS, [&](UINT n, auto** p) { m_context->GSGetShaderResources(0, n, p); });
                inspect(ShaderStage::PS, [&](UINT n, auto** p) { m_context->PSGetShaderResources(0, n, p); });
                inspect(ShaderStage::CS, [&](UINT n, auto** p) { m_context->CSGetShaderResources(0, n, p); });
            }

        private:
            static constexpr std::size_t kRenderTargetCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
            static constexpr std::size_t kViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

            enum class ShaderStage { VS, HS, DS, GS, PS, CS };
            struct HazardSrvBinding
            {
                ShaderStage stage;
                UINT slot;
                ID3D11ShaderResourceView* view;
            };

            ID3D11DeviceContext* m_context;
            std::array<ID3D11RenderTargetView*, kRenderTargetCount> m_rtvs{};
            UINT m_rtvCount = 0;
            ID3D11DepthStencilView* m_dsv = nullptr;
            std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> m_omUavs{};
            std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> m_csUavs{};
            std::array<D3D11_VIEWPORT, kViewportCount> m_viewports{};
            UINT m_viewportCount = 0;
            ID3D11InputLayout* m_inputLayout = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11VertexShader* m_vs = nullptr;
            ID3D11HullShader* m_hs = nullptr;
            ID3D11DomainShader* m_ds = nullptr;
            ID3D11GeometryShader* m_gs = nullptr;
            ID3D11PixelShader* m_ps = nullptr;
            static constexpr std::size_t kShaderClassCount = 256;
            std::array<ID3D11ClassInstance*, kShaderClassCount> m_vsClasses{};
            std::array<ID3D11ClassInstance*, kShaderClassCount> m_hsClasses{};
            std::array<ID3D11ClassInstance*, kShaderClassCount> m_dsClasses{};
            std::array<ID3D11ClassInstance*, kShaderClassCount> m_gsClasses{};
            std::array<ID3D11ClassInstance*, kShaderClassCount> m_psClasses{};
            UINT m_vsClassCount = 0;
            UINT m_hsClassCount = 0;
            UINT m_dsClassCount = 0;
            UINT m_gsClassCount = 0;
            UINT m_psClassCount = 0;
            ID3D11Buffer* m_psConstantBuffer = nullptr;
            std::array<ID3D11ShaderResourceView*, 2> m_psSrvs{};
            ID3D11SamplerState* m_psSampler = nullptr;
            ID3D11BlendState* m_blendState = nullptr;
            FLOAT m_blendFactor[4]{};
            UINT m_sampleMask = 0;
            ID3D11RasterizerState* m_rasterState = nullptr;
            ID3D11DepthStencilState* m_depthState = nullptr;
            UINT m_stencilRef = 0;
            std::vector<HazardSrvBinding> m_hazardSrvs;
        };

        bool TryReadExecutableByte_SEH(std::uintptr_t address, std::uint8_t* value)
        {
            __try {
                if (!address || !value) return false;
                const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
                    address < moduleBase || address >= moduleBase + nt->OptionalHeader.SizeOfImage) {
                    return false;
                }
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                    return false;
                }
                const DWORD protection = mbi.Protect & 0xFF;
                if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
                    protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
                    return false;
                }
                *value = *reinterpret_cast<const std::uint8_t*>(address);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool TryReadModulePointer_SEH(
            std::uintptr_t address, std::uintptr_t* value)
        {
            __try {
                if (!address || !value) return false;
                const auto moduleBase =
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                const auto* dos =
                    reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    moduleBase + dos->e_lfanew);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
                    nt->Signature != IMAGE_NT_SIGNATURE) {
                    return false;
                }
                const auto moduleEnd =
                    moduleBase + nt->OptionalHeader.SizeOfImage;
                if (address < moduleBase || address > moduleEnd ||
                    moduleEnd - address < sizeof(std::uintptr_t)) {
                    return false;
                }
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(
                        reinterpret_cast<void*>(address), &mbi,
                        sizeof(mbi)) != sizeof(mbi) ||
                    mbi.State != MEM_COMMIT ||
                    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                    return false;
                }
                *value = *reinterpret_cast<const std::uintptr_t*>(address);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ExecutableBytesEqual(std::uintptr_t address,
            const std::uint8_t* expected, std::size_t size)
        {
            if (!address || !expected || size == 0) return false;
            for (std::size_t i = 0; i < size; ++i) {
                std::uint8_t current = 0;
                if (!TryReadExecutableByte_SEH(address + i, &current) ||
                    current != expected[i]) {
                    return false;
                }
            }
            return true;
        }

        struct NativeLoadingTextSelectionSnapshot
        {
            bool fieldsReadable = false;
            bool loadingIntoInterior = false;
            bool loadScreenShown = false;
            bool hasArtScreen = false;
            std::uint32_t validScreensSize = 0;
        };

        // LoadingMenu layout verified against native SendLoadingText in
        // Fallout4VR 1.2.72 and Fallout4.exe 1.10.163/1.11.221/1.11.240:
        //   +0xE8  artScreen
        //   +0x100 validScreens.data
        //   +0x110 validScreens.size
        //   +0x198 numNonDefaultScreens
        //   +0x270 loadingIntoInterior
        //   +0x278 loadScreenShown
        // Read before forwarding SendLoadingText, because that function removes
        // a random candidate from validScreens. Any malformed/unreadable field
        // stays Unknown: flat remains live for lack of exact proof, while VR
        // reaches only its bounded background fallback.
        bool TryReadLoadingTextSelection_SEH(
            void* loadingMenu, NativeLoadingTextSelectionSnapshot* snapshot)
        {
            __try {
                if (!loadingMenu || !snapshot) return false;
                const auto base = reinterpret_cast<std::uintptr_t>(loadingMenu);
                if (base < 0x10000 || base > 0x7FFFFFFFFFFFULL) return false;

                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(
                        reinterpret_cast<void*>(base), &mbi,
                        sizeof(mbi)) != sizeof(mbi) ||
                    mbi.State != MEM_COMMIT ||
                    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                    return false;
                }
                const auto regionEnd =
                    reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                    mbi.RegionSize;
                if (regionEnd < base || regionEnd - base <= 0x278) {
                    return false;
                }

                const auto artScreen =
                    *reinterpret_cast<void* const*>(base + 0xE8);
                const auto validScreensData =
                    *reinterpret_cast<void* const*>(base + 0x100);
                const auto validScreensSize =
                    *reinterpret_cast<const std::uint32_t*>(base + 0x110);
                const auto numNonDefaultScreens =
                    *reinterpret_cast<const std::uint32_t*>(base + 0x198);
                const auto loadingIntoInterior =
                    *reinterpret_cast<const std::uint8_t*>(base + 0x270) != 0;
                const auto loadScreenShown =
                    *reinterpret_cast<const std::uint8_t*>(base + 0x278) != 0;

                // Native candidate lists are tiny. A huge count, a non-default
                // prefix beyond the array, or a populated array with no data
                // pointer is evidence that this is not the verified layout.
                static constexpr std::uint32_t kMaximumSaneLoadScreens = 65536;
                if (validScreensSize > kMaximumSaneLoadScreens ||
                    numNonDefaultScreens > validScreensSize ||
                    (validScreensSize != 0 && validScreensData == nullptr)) {
                    return false;
                }

                snapshot->fieldsReadable = true;
                snapshot->loadingIntoInterior = loadingIntoInterior;
                snapshot->loadScreenShown = loadScreenShown;
                snapshot->hasArtScreen = artScreen != nullptr;
                snapshot->validScreensSize = validScreensSize;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

    }

    // ========================================================================
    // OpenVR struct layouts (matches openvr.h)
    // ========================================================================
    struct VRTexture_t
    {
        void* handle;
        int eType;
        int eColorSpace;
    };

    D3D11Compositor::~D3D11Compositor()
    {
        m_renderReady.store(false, std::memory_order_release);
        m_enabled.store(false, std::memory_order_release);
        // OpenVR may retain references to the blocker, background, and baked
        // tips textures. Destroy those overlay handles before releasing any
        // compositor-owned D3D object they may reference.
        VRCompositorHelper::SetDevice(nullptr);
        ReleaseDeviceResources();
    }

    void D3D11Compositor::SetTipsExtractEnabled(bool enabled)
    {
        ConfigureTipsExtraction(enabled, false);
    }

    void D3D11Compositor::DisableTipsCapturePreservingFlatNativeSelection()
    {
        ConfigureTipsExtraction(false, true);
    }

    void D3D11Compositor::ConfigureTipsExtraction(
        bool enabled, bool preserveFlatNativeSelection)
    {
        // CLOSE and the next OPEN can race a render-thread query poll. Retire
        // the per-load query under the same mutex used by capture/finalization;
        // an old GPU result can therefore never authorize the next load.
        std::lock_guard resourceLock(m_renderResourceMutex);
        m_tipsExtractEnabled.store(false, std::memory_order_release);
        CancelLoadingMenuDeltaCapture();
        m_tipsDeltaQueryPending.store(false, std::memory_order_release);
        ReleaseVoidCom<ID3D11Query>(m_tipsDeltaQuery);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_flatTipsProofSRV);
        if (!m_isVR.load(std::memory_order_acquire)) {
            m_flatTipsDrawSerial.store(0, std::memory_order_release);
        }

        if (preserveFlatNativeSelection) {
            // Keep this exceptional path self-validating even if a future
            // caller bypasses LoadingScreenManager's policy gate. Content is
            // the release-published field of HookedSendLoadingText, and the
            // matching initial/current serial proves that no later refresh has
            // superseded the exact pre-OPEN minimal publication.
            const auto content =
                static_cast<Policy::FlatNativeLoadingContent>(
                    m_flatNativeLoadingContent.load(
                        std::memory_order_acquire));
            const auto readySerial = m_flatTipsReadySerial.load(
                std::memory_order_acquire);
            const bool verifiedFlatMinimalSelection =
                !m_isVR.load(std::memory_order_acquire) &&
                m_inLoadingScreen.load(std::memory_order_acquire) &&
                m_flatMode.load(std::memory_order_acquire) == 3 &&
                m_flatTipReadinessHookInstalled.load(
                    std::memory_order_acquire) &&
                content ==
                    Policy::FlatNativeLoadingContent::kBackgroundOnly &&
                m_loadingTextOwner.load(std::memory_order_acquire) != nullptr &&
                readySerial != 0 &&
                m_flatNativeInitialSelectionSerial.load(
                    std::memory_order_acquire) == readySerial;
            if (!verifiedFlatMinimalSelection) {
                preserveFlatNativeSelection = false;
                logger::warn(
                    "Flat tips: native-minimal capture-disable retention "
                    "rejected because publication proof changed");
            }
        }

        if (enabled) {
            m_scaleformHookLogCount.store(0, std::memory_order_relaxed);
            m_scaleformRTLogCount.store(0, std::memory_order_relaxed);
            m_renderMenusSkipCount.store(0, std::memory_order_relaxed);
            m_tipsScaleformLogCount.store(0, std::memory_order_relaxed);
            m_loadingMenuScaleformRtIndex.store(
                -1, std::memory_order_release);
            m_tipsCaptureComplete.store(false, std::memory_order_release);
            m_loadingMenuRenderFresh.store(false, std::memory_order_release);
            // The next capture measures its own glyph extent; never place this
            // load's text using the previous load's bounds.
            m_tipsContentBoundsValid = false;
            // Do not clear loadingTextReady/owner here. Native SendLoadingText
            // can complete immediately before the menu-open event; CLOSE always
            // invalidates both values below.
            m_tipsExtractEnabled.store(true, std::memory_order_release);
        } else if (!preserveFlatNativeSelection) {
            m_loadingTextReady.store(false, std::memory_order_release);
            m_loadingTextOwner.store(nullptr, std::memory_order_release);
        } else {
            logger::info(
                "Flat tips: capture disabled for verified native-minimal "
                "selection; owner/content/serial retained for solid-black "
                "Present proof");
        }
    }

    bool D3D11Compositor::TryGetFlatNativeLoadingSelection(
        FlatNativeLoadingSelectionSnapshot& a_snapshot,
        std::uint64_t a_requiredInitialSerial)
    {
        if (m_isVR.load(std::memory_order_acquire) ||
            !m_flatTipReadinessHookInstalled.load(
                std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
        const auto content = static_cast<Policy::NativeLoadingContent>(
            m_flatNativeLoadingContent.load(std::memory_order_acquire));
        const auto model = static_cast<Policy::NativeModelSelection>(
            m_flatNativeModelSelection.load(std::memory_order_acquire));
        const auto initialSerial =
            m_flatNativeInitialSelectionSerial.load(
                std::memory_order_acquire);
        void* const owner =
            m_loadingTextOwner.load(std::memory_order_acquire);
        if (content == Policy::NativeLoadingContent::kUnknown ||
            model == Policy::NativeModelSelection::kUnknown ||
            initialSerial == 0 || !owner ||
            (a_requiredInitialSerial != 0 &&
             initialSerial != a_requiredInitialSerial)) {
            return false;
        }

        a_snapshot.selection = { content, model };
        a_snapshot.owner = owner;
        a_snapshot.initialSerial = initialSerial;
        return true;
    }

    bool D3D11Compositor::RestoreFlatNativeLoadingSelectionForUpcomingOpen(
        const FlatNativeLoadingSelectionSnapshot& a_snapshot)
    {
        if (m_isVR.load(std::memory_order_acquire) || !a_snapshot.owner ||
            a_snapshot.initialSerial == 0 ||
            a_snapshot.selection.content ==
                Policy::NativeLoadingContent::kUnknown ||
            a_snapshot.selection.model ==
                Policy::NativeModelSelection::kUnknown) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
        if (m_enabled.load(std::memory_order_acquire) ||
            m_inLoadingScreen.load(std::memory_order_acquire)) {
            return false;
        }

        m_flatTipsDrawSerial.store(0, std::memory_order_release);
        m_flatTipsPresentedSerial.store(0, std::memory_order_release);
        m_loadingTextOwner.store(
            a_snapshot.owner, std::memory_order_release);
        m_loadingTextReady.store(
            Policy::FlatNativeContentNeedsVisibleProof(
                a_snapshot.selection.content),
            std::memory_order_release);
        m_flatNativeModelSelection.store(
            static_cast<std::uint8_t>(a_snapshot.selection.model),
            std::memory_order_release);
        const auto restoredSerial =
            m_flatTipsReadySerial.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        m_flatNativeInitialSelectionSerial.store(
            restoredSerial, std::memory_order_release);
        // Publish content last. DisplayMenu uses it as the acquire gate for
        // the owner/readiness fields restored above.
        m_flatNativeLoadingContent.store(
            static_cast<std::uint8_t>(a_snapshot.selection.content),
            std::memory_order_release);
        logger::info(
            "Flat tips: restored chained native selection "
            "(serial={} -> {}, model={}, content={})",
            a_snapshot.initialSerial, restoredSerial,
            a_snapshot.selection.model ==
                    Policy::NativeModelSelection::kModel ?
                "model" : "no-model",
            Policy::NativeLoadingContentName(
                a_snapshot.selection.content));
        return true;
    }

    void D3D11Compositor::ReleaseDeviceResources()
    {
        std::lock_guard resourceLock(m_renderResourceMutex);
        m_renderReady.store(false, std::memory_order_release);
        m_enabled.store(false, std::memory_order_release);
        m_tipsExtractEnabled.store(false, std::memory_order_release);
        m_tipsCaptureComplete.store(false, std::memory_order_release);
        m_loadingMenuDeltaArmed.store(false, std::memory_order_release);
        m_tipsDeltaQueryPending.store(false, std::memory_order_release);
        m_loadingTextReady.store(false, std::memory_order_release);
        m_loadingTextOwner.store(nullptr, std::memory_order_release);
        m_vrNativeSelectionSeen.store(false, std::memory_order_release);
        m_vrNativeSelectionEpoch.store(0, std::memory_order_release);
        m_vrNativeLoadingContent.store(
            static_cast<std::uint8_t>(
                Policy::NativeLoadingContent::kUnknown),
            std::memory_order_release);
        m_vrTipsLoadEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_flatNativeLoadingContent.store(
            static_cast<std::uint8_t>(
                Policy::FlatNativeLoadingContent::kUnknown),
            std::memory_order_release);
        m_flatNativeModelSelection.store(
            static_cast<std::uint8_t>(
                Policy::NativeModelSelection::kUnknown),
            std::memory_order_release);
        m_flatNativeInitialSelectionSerial.store(
            0, std::memory_order_release);
        m_flatTipsDrawSerial.store(0, std::memory_order_release);
        m_flatTipsPresentedSerial.store(0, std::memory_order_release);
        m_loadingMenuRenderFresh.store(false, std::memory_order_release);
        m_inLoadingScreen.store(false, std::memory_order_release);

        {
            std::lock_guard bgLock(m_bgMutex);
            ReleaseVoidCom<ID3D11ShaderResourceView>(m_bgSRV);
            m_bgWidth = 0;
            m_bgHeight = 0;
        }

        ReleaseVoidCom<ID3D11Texture2D>(m_tempTexture);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_tempSRV);
        ReleaseVoidCom<ID3D11Texture2D>(m_tipsScaleformTex);
        ReleaseVoidCom<ID3D11RenderTargetView>(m_tipsScaleformRTV);
        ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuBeforeTex);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_loadingMenuBeforeSRV);
        ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuAfterTex);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_loadingMenuAfterSRV);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_flatTipsProofSRV);
        CancelLoadingMenuDeltaCapture();
        ReleaseVoidCom<ID3D11Query>(m_tipsDeltaQuery);
        ReleaseVoidCom<ID3D11Texture2D>(m_bgPlusTipsTex);
        ReleaseVoidCom<ID3D11RenderTargetView>(m_bgPlusTipsRTV);
        ReleaseVoidCom<ID3D11BlendState>(m_bgPlusTipsBlend);
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_tipsSrcSRV);
        ReleaseVoidCom<ID3D11Texture2D>(m_tipsBoundsStaging);
        ReleaseVoidCom<ID3D11RenderTargetView>(m_tipsBoundsRTV);
        ReleaseVoidCom<ID3D11Texture2D>(m_tipsBoundsTex);
        m_tipsContentBoundsValid = false;
        ReleaseVoidCom<ID3D11ShaderResourceView>(m_tipsScaleformSrcSRV);

        ReleaseVoidCom<ID3D11SamplerState>(m_sampler);
        ReleaseVoidCom<ID3D11Buffer>(m_constantBuffer);
        ReleaseVoidCom<ID3D11BlendState>(m_blendState);
        ReleaseVoidCom<ID3D11BlendState>(m_alphaBlendState);
        ReleaseVoidCom<ID3D11RasterizerState>(m_rasterState);
        ReleaseVoidCom<ID3D11DepthStencilState>(m_depthState);
        ReleaseVoidCom<ID3D11VertexShader>(m_vsFullscreen);
        ReleaseVoidCom<ID3D11PixelShader>(m_psLuminanceKey);
        ReleaseVoidCom<ID3D11PixelShader>(m_psBackground);
        ReleaseVoidCom<ID3D11PixelShader>(m_psBlit);
        ReleaseVoidCom<ID3D11PixelShader>(m_psTipsKey);
        ReleaseVoidCom<ID3D11PixelShader>(m_psTipsDelta);
        ReleaseVoidCom<ID3D11PixelShader>(m_psFlatTipsProof);
        ReleaseVoidCom<ID3D11DeviceContext>(m_context);

        m_device = nullptr;  // borrowed renderer pointer
        m_tempWidth = m_tempHeight = m_tempFormat = 0;
        m_tipsScaleformSrcCached = nullptr;
        m_tipsScaleformWidth = m_tipsScaleformHeight =
            m_tipsScaleformFormat = 0;
        m_loadingMenuCaptureSubresource = 0;
        m_loadingMenuCaptureWidth = m_loadingMenuCaptureHeight =
            m_loadingMenuCaptureFormat = 0;
        m_loadingMenuCaptureViewFormat = 0;
        m_loadingMenuCaptureSrvFormat = 0;
        m_loadingMenuCaptureViewport = {};
        m_loadingMenuCaptureScissor = {};
        m_loadingMenuCaptureRegion = {};
        m_bgPlusTipsWidth = m_bgPlusTipsHeight = 0;
        m_lastLeftEye = nullptr;
        m_lastRightEye = nullptr;
    }

    bool D3D11Compositor::RebuildDeviceResources(void* d3dDevice)
    {
        if (!d3dDevice) return false;
        std::lock_guard resourceLock(m_renderResourceMutex);

        // Both recovery entry points call HandleDeviceLoss first, which clears
        // OpenVR overlay ownership and invalidates the manager's old-device
        // textures before reaching this release. The later failure release is
        // also safe because the replacement device is published to the helper
        // only after every resource below has been created successfully.
        ReleaseDeviceResources();
        m_device = d3dDevice;
        auto* device = static_cast<ID3D11Device*>(m_device);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (!context) {
            logger::error(
                "D3D11Compositor: device recovery failed — no immediate context");
            m_device = nullptr;
            return false;
        }
        m_context = context;

        if (!CompileShaders() || !CreatePipelineResources()) {
            logger::error(
                "D3D11Compositor: device recovery failed — resource rebuild");
            ReleaseDeviceResources();
            return false;
        }

        VRCompositorHelper::SetDevice(
            reinterpret_cast<REX::W32::ID3D11Device*>(device));
        m_deviceLost.store(false, std::memory_order_release);
        m_renderReady.store(true, std::memory_order_release);
        LoadingScreenManager::GetSingleton().OnD3DDeviceReady();
        logger::info(
            "D3D11Compositor: device-dependent resources rebuilt (device={:x})",
            reinterpret_cast<std::uintptr_t>(device));
        return true;
    }

    void D3D11Compositor::HandleDeviceLoss(const char* reason)
    {
        if (m_deviceLost.exchange(true, std::memory_order_acq_rel)) return;

        logger::warn(
            "D3D11Compositor: renderer identity/loss detected ({}); "
            "custom presentation disarmed until rebuild",
            reason ? reason : "unknown");
        SetEnabled(false);
        ResetDeferredState();
        // Overlay handles can retain m_bgPlusTipsTex and other submitted D3D
        // textures. OpenVR must relinquish that ownership before either the
        // compositor resources or the manager's source DDS are released.
        VRCompositorHelper::SetDevice(nullptr);
        ReleaseDeviceResources();
        LoadingScreenManager::GetSingleton().OnD3DDeviceChanged();
        if (!m_isVR.load(std::memory_order_acquire)) {
            m_flatLazyInit.store(true, std::memory_order_release);
        }
    }

    // ========================================================================
    // Initialization
    // ========================================================================

    // ========================================================================
    // Flat mode initialization — hooks Present for background compositing
    // ========================================================================

    // SEH-safe helper: try to vtable-hook Present on a swapchain
    // Returns true if hook installed, false if pointer was invalid
    static bool TryVtableHookPresent_SEH(void* swapChain, void* hookFn, void** outOriginal,
                                         void*** outVtableSlot = nullptr)
    {
        __try {
            void** vtable = *reinterpret_cast<void***>(swapChain);
            void* presentPtr = vtable[8];

            // Sanity check: vtable and Present should be in valid memory range
            if (reinterpret_cast<std::uintptr_t>(vtable) < 0x10000 ||
                reinterpret_cast<std::uintptr_t>(presentPtr) < 0x10000) {
                return false;
            }

            DWORD oldProtect;
            if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                return false;
            }
            *outOriginal = vtable[8];
            // 8-byte aligned pointer write is hardware-atomic on x64 — use
            // InterlockedExchangePointer so concurrent readers can't observe
            // a torn slot.
            InterlockedExchangePointer(&vtable[8], hookFn);
            DWORD ignoredProtect = 0;
            VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &ignoredProtect);
            if (outVtableSlot) *outVtableSlot = &vtable[8];
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe helper: try to read device/swapchain from RendererData
    // Must be in its own function because __try can't coexist with C++ objects
    // SEH-safe: try to QI a pointer as IDXGISwapChain. Returns true if valid.
    static bool TryValidateSwapChain_SEH(void* candidate)
    {
        __try {
            // Check it looks like a valid 64-bit heap pointer
            auto addr = reinterpret_cast<std::uintptr_t>(candidate);
            if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF) return false;

            // Try QueryInterface for IDXGISwapChain to validate it's a real COM object
            auto* unk = static_cast<IUnknown*>(candidate);
            IDXGISwapChain* sc = nullptr;
            HRESULT hr = unk->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc));
            if (SUCCEEDED(hr) && sc) {
                sc->Release();
                return true;
            }
            return false;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool TryReadRendererData_SEH(void* rendererData, void** outDevice, void** outSwapChain)
    {
        __try {
            auto base = reinterpret_cast<std::uintptr_t>(rendererData);

            *outDevice = *reinterpret_cast<void**>(base + 0x48);
            if (!*outDevice) return false;

            // Try known swapchain offsets: +0x58 (vanilla), +0x50 (with ENB proxy)
            static const int scOffsets[] = { 0x58, 0x50, 0x70 };
            for (int off : scOffsets) {
                void* candidate = *reinterpret_cast<void**>(base + off);
                if (candidate && TryValidateSwapChain_SEH(candidate)) {
                    logger::info("D3D11Compositor: found swapchain at RendererData+0x{:x} = {:x}",
                        off, reinterpret_cast<std::uintptr_t>(candidate));
                    *outSwapChain = candidate;
                    return true;
                }
            }

            // No valid swapchain found at known offsets
            logger::warn("D3D11Compositor: device found but no valid swapchain in RendererData");
            *outSwapChain = nullptr;
            return false;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool D3D11Compositor::TryGetDeviceFromRendererData()
    {
        if (REL::Module::IsNG()) {
            // The pinned CommonLib maps RendererData::GetSingleton through ID
            // 1235449, which is absent from both verified AE address libraries.
            // Its non-VR lower_bound fallback can therefore return a neighbour.
            // NG acquires the real device/swap chain through the factory and
            // Present hooks below; never dereference that unresolved singleton.
            return false;
        }
        try {
            auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
            if (rendererData) {
                void* dev = nullptr;
                void* sc = nullptr;
                if (TryReadRendererData_SEH(rendererData, &dev, &sc)) {
                    m_device = dev;
                    {
                        std::lock_guard lock(m_flatSwapChainMutex);
                        if (!m_swapChain) m_swapChain = sc;
                    }
                    logger::info("D3D11Compositor: got device from RendererData");
                    return true;
                }
                logger::warn("D3D11Compositor: RendererData access failed, using dummy device fallback");
            }
        } catch (...) {
            logger::warn("D3D11Compositor: RendererData lookup failed, using dummy device fallback");
        }
        return false;
    }

    bool D3D11Compositor::ResolveAdvanceMovie()
    {
        std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
        if (m_advanceMovieAddr) return true;

        try {
            // OG Address Library ID 314582 is LoadingMenu's destructor at RVA
            // 0x1296D40, not AdvanceMovie. It is never a valid patch target;
            // touching it risks interfering with LoadingMenu lifecycle cleanup.
            // ID 618896 is primary-vtable slot 4, LoadingMenu::AdvanceMovie, in
            // Fallout4.exe 1.10.163.
            static constexpr std::uint64_t kAdvanceMovieID_OG = 618896;
            static constexpr std::uintptr_t kAdvanceMovieRVA_OG = 0x01297120;
            static constexpr std::uint64_t kAdvanceMovieID_NG = 2249218;
            static constexpr std::uintptr_t kAdvanceMovieRVA_NG_221 = 0x010667B0;
            static constexpr std::uintptr_t kAdvanceMovieRVA_NG_240 = 0x01066B40;
            static constexpr std::uint64_t kLoadingMenuPrimaryVtableID = 172552;
            static constexpr std::uintptr_t kLoadingMenuPrimaryVtableRVA_NG_221 =
                0x025A8368;
            static constexpr std::uintptr_t kLoadingMenuPrimaryVtableRVA_NG_240 =
                0x025B0458;
            static constexpr std::size_t kAdvanceMovieVtableSlot = 4;
            static constexpr std::array<
                std::uint8_t, kAdvanceMovieVerifiedPrefixSize>
                kAdvanceMovieEntry_OG{
                    0x48, 0x8B, 0xC4, 0x55, 0x53, 0x57, 0x48, 0x8D,
                    0x68, 0xD8, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00,
                    0x00, 0x0F, 0x29, 0x70, 0xC8, 0x44, 0x0F, 0x29,
                    0x40, 0xA8
                };
            static constexpr std::array<
                std::uint8_t, kAdvanceMovieVerifiedPrefixSize>
                kAdvanceMovieEntry_NG{
                    0x48, 0x8B, 0xC4, 0x53, 0x57, 0x48, 0x81, 0xEC,
                    0xC8, 0x00, 0x00, 0x00, 0x48, 0x89, 0x68, 0x10,
                    0x48, 0x8B, 0xF9, 0x48, 0x89, 0x70, 0x20, 0x49,
                    0x8B, 0xF0
                };
            const bool isNG = REL::Module::IsNG();
            const auto version = REL::Module::get().version();
            const bool exactNG221 =
                isNG && version[0] == 1 && version[1] == 11 &&
                version[2] == 221 && version[3] == 0;
            const bool exactNG240 =
                isNG && version[0] == 1 && version[1] == 11 &&
                version[2] == 240 && version[3] == 0;
            if (isNG && !exactNG221 && !exactNG240) {
                logger::warn(
                    "D3D11Compositor: AdvanceMovie unavailable on unverified "
                    "NG runtime {}.{}.{}.{}",
                    version[0], version[1], version[2], version[3]);
                return false;
            }

            REL::Relocation<std::uintptr_t> advanceMovie{
                REL::RelocationID(kAdvanceMovieID_OG, kAdvanceMovieID_NG)
            };
            REL::Relocation<std::uintptr_t> loadingMenuPrimaryVtable{
                REL::ID(kLoadingMenuPrimaryVtableID)
            };
            const auto address = advanceMovie.address();
            std::uint8_t original = 0;
            const auto moduleBase = REL::Module::get().base();
            const auto advanceMovieRva = address >= moduleBase ?
                address - moduleBase : 0;
            const auto vtableAddress = loadingMenuPrimaryVtable.address();
            const auto vtableRva = vtableAddress >= moduleBase ?
                vtableAddress - moduleBase : 0;
            const auto expectedNGAdvanceMovieRva = exactNG221 ?
                kAdvanceMovieRVA_NG_221 : kAdvanceMovieRVA_NG_240;
            const auto expectedNGVtableRva = exactNG221 ?
                kLoadingMenuPrimaryVtableRVA_NG_221 :
                kLoadingMenuPrimaryVtableRVA_NG_240;
            const bool exactNGMappings =
                !isNG ||
                (advanceMovieRva == expectedNGAdvanceMovieRva &&
                 vtableRva == expectedNGVtableRva);
            std::uintptr_t vtableAdvanceMovie = 0;
            const bool exactVtableSlot = exactNGMappings &&
                TryReadModulePointer_SEH(
                    vtableAddress +
                        kAdvanceMovieVtableSlot * sizeof(std::uintptr_t),
                    &vtableAdvanceMovie) &&
                vtableAdvanceMovie == address;
            const bool exactRuntimeEntry = isNG ?
                (exactNGMappings &&
                 ExecutableBytesEqual(
                     address, kAdvanceMovieEntry_NG.data(),
                     kAdvanceMovieEntry_NG.size())) :
                (address >= moduleBase &&
                 address - moduleBase == kAdvanceMovieRVA_OG &&
                 ExecutableBytesEqual(
                     address, kAdvanceMovieEntry_OG.data(),
                     kAdvanceMovieEntry_OG.size()));
            // OG requires exact ID/RVA/prologue identity so the destructor can
            // never be mistaken for this patch target again. NG additionally
            // proves the exact runtime, both Address Library ID/RVA mappings,
            // vtable ownership, and the full entry prefix before allowing RET.
            if (!exactVtableSlot || !exactRuntimeEntry ||
                !TryReadExecutableByte_SEH(address, &original) ||
                original != 0x48) {
                logger::warn(
                    "D3D11Compositor: rejected unexpected AdvanceMovie entry "
                    "at {:x} (RVA={:#x}, byte=0x{:02x}, vtable={:#x}, "
                    "vtableRVA={:#x}, vtableSlot4={:#x})",
                    address,
                    advanceMovieRva, original, vtableAddress, vtableRva,
                    vtableAdvanceMovie);
                return false;
            }
            m_advanceMovieAddr = address;
            m_advanceMovieOrigBytes = isNG ?
                kAdvanceMovieEntry_NG : kAdvanceMovieEntry_OG;
            m_advanceMovieOrigByte = original;
            logger::info("D3D11Compositor: AdvanceMovie cached at {:x} (orig byte 0x{:02x})",
                address, original);
            return true;
        } catch (...) {
            logger::warn("D3D11Compositor: AdvanceMovie not resolvable — mode-2/3 kill disabled");
            return false;
        }
    }

    bool D3D11Compositor::TryRestoreAdvanceMovie()
    {
        std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
        return TryRestoreAdvanceMovieLocked();
    }

    bool D3D11Compositor::TryRestoreAdvanceMovieLocked()
    {
        if (!m_advanceMovieKilled.load(std::memory_order_relaxed)) {
            return true;
        }
        if (!m_advanceMovieAddr) {
            if (!m_advanceMovieRestoreReadFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                logger::error(
                    "Flat: AdvanceMovie restore retained: owned RET has no address");
            }
            return false;
        }

        static constexpr std::uint8_t RET = 0xC3;
        std::uint8_t current = 0;
        if (!TryReadExecutableByte_SEH(m_advanceMovieAddr, &current)) {
            // Crucially, do not clear ownership here. The live byte may still be
            // our RET; the primary Present hook retries until the site is
            // readable or an exact foreign byte proves ownership was lost.
            if (!m_advanceMovieRestoreReadFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                logger::error(
                    "Flat: AdvanceMovie restore retained for retry: "
                    "entry unreadable at {:x}",
                    m_advanceMovieAddr);
            }
            return false;
        }

        if (current != RET) {
            // A readable non-RET proves the site is no longer ours. Never
            // overwrite a foreign owner with the cached startup byte.
            m_advanceMovieKilled.store(false, std::memory_order_release);
            m_advanceMovieRestoreReadFailureLogged.store(
                false, std::memory_order_release);
            logger::warn(
                "Flat: AdvanceMovie restore skipped: ownership lost "
                "(current=0x{:02x})",
                current);
            return true;
        }

        // Our patch changes byte 0 only. If any byte in the verified function
        // tail changed while RET was installed, restoring 0x48 could activate
        // code now owned by another component. Retain the inert RET and block a
        // new custom-load epoch until the exact original tail is observable.
        if (!ExecutableBytesEqual(
                m_advanceMovieAddr + 1,
                m_advanceMovieOrigBytes.data() + 1,
                m_advanceMovieOrigBytes.size() - 1)) {
            if (!m_advanceMovieRestoreReadFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                logger::error(
                    "Flat: AdvanceMovie restore retained at {:x}: verified "
                    "function tail changed while RET was installed",
                    m_advanceMovieAddr);
            }
            return false;
        }

        REL::safe_write(m_advanceMovieAddr, &m_advanceMovieOrigByte, 1);
        std::uint8_t restored = 0;
        if (!TryReadExecutableByte_SEH(m_advanceMovieAddr, &restored)) {
            if (!m_advanceMovieRestoreReadFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                logger::error(
                    "Flat: AdvanceMovie restore postcondition unreadable at "
                    "{:x}; RET ownership retained for retry",
                    m_advanceMovieAddr);
            }
            return false;
        }
        if (restored == m_advanceMovieOrigByte) {
            m_advanceMovieKilled.store(false, std::memory_order_release);
            m_advanceMovieRestoreReadFailureLogged.store(
                false, std::memory_order_release);
            logger::info(
                "Flat: AdvanceMovie restored at {:x}",
                m_advanceMovieAddr);
            return true;
        }
        if (restored != RET) {
            m_advanceMovieKilled.store(false, std::memory_order_release);
            m_advanceMovieRestoreReadFailureLogged.store(
                false, std::memory_order_release);
            logger::warn(
                "Flat: AdvanceMovie restore lost ownership during write "
                "(current=0x{:02x})",
                restored);
            return true;
        }

        if (!m_advanceMovieRestoreReadFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            logger::error(
                "Flat: AdvanceMovie restore write was not observed at {:x}; "
                "RET ownership retained for retry",
                m_advanceMovieAddr);
        }
        return false;
    }

    bool D3D11Compositor::CreatePipelineResources()
    {
        if (!m_device) return false;
        if (m_sampler && m_constantBuffer && m_blendState && m_alphaBlendState &&
            m_rasterState && m_depthState) {
            return true;
        }

        ReleaseVoidCom<ID3D11SamplerState>(m_sampler);
        ReleaseVoidCom<ID3D11Buffer>(m_constantBuffer);
        ReleaseVoidCom<ID3D11BlendState>(m_blendState);
        ReleaseVoidCom<ID3D11BlendState>(m_alphaBlendState);
        ReleaseVoidCom<ID3D11RasterizerState>(m_rasterState);
        ReleaseVoidCom<ID3D11DepthStencilState>(m_depthState);

        auto* device = static_cast<ID3D11Device*>(m_device);
        ID3D11SamplerState* sampler = nullptr;
        ID3D11Buffer* constantBuffer = nullptr;
        ID3D11BlendState* opaqueBlend = nullptr;
        ID3D11BlendState* alphaBlend = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* depth = nullptr;

        auto fail = [&](const char* object, HRESULT hr) {
            logger::error("D3D11Compositor: {} creation failed (hr={:x})",
                object, static_cast<unsigned>(hr));
            ReleaseCom(sampler);
            ReleaseCom(constantBuffer);
            ReleaseCom(opaqueBlend);
            ReleaseCom(alphaBlend);
            ReleaseCom(raster);
            ReleaseCom(depth);
            return false;
        };

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        HRESULT hr = device->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr) || !sampler) return fail("sampler", hr);

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(CompositeParams);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&bufferDesc, nullptr, &constantBuffer);
        if (FAILED(hr) || !constantBuffer) return fail("constant buffer", hr);

        D3D11_BLEND_DESC opaqueDesc{};
        opaqueDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&opaqueDesc, &opaqueBlend);
        if (FAILED(hr) || !opaqueBlend) return fail("opaque blend state", hr);

        D3D11_BLEND_DESC alphaDesc{};
        alphaDesc.RenderTarget[0].BlendEnable = TRUE;
        alphaDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        alphaDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        alphaDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        // Preserve the destination alpha. Some wrappers/capture compositors
        // honor swapchain alpha; replacing it with sparse tip coverage can
        // turn untouched background pixels into transparent black.
        alphaDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        alphaDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        alphaDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        alphaDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&alphaDesc, &alphaBlend);
        if (FAILED(hr) || !alphaBlend) return fail("alpha blend state", hr);

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        hr = device->CreateRasterizerState(&rasterDesc, &raster);
        if (FAILED(hr) || !raster) return fail("rasterizer state", hr);

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = FALSE;
        depthDesc.StencilEnable = FALSE;
        hr = device->CreateDepthStencilState(&depthDesc, &depth);
        if (FAILED(hr) || !depth) return fail("depth-stencil state", hr);

        ReleaseVoidCom<ID3D11SamplerState>(m_sampler);
        ReleaseVoidCom<ID3D11Buffer>(m_constantBuffer);
        ReleaseVoidCom<ID3D11BlendState>(m_blendState);
        ReleaseVoidCom<ID3D11BlendState>(m_alphaBlendState);
        ReleaseVoidCom<ID3D11RasterizerState>(m_rasterState);
        ReleaseVoidCom<ID3D11DepthStencilState>(m_depthState);
        m_sampler = sampler;
        m_constantBuffer = constantBuffer;
        m_blendState = opaqueBlend;
        m_alphaBlendState = alphaBlend;
        m_rasterState = raster;
        m_depthState = depth;
        return true;
    }

    bool D3D11Compositor::InitializeFlat()
    {
        if (m_initialized.load(std::memory_order_acquire)) return true;

        m_renderReady.store(false, std::memory_order_release);
        m_isVR.store(false, std::memory_order_release);
        s_instance = this;

        // Flat mode 3 must never infer tip readiness from elapsed time or
        // Present cadence. Install the exact native publication hook up front;
        // failure is fail-open (custom compositing may continue, but the SWF is
        // never frozen). The installers accept only the explicitly verified
        // 1.10.163, 1.11.221 and 1.11.240 mappings and entry signatures.
        if (!REL::Module::IsVR()) {
            const bool selectionHookReady =
                InstallFlatSendLoadingTextHook();
            if (!selectionHookReady) {
                logger::warn(
                    "Flat tips: exact native selection hook unavailable; "
                    "mode 3 AdvanceMovie freeze disabled");
            } else if (!InstallFlatTipDrawHooks()) {
                // Background-only native selections remain safe to freeze from
                // the selection hook. Tip-bearing loads fail open and keep the
                // movie live when exact draw proof is unavailable.
                logger::warn(
                    "Flat tips: exact native draw hooks unavailable; "
                    "tip-bearing mode 3 loads will remain live");
            }
        }

        // Both hook strategies need this cached before Present can run. In
        // particular, Strategy 1 used to return before resolving the address.
        ResolveAdvanceMovie();

        // Detect an active ENB-style d3d11 forwarding proxy. Marker INIs and
        // d3dcompiler_46e.dll alone are not evidence that presentation is
        // proxied; old installations commonly leave those files behind.
        const bool enbPresent = HasLoadedD3D11Proxy();
        const bool enbMarkersPresent =
            std::filesystem::exists("enbseries.ini") ||
            std::filesystem::exists("enblocal.ini") ||
            (GetModuleHandleA("d3dcompiler_46e.dll") != nullptr);
        if (enbPresent) {
            logger::info(
                "D3D11Compositor: loaded non-system d3d11 forwarding proxy detected");
        } else if (enbMarkersPresent) {
            logger::info(
                "D3D11Compositor: ignoring ENB marker files/compiler because "
                "the loaded d3d11 runtime is native");
        }

        // Always try to get device from RendererData (works with or without ENB)
        bool hasRendererDevice = false;
        if (TryGetDeviceFromRendererData()) {
            hasRendererDevice = (m_device != nullptr);
            logger::info("D3D11Compositor: RendererData device={:x}, swapchain={:x}",
                reinterpret_cast<std::uintptr_t>(m_device),
                reinterpret_cast<std::uintptr_t>(m_swapChain));
        }

        // Generic present-proxy detection (no hardcoded mod names).
        // Native IDXGISwapChain::Present is normally owned by System32\dxgi.dll,
        // not d3d11.dll. Only a non-system owner is a separate present proxy
        // (frame generation/upscaler/ReShade). Compare the full module path so a
        // game-local dxgi.dll cannot masquerade as the native runtime. A local
        // d3d11.dll remains on the ENB-forwarding path selected above.
        bool proxyDetected = false;
        void* gamePresent = nullptr;
        if (m_swapChain) {
            gamePresent = (*reinterpret_cast<void***>(m_swapChain))[8];
            HMODULE ownerMod = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(gamePresent), &ownerMod) && ownerMod) {
                wchar_t modPath[MAX_PATH] = {};
                GetModuleFileNameW(ownerMod, modPath, MAX_PATH);
                const wchar_t* base = wcsrchr(modPath, L'\\');
                base = base ? base + 1 : modPath;
                const bool nativeRuntime = IsNativeSystemGraphicsRuntime(modPath);
                const bool d3d11Forwarder = _wcsicmp(base, L"d3d11.dll") == 0;
                if (!nativeRuntime && !d3d11Forwarder) {
                    proxyDetected = true;
                    char nb[MAX_PATH] = {};
                    WideCharToMultiByte(CP_UTF8, 0, base, -1, nb, sizeof(nb), nullptr, nullptr);
                    logger::info("D3D11Compositor: present proxy detected — game swapchain "
                                 "Present owned by non-system '{}'; hooking it directly", nb);
                } else if (nativeRuntime) {
                    char nb[MAX_PATH] = {};
                    WideCharToMultiByte(CP_UTF8, 0, base, -1, nb, sizeof(nb), nullptr, nullptr);
                    logger::info("D3D11Compositor: native system Present owner '{}' detected", nb);
                }
            }
        }

        // ================================================================
        // Strategy 1: RendererData → vtable hook (NO ENB, NO proxy only)
        // With ENB, RendererData's swapchain is ENB's internal object,
        // not the game's presentation path. Vtable-hooking it breaks
        // post-loading rendering. Skip to Strategy 2 for ENB.
        // With a present proxy (framegen/upscaler) we also route to Strategy 2,
        // which inline-hooks the proxy's Present function (vtable[8] of the game
        // swapchain) rather than swapping the vtable pointer — safer for a
        // foreign proxy.
        // ================================================================
        if (!enbPresent && !proxyDetected && hasRendererDevice && m_swapChain) {
            logger::info("D3D11Compositor: got device+swapchain from RendererData");

            VRCompositorHelper::SetDevice(reinterpret_cast<REX::W32::ID3D11Device*>(m_device));
            LoadingScreenManager::GetSingleton().OnD3DDeviceReady();

            auto* device = static_cast<ID3D11Device*>(m_device);
            ID3D11DeviceContext* ctx = nullptr;
            device->GetImmediateContext(&ctx);
            if (!ctx) {
                logger::error("D3D11Compositor: RendererData path — no context");
                goto fallback;
            }
            m_context = ctx;

            if (!CompileShaders()) {
                logger::error("D3D11Compositor: RendererData path — shader compilation failed");
                ctx->Release();
                m_context = nullptr;
                goto fallback;
            }

            if (!CreatePipelineResources()) {
                logger::error("D3D11Compositor: RendererData path — D3D state creation failed");
                ctx->Release();
                m_context = nullptr;
                goto fallback;
            }

            // Vtable hook Present — atomic pointer swap, not inline patching.
            {
                void* originalPresent = nullptr;
                void** slot = nullptr;
                if (TryVtableHookPresent_SEH(m_swapChain,
                        reinterpret_cast<void*>(&HookedPresentFlat), &originalPresent, &slot)) {
                    s_originalPresentFlat = reinterpret_cast<decltype(s_originalPresentFlat)>(originalPresent);
                    s_presentVtableSlot = slot;
                    s_originalPresentFlatForRestore = originalPresent;
                    m_flatLazyInit.store(false, std::memory_order_release);
                    m_renderReady.store(true, std::memory_order_release);
                    m_initialized.store(true, std::memory_order_release);
                    logger::info("D3D11Compositor: Present vtable-hooked via RendererData, original={:x}",
                        reinterpret_cast<std::uintptr_t>(originalPresent));
                    return true;
                }
                logger::warn("D3D11Compositor: swapchain vtable hook failed (invalid pointer?), "
                             "falling through to Strategy 2");
                goto fallback;
            }
        }

    fallback:
        // ================================================================
        // Strategy 2: MinHook inline hook on real DXGI Present
        // When ENB is present, load the real system d3d11.dll (bypassing
        // ENB's proxy) to create a dummy swapchain and find the real DXGI
        // Present address. ENB wraps at the COM level (wrapper objects),
        // not inline-patching DXGI code, so MinHook on the real Present
        // is safe. Without ENB, use normal D3D11CreateDeviceAndSwapChain.
        // ================================================================

        // Keep device from RendererData if we got one
        // Only reset swapchain — we'll capture it from the first Present call
        {
            bool hadDevice = (m_device != nullptr);
            // Capture the game's actual swapchain (the frontmost proxy when a
            // frame-gen mod is loaded) before we null it — the FG-proxy hook
            // path below needs its vtable to find the proxy's Present.
            void* gameSwapChain = m_swapChain;
            m_swapChain = nullptr;

            // If we have a device from RendererData, complete D3D setup now
            // (shaders, state objects, VRCompositorHelper — skipped when Strategy 1 was bypassed)
            if (hadDevice && !m_context) {
                auto* device = static_cast<ID3D11Device*>(m_device);
                VRCompositorHelper::SetDevice(reinterpret_cast<REX::W32::ID3D11Device*>(device));
                LoadingScreenManager::GetSingleton().OnD3DDeviceReady();

                ID3D11DeviceContext* ctx = nullptr;
                device->GetImmediateContext(&ctx);
                if (ctx) {
                    m_context = ctx;
                    if (CompileShaders() && CreatePipelineResources()) {
                        logger::info("D3D11Compositor: Strategy 2 — D3D setup complete with RendererData device");
                    } else {
                        logger::warn("D3D11Compositor: D3D setup failed, will retry in CompleteFlatInit");
                        ctx->Release();
                        m_context = nullptr;
                        hadDevice = false;  // force lazy init
                    }
                } else {
                    logger::warn("D3D11Compositor: no context from RendererData device");
                    hadDevice = false;
                }
            } else if (!hadDevice) {
                m_context = nullptr;
            }

            logger::info("D3D11Compositor: Strategy 2 — MinHook on real DXGI Present (hadDevice={})", hadDevice);

            MH_STATUS mhStatus = MH_Initialize();
            if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error("D3D11Compositor: MinHook init failed ({})", MH_StatusToString(mhStatus));
                return false;
            }

            // When ENB is present, load the REAL system d3d11.dll to bypass the proxy.
            // ENB's d3d11.dll in the game directory intercepts D3D11CreateDeviceAndSwapChain
            // and returns wrapper objects. We need the real DXGI swapchain vtable.
            void* presentAddr = nullptr;

            if (proxyDetected && gameSwapChain) {
                // Present-proxy path: the game's swapchain (gameSwapChain, from
                // RendererData+0x70) IS the proxy object the game calls Present
                // on, and its Present lives in a non-d3d11 module. Hook that
                // Present (vtable[8]) directly — the proxy presents via its own
                // path (often D3D12) so the real d3d11 Present is never reached.
                // Re-store m_swapChain so CompositeFlatFrame uses the proxy for
                // GetBuffer (the proxy returns the live D3D11 backbuffer).
                m_swapChain = gameSwapChain;
                presentAddr = gamePresent;  // already = gameSwapChain vtable[8]
                logger::info("D3D11Compositor: hooking present-proxy Present at {:x} "
                             "(game swapchain vtable[8])",
                    reinterpret_cast<std::uintptr_t>(presentAddr));
            } else {
                using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
                    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
                    IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
                CreateFn createFunc = &D3D11CreateDeviceAndSwapChain;  // default: normal path

                HMODULE realD3D11 = nullptr;
                if (enbPresent) {
                    wchar_t sysDir[MAX_PATH];
                    GetSystemDirectoryW(sysDir, MAX_PATH);
                    std::wstring realPath = std::wstring(sysDir) + L"\\d3d11.dll";
                    realD3D11 = LoadLibraryW(realPath.c_str());
                    if (realD3D11) {
                        auto realCreate = reinterpret_cast<CreateFn>(
                            GetProcAddress(realD3D11, "D3D11CreateDeviceAndSwapChain"));
                        if (realCreate) {
                            createFunc = realCreate;
                            logger::info("D3D11Compositor: loaded real system d3d11.dll, bypassing ENB proxy");
                        } else {
                            logger::warn("D3D11Compositor: couldn't find export in system d3d11.dll");
                        }
                    } else {
                        logger::warn("D3D11Compositor: couldn't load system d3d11.dll");
                    }
                }

                WNDCLASSEXA wc = {};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = DefWindowProcA;
                wc.hInstance = GetModuleHandleA(nullptr);
                wc.lpszClassName = "LoadingScreensDummy";
                const ATOM classAtom = RegisterClassExA(&wc);
                const bool ownsWindowClass = classAtom != 0;
                if (!classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                    if (realD3D11) FreeLibrary(realD3D11);
                    logger::error("D3D11Compositor: dummy window class registration failed ({})",
                        GetLastError());
                    return false;
                }

                HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                    0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
                if (!hWnd) {
                    if (ownsWindowClass) UnregisterClassA(wc.lpszClassName, wc.hInstance);
                    if (realD3D11) FreeLibrary(realD3D11);
                    logger::error("D3D11Compositor: dummy window creation failed ({})", GetLastError());
                    return false;
                }

                ID3D11Device* dummyDev = nullptr;
                ID3D11DeviceContext* dummyCtx = nullptr;
                IDXGISwapChain* dummySC = nullptr;
                D3D_FEATURE_LEVEL fl;

                DXGI_SWAP_CHAIN_DESC sd = {};
                sd.BufferCount = 1;
                sd.BufferDesc.Width = 2;
                sd.BufferDesc.Height = 2;
                sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.OutputWindow = hWnd;
                sd.SampleDesc.Count = 1;
                sd.Windowed = TRUE;
                sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

                HRESULT hr = createFunc(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                    &dummySC, &dummyDev, &fl, &dummyCtx);

                if (FAILED(hr) || !dummySC) {
                    ReleaseCom(dummySC);
                    ReleaseCom(dummyCtx);
                    ReleaseCom(dummyDev);
                    DestroyWindow(hWnd);
                    if (ownsWindowClass) UnregisterClassA(wc.lpszClassName, wc.hInstance);
                    if (realD3D11) FreeLibrary(realD3D11);
                    logger::error("D3D11Compositor: dummy device creation failed (hr={:x})", (unsigned)hr);
                    return false;
                }

                void** vtable = *reinterpret_cast<void***>(dummySC);
                presentAddr = vtable[8];
                logger::info("D3D11Compositor: discovered real DXGI Present at {:x}{}",
                    reinterpret_cast<std::uintptr_t>(presentAddr),
                    enbPresent ? " (via system d3d11.dll)" : "");

                ReleaseCom(dummySC);
                ReleaseCom(dummyCtx);
                ReleaseCom(dummyDev);
                DestroyWindow(hWnd);
                if (ownsWindowClass) UnregisterClassA(wc.lpszClassName, wc.hInstance);
                if (realD3D11) FreeLibrary(realD3D11);
            }

            if (!presentAddr) {
                logger::error("D3D11Compositor: no valid Present address was discovered");
                return false;
            }

            void* originalTrampoline = nullptr;
            mhStatus = MH_CreateHook(presentAddr, reinterpret_cast<void*>(&HookedPresentFlat),
                &originalTrampoline);
            if (mhStatus != MH_OK) {
                logger::error("D3D11Compositor: MH_CreateHook failed ({})", MH_StatusToString(mhStatus));
                return false;
            }
            if (!originalTrampoline) {
                logger::error("D3D11Compositor: MH_CreateHook returned no Present trampoline");
                MH_RemoveHook(presentAddr);
                return false;
            }

            s_originalPresentFlat = reinterpret_cast<decltype(s_originalPresentFlat)>(originalTrampoline);

            mhStatus = MH_EnableHook(presentAddr);
            if (mhStatus != MH_OK) {
                logger::error("D3D11Compositor: MH_EnableHook failed ({})", MH_StatusToString(mhStatus));
                MH_RemoveHook(presentAddr);  // don't leak the created-but-disabled hook
                return false;
            }

            // If we already have a device from RendererData (shaders+state created),
            // we just need the swapchain — captured from first Present call
            m_flatLazyInit.store(!hadDevice, std::memory_order_release);
            m_renderReady.store(hadDevice, std::memory_order_release);
            m_initialized.store(true, std::memory_order_release);
            logger::info("D3D11Compositor: Present inline-hooked via MinHook (trampoline={:x}, lazyInit={})",
                reinterpret_cast<std::uintptr_t>(originalTrampoline),
                m_flatLazyInit.load(std::memory_order_acquire));
            return true;
        }
    }

    // ========================================================================
    // Flat lazy init — completes initialization on first Present call
    // (used when RendererData was unavailable, e.g. NG without address library)
    // ========================================================================

    bool D3D11Compositor::AcceptFlatSwapChain(void* swapChain)
    {
        if (!swapChain) return false;
        auto* candidate = static_cast<IDXGISwapChain*>(swapChain);
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(candidate->GetDesc(&desc)) || !desc.OutputWindow) return false;

        DWORD windowProcess = 0;
        GetWindowThreadProcessId(desc.OutputWindow, &windowProcess);
        if (windowProcess != GetCurrentProcessId()) return false;
        if (const HWND mainWindow = GetLargestProcessWindow();
            mainWindow && desc.OutputWindow != mainWindow) {
            return false;
        }

        ID3D11Device* candidateDevice = nullptr;
        const HRESULT deviceHr = candidate->GetDevice(
            __uuidof(ID3D11Device),
            reinterpret_cast<void**>(&candidateDevice));
        bool replacement = false;
        bool deviceChanged = false;
        {
            std::lock_guard lock(m_flatSwapChainMutex);
            if (m_swapChain == swapChain) {
                ReleaseCom(candidateDevice);
                return true;
            }

            replacement = m_swapChain != nullptr;
            deviceChanged = replacement && m_device && SUCCEEDED(deviceHr) &&
                candidateDevice && candidateDevice != m_device;
            m_swapChain = swapChain;
        }

        // A wrapper chain may expose a renderer-facing device while its real
        // DXGI Present receives the underlying device. Raw interface-pointer
        // equality is therefore diagnostic only. The validated main output
        // window remains authoritative, but a later chain for that same window
        // is a legitimate DXGI recreation and replaces the stale pointer.
        if (m_device) {
            const bool sameDevice =
                SUCCEEDED(deviceHr) && candidateDevice == m_device;
            if (!sameDevice) {
                logger::info(
                    "D3D11Compositor: primary-window swapchain uses a "
                    "wrapped/replacement device");
            }
        }

        if (replacement) {
            logger::info(
                "D3D11Compositor: accepted recreated primary swapchain {:x} "
                "(deviceChanged={})",
                reinterpret_cast<std::uintptr_t>(swapChain), deviceChanged);
        } else {
            logger::info("D3D11Compositor: selected primary swapchain {:x}",
                reinterpret_cast<std::uintptr_t>(swapChain));
        }
        ReleaseCom(candidateDevice);

        if (deviceChanged) {
            HandleDeviceLoss("flat primary swapchain device changed");
        }
        return true;
    }

    bool D3D11Compositor::CompleteFlatInit(void* swapChain)
    {
        m_renderReady.store(false, std::memory_order_release);
        auto* sc = static_cast<IDXGISwapChain*>(swapChain);
        if (!sc) return false;
        ID3D11Device* dev = nullptr;
        bool releaseDeviceRef = false;
        const HRESULT deviceHr = sc->GetDevice(
            __uuidof(ID3D11Device), reinterpret_cast<void**>(&dev));
        if (FAILED(deviceHr) || !dev) {
            // Fallback: get device from RendererData (works if swapchain is wrapped by another mod)
            logger::warn("D3D11Compositor: GetDevice from swapchain failed, trying RendererData");
            if (!TryGetDeviceFromRendererData()) {
                logger::error("D3D11Compositor: CompleteFlatInit failed — no device");
                return false;
            }
            dev = static_cast<ID3D11Device*>(m_device);
            // TryGetDeviceFromRendererData may have found an internal wrapper;
            // the hook-selected primary chain remains authoritative.
            {
                std::lock_guard lock(m_flatSwapChainMutex);
                m_swapChain = swapChain;
            }
        } else {
            releaseDeviceRef = true;  // GetDevice returned an owned COM reference
            m_device = dev;
        }

        // Also give the device to VRCompositorHelper for DDS texture loading
        VRCompositorHelper::SetDevice(reinterpret_cast<REX::W32::ID3D11Device*>(dev));
        LoadingScreenManager::GetSingleton().OnD3DDeviceReady();

        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (!ctx) {
            logger::error("D3D11Compositor: CompleteFlatInit failed — no context");
            if (releaseDeviceRef) dev->Release();
            return false;
        }
        m_context = ctx;

        if (!CompileShaders()) {
            logger::error("D3D11Compositor: CompleteFlatInit failed — shader compilation");
            ctx->Release();
            m_context = nullptr;
            if (releaseDeviceRef) dev->Release();
            return false;
        }

        if (!CreatePipelineResources()) {
            logger::error("D3D11Compositor: CompleteFlatInit failed — D3D state creation");
            ctx->Release();
            m_context = nullptr;
            if (releaseDeviceRef) dev->Release();
            return false;
        }

        m_flatLazyInit.store(false, std::memory_order_release);
        m_deviceLost.store(false, std::memory_order_release);
        m_renderReady.store(true, std::memory_order_release);
        logger::info("D3D11Compositor: flat lazy init complete (device={:x})",
            reinterpret_cast<std::uintptr_t>(dev));

        if (releaseDeviceRef) dev->Release();

        return true;
    }

    // ========================================================================
    // Flat Present hook — composites background behind loading screen content
    // ========================================================================

    HRESULT WINAPI D3D11Compositor::HookedPresentFlat(void* swapChain, UINT syncInterval, UINT flags)
    {
        auto* self = s_instance;
        auto flatTipsDecision = Policy::FlatTipsFreezeDecision::kWait;
        bool flatTipsReadyFrameCandidate = false;
        std::uint64_t flatTipsReadySerial = 0;
        std::uint64_t flatTipsLoadEpoch = 0;
        std::uint32_t flatTipsReadyFrame = 0;
        auto flatNativeContent =
            Policy::FlatNativeLoadingContent::kUnknown;

        // Minimize/restore and DXGI's occlusion probes must be completely
        // transparent. In particular, do not run callbacks, touch compositor
        // state, or alter the caller's sync interval while the output is hidden.
        if ((flags & DXGI_PRESENT_TEST) != 0) {
            return s_originalPresentFlat
                ? s_originalPresentFlat(swapChain, syncInterval, flags)
                : E_FAIL;
        }
        if (swapChain) {
            DXGI_SWAP_CHAIN_DESC desc{};
            auto* sc = static_cast<IDXGISwapChain*>(swapChain);
            if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow &&
                (IsIconic(desc.OutputWindow) || !IsWindowVisible(desc.OutputWindow))) {
                return s_originalPresentFlat
                    ? s_originalPresentFlat(swapChain, syncInterval, flags)
                    : E_FAIL;
            }
        }

        // Strategy 2 patches a shared Present implementation. Ignore secondary
        // swapchains completely so they cannot drive callbacks/counters, receive
        // our VSync policy, or race the primary chain's D3D state.
        if (self && !self->AcceptFlatSwapChain(swapChain)) {
            return s_originalPresentFlat
                ? s_originalPresentFlat(swapChain, syncInterval, flags)
                : E_FAIL;
        }

        // Lazy init: complete setup on first Present call
        if (self && self->m_flatLazyInit.exchange(false, std::memory_order_acq_rel)) {
            if (!self->CompleteFlatInit(swapChain)) {
                logger::error("D3D11Compositor: lazy initialization failed; Present will pass through");
                // Transient device/context/shader failures are retryable on the
                // next primary Present rather than permanently disarming init.
                self->m_flatLazyInit.store(true, std::memory_order_release);
            }
        }

        // A failed live-byte read at native CLOSE retains AdvanceMovie RET
        // ownership. Retry from subsequent primary Presents until we either
        // restore our exact byte or a readable foreign byte proves ownership
        // was lost.
        if (self &&
            !self->m_enabled.load(std::memory_order_acquire) &&
            self->m_advanceMovieKilled.load(std::memory_order_acquire)) {
            self->TryRestoreAdvanceMovie();
        }

        if (self) {
            // OG has no deferred load state to service outside LoadingMenu. Keep
            // its always-installed Present hook inert between loads. NG/AE still
            // needs the callback while disabled because it owns deferred
            // load-enable/close signaling.
            if (self->m_enabled.load(std::memory_order_acquire) || REL::Module::IsNG()) {
                if (auto callback = self->m_frameCallback.load(std::memory_order_acquire)) {
                    callback();
                }
            }

            if (self->m_enabled.load(std::memory_order_acquire)) {
                const int flatMode = self->m_flatMode.load(std::memory_order_acquire);
                const auto presentCount = self->m_flatPresentCount.load(std::memory_order_relaxed);
                // Mode 0 (Blank): killed early in OnLoadingMenuOpen
                // Mode 1 (Native): no kill — game renders tips/spinner natively
                // Mode 2 (BG only): kill immediately, composite background-only
                // Mode 3 (BG+tips): every verified flat runtime follows
                // SendLoadingText's exact native content branch. A true native
                // background-only/minimal load presents and then freezes solid
                // black. Tip-bearing loads keep rendering over custom art until
                // an owner-matched visible DisplayMovie and a later successful
                // Present prove the native UI reached the screen.
                // There is deliberately no time-only fallback: unavailable
                // proof leaves AdvanceMovie running on OG and NG alike.
                //   Mode 2: also flip to bg-only — tips not wanted.
                //   Mode 3: keep luminance-key running with a raised threshold
                //           (see SetEnabled) so the frozen tip/level pixels
                //           in the backbuffer get preserved every frame while
                //           game bg art (mid-tone) is replaced by our bg.
                if (!self->m_advanceMovieKilled.load(std::memory_order_acquire) &&
                    !self->m_isVR.load(std::memory_order_acquire)) {
                    const int killFrames = self->m_advanceMovieKillFrames.load(std::memory_order_acquire);
                    flatTipsLoadEpoch =
                        self->m_flatTipsLoadEpoch.load(
                            std::memory_order_acquire);
                    flatTipsReadySerial =
                        self->m_flatTipsReadySerial.load(
                            std::memory_order_acquire);
                    flatNativeContent =
                        static_cast<Policy::FlatNativeLoadingContent>(
                            self->m_flatNativeLoadingContent.load(
                                std::memory_order_acquire));
                    flatTipsDecision = Policy::DecideFlatTipsFreeze(
                        self->m_flatTipReadinessHookInstalled.load(
                            std::memory_order_acquire),
                        flatNativeContent,
                        self->m_flatTipDrawHooksInstalled.load(
                            std::memory_order_acquire),
                        self->m_flatTipsDrawSerial.load(
                            std::memory_order_acquire) ==
                            flatTipsReadySerial,
                        self->m_loadingTextOwner.load(
                            std::memory_order_acquire) != nullptr,
                        self->m_flatTipsPresentedSerial.load(
                            std::memory_order_acquire) ==
                            flatTipsReadySerial,
                        presentCount, killFrames);
                    const bool freezeMode3 =
                        flatTipsDecision ==
                            Policy::FlatTipsFreezeDecision::kFreezeTips ||
                        flatTipsDecision ==
                            Policy::FlatTipsFreezeDecision::
                                kFreezeBackgroundOnly;
                    const bool killNow = flatMode == 2 ||
                        (flatMode == 3 && freezeMode3);
                    if (killNow) {
                        std::lock_guard<std::mutex> lock(self->m_advanceMovieMutex);
                        // Menu CLOSE/OPEN and a later SendLoadingText can race
                        // the decision above. Revalidate the complete ownership
                        // epoch while holding the same lock as the code write;
                        // a stale decision merely retries on the next Present.
                        const int currentFlatMode =
                            self->m_flatMode.load(std::memory_order_acquire);
                        const bool sameLoad =
                            self->m_flatTipsLoadEpoch.load(
                                std::memory_order_acquire) == flatTipsLoadEpoch;
                        bool freezeStillAuthorized =
                            sameLoad && currentFlatMode == flatMode &&
                            currentFlatMode == 2;
                        void* committedOwner = nullptr;
                        auto committedFlatDecision =
                            Policy::FlatTipsFreezeDecision::kWait;
                        auto committedNativeContent =
                            Policy::FlatNativeLoadingContent::kUnknown;
                        if (sameLoad && currentFlatMode == flatMode &&
                            currentFlatMode == 3) {
                            committedOwner =
                                self->m_loadingTextOwner.load(
                                    std::memory_order_acquire);
                            committedNativeContent =
                                static_cast<
                                    Policy::FlatNativeLoadingContent>(
                                    self->m_flatNativeLoadingContent.load(
                                        std::memory_order_acquire));
                            committedFlatDecision =
                                Policy::DecideFlatTipsFreeze(
                                    self->m_flatTipReadinessHookInstalled.load(
                                        std::memory_order_acquire),
                                    committedNativeContent,
                                    self->m_flatTipDrawHooksInstalled.load(
                                        std::memory_order_acquire),
                                    self->m_flatTipsDrawSerial.load(
                                        std::memory_order_acquire) ==
                                        flatTipsReadySerial,
                                    committedOwner != nullptr,
                                    self->m_flatTipsPresentedSerial.load(
                                        std::memory_order_acquire) ==
                                        flatTipsReadySerial,
                                    self->m_flatPresentCount.load(
                                        std::memory_order_acquire),
                                    self->m_advanceMovieKillFrames.load(
                                        std::memory_order_acquire));
                            freezeStillAuthorized =
                                self->m_flatTipsReadySerial.load(
                                    std::memory_order_acquire) ==
                                        flatTipsReadySerial &&
                                (committedFlatDecision ==
                                        Policy::FlatTipsFreezeDecision::
                                            kFreezeTips ||
                                 committedFlatDecision ==
                                        Policy::FlatTipsFreezeDecision::
                                            kFreezeBackgroundOnly);
                        }
                        if (!self->m_advanceMovieKilled.load(std::memory_order_relaxed) &&
                            self->m_enabled.load(std::memory_order_acquire) &&
                            self->m_inLoadingScreen.load(
                                std::memory_order_acquire) &&
                            freezeStillAuthorized && self->m_advanceMovieAddr) {
                            static constexpr std::uint8_t RET = 0xC3;
                            std::uint8_t current = 0;
                            if (TryReadExecutableByte_SEH(
                                    self->m_advanceMovieAddr, &current) &&
                                current == self->m_advanceMovieOrigByte &&
                                ExecutableBytesEqual(
                                    self->m_advanceMovieAddr,
                                    self->m_advanceMovieOrigBytes.data(),
                                    self->m_advanceMovieOrigBytes.size())) {
                                REL::safe_write(self->m_advanceMovieAddr, &RET, 1);
                                self->m_advanceMovieKilled.store(
                                    true, std::memory_order_release);
                                self->m_advanceMovieRestoreReadFailureLogged.store(
                                    false, std::memory_order_release);
                                if (flatMode == 2 ||
                                    committedFlatDecision ==
                                        Policy::FlatTipsFreezeDecision::
                                            kFreezeBackgroundOnly) {
                                    self->m_flatBackgroundOnly.store(
                                        true, std::memory_order_release);
                                }
                                if (flatMode == 3) {
                                    const bool backgroundOnly =
                                        committedFlatDecision ==
                                            Policy::FlatTipsFreezeDecision::
                                                kFreezeBackgroundOnly;
                                    logger::info(
                                        "AdvanceMovie RET at frame {} mode=3 "
                                        "(orig 0x{:02x}, serial={}, "
                                        "owner={:#x}, native={}) — {}",
                                        presentCount,
                                        self->m_advanceMovieOrigByte,
                                        flatTipsReadySerial,
                                        reinterpret_cast<std::uintptr_t>(
                                            committedOwner),
                                        Policy::NativeLoadingContentName(
                                            committedNativeContent),
                                        backgroundOnly ?
                                            "freeze native-minimal black" :
                                            "retain drawn native UI pixels");
                                } else {
                                    logger::info(
                                        "AdvanceMovie RET at frame {} mode={} "
                                        "(orig 0x{:02x})",
                                        presentCount, flatMode,
                                        self->m_advanceMovieOrigByte);
                                }
                            } else {
                                logger::warn(
                                    "AdvanceMovie RET skipped: entry ownership "
                                    "changed (current=0x{:02x}, expected=0x{:02x})",
                                    current, self->m_advanceMovieOrigByte);
                            }
                        }
                    }
                }

                // Composite every present. (Reverted the every-Nth experiment: it gave
                // zero speed benefit — mode 3 tied mode 2 — and broke the mode-3 visual,
                // because mode 3 keeps the SWF rendering to capture tips, so any skipped
                // present shows the live vanilla loading screen through our background.)
                // Composite: mode 0 = black, mode 1 = native, mode 2 = custom
                // art. Mode 3 uses exact native content: minimal is solid black,
                // while tip-bearing screens use custom art plus captured UI.
                const auto presentationContent =
                    static_cast<Policy::FlatNativeLoadingContent>(
                        self->m_flatNativeLoadingContent.load(
                            std::memory_order_acquire));
                const bool solidBlack = Policy::ShouldPresentSolidBlack(
                    flatMode,
                    self->m_flatTipReadinessHookInstalled.load(
                        std::memory_order_acquire),
                    presentationContent);
                if (solidBlack ||
                    (flatMode >= 2 && self->HasBackgroundTexture())) {
                    // Use real swapchain from RendererData if available (HFPF wraps the hook parameter)
                    auto* sc = self->m_swapChain
                        ? static_cast<IDXGISwapChain*>(self->m_swapChain)
                        : static_cast<IDXGISwapChain*>(swapChain);
                    ID3D11Texture2D* backbuffer = nullptr;
                    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
                    if (SUCCEEDED(hr) && backbuffer) {
                        const auto compositesBefore =
                            self->m_submitCompositeCount.load(
                                std::memory_order_relaxed);
                        self->CompositeFlatFrame(backbuffer);
                        const bool compositeSucceeded =
                            self->m_submitCompositeCount.load(
                                std::memory_order_relaxed) != compositesBefore;
                        backbuffer->Release();
                        if (flatMode == 3 && compositeSucceeded &&
                            flatTipsDecision == Policy::FlatTipsFreezeDecision::
                                kPresentReadyFrame) {
                            flatTipsReadyFrameCandidate = true;
                            flatTipsReadyFrame = presentCount;
                        }
                    }
                }
                self->m_flatPresentCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const bool useLoadingPresentPolicy =
            self && self->m_enabled.load(std::memory_order_acquire) &&
            !self->m_isVR.load(std::memory_order_acquire) &&
            self->m_disableVSyncWhileLoading.load(std::memory_order_acquire);
        const UINT incomingSyncInterval = syncInterval;

        // HFPF uses two independent controls while LoadingMenu is open:
        // SyncInterval=0 and a 350-FPS pre-Present limiter (2.857 ms). Mirror
        // that bounded policy instead of the old 60-FPS loop, which conflated
        // HFPF's separate FixCPUThreads maximum-yield window with its render cap.
        // This limiter is intentionally independent of the PresentThread wait
        // patch. HFPF ownership of that CPU patch does not imply that its loading
        // limiter is enabled; main.cpp disables this whole policy only after
        // confirming HFPF owns both VSync-off and a positive loading FPS cap.
        if (useLoadingPresentPolicy) {
            static constexpr long long kLoadingPresentIntervalUs = 2857;  // 350 FPS
            long long deadlineUs =
                self->m_flatLimiterDeadlineUs.load(std::memory_order_relaxed);
            long long nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // First frame and slow-frame recovery never wait. Subsequent fast
            // frames are paced relative to the previous target, as in HFPF.
            if (deadlineUs == 0 ||
                nowUs > deadlineUs + kLoadingPresentIntervalUs) {
                deadlineUs = nowUs;
            }

            const long long waitStartUs = nowUs;
            while (nowUs < deadlineUs) {
                const long long remainingUs = deadlineUs - nowUs;
                if (remainingUs > 2000) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(remainingUs - 1500));
                } else {
                    _mm_pause();
                }
                nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            if (nowUs > waitStartUs) {
                self->m_flatLimiterWaitCount.fetch_add(
                    1, std::memory_order_relaxed);
                self->m_flatLimiterWaitUs.fetch_add(
                    static_cast<std::uint64_t>(nowUs - waitStartUs),
                    std::memory_order_relaxed);
            }
            self->m_flatLimiterDeadlineUs.store(
                deadlineUs + kLoadingPresentIntervalUs,
                std::memory_order_relaxed);
        }

        // Only override VSync during LoadingMenu. PerformancePatches separately
        // restores RendererData::presentInterval on close.
        if (useLoadingPresentPolicy) {
            syncInterval = 0;
        }
        if (useLoadingPresentPolicy &&
            !self->m_flatSyncSampled.exchange(true, std::memory_order_relaxed)) {
            self->m_flatIncomingSyncInterval.store(
                incomingSyncInterval, std::memory_order_relaxed);
            self->m_flatOutgoingSyncInterval.store(
                syncInterval, std::memory_order_relaxed);
        }
        if (!s_originalPresentFlat) {
            logger::critical("D3D11Compositor: Present hook has no original trampoline");
            return E_FAIL;
        }
        const HRESULT result =
            s_originalPresentFlat(swapChain, syncInterval, flags);
        // Only S_OK proves this exact composite reached the visible swapchain.
        // DXGI_STATUS_OCCLUDED is also SUCCEEDED(), but certifying it would let
        // an entirely covered window authorize an unseen tip frame.
        if (self && flatTipsReadyFrameCandidate && result == S_OK) {
            const auto committedContent =
                static_cast<Policy::FlatNativeLoadingContent>(
                    self->m_flatNativeLoadingContent.load(
                        std::memory_order_acquire));
            const bool drawStillReady =
                !Policy::FlatNativeContentNeedsVisibleProof(
                    committedContent) ||
                self->m_flatTipsDrawSerial.load(
                    std::memory_order_acquire) == flatTipsReadySerial;
            const bool stillOwned =
                self->m_enabled.load(std::memory_order_acquire) &&
                self->m_inLoadingScreen.load(std::memory_order_acquire) &&
                self->m_flatMode.load(std::memory_order_acquire) == 3 &&
                !self->m_advanceMovieKilled.load(
                    std::memory_order_acquire) &&
                committedContent !=
                    Policy::FlatNativeLoadingContent::kUnknown &&
                drawStillReady &&
                self->m_loadingTextOwner.load(
                    std::memory_order_acquire) != nullptr &&
                self->m_flatTipsReadySerial.load(
                    std::memory_order_acquire) == flatTipsReadySerial &&
                self->m_flatTipsLoadEpoch.load(
                    std::memory_order_acquire) == flatTipsLoadEpoch;
            auto observed = self->m_flatTipsPresentedSerial.load(
                std::memory_order_acquire);
            bool published = false;
            while (stillOwned && observed < flatTipsReadySerial) {
                if (self->m_flatTipsPresentedSerial.compare_exchange_weak(
                        observed, flatTipsReadySerial,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    published = true;
                    break;
                }
            }
            if (published) {
                logger::info(
                    "Flat tips: post-publication frame {} presented "
                    "successfully (serial={}, owner={:#x}, native={}); "
                    "AdvanceMovie freeze armed",
                    flatTipsReadyFrame, flatTipsReadySerial,
                    reinterpret_cast<std::uintptr_t>(
                        self->m_loadingTextOwner.load(
                            std::memory_order_acquire)),
                    Policy::NativeLoadingContentName(committedContent));
            }
        }

        // The exact-delta occlusion query is asynchronous. Poll it from the
        // render thread before evaluating freeze policy so a final successful
        // DisplayMovie does not require another movie draw to publish proof.
        if (self &&
            self->m_enabled.load(std::memory_order_acquire) &&
            !self->m_isVR.load(std::memory_order_acquire) &&
            self->m_flatMode.load(std::memory_order_acquire) == 3 &&
            self->m_tipsDeltaQueryPending.load(
                std::memory_order_acquire)) {
            self->IsTipsTextureReady();
        }
        if (self && (result == DXGI_ERROR_DEVICE_REMOVED ||
                     result == DXGI_ERROR_DEVICE_RESET ||
                     result == DXGI_ERROR_DRIVER_INTERNAL_ERROR)) {
            self->HandleDeviceLoss("DXGI Present reported device loss");
        }
        return result;
    }

    void D3D11Compositor::CompositeFlatFrame(void* backbufferTex)
    {
        if (!m_context || !m_device || !backbufferTex) return;
        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* bbTex = static_cast<ID3D11Texture2D*>(backbufferTex);

        D3D11_TEXTURE2D_DESC bbDesc{};
        bbTex->GetDesc(&bbDesc);
        ScopedPipelineState savedState(ctx);

        ID3D11RenderTargetView* temporaryRTV = nullptr;
        ID3D11RenderTargetView* rtv = savedState.FindRenderTargetView(bbTex);
        if (!rtv) {
            savedState.CaptureHazardBindings(bbTex);
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = bbDesc.Format;
            rtvDesc.ViewDimension = bbDesc.SampleDesc.Count > 1
                ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                : D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = device->CreateRenderTargetView(bbTex, &rtvDesc, &temporaryRTV);
            if (FAILED(hr) || !temporaryRTV) {
                logger::warn("D3D11Compositor: backbuffer RTV creation failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return;
            }
            rtv = temporaryRTV;
        }

        const int flatMode = m_flatMode.load(std::memory_order_acquire);
        const auto nativeContent =
            static_cast<Policy::FlatNativeLoadingContent>(
                m_flatNativeLoadingContent.load(std::memory_order_acquire));
        if (Policy::ShouldPresentSolidBlack(
                flatMode,
                m_flatTipReadinessHookInstalled.load(
                    std::memory_order_acquire),
                nativeContent)) {
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            ctx->ClearRenderTargetView(rtv, black);
            ReleaseCom(temporaryRTV);
            m_submitCompositeCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        ID3D11ShaderResourceView* flatTipsReplaySRV = nullptr;
        LoadingMenuCaptureRegion flatTipsReplayRegion{};
        unsigned int flatTipsReplaySourceWidth = 0;
        unsigned int flatTipsReplaySourceHeight = 0;
        const auto flatReadySerial = m_flatTipsReadySerial.load(
            std::memory_order_acquire);
        const bool flatTipProofPublished = flatMode == 3 &&
            flatReadySerial != 0 &&
            m_flatTipsDrawSerial.load(std::memory_order_acquire) ==
                flatReadySerial;
        if (flatTipProofPublished) {
            std::lock_guard resourceLock(m_renderResourceMutex);
            if (m_flatTipsDrawSerial.load(std::memory_order_acquire) ==
                    flatReadySerial &&
                m_flatTipsProofSRV &&
                m_loadingMenuCaptureWidth != 0 &&
                m_loadingMenuCaptureHeight != 0 &&
                m_loadingMenuCaptureRegion.right >
                    m_loadingMenuCaptureRegion.left &&
                m_loadingMenuCaptureRegion.bottom >
                    m_loadingMenuCaptureRegion.top) {
                flatTipsReplaySRV =
                    static_cast<ID3D11ShaderResourceView*>(
                        m_flatTipsProofSRV);
                flatTipsReplaySRV->AddRef();
                flatTipsReplayRegion = m_loadingMenuCaptureRegion;
                flatTipsReplaySourceWidth = m_loadingMenuCaptureWidth;
                flatTipsReplaySourceHeight = m_loadingMenuCaptureHeight;
            }
        }
        ComReleaseGuard flatTipsReplayGuard{ flatTipsReplaySRV };
        // A published proof and its replay texture are one transaction. Never
        // fall back to sampling an arbitrary flip-chain buffer after proof.
        if (flatTipProofPublished && !flatTipsReplaySRV) {
            ReleaseCom(temporaryRTV);
            return;
        }

        unsigned int bgWidth = 0;
        unsigned int bgHeight = 0;
        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(
            AcquireBackgroundSRV(bgWidth, bgHeight));
        if (!bgSRV) {
            ReleaseCom(temporaryRTV);
            return;
        }

        // Mode 3 never samples the arbitrary live flip-chain buffer. It shows
        // the plain custom background until an exact LoadingMenu delta is
        // proven, then replays that persistent texture below. This closes the
        // initial MainMenu/title-pixel leak as well as post-RET buffer rotation.
        const bool backgroundOnly =
            m_flatBackgroundOnly.load(std::memory_order_acquire) ||
            flatMode == 2 || flatMode == 3;

        if (!backgroundOnly) {
            // Copy backbuffer to temp texture (game's loading screen content)
            if (!EnsureTempTexture(bbDesc.Width, bbDesc.Height, bbDesc.Format)) {
                bgSRV->Release();
                ReleaseCom(temporaryRTV);
                return;
            }
            ctx->CopyResource(static_cast<ID3D11Texture2D*>(m_tempTexture), bbTex);
        }

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(bbDesc.Width);
        vp.Height = static_cast<float>(bbDesc.Height);
        vp.MaxDepth = 1.0f;

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));

        if (backgroundOnly) {
            // Background-only: render our image fullscreen, no game content needed
            ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);
        } else {
            // Luminance key: blend game content with background
            ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psLuminanceKey), nullptr, 0);
        }

        // Compute aspect-correct UV scales for background texture
        CompositeParams params{};
        params.threshold = m_luminanceThreshold.load(std::memory_order_acquire);
        if (bgWidth > 0 && bgHeight > 0) {
            float texAspect = static_cast<float>(bgWidth) / static_cast<float>(bgHeight);
            float screenAspect = static_cast<float>(bbDesc.Width) / static_cast<float>(bbDesc.Height);
            if (texAspect > screenAspect) {
                params.bgUvScaleX = screenAspect / texAspect;
                params.bgUvScaleY = 1.0f;
            } else {
                params.bgUvScaleX = 1.0f;
                params.bgUvScaleY = texAspect / screenAspect;
            }
        } else {
            params.bgUvScaleX = 1.0f;
            params.bgUvScaleY = 1.0f;
        }

        // Update constant buffer
        auto* cb = static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = cb
            ? ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
            : E_POINTER;
        if (FAILED(mapHr)) {
            bgSRV->Release();
            ReleaseCom(temporaryRTV);
            return;
        }
        std::memcpy(mapped.pData, &params, sizeof(params));
        ctx->Unmap(cb, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        if (backgroundOnly) {
            // Background shader uses t0 = background texture
            ctx->PSSetShaderResources(0, 1, &bgSRV);
        } else {
            // Luminance key uses t0 = game content, t1 = background
            ID3D11ShaderResourceView* srvs[2] = {
                static_cast<ID3D11ShaderResourceView*>(m_tempSRV),
                bgSRV
            };
            ctx->PSSetShaderResources(0, 2, srvs);
        }

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);

        if (flatTipsReplaySRV) {
            // Replay the accepted exact LoadingMenu delta into the same
            // normalized source region on every flip-chain buffer. The proof
            // shader produced straight alpha matching the live luminance key,
            // so no title/world pixels can leak through this pass.
            D3D11_VIEWPORT tipsViewport{};
            tipsViewport.TopLeftX =
                static_cast<float>(flatTipsReplayRegion.left) *
                static_cast<float>(bbDesc.Width) /
                static_cast<float>(flatTipsReplaySourceWidth);
            tipsViewport.TopLeftY =
                static_cast<float>(flatTipsReplayRegion.top) *
                static_cast<float>(bbDesc.Height) /
                static_cast<float>(flatTipsReplaySourceHeight);
            tipsViewport.Width =
                static_cast<float>(
                    flatTipsReplayRegion.right -
                    flatTipsReplayRegion.left) *
                static_cast<float>(bbDesc.Width) /
                static_cast<float>(flatTipsReplaySourceWidth);
            tipsViewport.Height =
                static_cast<float>(
                    flatTipsReplayRegion.bottom -
                    flatTipsReplayRegion.top) *
                static_cast<float>(bbDesc.Height) /
                static_cast<float>(flatTipsReplaySourceHeight);
            tipsViewport.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &tipsViewport);
            ctx->PSSetShader(
                static_cast<ID3D11PixelShader*>(m_psBlit), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &flatTipsReplaySRV);
            ctx->OMSetBlendState(
                static_cast<ID3D11BlendState*>(m_alphaBlendState),
                nullptr, 0xFFFFFFFF);
            ctx->Draw(3, 0);
        }

        bgSRV->Release();
        ReleaseCom(temporaryRTV);
        m_submitCompositeCount.fetch_add(1, std::memory_order_relaxed);
    }

    // ========================================================================
    // VR mode initialization
    // ========================================================================

    bool D3D11Compositor::Initialize(void* vrCompositor, void* d3dDevice)
    {
        if (!vrCompositor || !d3dDevice) return false;
        if (m_initialized.load(std::memory_order_acquire)) {
            if (m_device == d3dDevice &&
                m_renderReady.load(std::memory_order_acquire)) {
                return true;
            }
            HandleDeviceLoss("VR renderer device changed");
            return RebuildDeviceResources(d3dDevice);
        }

        m_renderReady.store(false, std::memory_order_release);
        m_isVR.store(true, std::memory_order_release);
        s_instance = this;
        m_device = d3dDevice;

        // Get immediate context
        auto* device = static_cast<ID3D11Device*>(m_device);
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);
        if (!ctx) {
            logger::error("D3D11Compositor: failed to get immediate context");
            return false;
        }
        m_context = ctx;

        // Compile shaders
        if (!CompileShaders()) {
            logger::error("D3D11Compositor: shader compilation failed");
            ctx->Release();
            m_context = nullptr;
            return false;
        }

        if (!CreatePipelineResources()) {
            logger::error("D3D11Compositor: D3D state creation failed");
            ctx->Release();
            m_context = nullptr;
            return false;
        }

        // Hook IVRCompositor::Submit at vtable[5].
        // Atomic 8-byte pointer write (hardware-atomic on x64) — the write
        // itself can't tear; any in-flight Submit call is unavoidable. Save
        // the slot and original target for a future restore path (no shutdown
        // hook unswaps these yet, so the plugin DLL must remain pinned).
        void** vtable = *reinterpret_cast<void***>(vrCompositor);
        void* originalSubmitPtr = vtable[5];

        DWORD oldProtect = 0;
        if (!VirtualProtect(&vtable[5], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            logger::error("D3D11Compositor: Submit vtable protection change failed ({})",
                GetLastError());
            return false;
        }
        s_originalSubmit = reinterpret_cast<decltype(s_originalSubmit)>(originalSubmitPtr);
        s_submitVtableSlot = &vtable[5];
        s_originalSubmitForRestore = originalSubmitPtr;
        InterlockedExchangePointer(&vtable[5], reinterpret_cast<void*>(&HookedSubmit));
        DWORD ignoredProtect = 0;
        if (!VirtualProtect(&vtable[5], sizeof(void*), oldProtect, &ignoredProtect)) {
            logger::warn("D3D11Compositor: failed to restore Submit vtable protection");
        }

        logger::info("D3D11Compositor: Submit hook installed at vtable[5]");

        // Hook ID3D11DeviceContext::ClearRenderTargetView at vtable[50].
        // Sanity-check that the pre-swap pointer at slot 50 actually lives
        // inside d3d11.dll — different runtimes and deferred contexts can
        // have different vtable layouts, and hooking the wrong slot crashes.
        void** ctxVtable = *reinterpret_cast<void***>(ctx);
        void* originalClearPtr = ctxVtable[50];
        HMODULE clearOwner = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(originalClearPtr), &clearOwner) || !clearOwner) {
            logger::warn("D3D11Compositor: ClearRTV vtable[50] owner unresolvable — skipping hook");
        } else {
            wchar_t modPath[MAX_PATH] = {};
            GetModuleFileNameW(clearOwner, modPath, MAX_PATH);
            const wchar_t* baseName = wcsrchr(modPath, L'\\');
            baseName = baseName ? baseName + 1 : modPath;
            if (_wcsicmp(baseName, L"d3d11.dll") != 0) {
                char nb[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, baseName, -1, nb, sizeof(nb), nullptr, nullptr);
                logger::warn("D3D11Compositor: ClearRTV vtable[50] points into '{}' "
                             "(not d3d11.dll) — vtable layout differs, skipping hook", nb);
            } else {
                oldProtect = 0;
                if (!VirtualProtect(&ctxVtable[50], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    logger::warn("D3D11Compositor: ClearRTV vtable protection change failed; hook skipped");
                } else {
                    s_originalClearRTV = reinterpret_cast<decltype(s_originalClearRTV)>(originalClearPtr);
                    s_clearRTVVtableSlot = &ctxVtable[50];
                    s_originalClearRTVForRestore = originalClearPtr;
                    InterlockedExchangePointer(&ctxVtable[50], reinterpret_cast<void*>(&HookedClearRTV));
                    ignoredProtect = 0;
                    if (!VirtualProtect(&ctxVtable[50], sizeof(void*), oldProtect, &ignoredProtect)) {
                        logger::warn("D3D11Compositor: failed to restore ClearRTV vtable protection");
                    }
                    logger::info("D3D11Compositor: ClearRTV hook installed at vtable[50]");
                }
            }
        }

        m_renderReady.store(true, std::memory_order_release);
        m_deviceLost.store(false, std::memory_order_release);
        m_initialized.store(true, std::memory_order_release);
        LoadingScreenManager::GetSingleton().OnD3DDeviceReady();
        logger::info("D3D11Compositor: initialized (mode={})",
            m_mode.load(std::memory_order_acquire) == CompositeMode::LuminanceKey
                ? "LuminanceKey" : "ClearIntercept");
        return true;
    }

    // ========================================================================
    // Scaleform RT capture (VR). Hooks BSGraphics::Renderer::GetScaleformSurfaceType
    // to record the RT index scaleform is drawing the UI into, so we can pull the
    // raw ID3D11Texture2D* out of BSGraphics::RendererData::renderTargets[].
    // ========================================================================

    static constexpr std::uintptr_t GetScaleformSurfaceType_Offset_VR = 0x01d97a40;
    static constexpr std::uintptr_t ScreenSpaceSetRT_Offset_VR         = 0x020cc660;
    static constexpr std::uintptr_t BSGraphics_RendererData_Offset_VR  = 0x060f3ce8;
    static constexpr std::size_t    RendererData_RenderTargets_Offset  = 0xa58;
    static constexpr std::size_t    RenderTarget_Stride                = 48;
    static constexpr std::size_t    RenderTarget_Texture_Offset        = 0x00;
    static constexpr std::size_t    RenderTarget_RTV_Offset            = 0x10;
    // Screen-space sibling of ImageSpace_RenderMenus. This is the path the
    // engine takes for full-screen 2D UI (LoadingMenu, MainMenu, etc.).
    // Signature: void __fastcall(void* this) — no name/flag args, so we
    // gate on our own loading-screen state rather than the menu name.
    static constexpr std::uintptr_t RenderMenus_Offset_VR              = 0x020cc750;
    // IMenu::DisplayMenu — called per-menu inside ScreenSpace_RenderMenus's
    // walk. Calls BSScaleformRenderer::DisplayMovie which writes the menu's
    // scaleform RT into the bound eye texture. We gate by checking if the
    // IMenu* belongs to LoadingMenu (vtable check) so we only suppress
    // LoadingMenu's compositor pass, not other menus.
    static constexpr std::uintptr_t DisplayMenu_Offset_VR              = 0x02199300;
    // Flat Fallout equivalents. These two hooks form one proof: DisplayMenu
    // supplies exact LoadingMenu ownership, and DisplayMovie brackets the exact
    // before/after pixel delta later accepted by the GPU coverage query.
    static constexpr std::uint64_t DisplayMenu_ID_OG                   = 218939;
    static constexpr std::uintptr_t DisplayMenu_Offset_OG              = 0x0210f450;
    static constexpr std::uint64_t DisplayMenu_ID_NG                   = 2287380;
    static constexpr std::uintptr_t DisplayMenu_Offset_NG_221          = 0x01b19560;
    static constexpr std::uintptr_t DisplayMenu_Offset_NG_240          = 0x01b19a20;
    static constexpr std::array<std::uint8_t, 25>
        DisplayMenu_Entry_OG{
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49,
            0x40, 0x0F, 0xB6, 0xF2, 0x48, 0x85, 0xC9, 0x74,
            0x6C
        };
    static constexpr std::array<std::uint8_t, 25>
        DisplayMenu_Entry_NG{
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x0F, 0xB6, 0xFA,
            0x48, 0x8B, 0x49, 0x40, 0x48, 0x85, 0xC9, 0x74,
            0x6C
        };
    // LoadingMenu vtables — primary (RTTI) and secondary (multiple-inheritance).
    // Static binary base is 0x140000000; these are RVAs.
    // Combined VR Ghidra data references: VA 0x142DCA698/0x142DCA748.
    // The former 0x00DCA... RVAs pointed into executable Actor code, so the
    // LoadingMenu-specific DisplayMenu test could never match.
    static constexpr std::uintptr_t LoadingMenu_Vtable1_Offset         = 0x2dca698;
    static constexpr std::uintptr_t LoadingMenu_Vtable2_Offset         = 0x2dca748;
    // MainMenu vtables (VA 0x142DCBB38/0x142DCBC08). After a save load started
    // from the title screen, the engine keeps MainMenu on the stack and
    // re-renders it for ~0.5 s after LoadingMenu's CLOSE before removing it —
    // the source of the post-load title-screen flash. Only consulted inside
    // the bounded post-close window, and only for closes that entered a game
    // session, so a real quit-to-menu is never suppressed.
    static constexpr std::uintptr_t MainMenu_Vtable1_Offset            = 0x2dcbb38;
    static constexpr std::uintptr_t MainMenu_Vtable2_Offset            = 0x2dcbc08;
    // BSScaleformRenderer::DisplayMovie — universal compositor for scaleform UI.
    // Reached from every scaleform render path; hooking here catches all leaks
    // that bypass UI::ScreenSpace_RenderMenus / IMenu::DisplayMenu.
    static constexpr std::uintptr_t DisplayMovie_Offset_VR             = 0x020d7820;
    static constexpr std::uint64_t DisplayMovie_ID_OG                  = 1096456;
    static constexpr std::uintptr_t DisplayMovie_Offset_OG             = 0x0204d9c0;
    static constexpr std::uint64_t DisplayMovie_ID_NG                  = 2284943;
    static constexpr std::uintptr_t DisplayMovie_Offset_NG_221         = 0x01a89f50;
    static constexpr std::uintptr_t DisplayMovie_Offset_NG_240         = 0x01a8a410;
    static constexpr std::array<std::uint8_t, 24>
        DisplayMovie_Entry_OG{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
        };
    // LoadingMenu::SendLoadingText always reaches its level update, but it
    // calls SetLoadingText only when a TESLoadScreen was selected and the
    // transition is not native minimal/interior mode. The detour snapshots
    // that branch before forwarding; returning alone is not readiness proof.
    static constexpr std::uintptr_t SendLoadingText_Offset_VR          = 0x013157f0;
    // Fallout4.exe 1.10.163 equivalent. Address Library
    // version-1-10-163-0.bin maps ID 277976 to this exact RVA. The full prefix
    // is checked before MinHook is allowed to create a trampoline; a different
    // runtime or a foreign entry detour therefore fails closed.
    static constexpr std::uint64_t SendLoadingText_ID_OG               = 277976;
    static constexpr std::uintptr_t SendLoadingText_Offset_OG          = 0x012985d0;
    static constexpr std::uint64_t SendLoadingText_ID_NG               = 2249231;
    static constexpr std::uintptr_t SendLoadingText_Offset_NG_221      = 0x01067df0;
    static constexpr std::uintptr_t SendLoadingText_Offset_NG_240      = 0x01068180;
    static constexpr std::array<std::uint8_t, 22>
        SendLoadingText_Entry_OG{
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55,
            0x57, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48,
            0x81, 0xEC, 0x00, 0x01, 0x00, 0x00
        };
    static constexpr std::array<std::uint8_t, 22>
        SendLoadingText_Entry_NG{
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55,
            0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48,
            0x81, 0xEC, 0x00, 0x01, 0x00, 0x00
        };

    enum class VerifiedFlatTipsRuntime : std::uint8_t
    {
        kUnsupported,
        kOG163,
        kNG221,
        kNG240
    };

    [[nodiscard]] static VerifiedFlatTipsRuntime GetVerifiedFlatTipsRuntime()
    {
        if (REL::Module::IsVR()) {
            return VerifiedFlatTipsRuntime::kUnsupported;
        }
        const auto version = REL::Module::get().version();
        if (!REL::Module::IsNG() &&
            version[0] == 1 && version[1] == 10 &&
            version[2] == 163 && version[3] == 0) {
            return VerifiedFlatTipsRuntime::kOG163;
        }
        if (REL::Module::IsNG() &&
            version[0] == 1 && version[1] == 11 && version[3] == 0) {
            if (version[2] == 221) {
                return VerifiedFlatTipsRuntime::kNG221;
            }
            if (version[2] == 240) {
                return VerifiedFlatTipsRuntime::kNG240;
            }
        }
        return VerifiedFlatTipsRuntime::kUnsupported;
    }

    [[nodiscard]] static const char* FlatTipsRuntimeName(
        VerifiedFlatTipsRuntime runtime)
    {
        switch (runtime) {
        case VerifiedFlatTipsRuntime::kOG163: return "1.10.163";
        case VerifiedFlatTipsRuntime::kNG221: return "1.11.221";
        case VerifiedFlatTipsRuntime::kNG240: return "1.11.240";
        default: return "unsupported";
        }
    }
    // LoadingMenu::UpdateSounds(this, nowMs, autoSpinning) — the only caller
    // is LoadingMenu::AdvanceMovie. Verified logic:
    //     if (!handleValid)                                  -> do nothing
    //     if (autoSpinning && !bPlaySoundLoopDuringAutoSpin) -> Stop
    //     else if (nowMs - m_228 >= uDelayBeforeSoundLoopEnds) -> Stop
    //     else                                               -> PLAY
    // m_228 is stamped only by LoadingMenu::RotateModel, which returns early
    // when there is no foreground model — and this plugin suppresses that
    // model. So m_228 stays 0 and the ONLY thing preventing the looping
    // rotate sound from playing forever is nowMs being larger than the
    // 100 ms threshold; nowMs comes from BSTimer, which the plugin's own
    // FPS/timer patches modify. Rather than depend on that, suppress the
    // orphaned sounds directly: we removed the model, so its audio must go
    // with it. (Reported as a constant spinning/clicking sound.)
    static constexpr std::uintptr_t UpdateSounds_Offset_VR             = 0x01315570;
    static constexpr std::uintptr_t SoundHandleStop_Offset_VR          = 0x01b4aa90;
    static constexpr std::ptrdiff_t kLoadingMenuRotateSoundHandle      = 0x238;
    static constexpr std::ptrdiff_t kLoadingMenuZoomSoundHandle        = 0x240;

    void* __fastcall D3D11Compositor::HookedGetScaleformSurfaceType(
        void* rendererThis, unsigned int index)
    {
        auto* self = s_instance;
        if (self) {
            self->m_scaleformRtIndex.store(static_cast<int>(index), std::memory_order_relaxed);

            // Combined Ghidra proves this function has one generic caller and
            // carries no menu identity. It is useful for diagnostics, but it
            // must never publish LoadingMenu ownership. That is proven later by
            // matching Fallout's currently bound D3D RTV inside the verified
            // LoadingMenu DisplayMenu hook.
            if (s_insideScreenSpaceSetRT) {
                const int previous = self->m_screenSpaceRtIndex.exchange(
                    static_cast<int>(index), std::memory_order_acq_rel);
                if (previous != static_cast<int>(index)) {
                    logger::info(
                        "Scaleform tips: screen-space RT idx={} captured", index);
                }
            }

            // Log first 12 calls per load window (counters are reset on LoadingMenu OPEN).
            int n = self->m_scaleformHookLogCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 12) {
                // Capture caller RVA — different scaleform movies have different
                // call sites. Helps identify which movie owns which slot.
                std::uintptr_t retAddr = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
                std::uintptr_t rva = retAddr - base;
                logger::info("SFT call #{}: idx={} caller_rva={:x}", n + 1, index, rva);
            }
        }
        return s_originalGetScaleformSurfaceType
            ? s_originalGetScaleformSurfaceType(rendererThis, index)
            : nullptr;
    }

    void __fastcall D3D11Compositor::HookedScreenSpaceSetRT(void* uiSingleton)
    {
        const bool previous = s_insideScreenSpaceSetRT;
        s_insideScreenSpaceSetRT = true;
        if (s_originalScreenSpaceSetRT) {
            s_originalScreenSpaceSetRT(uiSingleton);
        }
        s_insideScreenSpaceSetRT = previous;
    }

    bool D3D11Compositor::InstallScaleformRTHook()
    {
        logger::info("Scaleform RT hook: Install requested (VR={}, alreadyInstalled={})",
            REL::Module::IsVR(), m_scaleformRTHookInstalled);
        if (m_scaleformRTHookInstalled) return true;
        if (!REL::Module::IsVR()) return false;

        // VR init path doesn't call InitializeFlat, so MinHook may be uninitialized here.
        // Idempotent: MH_ERROR_ALREADY_INITIALIZED is fine.
        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("Scaleform RT hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        REL::Relocation<void*> target{ REL::Offset(GetScaleformSurfaceType_Offset_VR) };
        void* targetAddr = reinterpret_cast<void*>(target.address());
        logger::info("Scaleform RT hook: target addr = {:x}", target.address());

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedGetScaleformSurfaceType),
            reinterpret_cast<void**>(&s_originalGetScaleformSurfaceType));
        if (st != MH_OK) {
            logger::error("Scaleform RT hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_originalGetScaleformSurfaceType) {
            logger::error("Scaleform RT hook: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error("Scaleform RT hook: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        m_scaleformRTHookInstalled = true;
        logger::info("Scaleform RT hook installed at GetScaleformSurfaceType ({:x})",
            target.address());
        return true;
    }

    bool D3D11Compositor::InstallScreenSpaceRTHook()
    {
        if (m_screenSpaceRTHookInstalled) return true;
        if (!REL::Module::IsVR()) return false;

        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error(
                "Screen-space RT hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        REL::Relocation<void*> target{ REL::Offset(ScreenSpaceSetRT_Offset_VR) };
        void* targetAddr = reinterpret_cast<void*>(target.address());
        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedScreenSpaceSetRT),
            reinterpret_cast<void**>(&s_originalScreenSpaceSetRT));
        if (st != MH_OK || !s_originalScreenSpaceSetRT) {
            logger::error(
                "Screen-space RT hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            MH_RemoveHook(targetAddr);
            s_originalScreenSpaceSetRT = nullptr;
            logger::error(
                "Screen-space RT hook: MH_EnableHook failed: {}", int(st));
            return false;
        }

        m_screenSpaceRTHookInstalled = true;
        logger::info(
            "Screen-space RT hook installed at UI::ScreenSpace_SetRenderTarget ({:x})",
            target.address());
        return true;
    }

    void __fastcall D3D11Compositor::HookedRenderMenus(void* uiSingleton)
    {
        // This pass can contain MessageBox, console, or mod UI in addition to
        // LoadingMenu. It has no owner identity, so it must remain transparent.
        if (s_originalRenderMenus) s_originalRenderMenus(uiSingleton);
    }

    bool D3D11Compositor::InstallRenderMenusHook()
    {
        if (m_renderMenusHookInstalled) return true;
        if (!REL::Module::IsVR()) {
            // VR-only offset for now. Flat-game offsets differ per runtime.
            return false;
        }

        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("RenderMenus hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        REL::Relocation<void*> target{ REL::Offset(RenderMenus_Offset_VR) };
        void* targetAddr = reinterpret_cast<void*>(target.address());

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedRenderMenus),
            reinterpret_cast<void**>(&s_originalRenderMenus));
        if (st != MH_OK) {
            logger::error("RenderMenus hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_originalRenderMenus) {
            logger::error("RenderMenus hook: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error("RenderMenus hook: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        m_renderMenusHookInstalled = true;
        logger::info("RenderMenus hook installed at UI::ScreenSpace_RenderMenus ({:x})",
            target.address());
        return true;
    }

    void __fastcall D3D11Compositor::HookedDisplayMenu(void* iMenu, bool param0)
    {
        auto* self = s_instance;
        // Flat draw proof. SendLoadingText supplied the exact LoadingMenu
        // owner and native content choice. Scope the universal DisplayMovie
        // hook to that owner only, and carry its publication/load identity
        // through TLS so a concurrent refresh or CLOSE cannot certify stale
        // pixels. VR keeps its existing vtable/capture path below.
        if (self && !self->m_isVR.load(std::memory_order_acquire)) {
            const auto nativeContent =
                static_cast<Policy::FlatNativeLoadingContent>(
                    self->m_flatNativeLoadingContent.load(
                        std::memory_order_acquire));
            // HookedSendLoadingText publishes content last. Observing a real
            // content kind with acquire semantics therefore also observes the
            // owner and serial belonging to that exact publication.
            const auto serial = self->m_flatTipsReadySerial.load(
                std::memory_order_acquire);
            const auto loadEpoch = self->m_flatTipsLoadEpoch.load(
                std::memory_order_acquire);
            const bool ownedNativeUIDraw = iMenu && param0 && serial != 0 &&
                self->m_enabled.load(std::memory_order_acquire) &&
                self->m_inLoadingScreen.load(std::memory_order_acquire) &&
                self->m_flatMode.load(std::memory_order_acquire) == 3 &&
                Policy::FlatNativeContentNeedsVisibleProof(nativeContent) &&
                self->m_loadingTextOwner.load(
                    std::memory_order_acquire) == iMenu;

            const bool previousInside = s_insideLoadingMenuDisplay;
            void* const previousOwner = s_loadingMenuDisplayOwner;
            const auto previousSerial = s_loadingMenuDisplaySerial;
            const auto previousEpoch = s_loadingMenuDisplayLoadEpoch;
            if (ownedNativeUIDraw) {
                s_insideLoadingMenuDisplay = true;
                s_loadingMenuDisplayOwner = iMenu;
                s_loadingMenuDisplaySerial = serial;
                s_loadingMenuDisplayLoadEpoch = loadEpoch;
            }
            if (s_originalDisplayMenu) {
                s_originalDisplayMenu(iMenu, param0);
            }
            s_insideLoadingMenuDisplay = previousInside;
            s_loadingMenuDisplayOwner = previousOwner;
            s_loadingMenuDisplaySerial = previousSerial;
            s_loadingMenuDisplayLoadEpoch = previousEpoch;
            return;
        }

        bool isLoadingMenu = false;
        int mode = 1;
        const bool inLoadingScreen =
            self && self->m_inLoadingScreen.load(std::memory_order_acquire);
        // Post-close suppression window: the native CLOSE event fires while
        // LoadingMenu is still on the menu stack for its fade-out, so without
        // this the menu's tail frames (black + tip/level + spinner) present
        // right after the custom overlays are released. Armed only for loads
        // whose presentation was custom-owned; expires on its own and is
        // cleared by the next OPEN.
        bool postCloseWindow = false;
        bool observeWindow = false;
        if (self && !inLoadingScreen) {
            const auto nowTicks =
                std::chrono::steady_clock::now().time_since_epoch().count();
            const auto suppressUntil =
                self->m_lmPostCloseSuppressUntilTicks.load(
                    std::memory_order_acquire);
            postCloseWindow = suppressUntil != 0 && nowTicks < suppressUntil;
            // Observation runs LONGER than suppression, and only logs. The
            // census was previously bounded by the suppression deadline, so it
            // could never answer the one question that matters: does the native
            // LoadingMenu keep drawing after we stop suppressing it and after
            // the cover is gone? The reported "grid" is Fallout's own loading
            // backdrop, so a native draw landing in that gap is exactly what
            // the artifact would look like.
            const auto observeUntil =
                self->m_lmPostCloseObserveUntilTicks.load(
                    std::memory_order_acquire);
            observeWindow = observeUntil != 0 && nowTicks < observeUntil;
        }
        // postCloseWindow is listed explicitly even though observeWindow is
        // normally a superset. It is NOT a superset transiently: the two
        // deadlines are separate atomics, so between arming suppress and
        // arming observe there is a state where suppress is live and observe is
        // stale/expired. Gating on observeWindow alone made SUPPRESSION depend
        // on a diagnostic deadline, and a native tail frame in that window is
        // exactly the grid flash this release exists to close.
        if (self && iMenu &&
            (inLoadingScreen || postCloseWindow || observeWindow)) {
            void* vtable = *reinterpret_cast<void**>(iMenu);
            REL::Relocation<void*> lmV1{ REL::Offset(LoadingMenu_Vtable1_Offset) };
            REL::Relocation<void*> lmV2{ REL::Offset(LoadingMenu_Vtable2_Offset) };
            std::uintptr_t vtAddr = reinterpret_cast<std::uintptr_t>(vtable);
            isLoadingMenu =
                vtAddr == lmV1.address() || vtAddr == lmV2.address();
            // MainMenu is still on the stack after a title-screen save load and
            // the engine re-renders it for a few tenths of a second after
            // LoadingMenu's CLOSE — even with scrubbed eye buffers, that is
            // freshly drawn title content (log-proven). Suppress it in the same
            // bounded window, but only when the close entered a game session.
            // Identity and suppression are deliberately separate. The census
            // must be able to NAME MainMenu in the observe-only window - it is
            // the one menu known to redraw title content after CLOSE, i.e. the
            // whole reason the window exists - while suppression stays gated on
            // exactly the previous condition so behaviour is unchanged. Folding
            // the two together meant a post-suppression MainMenu draw logged as
            // "other" with a vtable printed nowhere else, so the field test
            // could only ever conclude "no MainMenu draws".
            bool isMainMenuVtable = false;
            if (!isLoadingMenu) {
                REL::Relocation<void*> mmV1{
                    REL::Offset(MainMenu_Vtable1_Offset)
                };
                REL::Relocation<void*> mmV2{
                    REL::Offset(MainMenu_Vtable2_Offset)
                };
                isMainMenuVtable =
                    vtAddr == mmV1.address() || vtAddr == mmV2.address();
            }
            const bool isPostCloseMainMenu = isMainMenuVtable &&
                postCloseWindow &&
                self->m_lmPostCloseSuppressMainMenu.load(
                    std::memory_order_acquire);
            // Post-close menu census. The skip log below shares one 4-entry
            // budget with the in-load suppressions, which the load itself
            // normally exhausts, so everything the engine drew between the
            // last logged skip and the cover release was invisible — exactly
            // the window the flash lives in. This budget is separate, covers
            // the whole window, and records menus we do NOT recognise (a menu
            // drawing title art through a vtable we do not match would
            // otherwise look like nothing happening at all).
            // Decide suppression FIRST, so the census below reports what this
            // hook actually did rather than merely which window it was in.
            //
            // Mode 0 (Black):           suppress always — no native render at all.
            // Mode 1 (Native, no 3D):   DON'T suppress — user wants native render.
            // Mode 2 (Background):      suppress always — bg overlay only.
            // Mode 3 (Background+Tips): suppress only after an exact-owner
            //                           Scaleform capture has completed.
            // Post-close window:        suppress every custom mode's tail.
            // Observe-only window:      suppress NOTHING. It is log-only.
            //
            // That last case is the 2.1.20 defect this restores. The observation
            // window was documented and logged as log-only, but its draws fell
            // through to the IN-LOAD switch, where modes 0/2 skip
            // unconditionally and mode 3 consults an m_tipsCaptureComplete left
            // over from the finished load. Suppression therefore ran silently to
            // 4.5 s instead of the stated 2 s, mode-3 tails became
            // nondeterministic on stale cross-load state, and the census printed
            // suppressed=false for draws it then skipped — corrupting exactly the
            // data the window exists to collect.
            bool skip = false;
            if (isLoadingMenu || isPostCloseMainMenu) {
                mode = self->m_flatMode.load(std::memory_order_acquire);
                if (postCloseWindow) {
                    skip = mode != 1;
                } else if (inLoadingScreen) {
                    switch (mode) {
                        case 0: skip = true; break;
                        case 1: skip = false; break;
                        case 2: skip = true; break;
                        case 3:
                            skip = self->m_tipsCaptureComplete.load(
                                std::memory_order_acquire);
                            break;
                        default: skip = false; break;
                    }
                }
            }
            if (skip) {
                // March's artwork and scene fade covered these native pixels;
                // it never prevented Fallout from drawing them. Suppression
                // left the title buffers intact underneath our cover, so keep
                // the diagnostic decision but always forward the real draw.
                const int n = self->m_displayMenuSkipCount.fetch_add(
                    1, std::memory_order_relaxed);
                if (n < 4) {
                    logger::info(
                        "DisplayMenu: native {} render retained by March "
                        "handoff (mode={}{} #{})",
                        isPostCloseMainMenu ? "MainMenu" : "LoadingMenu",
                        mode, postCloseWindow ? " post-close" : "", n + 1);
                }
                skip = false;
            }
            if (observeWindow) {
                // VISIBLE draws only. The first census spent its whole budget
                // inside 20 ms on eight invisible HUD menus (WSCompass,
                // WSLootMenu, WSHMDHUDInfo...) and went silent at +168 ms,
                // while the artifact window is +270 to +430 ms — so it was
                // blind to exactly the interval it was built to observe. An
                // invisible menu cannot draw the title screen, so filtering
                // on param0 spends the budget where the answer is. Elapsed is
                // logged too: a census entry with no timestamp cannot be
                // placed against the release boundaries.
                if (param0) {
                    const int seen = self->m_diagPostCloseMenuCalls.fetch_add(
                        1, std::memory_order_relaxed);
                    if (seen < 24) {
                        const auto sinceClose = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() -
                            std::chrono::steady_clock::time_point(
                                std::chrono::steady_clock::duration(
                                    self->m_lmPostCloseArmedTicks.load(
                                        std::memory_order_acquire))))
                            .count();
                        const char* identity = isLoadingMenu ? "LoadingMenu"
                            : isMainMenuVtable ? "MainMenu" : "other";
                        logger::info(
                            "Post-close VISIBLE menu #{} (+{} ms): {} "
                            "vtable={:#x} window={} skipped={}",
                            seen + 1, sinceClose, identity, vtAddr,
                            postCloseWindow ? "suppress" : "observe", skip);
                    }
                }
            }
            if (isLoadingMenu || isPostCloseMainMenu) {
                if (inLoadingScreen) {
                    self->m_diagLoadingMenuDisplayCalls.fetch_add(
                        1, std::memory_order_relaxed);
                    if (param0) {
                        if (self->m_diagLoadingMenuDisplayVisible.fetch_add(
                                1, std::memory_order_relaxed) == 0) {
                            const auto sinceTicks =
                                self->m_inLoadingScreenSinceTicks.load(
                                    std::memory_order_acquire);
                            const auto elapsed =
                                std::chrono::steady_clock::now() -
                                std::chrono::steady_clock::time_point(
                                    std::chrono::steady_clock::duration(
                                        sinceTicks));
                            self->m_diagFirstVisibleDisplayMs.store(
                                static_cast<int>(
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(elapsed)
                                        .count()),
                                std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
        const bool captureLoadingMenuRT =
            self && isLoadingMenu && param0 && mode == 3 &&
            self->m_tipsExtractEnabled.load(std::memory_order_acquire) &&
            self->m_inLoadingScreen.load(std::memory_order_acquire);
        const bool previousOwner = s_insideLoadingMenuDisplay;
        void* const previousMenuOwner = s_loadingMenuDisplayOwner;
        if (captureLoadingMenuRT)
            s_insideLoadingMenuDisplay = true;
        if (captureLoadingMenuRT)
            s_loadingMenuDisplayOwner = iMenu;
        if (s_originalDisplayMenu) {
            s_originalDisplayMenu(iMenu, param0);
        }
        s_insideLoadingMenuDisplay = previousOwner;
        s_loadingMenuDisplayOwner = previousMenuOwner;
    }

    bool D3D11Compositor::InstallDisplayMenuHook()
    {
        if (m_displayMenuHookInstalled) return true;
        const bool vrRuntime = REL::Module::IsVR();
        const auto flatRuntime = GetVerifiedFlatTipsRuntime();
        if (!vrRuntime &&
            flatRuntime == VerifiedFlatTipsRuntime::kUnsupported) {
            const auto version = REL::Module::get().version();
            logger::warn(
                "Flat tips: IMenu::DisplayMenu unavailable on unverified "
                "runtime {}.{}.{}.{}",
                version[0], version[1], version[2], version[3]);
            return false;
        }

        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("DisplayMenu hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        std::uintptr_t targetAddress = 0;
        if (vrRuntime) {
            REL::Relocation<void*> target{
                REL::Offset(DisplayMenu_Offset_VR)
            };
            targetAddress = target.address();
        } else {
            const bool ngRuntime =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ||
                flatRuntime == VerifiedFlatTipsRuntime::kNG240;
            const std::uint64_t targetID = ngRuntime ?
                DisplayMenu_ID_NG : DisplayMenu_ID_OG;
            const std::uintptr_t expectedRVA =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ?
                    DisplayMenu_Offset_NG_221 :
                flatRuntime == VerifiedFlatTipsRuntime::kNG240 ?
                    DisplayMenu_Offset_NG_240 : DisplayMenu_Offset_OG;
            const auto& expectedEntry = ngRuntime ?
                DisplayMenu_Entry_NG : DisplayMenu_Entry_OG;
            if (ngRuntime) {
                REL::Relocation<void*> target{ REL::ID(DisplayMenu_ID_NG) };
                targetAddress = target.address();
            } else {
                REL::Relocation<void*> target{
                    REL::RelocationID(DisplayMenu_ID_OG, 0, 0)
                };
                targetAddress = target.address();
            }
            const auto moduleBase = REL::Module::get().base();
            if (!targetAddress || targetAddress < moduleBase ||
                targetAddress - moduleBase != expectedRVA ||
                !ExecutableBytesEqual(
                    targetAddress, expectedEntry.data(),
                    expectedEntry.size())) {
                logger::error(
                    "Flat tips: rejected IMenu::DisplayMenu target "
                    "for {} (ID {}, RVA={:#x}, expected={:#x}, "
                    "signature={})",
                    FlatTipsRuntimeName(flatRuntime), targetID,
                    targetAddress >= moduleBase ?
                        targetAddress - moduleBase : 0,
                    expectedRVA,
                    targetAddress && ExecutableBytesEqual(
                        targetAddress, expectedEntry.data(),
                        expectedEntry.size()));
                return false;
            }
        }
        void* targetAddr = reinterpret_cast<void*>(targetAddress);

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedDisplayMenu),
            reinterpret_cast<void**>(&s_originalDisplayMenu));
        if (st != MH_OK) {
            logger::error("DisplayMenu hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_originalDisplayMenu) {
            logger::error("DisplayMenu hook: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error("DisplayMenu hook: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        m_displayMenuHookInstalled = true;
        if (vrRuntime) {
            REL::Relocation<void*> lmV1{
                REL::Offset(LoadingMenu_Vtable1_Offset)
            };
            REL::Relocation<void*> lmV2{
                REL::Offset(LoadingMenu_Vtable2_Offset)
            };
            REL::Relocation<void*> mmV1{
                REL::Offset(MainMenu_Vtable1_Offset)
            };
            REL::Relocation<void*> mmV2{
                REL::Offset(MainMenu_Vtable2_Offset)
            };
            // Both vtable pairs are logged so a census line's raw vtable=
            // value can actually be resolved by whoever reads the log.
            logger::info(
                "DisplayMenu hook installed at IMenu::DisplayMenu ({:x}); "
                "LoadingMenu vtables: {:x}, {:x}; "
                "MainMenu vtables: {:x}, {:x}",
                targetAddress, lmV1.address(), lmV2.address(),
                mmV1.address(), mmV2.address());
        } else {
            const bool ngRuntime =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ||
                flatRuntime == VerifiedFlatTipsRuntime::kNG240;
            const auto targetID = ngRuntime ?
                DisplayMenu_ID_NG : DisplayMenu_ID_OG;
            const auto expectedRVA =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ?
                    DisplayMenu_Offset_NG_221 :
                flatRuntime == VerifiedFlatTipsRuntime::kNG240 ?
                    DisplayMenu_Offset_NG_240 : DisplayMenu_Offset_OG;
            logger::info(
                "Flat tips: exact IMenu::DisplayMenu hook installed at "
                "{:#x} for {} (ID {}, RVA {:#x})",
                targetAddress, FlatTipsRuntimeName(flatRuntime), targetID,
                expectedRVA);
        }
        return true;
    }

    char __fastcall D3D11Compositor::HookedDisplayMovie(void* renderer, void* handle, bool param)
    {
        auto* self = s_instance;
        if (self && s_insideLoadingMenuDisplay &&
            self->m_tipsDeltaQueryPending.load(std::memory_order_acquire)) {
            std::lock_guard resourceLock(self->m_renderResourceMutex);
            self->TryFinalizeTipsDeltaQuery();
        }
        bool captureArmed = false;
        VRNativeLoadingSelectionSnapshot vrNativeSelection{};
        const bool vrProofCapture = self &&
            self->m_isVR.load(std::memory_order_acquire) &&
            s_insideLoadingMenuDisplay &&
            self->TryGetVRNativeLoadingSelection(vrNativeSelection) &&
            vrNativeSelection.content ==
                Policy::NativeLoadingContent::kTipAndLevel &&
            vrNativeSelection.owner == s_loadingMenuDisplayOwner;
        const bool flatProofCapture = self &&
            !self->m_isVR.load(std::memory_order_acquire) &&
            s_insideLoadingMenuDisplay &&
            s_loadingMenuDisplaySerial != 0 &&
            Policy::FlatNativeContentNeedsVisibleProof(
                static_cast<Policy::FlatNativeLoadingContent>(
                    self->m_flatNativeLoadingContent.load(
                        std::memory_order_acquire))) &&
            self->m_flatTipsReadySerial.load(
                std::memory_order_acquire) ==
                s_loadingMenuDisplaySerial &&
            self->m_flatTipsLoadEpoch.load(
                std::memory_order_acquire) ==
                s_loadingMenuDisplayLoadEpoch;
        if (self &&
            (vrProofCapture || (flatProofCapture &&
              self->m_flatTipsDrawSerial.load(
                  std::memory_order_acquire) !=
                  s_loadingMenuDisplaySerial)) &&
            s_insideLoadingMenuDisplay && param &&
            self->m_inLoadingScreen.load(std::memory_order_acquire) &&
            self->m_tipsExtractEnabled.load(std::memory_order_acquire) &&
            self->m_loadingTextReady.load(std::memory_order_acquire) &&
            self->m_loadingTextOwner.load(std::memory_order_acquire) ==
                s_loadingMenuDisplayOwner &&
            !self->m_tipsCaptureComplete.load(std::memory_order_acquire) &&
            // Checked HERE as well as inside Begin: otherwise every remaining
            // draw of a hopeless load still takes m_renderResourceMutex and
            // charges its refusal to m_diagCaptureBeginRejects, which makes a
            // normal bailout indistinguishable in the log from a genuinely
            // broken RTV capture.
            !self->m_tipsCaptureHopeless.load(std::memory_order_acquire)) {
            self->m_diagDisplayMovieCaptureEligible.fetch_add(
                1, std::memory_order_relaxed);
            // Snapshot plugin resources, then release the lock before entering
            // Scaleform. Engine code is never called while holding our device
            // teardown mutex.
            std::lock_guard resourceLock(self->m_renderResourceMutex);
            captureArmed = self->BeginLoadingMenuDeltaCapture();
            if (captureArmed) {
                self->m_loadingMenuCaptureFlatProof = flatProofCapture;
                self->m_loadingMenuCaptureFlatOwner = flatProofCapture ?
                    s_loadingMenuDisplayOwner : nullptr;
                self->m_loadingMenuCaptureFlatSerial = flatProofCapture ?
                    s_loadingMenuDisplaySerial : 0;
                self->m_loadingMenuCaptureFlatLoadEpoch = flatProofCapture ?
                    s_loadingMenuDisplayLoadEpoch : 0;
                self->m_loadingMenuCaptureVRProof = vrProofCapture;
                self->m_loadingMenuCaptureVROwner = vrProofCapture ?
                    vrNativeSelection.owner : nullptr;
                self->m_loadingMenuCaptureVRSerial = vrProofCapture ?
                    vrNativeSelection.serial : 0;
                self->m_loadingMenuCaptureVREpoch = vrProofCapture ?
                    vrNativeSelection.epoch : 0;
            }
            (captureArmed ? self->m_diagCaptureArms
                          : self->m_diagCaptureBeginRejects)
                .fetch_add(1, std::memory_order_relaxed);
        }

        const char result = s_originalDisplayMovie
            ? s_originalDisplayMovie(renderer, handle, param)
            : 0;

        if (self && captureArmed) {
            std::lock_guard resourceLock(self->m_renderResourceMutex);
            const bool drawReported = result != 0 && param &&
                self->m_inLoadingScreen.load(std::memory_order_acquire) &&
                self->m_tipsExtractEnabled.load(std::memory_order_acquire);
            if (drawReported) {
                self->CompleteLoadingMenuDeltaCapture();
            } else {
                self->m_diagDisplayMovieResultZero.fetch_add(
                    1, std::memory_order_relaxed);
                self->CancelLoadingMenuDeltaCapture();
            }
            // Completion ran but produced neither a pending non-empty proof nor
            // a finished capture, so one of its gates rejected the draw.
            if (drawReported &&
                !self->m_tipsDeltaQueryPending.load(
                    std::memory_order_relaxed) &&
                !self->m_tipsCaptureComplete.load(
                    std::memory_order_relaxed)) {
                self->m_diagCaptureCompleteCancels.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        return result;
    }

    void __fastcall D3D11Compositor::HookedUpdateSounds(
        void* loadingMenu, std::uint64_t nowMs, bool autoSpinning)
    {
        auto* self = s_instance;
        // Suppress only while our custom loading screen owns presentation and
        // the loading model is therefore absent. Mode 1 keeps Bethesda's
        // native screen, model and sounds included.
        if (self && loadingMenu &&
            self->m_inLoadingScreen.load(std::memory_order_acquire) &&
            self->m_flatMode.load(std::memory_order_acquire) != 1) {
            using StopSound_t = void (__fastcall*)(void*);
            REL::Relocation<StopSound_t> stopSound{
                REL::Offset(SoundHandleStop_Offset_VR)
            };
            auto* base = static_cast<std::uint8_t*>(loadingMenu);
            // Idempotent: stopping a handle that is not playing is a no-op,
            // and this runs in place of the original decision, so the loops
            // can never be started while the model is suppressed.
            stopSound(base + kLoadingMenuRotateSoundHandle);
            stopSound(base + kLoadingMenuZoomSoundHandle);
            if (self->m_loadingSoundSuppressLogCount.fetch_add(
                    1, std::memory_order_relaxed) == 0) {
                logger::info(
                    "LoadingMenu: suppressed orphaned loading-model sounds "
                    "(no model is shown in mode {})",
                    self->m_flatMode.load(std::memory_order_acquire));
            }
            return;
        }
        if (s_originalUpdateSounds) {
            s_originalUpdateSounds(loadingMenu, nowMs, autoSpinning);
        }
    }

    bool D3D11Compositor::InstallUpdateSoundsHook()
    {
        if (m_updateSoundsHookInstalled) return true;
        if (!REL::Module::IsVR()) {
            return false;
        }

        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("UpdateSounds hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        REL::Relocation<void*> target{ REL::Offset(UpdateSounds_Offset_VR) };
        void* targetAddr = reinterpret_cast<void*>(target.address());

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedUpdateSounds),
            reinterpret_cast<void**>(&s_originalUpdateSounds));
        if (st != MH_OK) {
            logger::error("UpdateSounds hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_originalUpdateSounds) {
            logger::error("UpdateSounds hook: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error("UpdateSounds hook: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        m_updateSoundsHookInstalled = true;
        logger::info(
            "UpdateSounds hook installed at LoadingMenu::UpdateSounds ({:x})",
            target.address());
        return true;
    }

    void __fastcall D3D11Compositor::HookedSendLoadingText(void* loadingMenu)
    {
        // A missing trampoline can never be treated as native readiness. The
        // installer rejects that state, but keep the detour itself fail-closed.
        if (!s_originalSendLoadingText) {
            return;
        }

        auto* self = s_instance;
        const bool flatSelection = self && loadingMenu &&
            self->m_flatTipReadinessHookInstalled.load(
                std::memory_order_acquire) &&
            !self->m_isVR.load(std::memory_order_acquire);
        const bool vrSelection = self && loadingMenu &&
            self->m_isVR.load(std::memory_order_acquire) &&
            self->m_vrMode3HooksReady.load(std::memory_order_acquire);
        NativeLoadingTextSelectionSnapshot nativeSnapshot{};
        Policy::NativeLoadingSelection selectedNative{};
        if (flatSelection || vrSelection) {
            nativeSnapshot.fieldsReadable =
                TryReadLoadingTextSelection_SEH(
                    loadingMenu, &nativeSnapshot);
            selectedNative = flatSelection && REL::Module::IsNG() ?
                Policy::ClassifyNGFlatNativeLoadingSelection(
                    nativeSnapshot.fieldsReadable,
                    nativeSnapshot.loadingIntoInterior,
                    nativeSnapshot.hasArtScreen,
                    nativeSnapshot.validScreensSize) :
                Policy::ClassifyNativeLoadingSelection(
                    nativeSnapshot.fieldsReadable,
                    nativeSnapshot.loadingIntoInterior,
                    nativeSnapshot.hasArtScreen,
                    nativeSnapshot.validScreensSize);
        }
        auto selectedContent = selectedNative.content;
        const auto selectedModel = selectedNative.model;
        // loadScreenShown is false only for the publication created by the
        // Show's PopulateLoadScreens call. AdvanceMovie later clears artScreen,
        // so model eligibility must be latched from this first call and never
        // inferred from a text-only refresh.
        const bool flatInitialSelection = flatSelection &&
            (!nativeSnapshot.fieldsReadable ||
             !nativeSnapshot.loadScreenShown);

        bool retainedExistingTip = false;
        bool activePublicationAtEntry = false;
        bool retainFrozenPublication = false;
        std::uint64_t entryLoadEpoch = 0;
        if (flatSelection) {
            // First half of a two-phase publication. Revoke the prior
            // draw/Present proof BEFORE Bethesda starts changing the movie.
            // Present installs RET while holding this same lock, so the old
            // publication cannot be committed across this boundary.
            std::lock_guard<std::mutex> lock(
                self->m_advanceMovieMutex);
            const auto previousContent =
                static_cast<Policy::FlatNativeLoadingContent>(
                    self->m_flatNativeLoadingContent.load(
                        std::memory_order_acquire));
            const bool sameOwner =
                self->m_loadingTextOwner.load(
                    std::memory_order_acquire) == loadingMenu;
            retainedExistingTip = !flatInitialSelection && sameOwner &&
                previousContent ==
                Policy::FlatNativeLoadingContent::kTipAndLevel &&
                selectedContent !=
                    Policy::FlatNativeLoadingContent::kTipAndLevel;
            activePublicationAtEntry =
                self->m_enabled.load(std::memory_order_acquire) &&
                self->m_inLoadingScreen.load(std::memory_order_acquire);
            entryLoadEpoch = self->m_flatTipsLoadEpoch.load(
                std::memory_order_acquire);
            retainFrozenPublication =
                Policy::ShouldRetainFrozenFlatPublication(
                    self->m_advanceMovieKilled.load(
                        std::memory_order_acquire),
                    sameOwner, flatInitialSelection);
            if (!retainFrozenPublication) {
                self->m_flatNativeLoadingContent.store(
                    static_cast<std::uint8_t>(
                        Policy::FlatNativeLoadingContent::kUnknown),
                    std::memory_order_release);
                if (flatInitialSelection) {
                    self->m_flatNativeModelSelection.store(
                        static_cast<std::uint8_t>(
                            Policy::NativeModelSelection::kUnknown),
                        std::memory_order_release);
                    self->m_flatNativeInitialSelectionSerial.store(
                        0, std::memory_order_release);
                }
                self->m_flatTipsDrawSerial.store(
                    0, std::memory_order_release);
                self->m_flatTipsPresentedSerial.store(
                    0, std::memory_order_release);
                self->m_loadingTextReady.store(
                    false, std::memory_order_release);
                self->m_loadingTextOwner.store(
                    nullptr, std::memory_order_release);
                self->m_tipsCaptureComplete.store(
                    false, std::memory_order_release);
                self->m_loadingMenuRenderFresh.store(
                    false, std::memory_order_release);
                self->m_tipsCaptureHopeless.store(
                    false, std::memory_order_release);
                self->m_tipsFullSurfaceRun.store(
                    0, std::memory_order_release);
            }
        }

        bool vrRetainedExistingTip = false;
        bool vrActivePublicationAtEntry = false;
        std::uint64_t vrEntryLoadEpoch = 0;
        if (vrSelection) {
            vrEntryLoadEpoch = self->m_vrTipsLoadEpoch.load(
                std::memory_order_acquire);
            vrActivePublicationAtEntry = self->m_inLoadingScreen.load(
                std::memory_order_acquire);
            const bool sameOwner = self->m_loadingTextOwner.load(
                std::memory_order_acquire) == loadingMenu;
            const auto previousContent =
                static_cast<Policy::NativeLoadingContent>(
                    self->m_vrNativeLoadingContent.load(
                        std::memory_order_acquire));
            vrRetainedExistingTip = sameOwner &&
                self->m_vrNativeSelectionSeen.load(
                    std::memory_order_acquire) &&
                previousContent ==
                    Policy::NativeLoadingContent::kTipAndLevel &&
                selectedContent !=
                    Policy::NativeLoadingContent::kTipAndLevel;

            // Revoke the prior readiness before Bethesda mutates the movie.
            // The exact publication below is release-stored only if this call
            // still belongs to the same active/upcoming VR load epoch.
            self->m_vrNativeSelectionSeen.store(
                false, std::memory_order_release);
            self->m_loadingTextReady.store(
                false, std::memory_order_release);
            self->m_loadingTextOwner.store(
                nullptr, std::memory_order_release);
        }

        // Forward exactly once. Do not call SendLoadingText proactively: it
        // selects/removes a TESLoadScreen and is not an idempotent UI setter.
        s_originalSendLoadingText(loadingMenu);

        if (self && loadingMenu) {
            if (flatSelection) {
                // The independently owned animation-loop NOP may already have
                // followed RET. Restoring only AdvanceMovie here would promise
                // a refresh that can never render. Preserve the previously
                // certified frame/serial instead; Bethesda was still forwarded
                // exactly once, but this frozen custom screen intentionally
                // does not rotate its tip after the speed boundary.
                if (retainFrozenPublication) {
                    logger::info(
                        "Flat tips: native refresh arrived after freeze; "
                        "retained the prior certified visual");
                    return;
                }
                // SendLoadingText may be called again to refresh the level or
                // rotate candidates. A no-text refresh does not hide an
                // already visible tip, so never downgrade a known tip for the
                // same LoadingMenu owner. An unreadable first publication,
                // however, stays Unknown and therefore fail-open/live.
                if (retainedExistingTip) {
                    selectedContent =
                        Policy::FlatNativeLoadingContent::kTipAndLevel;
                }

                std::uint64_t flatReadySerial = 0;
                bool publicationStillOwned = false;
                {
                    // Publish only into the lifecycle epoch in which this call
                    // began. The expected pre-OPEN race may advance once via
                    // SetEnabled(true); a CLOSE/reopen cannot leak this
                    // selection into the next loading screen.
                    std::lock_guard<std::mutex> lock(
                        self->m_advanceMovieMutex);
                    const auto currentEpoch =
                        self->m_flatTipsLoadEpoch.load(
                            std::memory_order_acquire);
                    const bool activeNow =
                        self->m_enabled.load(std::memory_order_acquire) &&
                        self->m_inLoadingScreen.load(
                            std::memory_order_acquire);
                    publicationStillOwned = activePublicationAtEntry ?
                        activeNow && currentEpoch == entryLoadEpoch :
                        !nativeSnapshot.loadScreenShown &&
                            (currentEpoch == entryLoadEpoch ||
                             (activeNow &&
                              currentEpoch == entryLoadEpoch + 1));
                    if (publicationStillOwned &&
                        (flatInitialSelection ||
                         !self->m_advanceMovieKilled.load(
                             std::memory_order_acquire))) {
                        self->m_flatTipsDrawSerial.store(
                            0, std::memory_order_release);
                        self->m_flatTipsPresentedSerial.store(
                            0, std::memory_order_release);
                        self->m_loadingTextOwner.store(
                            loadingMenu, std::memory_order_release);
                        self->m_loadingTextReady.store(
                            Policy::FlatNativeContentNeedsVisibleProof(
                                selectedContent),
                            std::memory_order_release);
                        if (flatInitialSelection) {
                            self->m_flatNativeModelSelection.store(
                                static_cast<std::uint8_t>(selectedModel),
                                std::memory_order_release);
                        }
                        flatReadySerial =
                            self->m_flatTipsReadySerial.fetch_add(
                                1, std::memory_order_acq_rel) + 1;
                        if (flatInitialSelection) {
                            self->m_flatNativeInitialSelectionSerial.store(
                                flatReadySerial, std::memory_order_release);
                        }
                        // Release-publish last. DisplayMenu reads this first.
                        self->m_flatNativeLoadingContent.store(
                            static_cast<std::uint8_t>(selectedContent),
                            std::memory_order_release);
                    } else {
                        publicationStillOwned = false;
                    }
                }

                if (!publicationStillOwned) {
                    logger::warn(
                        "Flat tips: discarded native selection after its "
                        "load epoch changed or AdvanceMovie restoration failed");
                    return;
                }

                logger::info(
                    "Flat tips: native selection={} "
                    "(serial={}, initial={}, model={}, owner={:#x}, "
                    "interior={}, artScreen={}, candidates={}{}); "
                    "waiting for {}",
                    Policy::NativeLoadingContentName(selectedContent),
                    flatReadySerial,
                    flatInitialSelection,
                    selectedModel == Policy::NativeModelSelection::kModel ?
                        "model" :
                        selectedModel ==
                                Policy::NativeModelSelection::kNoModel ?
                            "no-model" : "unknown",
                    reinterpret_cast<std::uintptr_t>(loadingMenu),
                    nativeSnapshot.loadingIntoInterior,
                    nativeSnapshot.hasArtScreen,
                    nativeSnapshot.validScreensSize,
                    retainedExistingTip ? ", retained-existing-tip" : "",
                    Policy::FlatNativeContentNeedsVisibleProof(
                            selectedContent) ?
                        "visible DisplayMovie" :
                        selectedContent ==
                                Policy::FlatNativeLoadingContent::kBackgroundOnly ?
                            "solid-black Present" : "valid native proof");
            } else if (vrSelection) {
                // A no-text refresh does not erase an already-visible native
                // tip for the same owner. This mirrors the flat publication
                // rule and prevents a level-only refresh from downgrading a
                // still-capturable tip screen.
                if (vrRetainedExistingTip) {
                    selectedContent =
                        Policy::NativeLoadingContent::kTipAndLevel;
                }

                const auto currentEpoch = self->m_vrTipsLoadEpoch.load(
                    std::memory_order_acquire);
                const bool activeNow = self->m_inLoadingScreen.load(
                    std::memory_order_acquire);
                std::uint64_t publicationEpoch = 0;
                bool publicationStillOwned = false;
                if (vrActivePublicationAtEntry) {
                    publicationStillOwned = activeNow &&
                        currentEpoch == vrEntryLoadEpoch;
                    publicationEpoch = currentEpoch;
                } else if (!nativeSnapshot.loadScreenShown) {
                    // SendLoadingText may finish immediately before OPEN or
                    // while OPEN is publishing its compositor epoch.
                    publicationStillOwned =
                        currentEpoch == vrEntryLoadEpoch ||
                        currentEpoch == vrEntryLoadEpoch + 1;
                    publicationEpoch = currentEpoch == vrEntryLoadEpoch ?
                        vrEntryLoadEpoch + 1 : currentEpoch;
                }

                if (!publicationStillOwned) {
                    logger::warn(
                        "VR tips: discarded native selection after its load "
                        "epoch changed (entryEpoch={}, currentEpoch={}, "
                        "activeEntry={}, activeNow={})",
                        vrEntryLoadEpoch, currentEpoch,
                        vrActivePublicationAtEntry, activeNow);
                    return;
                }

                self->m_loadingTextOwner.store(
                    loadingMenu, std::memory_order_release);
                self->m_loadingTextReady.store(
                    selectedContent ==
                        Policy::NativeLoadingContent::kTipAndLevel,
                    std::memory_order_release);
                self->m_vrNativeLoadingContent.store(
                    static_cast<std::uint8_t>(selectedContent),
                    std::memory_order_release);
                self->m_vrNativeSelectionEpoch.store(
                    publicationEpoch, std::memory_order_release);
                // Serial and `seen` publish last. Update checks the serial on
                // both sides of its content read, so a same-epoch refresh cannot
                // ABA false->true and mix the old choice with the new one.
                const auto serial =
                    self->m_vrNativeSelectionSerial.fetch_add(
                        1, std::memory_order_acq_rel) + 1;
                self->m_vrNativeSelectionSeen.store(
                    true, std::memory_order_release);

                const char* contentName = selectedContent ==
                        Policy::NativeLoadingContent::kTipAndLevel ?
                    "tip+level" : selectedContent ==
                        Policy::NativeLoadingContent::kBackgroundOnly ?
                    "background-only" : "unknown/fallback";
                logger::info(
                    "VR tips: native selection={} (serial={}, epoch={}, "
                    "owner={:#x}, interior={}, artScreen={}, candidates={}{}); "
                    "{}",
                    contentName, serial, publicationEpoch,
                    reinterpret_cast<std::uintptr_t>(loadingMenu),
                    nativeSnapshot.loadingIntoInterior,
                    nativeSnapshot.hasArtScreen,
                    nativeSnapshot.validScreensSize,
                    vrRetainedExistingTip ? ", retained-existing-tip" : "",
                    selectedContent ==
                            Policy::NativeLoadingContent::kTipAndLevel ?
                        "waiting for exact visible delta" :
                        "no tip capture required");
            }
        }
    }

    bool D3D11Compositor::InstallFlatSendLoadingTextHook()
    {
        if (m_flatTipReadinessHookInstalled.load(
                std::memory_order_acquire)) {
            return true;
        }
        const auto flatRuntime = GetVerifiedFlatTipsRuntime();
        if (REL::Module::IsVR() ||
            flatRuntime == VerifiedFlatTipsRuntime::kUnsupported) {
            if (!REL::Module::IsVR()) {
                const auto version = REL::Module::get().version();
                logger::warn(
                    "Flat tips: LoadingMenu::SendLoadingText unavailable on "
                    "unverified runtime {}.{}.{}.{}",
                    version[0], version[1], version[2], version[3]);
            }
            return false;
        }

        try {
            const bool ngRuntime =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ||
                flatRuntime == VerifiedFlatTipsRuntime::kNG240;
            const std::uint64_t targetID = ngRuntime ?
                SendLoadingText_ID_NG : SendLoadingText_ID_OG;
            const std::uintptr_t expectedRVA =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ?
                    SendLoadingText_Offset_NG_221 :
                flatRuntime == VerifiedFlatTipsRuntime::kNG240 ?
                    SendLoadingText_Offset_NG_240 :
                    SendLoadingText_Offset_OG;
            const auto& expectedEntry = ngRuntime ?
                SendLoadingText_Entry_NG : SendLoadingText_Entry_OG;
            std::uintptr_t targetAddress = 0;
            if (ngRuntime) {
                REL::Relocation<void*> target{
                    REL::ID(SendLoadingText_ID_NG)
                };
                targetAddress = target.address();
            } else {
                REL::Relocation<void*> target{
                    REL::RelocationID(SendLoadingText_ID_OG, 0, 0)
                };
                targetAddress = target.address();
            }
            const auto moduleBase = REL::Module::get().base();
            if (!targetAddress || targetAddress < moduleBase ||
                targetAddress - moduleBase != expectedRVA) {
                logger::error(
                    "Flat tips: SendLoadingText ID {} for {} resolved to "
                    "RVA {:#x}; expected {:#x}",
                    targetID, FlatTipsRuntimeName(flatRuntime),
                    targetAddress >= moduleBase ?
                        targetAddress - moduleBase : 0,
                    expectedRVA);
                return false;
            }
            if (!ExecutableBytesEqual(
                    targetAddress,
                    expectedEntry.data(), expectedEntry.size())) {
                logger::error(
                    "Flat tips: rejected unexpected SendLoadingText entry at "
                    "{:#x} (Fallout4.exe {} signature mismatch)",
                    targetAddress, FlatTipsRuntimeName(flatRuntime));
                return false;
            }

            const MH_STATUS initStatus = MH_Initialize();
            if (initStatus != MH_OK &&
                initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                logger::error(
                    "Flat tips: MH_Initialize failed for SendLoadingText: {}",
                    static_cast<int>(initStatus));
                return false;
            }

            void* const targetPointer =
                reinterpret_cast<void*>(targetAddress);
            const MH_STATUS createStatus = MH_CreateHook(
                targetPointer,
                reinterpret_cast<void*>(&HookedSendLoadingText),
                reinterpret_cast<void**>(&s_originalSendLoadingText));
            if (createStatus != MH_OK || !s_originalSendLoadingText) {
                logger::error(
                    "Flat tips: MH_CreateHook failed for SendLoadingText: {} "
                    "(trampoline={})",
                    static_cast<int>(createStatus),
                    s_originalSendLoadingText != nullptr);
                if (createStatus == MH_OK) {
                    MH_RemoveHook(targetPointer);
                }
                s_originalSendLoadingText = nullptr;
                return false;
            }

            // Publish before enabling so a native call that races the enable
            // cannot return through our detour without receiving a serial.
            m_flatTipReadinessHookInstalled.store(
                true, std::memory_order_release);
            const MH_STATUS enableStatus = MH_EnableHook(targetPointer);
            if (enableStatus != MH_OK) {
                m_flatTipReadinessHookInstalled.store(
                    false, std::memory_order_release);
                logger::error(
                    "Flat tips: MH_EnableHook failed for SendLoadingText: {}",
                    static_cast<int>(enableStatus));
                MH_RemoveHook(targetPointer);
                s_originalSendLoadingText = nullptr;
                return false;
            }

            m_sendLoadingTextHookInstalled = true;
            logger::info(
                "Flat tips: exact LoadingMenu::SendLoadingText hook installed "
                "at {:#x} for {} (ID {}, RVA {:#x})",
                targetAddress, FlatTipsRuntimeName(flatRuntime), targetID,
                expectedRVA);
            return true;
        } catch (const std::exception& e) {
            logger::error(
                "Flat tips: SendLoadingText hook resolution failed: {}",
                e.what());
        } catch (...) {
            logger::error(
                "Flat tips: SendLoadingText hook resolution failed");
        }
        return false;
    }

    void D3D11Compositor::ExtendPostCloseLoadingMenuSuppression(int a_marginMs)
    {
        if (a_marginMs <= 0) return;
        auto current =
            m_lmPostCloseSuppressUntilTicks.load(std::memory_order_acquire);
        // Zero means no window is armed, or a fresh OPEN cancelled it. Never
        // resurrect one: the next load owns presentation now.
        if (current == 0) return;

        const auto target =
            (std::chrono::steady_clock::now() +
             std::chrono::milliseconds(a_marginMs))
                .time_since_epoch().count();

        // OBSERVE FIRST, then suppress. These are two separate atomics, so
        // whichever is published second leaves a transient window where they
        // disagree. Extending observe first means the disagreement is always
        // "observing more than we suppress" (harmless extra logging) rather
        // than "suppressing outside the observed window", which readers of
        // either deadline could mistake for no window at all.
        const auto observeTarget = target +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(kPostCloseObserveExtraMs)).count();
        auto observe =
            m_lmPostCloseObserveUntilTicks.load(std::memory_order_acquire);
        while (observe != 0 && observeTarget > observe) {
            if (m_lmPostCloseObserveUntilTicks.compare_exchange_weak(
                    observe, observeTarget,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }

        // CAS-max, so this can only ever extend, never shorten, and races with
        // a cancelling OPEN resolve in the OPEN's favour.
        while (target > current) {
            if (m_lmPostCloseSuppressUntilTicks.compare_exchange_weak(
                    current, target,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
            if (current == 0) return;
        }
    }

    void D3D11Compositor::BeginPostCloseLoadingMenuSuppression(
        int a_maxMs, bool a_includeMainMenu)
    {
        if (a_maxMs <= 0) {
            m_lmPostCloseSuppressMainMenu.store(
                false, std::memory_order_release);
            m_lmPostCloseSuppressUntilTicks.store(
                0, std::memory_order_release);
            m_lmPostCloseObserveUntilTicks.store(
                0, std::memory_order_release);
            return;
        }
        // Publish the menu-set flag before the deadline that gates reading it.
        // Fresh census budget for each window, so a load that exhausted the
        // in-load skip log still reports everything drawn after CLOSE.
        m_diagPostCloseMenuCalls.store(0, std::memory_order_relaxed);
        m_lmPostCloseArmedTicks.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_release);
        m_lmPostCloseSuppressMainMenu.store(
            a_includeMainMenu, std::memory_order_release);
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(a_maxMs);
        // Observe first, then suppress — same ordering rule as the extend path:
        // never leave a window where suppression is live but unobserved.
        m_lmPostCloseObserveUntilTicks.store(
            (deadline + std::chrono::milliseconds(kPostCloseObserveExtraMs))
                .time_since_epoch().count(),
            std::memory_order_release);
        m_lmPostCloseSuppressUntilTicks.store(
            deadline.time_since_epoch().count(), std::memory_order_release);
    }

    void D3D11Compositor::SampleSubmittedEye(
        void* a_eyeTexture, int a_tag, int a_eye)
    {
        // Diagnostic only, and deliberately unable to affect presentation: any
        // failure just skips the log line. Called at most twice per load, from
        // the Submit thread that already owns the context.
        if (!a_eyeTexture || !m_device || !m_context) return;
        const int tagId = a_tag & 0xFF;
        const int burstIndex = a_tag >> 8;
        const char* const tag =
            tagId == VRCompositorHelper::kEyeSampleArtRelease ? "art-release" :
            tagId == VRCompositorHelper::kEyeSampleCoverRelease
                ? "cover-release" : "?";

        auto* eyeTex = static_cast<ID3D11Texture2D*>(a_eyeTexture);
        D3D11_TEXTURE2D_DESC eyeDesc{};
        eyeTex->GetDesc(&eyeDesc);
        // CopySubresourceRegion rejects multisampled sources; the game submits
        // resolved textures, so this is a guard, not an expected path.
        if (eyeDesc.SampleDesc.Count > 1 ||
            eyeDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            logger::info(
                "Eye sample [{} {} f{}]: skipped (fmt={}, samples={})",
                tag, a_eye == 0 ? "L" : "R", burstIndex, static_cast<int>(eyeDesc.Format),
                eyeDesc.SampleDesc.Count);
            return;
        }

        // The submitted texture is the side-by-side pair; sample a patch from
        // the middle of the right half, which is where a menu or a world both
        // put content.
        constexpr UINT kPatch = 64;
        if (eyeDesc.Width < kPatch * 4 || eyeDesc.Height < kPatch * 2) return;
        const UINT originX = (eyeDesc.Width * 3u) / 4u - kPatch / 2u;
        const UINT originY = eyeDesc.Height / 2u - kPatch / 2u;

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = kPatch;
        stagingDesc.Height = kPatch;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = eyeDesc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(static_cast<ID3D11Device*>(m_device)->CreateTexture2D(
                &stagingDesc, nullptr, &staging)) || !staging) {
            return;
        }

        D3D11_BOX box{};
        box.left = originX;
        box.top = originY;
        box.front = 0;
        box.right = originX + kPatch;
        box.bottom = originY + kPatch;
        box.back = 1;
        auto* context = static_cast<ID3D11DeviceContext*>(m_context);
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, eyeTex, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) &&
            mapped.pData) {
            std::uint64_t sumR = 0, sumG = 0, sumB = 0;
            int nonBlack = 0;
            int maxLum = 0;
            // Coarse 8x8 luminance thumbnail. Mean colour alone cannot tell a
            // dark menu from a dark world view; the spatial pattern can (a
            // menu has large flat regions, a world view does not), and it also
            // shows a frame changing across the burst.
            constexpr UINT kCell = kPatch / 8;
            std::uint32_t cellSum[64] = {};
            const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
            for (UINT y = 0; y < kPatch; ++y) {
                const auto* px = rows + static_cast<size_t>(y) * mapped.RowPitch;
                for (UINT x = 0; x < kPatch; ++x, px += 4) {
                    sumR += px[0];
                    sumG += px[1];
                    sumB += px[2];
                    const int lum = (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
                    if (lum > maxLum) maxLum = lum;
                    if (lum > 8) ++nonBlack;
                    cellSum[(y / kCell) * 8 + (x / kCell)] +=
                        static_cast<std::uint32_t>(lum);
                }
            }
            context->Unmap(staging, 0);
            constexpr int kPixels = kPatch * kPatch;
            constexpr int kCellPixels = kCell * kCell;
            std::string thumb;
            thumb.reserve(72);
            for (int i = 0; i < 64; ++i) {
                if (i && (i % 8) == 0) thumb += '|';
                const auto avg = cellSum[i] / kCellPixels;
                thumb += "0123456789abcdef"[(avg >> 4) & 0xF];
            }
            logger::info(
                "Eye sample [{} {} f{}]: mean=({},{},{}) maxLum={} nonBlack={}/{} "
                "thumb={}",
                tag, a_eye == 0 ? "L" : "R", burstIndex,
                static_cast<int>(sumR / kPixels),
                static_cast<int>(sumG / kPixels),
                static_cast<int>(sumB / kPixels),
                maxLum, nonBlack, kPixels, thumb);
        }
        staging->Release();
    }

    void D3D11Compositor::LogTipsPipelineDiagnostics(const char* a_stage)
    {
        logger::info(
            "Tips pipeline [{}]: displayMenu={} visible={} firstVisibleMs={} "
            "movieEligible={} arms={} beginRejects={} noDraw={} "
            "completeCancels={} proofPending={} captureComplete={}",
            a_stage ? a_stage : "?",
            m_diagLoadingMenuDisplayCalls.load(std::memory_order_relaxed),
            m_diagLoadingMenuDisplayVisible.load(std::memory_order_relaxed),
            m_diagFirstVisibleDisplayMs.load(std::memory_order_relaxed),
            m_diagDisplayMovieCaptureEligible.load(std::memory_order_relaxed),
            m_diagCaptureArms.load(std::memory_order_relaxed),
            m_diagCaptureBeginRejects.load(std::memory_order_relaxed),
            m_diagDisplayMovieResultZero.load(std::memory_order_relaxed),
            m_diagCaptureCompleteCancels.load(std::memory_order_relaxed),
            m_tipsDeltaQueryPending.load(std::memory_order_relaxed),
            m_tipsCaptureComplete.load(std::memory_order_relaxed));
        // Rejection reasons, split. "captureComplete=false" alone never said
        // WHY, so a load that showed no tips was indistinguishable from one
        // that was never eligible.
        logger::info(
            "Tips rejects [{}]: insufficient={} fullSurface={}",
            a_stage ? a_stage : "?",
            m_diagRejectInsufficient.load(std::memory_order_relaxed),
            m_diagRejectFullSurface.load(std::memory_order_relaxed));
        // Kept as explicit field evidence during the March compatibility run:
        // both counters must stay zero because submitted eye textures are no
        // longer mutated by this plugin.
        logger::info(
            "Eye scrub [{}]: DISABLED (March handoff), ok={} fail={} "
            "lastHr={:x}",
            a_stage ? a_stage : "?",
            m_eyeScrubOkCount.load(std::memory_order_relaxed),
            m_eyeScrubFailCount.load(std::memory_order_relaxed),
            m_eyeScrubLastHr.load(std::memory_order_relaxed));
    }

    bool D3D11Compositor::InstallDisplayMovieHook()
    {
        if (m_displayMovieHookInstalled) return true;
        const bool vrRuntime = REL::Module::IsVR();
        const auto flatRuntime = GetVerifiedFlatTipsRuntime();
        if (!vrRuntime &&
            flatRuntime == VerifiedFlatTipsRuntime::kUnsupported) {
            const auto version = REL::Module::get().version();
            logger::warn(
                "Flat tips: BSScaleformRenderer::DisplayMovie unavailable on "
                "unverified runtime {}.{}.{}.{}",
                version[0], version[1], version[2], version[3]);
            return false;
        }

        MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("DisplayMovie hook: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        std::uintptr_t targetAddress = 0;
        if (vrRuntime) {
            REL::Relocation<void*> target{
                REL::Offset(DisplayMovie_Offset_VR)
            };
            targetAddress = target.address();
        } else {
            const bool ngRuntime =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ||
                flatRuntime == VerifiedFlatTipsRuntime::kNG240;
            const std::uint64_t targetID = ngRuntime ?
                DisplayMovie_ID_NG : DisplayMovie_ID_OG;
            const std::uintptr_t expectedRVA =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ?
                    DisplayMovie_Offset_NG_221 :
                flatRuntime == VerifiedFlatTipsRuntime::kNG240 ?
                    DisplayMovie_Offset_NG_240 : DisplayMovie_Offset_OG;
            if (ngRuntime) {
                REL::Relocation<void*> target{ REL::ID(DisplayMovie_ID_NG) };
                targetAddress = target.address();
            } else {
                REL::Relocation<void*> target{
                    REL::RelocationID(DisplayMovie_ID_OG, 0, 0)
                };
                targetAddress = target.address();
            }
            const auto moduleBase = REL::Module::get().base();
            if (!targetAddress || targetAddress < moduleBase ||
                targetAddress - moduleBase != expectedRVA ||
                !ExecutableBytesEqual(
                    targetAddress, DisplayMovie_Entry_OG.data(),
                    DisplayMovie_Entry_OG.size())) {
                logger::error(
                    "Flat tips: rejected BSScaleformRenderer::DisplayMovie "
                    "target for {} (ID {}, RVA={:#x}, expected={:#x}, "
                    "signature={})",
                    FlatTipsRuntimeName(flatRuntime), targetID,
                    targetAddress >= moduleBase ?
                        targetAddress - moduleBase : 0,
                    expectedRVA,
                    targetAddress && ExecutableBytesEqual(
                        targetAddress, DisplayMovie_Entry_OG.data(),
                        DisplayMovie_Entry_OG.size()));
                return false;
            }
        }
        void* targetAddr = reinterpret_cast<void*>(targetAddress);

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedDisplayMovie),
            reinterpret_cast<void**>(&s_originalDisplayMovie));
        if (st != MH_OK) {
            logger::error("DisplayMovie hook: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_originalDisplayMovie) {
            logger::error("DisplayMovie hook: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error("DisplayMovie hook: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        m_displayMovieHookInstalled = true;
        if (vrRuntime) {
            logger::info(
                "DisplayMovie hook installed at "
                "BSScaleformRenderer::DisplayMovie ({:x})",
                targetAddress);
        } else {
            const bool ngRuntime =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ||
                flatRuntime == VerifiedFlatTipsRuntime::kNG240;
            const auto targetID = ngRuntime ?
                DisplayMovie_ID_NG : DisplayMovie_ID_OG;
            const auto expectedRVA =
                flatRuntime == VerifiedFlatTipsRuntime::kNG221 ?
                    DisplayMovie_Offset_NG_221 :
                flatRuntime == VerifiedFlatTipsRuntime::kNG240 ?
                    DisplayMovie_Offset_NG_240 : DisplayMovie_Offset_OG;
            logger::info(
                "Flat tips: exact BSScaleformRenderer::DisplayMovie hook "
                "installed at {:#x} for {} (ID {}, RVA {:#x})",
                targetAddress, FlatTipsRuntimeName(flatRuntime), targetID,
                expectedRVA);
        }
        return true;
    }

    bool D3D11Compositor::InstallFlatTipDrawHooks()
    {
        if (REL::Module::IsVR() ||
            GetVerifiedFlatTipsRuntime() ==
                VerifiedFlatTipsRuntime::kUnsupported) {
            m_flatTipDrawHooksInstalled.store(
                false, std::memory_order_release);
            return false;
        }

        const bool displayMenuReady = InstallDisplayMenuHook();
        const bool displayMovieReady = InstallDisplayMovieHook();
        const bool ready = displayMenuReady && displayMovieReady;
        m_flatTipDrawHooksInstalled.store(
            ready, std::memory_order_release);
        if (ready) {
            logger::info(
                "Flat tips: native owner/draw proof pipeline ready");
        }
        return ready;
    }

    bool D3D11Compositor::InstallVRMode3Hooks()
    {
        if (!REL::Module::IsVR()) {
            return false;
        }

        std::lock_guard installLock(m_vrMode3HookInstallMutex);

        const bool alreadyReady =
            m_displayMenuHookInstalled &&
            m_displayMovieHookInstalled &&
            m_sendLoadingTextHookInstalled;
        if (alreadyReady) {
            m_vrMode3HooksReady.store(true, std::memory_order_release);
            return true;
        }

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error(
                "VR mode-3 hooks: MH_Initialize failed: {}",
                static_cast<int>(initStatus));
            m_vrMode3HooksReady.store(false, std::memory_order_release);
            return false;
        }

        REL::Relocation<void*> displayMenuTarget{
            REL::Offset(DisplayMenu_Offset_VR)
        };
        REL::Relocation<void*> displayMovieTarget{
            REL::Offset(DisplayMovie_Offset_VR)
        };
        REL::Relocation<void*> sendLoadingTextTarget{
            REL::Offset(SendLoadingText_Offset_VR)
        };
        // The VR layout classifier below is justified by this exact 1.2.72
        // function body. Refuse to install the batch if another runtime or an
        // earlier foreign entry detour occupies SendLoadingText; background-only
        // must never be inferred from unverified object offsets.
        if (!ExecutableBytesEqual(
                sendLoadingTextTarget.address(),
                SendLoadingText_Entry_OG.data(),
                SendLoadingText_Entry_OG.size())) {
            logger::error(
                "VR mode-3 hooks: rejected unexpected "
                "LoadingMenu::SendLoadingText entry at {:#x} "
                "(expected Fallout4VR 1.2.72 RVA {:#x})",
                sendLoadingTextTarget.address(),
                SendLoadingText_Offset_VR);
            m_vrMode3HooksReady.store(false, std::memory_order_release);
            return false;
        }

        struct HookRequest
        {
            void* target;
            void* detour;
            void** original;
            bool* installed;
            const char* name;
            bool created = false;
            bool queued = false;
        };
        std::array<HookRequest, 3> requests{ {
            {
                reinterpret_cast<void*>(displayMenuTarget.address()),
                reinterpret_cast<void*>(&HookedDisplayMenu),
                reinterpret_cast<void**>(&s_originalDisplayMenu),
                &m_displayMenuHookInstalled,
                "IMenu::DisplayMenu"
            },
            {
                reinterpret_cast<void*>(displayMovieTarget.address()),
                reinterpret_cast<void*>(&HookedDisplayMovie),
                reinterpret_cast<void**>(&s_originalDisplayMovie),
                &m_displayMovieHookInstalled,
                "BSScaleformRenderer::DisplayMovie"
            },
            {
                reinterpret_cast<void*>(sendLoadingTextTarget.address()),
                reinterpret_cast<void*>(&HookedSendLoadingText),
                reinterpret_cast<void**>(&s_originalSendLoadingText),
                &m_sendLoadingTextHookInstalled,
                "LoadingMenu::SendLoadingText"
            }
        } };

        auto rollback = [&]() {
            // First overwrite any not-yet-applied enable request with a queued
            // disable. ApplyQueued can itself fail after partially changing
            // targets, so synchronously verify every affected hook is disabled
            // before attempting removal. Never null an original trampoline
            // while its detour might still be live.
            bool disableQueued = false;
            for (auto& request : requests) {
                if (request.queued) {
                    const MH_STATUS status =
                        MH_QueueDisableHook(request.target);
                    disableQueued = disableQueued || status == MH_OK;
                    if (status != MH_OK &&
                        status != MH_ERROR_DISABLED) {
                        logger::error(
                            "VR mode-3 hooks: rollback queue-disable {} "
                            "failed: {}",
                            request.name, static_cast<int>(status));
                    }
                }
            }
            if (disableQueued) {
                const MH_STATUS status = MH_ApplyQueued();
                if (status != MH_OK) {
                    logger::error(
                        "VR mode-3 hooks: rollback ApplyQueued failed: {}",
                        static_cast<int>(status));
                }
            }
            for (auto& request : requests) {
                if (!request.queued) continue;
                const MH_STATUS disableStatus =
                    MH_DisableHook(request.target);
                const bool disabled =
                    disableStatus == MH_OK ||
                    disableStatus == MH_ERROR_DISABLED;
                if (!disabled) {
                    logger::critical(
                        "VR mode-3 hooks: rollback could not disable {} "
                        "(status={}); retaining trampoline",
                        request.name, static_cast<int>(disableStatus));
                    continue;
                }
                *request.installed = false;
                if (request.created) {
                    const MH_STATUS removeStatus =
                        MH_RemoveHook(request.target);
                    if (removeStatus == MH_OK) {
                        *request.original = nullptr;
                    } else {
                        logger::error(
                            "VR mode-3 hooks: rollback remove {} failed: {}; "
                            "retaining trampoline",
                            request.name, static_cast<int>(removeStatus));
                    }
                }
            }
        };

        bool prepareSucceeded = true;
        for (auto& request : requests) {
            if (*request.installed) continue;

            MH_STATUS status = MH_CreateHook(
                request.target, request.detour, request.original);
            if (status == MH_OK) {
                request.created = true;
            } else if (
                status != MH_ERROR_ALREADY_CREATED ||
                !*request.original) {
                logger::error(
                    "VR mode-3 hooks: create {} failed: {}",
                    request.name, static_cast<int>(status));
                prepareSucceeded = false;
                break;
            }
            if (!*request.original) {
                logger::error(
                    "VR mode-3 hooks: {} has no original trampoline",
                    request.name);
                prepareSucceeded = false;
                break;
            }

            status = MH_QueueEnableHook(request.target);
            if (status != MH_OK) {
                logger::error(
                    "VR mode-3 hooks: queue {} failed: {}",
                    request.name, static_cast<int>(status));
                prepareSucceeded = false;
                break;
            }
            request.queued = true;
        }

        if (!prepareSucceeded) {
            rollback();
            m_vrMode3HooksReady.store(false, std::memory_order_release);
            return false;
        }

        // One ApplyQueued means one process-thread suspension, replacing the
        // three 98-123 ms MH_EnableHook stalls observed in the VR boot log.
        const MH_STATUS applyStatus = MH_ApplyQueued();
        if (applyStatus != MH_OK) {
            logger::error(
                "VR mode-3 hooks: batched enable failed: {}",
                static_cast<int>(applyStatus));
            rollback();
            m_vrMode3HooksReady.store(false, std::memory_order_release);
            return false;
        }
        for (auto& request : requests) {
            if (request.queued) {
                *request.installed = true;
            }
        }

        const bool ready =
            m_displayMenuHookInstalled &&
            m_displayMovieHookInstalled &&
            m_sendLoadingTextHookInstalled;
        m_vrMode3HooksReady.store(ready, std::memory_order_release);

        if (ready) {
            REL::Relocation<void*> lmV1{
                REL::Offset(LoadingMenu_Vtable1_Offset)
            };
            REL::Relocation<void*> lmV2{
                REL::Offset(LoadingMenu_Vtable2_Offset)
            };
            // MainMenu vtables belong here too: the post-close census prints a
            // raw vtable= value, and without both pairs in the log a reader
            // cannot resolve what drew. (InstallDisplayMenuHook has the same
            // log but is never called on VR - this batch installer is the live
            // path, so the pairs have to be printed HERE to exist at all.)
            REL::Relocation<void*> mmV1{
                REL::Offset(MainMenu_Vtable1_Offset)
            };
            REL::Relocation<void*> mmV2{
                REL::Offset(MainMenu_Vtable2_Offset)
            };
            logger::info(
                "VR mode-3 hooks batch-enabled before loading "
                "(DisplayMenu={:x}, DisplayMovie={:x}, SendLoadingText={:x}, "
                "LoadingMenu vtables={:x}/{:x}, MainMenu vtables={:x}/{:x})",
                displayMenuTarget.address(),
                displayMovieTarget.address(),
                sendLoadingTextTarget.address(),
                lmV1.address(), lmV2.address(),
                mmV1.address(), mmV2.address());
        } else {
            logger::warn(
                "VR mode-3 hook set incomplete; "
                "native fallback retained");
        }
        return ready;
    }

    int D3D11Compositor::IdentifyBoundRenderTargetIndex() const
    {
        // Retained only as a diagnostic API for old logs. Mode 3 now captures
        // the exact bound RTV subresource and never indexes RendererData.
        return -1;
#if 0
        if (!m_context || !REL::Module::IsVR()) return -1;

        auto* context = static_cast<ID3D11DeviceContext*>(m_context);
        ID3D11RenderTargetView* boundView = nullptr;
        context->OMGetRenderTargets(1, &boundView, nullptr);
        ComReleaseGuard boundViewGuard{ boundView };
        if (!boundView) return -1;

        REL::Relocation<void**> rendererDataPtr{
            REL::Offset(BSGraphics_RendererData_Offset_VR)
        };
        void* rendererData = nullptr;
        __try {
            rendererData = *rendererDataPtr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        if (!rendererData) return -1;

        const auto base = reinterpret_cast<std::uintptr_t>(rendererData);
        auto readView = [&](int index) -> ID3D11RenderTargetView* {
            __try {
                const auto address = base + RendererData_RenderTargets_Offset
                    + static_cast<std::size_t>(index) * RenderTarget_Stride
                    + RenderTarget_RTV_Offset;
                return *reinterpret_cast<ID3D11RenderTargetView**>(address);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        };

        int matchedIndex = -1;
        for (int index = 0; index < 101; ++index) {
            if (readView(index) == boundView) {
                matchedIndex = index;
                break;
            }
        }

        // Scaleform may wrap the same texture in its own RTV. If pointer
        // identity differs, compare the underlying resource while the exact
        // LoadingMenu owner scope still holds.
        ID3D11Resource* boundResource = nullptr;
        ComReleaseGuard boundResourceGuard{};
        if (matchedIndex < 0) {
            boundView->GetResource(&boundResource);
            boundResourceGuard.object = boundResource;
            if (boundResource) {
                for (int index = 0; index < 101; ++index) {
                    auto* candidateView = readView(index);
                    if (!candidateView) continue;
                    ID3D11Resource* candidateResource = nullptr;
                    candidateView->GetResource(&candidateResource);
                    const bool matches =
                        candidateResource == boundResource;
                    ReleaseCom(candidateResource);
                    if (matches) {
                        matchedIndex = index;
                        break;
                    }
                }
            }
        }

        if (matchedIndex >= 0) {
            const int previous = m_loadingMenuScaleformRtIndex.exchange(
                matchedIndex, std::memory_order_acq_rel);
            if (previous != matchedIndex) {
                logger::info(
                    "Scaleform tips: LoadingMenu bound RT idx={} verified",
                    matchedIndex);
            }
        }
        return matchedIndex;
#endif
    }

    void* D3D11Compositor::AcquireScaleformRTTexture() const
    {
        // This is the render target matched against Fallout's bound RTV inside
        // the verified LoadingMenu DisplayMenu owner window. It is intentionally
        // not the generic last Scaleform index or submitted eye texture.
        // CommonLib/Ghidra establish the VR RenderTarget layout as texture@+0x00
        // and rtView@+0x10. In VR the direct texture field can be null while the
        // RTV remains valid, so the RTV's resource is the authoritative fallback.
        const int idx =
            m_loadingMenuScaleformRtIndex.load(std::memory_order_acquire);
        if (idx < 0 || idx >= 101) return nullptr;

        REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset_VR) };
        void* rendererData = *rendererDataPtr;
        if (!rendererData) return nullptr;

        auto base = reinterpret_cast<std::uintptr_t>(rendererData);
        const auto rtBase = base + RendererData_RenderTargets_Offset
            + static_cast<std::size_t>(idx) * RenderTarget_Stride;

        ID3D11Texture2D* tex = *reinterpret_cast<ID3D11Texture2D**>(
            rtBase + RenderTarget_Texture_Offset);
        const char* source = "texture";
        if (tex) {
            tex->AddRef();
        } else {
            auto* rtv = *reinterpret_cast<ID3D11RenderTargetView**>(
                rtBase + RenderTarget_RTV_Offset);
            if (!rtv) return nullptr;

            ID3D11Resource* resource = nullptr;
            rtv->GetResource(&resource);
            if (!resource) return nullptr;
            const HRESULT hr = resource->QueryInterface(
                __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            resource->Release();
            if (FAILED(hr) || !tex) return nullptr;
            source = "rtv";
        }

        int n = m_scaleformRTLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 12) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            logger::info("SFT read #{}: idx={} source={} tex={} {}x{} fmt={}",
                n + 1, idx, source, static_cast<void*>(tex),
                d.Width, d.Height, (int)d.Format);
        }
        return tex;
    }

    namespace
    {
        struct BoundRTCapture
        {
            ID3D11Texture2D* texture = nullptr;  // owned COM reference
            UINT subresource = 0;
            UINT width = 0;
            UINT height = 0;
            DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
            DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
            D3D11_VIEWPORT viewport{};
            bool scissorEnabled = false;
            D3D11_RECT scissor{};
            UINT cropLeft = 0;
            UINT cropTop = 0;
            UINT cropRight = 0;
            UINT cropBottom = 0;
        };

        void ReleaseBoundRTCapture(BoundRTCapture& capture)
        {
            ReleaseCom(capture.texture);
            capture = {};
        }

        bool GetBoundRTCapture(
            ID3D11DeviceContext* context, BoundRTCapture& capture)
        {
            ReleaseBoundRTCapture(capture);
            if (!context) return false;

            ID3D11RenderTargetView* view = nullptr;
            context->OMGetRenderTargets(1, &view, nullptr);
            ComReleaseGuard viewGuard{ view };
            if (!view) return false;

            D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
            view->GetDesc(&viewDesc);

            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            ComReleaseGuard resourceGuard{ resource };
            if (!resource) return false;

            ID3D11Texture2D* texture = nullptr;
            const HRESULT queryResult = resource->QueryInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture));
            if (FAILED(queryResult) || !texture) return false;
            ComReleaseGuard textureGuard{ texture };

            D3D11_TEXTURE2D_DESC textureDesc{};
            texture->GetDesc(&textureDesc);
            if (textureDesc.SampleDesc.Count != 1 ||
                textureDesc.MipLevels == 0 ||
                textureDesc.ArraySize == 0) {
                return false;
            }

            UINT mipSlice = 0;
            UINT arraySlice = 0;
            switch (viewDesc.ViewDimension) {
            case D3D11_RTV_DIMENSION_TEXTURE2D:
                mipSlice = viewDesc.Texture2D.MipSlice;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                // A multi-slice RTV has no single exact source subresource.
                // Refuse it instead of guessing which eye/layer Scaleform drew.
                if (viewDesc.Texture2DArray.ArraySize != 1) {
                    return false;
                }
                mipSlice = viewDesc.Texture2DArray.MipSlice;
                arraySlice = viewDesc.Texture2DArray.FirstArraySlice;
                break;
            default:
                return false;
            }
            if (mipSlice >= textureDesc.MipLevels ||
                arraySlice >= textureDesc.ArraySize) {
                return false;
            }

            const UINT width =
                std::max(1u, textureDesc.Width >> mipSlice);
            const UINT height =
                std::max(1u, textureDesc.Height >> mipSlice);

            // Screen-space Scaleform can target one viewport inside a larger
            // eye/SBS surface. Multiple viewports are ambiguous: publishing the
            // full RTV monoscopically could halve, offset, or duplicate the UI.
            UINT viewportCount = 0;
            context->RSGetViewports(&viewportCount, nullptr);
            if (viewportCount != 1) return false;
            D3D11_VIEWPORT viewport{};
            viewportCount = 1;
            context->RSGetViewports(&viewportCount, &viewport);
            const float viewportRight =
                viewport.TopLeftX + viewport.Width;
            const float viewportBottom =
                viewport.TopLeftY + viewport.Height;
            if (viewportCount != 1 ||
                !std::isfinite(viewport.TopLeftX) ||
                !std::isfinite(viewport.TopLeftY) ||
                !std::isfinite(viewport.Width) ||
                !std::isfinite(viewport.Height) ||
                !std::isfinite(viewport.MinDepth) ||
                !std::isfinite(viewport.MaxDepth) ||
                !std::isfinite(viewportRight) ||
                !std::isfinite(viewportBottom) ||
                viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
                viewport.MinDepth < 0.0f || viewport.MinDepth > 1.0f ||
                viewport.MaxDepth < 0.0f || viewport.MaxDepth > 1.0f ||
                viewport.MinDepth > viewport.MaxDepth) {
                return false;
            }

            ID3D11RasterizerState* rasterState = nullptr;
            context->RSGetState(&rasterState);
            ComReleaseGuard rasterGuard{ rasterState };
            D3D11_RASTERIZER_DESC rasterDesc{};
            if (rasterState) {
                rasterState->GetDesc(&rasterDesc);
            }

            D3D11_RECT scissor{};
            const bool scissorEnabled = rasterDesc.ScissorEnable != FALSE;
            if (scissorEnabled) {
                UINT scissorCount = 0;
                context->RSGetScissorRects(&scissorCount, nullptr);
                if (scissorCount != 1) return false;
                scissorCount = 1;
                context->RSGetScissorRects(&scissorCount, &scissor);
                if (scissorCount != 1 ||
                    scissor.left >= scissor.right ||
                    scissor.top >= scissor.bottom) {
                    return false;
                }
            }

            float cropLeft = std::max(0.0f, viewport.TopLeftX);
            float cropTop = std::max(0.0f, viewport.TopLeftY);
            float cropRight =
                std::min(static_cast<float>(width), viewportRight);
            float cropBottom =
                std::min(static_cast<float>(height), viewportBottom);
            if (scissorEnabled) {
                cropLeft = std::max(
                    cropLeft, static_cast<float>(scissor.left));
                cropTop = std::max(
                    cropTop, static_cast<float>(scissor.top));
                cropRight = std::min(
                    cropRight, static_cast<float>(scissor.right));
                cropBottom = std::min(
                    cropBottom, static_cast<float>(scissor.bottom));
            }
            if (!(cropRight > cropLeft && cropBottom > cropTop)) {
                return false;
            }

            const UINT pixelLeft = std::min(
                width, static_cast<UINT>(std::floor(cropLeft)));
            const UINT pixelTop = std::min(
                height, static_cast<UINT>(std::floor(cropTop)));
            const UINT pixelRight = std::min(
                width, static_cast<UINT>(std::ceil(cropRight)));
            const UINT pixelBottom = std::min(
                height, static_cast<UINT>(std::ceil(cropBottom)));
            constexpr UINT kMinimumCaptureRegionDimension = 256;
            if (pixelRight <= pixelLeft || pixelBottom <= pixelTop ||
                pixelRight - pixelLeft < kMinimumCaptureRegionDimension ||
                pixelBottom - pixelTop < kMinimumCaptureRegionDimension) {
                return false;
            }

            capture.texture = texture;
            textureGuard.object = nullptr;
            capture.subresource = D3D11CalcSubresource(
                mipSlice, arraySlice, textureDesc.MipLevels);
            capture.width = width;
            capture.height = height;
            capture.resourceFormat = textureDesc.Format;
            capture.viewFormat = viewDesc.Format;
            capture.viewport = viewport;
            capture.scissorEnabled = scissorEnabled;
            capture.scissor = scissor;
            capture.cropLeft = pixelLeft;
            capture.cropTop = pixelTop;
            capture.cropRight = pixelRight;
            capture.cropBottom = pixelBottom;
            return true;
        }

        DXGI_FORMAT NormalizeSrvFormat(
            DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
        {
            if (viewFormat != DXGI_FORMAT_UNKNOWN) return viewFormat;
            switch (resourceFormat) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:
                return resourceFormat;
            }
        }

        std::uint32_t CaptureBytesPerTexel(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16_TYPELESS:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R16G16_UNORM:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
            case DXGI_FORMAT_R32G32_TYPELESS:
            case DXGI_FORMAT_R32G32_FLOAT:
                return 8;
            case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return 16;
            default:
                return 0;
            }
        }
    }

    void D3D11Compositor::CancelLoadingMenuDeltaCapture()
    {
        m_loadingMenuDeltaArmed.store(false, std::memory_order_release);
        ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuCaptureSource);
        m_loadingMenuCaptureSubresource = 0;
        m_loadingMenuCaptureViewFormat = 0;
        m_loadingMenuCaptureViewport = {};
        m_loadingMenuCaptureScissor = {};
        m_loadingMenuCaptureRegion = {};
        m_loadingMenuCaptureFlatProof = false;
        m_loadingMenuCaptureFlatOwner = nullptr;
        m_loadingMenuCaptureFlatSerial = 0;
        m_loadingMenuCaptureFlatLoadEpoch = 0;
        m_loadingMenuCaptureVRProof = false;
        m_loadingMenuCaptureVROwner = nullptr;
        m_loadingMenuCaptureVRSerial = 0;
        m_loadingMenuCaptureVREpoch = 0;
    }

    bool D3D11Compositor::BeginLoadingMenuDeltaCapture()
    {
        if (!m_device || !m_context ||
            !m_loadingTextReady.load(std::memory_order_acquire) ||
            !m_tipsExtractEnabled.load(std::memory_order_acquire) ||
            m_tipsDeltaQueryPending.load(std::memory_order_acquire) ||
            m_tipsCaptureComplete.load(std::memory_order_acquire) ||
            // Provably non-convergent this load (N consecutive full-surface
            // rejects): stop issuing 4 MiB snapshot copies + delta draws that
            // can only reject again. Audit measured 8-27 futile armed attempts
            // (~70-230 MiB of copies) per short load without this.
            m_tipsCaptureHopeless.load(std::memory_order_acquire)) {
            return false;
        }

        CancelLoadingMenuDeltaCapture();

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* context = static_cast<ID3D11DeviceContext*>(m_context);
        BoundRTCapture capture{};
        if (!GetBoundRTCapture(context, capture)) {
            if (m_tipsScaleformLogCount.fetch_add(
                    1, std::memory_order_relaxed) < 2) {
                logger::warn(
                    "Scaleform tips: current LoadingMenu draw has no single "
                    "safe RTV viewport/scissor region; capture will retry");
            }
            return false;
        }
        ComReleaseGuard captureGuard{ capture.texture };

        // Limit one-time copy cost and reject pathological/invalid targets.
        // Submitted VR eyes are allowed here because the exact movie delta,
        // not the eye pixels themselves, is the presentation source.
        if (capture.width < 256 || capture.height < 256 ||
            capture.width > 8192 || capture.height > 8192) {
            return false;
        }
        const std::uint32_t bytesPerTexel =
            CaptureBytesPerTexel(capture.resourceFormat);
        const std::uint64_t pixelCount =
            static_cast<std::uint64_t>(capture.width) * capture.height;
        constexpr std::uint64_t kMaxCapturePixels = 16ull * 1024ull * 1024ull;
        constexpr std::uint64_t kMaxCapturePairBytes =
            192ull * 1024ull * 1024ull;
        if (bytesPerTexel == 0 || pixelCount > kMaxCapturePixels ||
            pixelCount * bytesPerTexel * 2ull > kMaxCapturePairBytes) {
            if (m_tipsScaleformLogCount.fetch_add(
                    1, std::memory_order_relaxed) < 2) {
                logger::warn(
                    "Scaleform tips: refusing oversized/unsupported exact "
                    "delta {}x{} fmt={} bytesPerTexel={}",
                    capture.width, capture.height,
                    static_cast<int>(capture.resourceFormat),
                    bytesPerTexel);
            }
            return false;
        }

        const DXGI_FORMAT srvFormat = NormalizeSrvFormat(
            capture.resourceFormat, capture.viewFormat);
        const bool recreate =
            !m_loadingMenuBeforeTex || !m_loadingMenuAfterTex ||
            !m_loadingMenuBeforeSRV || !m_loadingMenuAfterSRV ||
            m_loadingMenuCaptureWidth != capture.width ||
            m_loadingMenuCaptureHeight != capture.height ||
            m_loadingMenuCaptureFormat !=
                static_cast<unsigned int>(capture.resourceFormat) ||
            m_loadingMenuCaptureSrvFormat !=
                static_cast<unsigned int>(srvFormat);
        if (recreate) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = capture.width;
            desc.Height = capture.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = capture.resourceFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* beforeTexture = nullptr;
            ID3D11Texture2D* afterTexture = nullptr;
            ID3D11ShaderResourceView* beforeView = nullptr;
            ID3D11ShaderResourceView* afterView = nullptr;
            HRESULT hr = device->CreateTexture2D(
                &desc, nullptr, &beforeTexture);
            if (SUCCEEDED(hr)) {
                hr = device->CreateTexture2D(
                    &desc, nullptr, &afterTexture);
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (SUCCEEDED(hr)) {
                hr = device->CreateShaderResourceView(
                    beforeTexture, &srvDesc, &beforeView);
            }
            if (SUCCEEDED(hr)) {
                hr = device->CreateShaderResourceView(
                    afterTexture, &srvDesc, &afterView);
            }
            if (FAILED(hr) || !beforeTexture || !afterTexture ||
                !beforeView || !afterView) {
                ReleaseCom(beforeView);
                ReleaseCom(afterView);
                ReleaseCom(beforeTexture);
                ReleaseCom(afterTexture);
                if (m_tipsScaleformLogCount.fetch_add(
                        1, std::memory_order_relaxed) < 2) {
                    logger::warn(
                        "Scaleform tips: exact delta resources unavailable "
                        "({}x{}, resourceFmt={}, viewFmt={}, hr={:x})",
                        capture.width, capture.height,
                        static_cast<int>(capture.resourceFormat),
                        static_cast<int>(srvFormat),
                        static_cast<unsigned>(hr));
                }
                return false;
            }

            ReleaseVoidCom<ID3D11ShaderResourceView>(
                m_loadingMenuBeforeSRV);
            ReleaseVoidCom<ID3D11ShaderResourceView>(
                m_loadingMenuAfterSRV);
            ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuBeforeTex);
            ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuAfterTex);
            m_loadingMenuBeforeTex = beforeTexture;
            m_loadingMenuAfterTex = afterTexture;
            m_loadingMenuBeforeSRV = beforeView;
            m_loadingMenuAfterSRV = afterView;
            m_loadingMenuCaptureWidth = capture.width;
            m_loadingMenuCaptureHeight = capture.height;
            m_loadingMenuCaptureFormat =
                static_cast<unsigned int>(capture.resourceFormat);
            m_loadingMenuCaptureSrvFormat =
                static_cast<unsigned int>(srvFormat);
        }

        context->CopySubresourceRegion(
            static_cast<ID3D11Texture2D*>(m_loadingMenuBeforeTex),
            0, 0, 0, 0, capture.texture, capture.subresource, nullptr);

        m_loadingMenuCaptureSource = capture.texture;
        captureGuard.object = nullptr;
        m_loadingMenuCaptureSubresource = capture.subresource;
        m_loadingMenuCaptureViewFormat =
            static_cast<unsigned int>(capture.viewFormat);
        m_loadingMenuCaptureViewport = {
            capture.viewport.TopLeftX,
            capture.viewport.TopLeftY,
            capture.viewport.Width,
            capture.viewport.Height,
            capture.viewport.MinDepth,
            capture.viewport.MaxDepth
        };
        m_loadingMenuCaptureScissor = {
            static_cast<std::int32_t>(capture.scissor.left),
            static_cast<std::int32_t>(capture.scissor.top),
            static_cast<std::int32_t>(capture.scissor.right),
            static_cast<std::int32_t>(capture.scissor.bottom),
            capture.scissorEnabled
        };
        m_loadingMenuCaptureRegion = {
            capture.cropLeft,
            capture.cropTop,
            capture.cropRight,
            capture.cropBottom
        };
        m_loadingMenuDeltaArmed.store(true, std::memory_order_release);
        return true;
    }

    void D3D11Compositor::CompleteLoadingMenuDeltaCapture()
    {
        if (!m_loadingMenuDeltaArmed.load(std::memory_order_acquire) ||
            !m_device || !m_context ||
            (m_loadingMenuCaptureFlatProof ?
                !m_psFlatTipsProof : !m_psTipsDelta) ||
            !m_constantBuffer ||
            !m_loadingMenuCaptureSource ||
            !m_loadingMenuBeforeTex || !m_loadingMenuAfterTex ||
            !m_loadingMenuBeforeSRV || !m_loadingMenuAfterSRV) {
            CancelLoadingMenuDeltaCapture();
            return;
        }

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* context = static_cast<ID3D11DeviceContext*>(m_context);
        const auto& capturedRegion = m_loadingMenuCaptureRegion;

        // Copy the "after" image straight from the armed source rather than from
        // whatever render target happens to be bound now. Scaleform's D3D1x HAL
        // rebinds the viewport inside the draw and never restores it before
        // DisplayMovie returns (TreeCacheRoot::Draw -> HAL::BeginDisplay ->
        // beginDisplay -> D3D1x::HAL::updateViewport issues RSSetViewports with
        // MinDepth == MaxDepth == 0), and BSScaleformRenderer::EndScene only runs
        // after IMenu::DisplayMenu has returned. Re-querying the bound state here
        // therefore never matched the "before" snapshot and silently cancelled
        // every capture. The armed reference is still the exact surface the movie
        // drew into: HAL::BeginScene only *reads* the bound RTV
        // (OMGetRenderTargets) and the HAL's only OMSetRenderTargets pair is the
        // balanced PushRenderTarget/pop used for filters. Ownership safety is
        // unchanged because the delta remains bounded by this single verified
        // LoadingMenu draw, and a delta that captured nothing is still rejected
        // (and logged) by the occlusion proof below.
        context->CopySubresourceRegion(
            static_cast<ID3D11Texture2D*>(m_loadingMenuAfterTex),
            0, 0, 0, 0,
            static_cast<ID3D11Texture2D*>(m_loadingMenuCaptureSource),
            m_loadingMenuCaptureSubresource, nullptr);

        // Preserve the effective viewport/scissor region aspect and cap the
        // persistent overlay texture. The one-time full-RTV copies above retain
        // exact glyph edges; the delta shader samples only the crop recorded when
        // the capture armed.
        if (capturedRegion.right <= capturedRegion.left ||
            capturedRegion.bottom <= capturedRegion.top) {
            CancelLoadingMenuDeltaCapture();
            return;
        }
        const UINT sourceRegionWidth =
            capturedRegion.right - capturedRegion.left;
        const UINT sourceRegionHeight =
            capturedRegion.bottom - capturedRegion.top;
        UINT destWidth = std::min(2048u, sourceRegionWidth);
        UINT destHeight = static_cast<UINT>(std::max(
            1.0, std::round(
                static_cast<double>(sourceRegionHeight) *
                static_cast<double>(destWidth) /
                static_cast<double>(sourceRegionWidth))));
        if (destHeight > 2048) {
            destWidth = static_cast<UINT>(std::max(
                1.0, std::round(
                    static_cast<double>(destWidth) * 2048.0 /
                    static_cast<double>(destHeight))));
            destHeight = 2048;
        }

        if (!m_tipsScaleformTex || !m_tipsScaleformRTV ||
            m_tipsScaleformWidth != destWidth ||
            m_tipsScaleformHeight != destHeight) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = destWidth;
            desc.Height = destHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* outputTexture = nullptr;
            ID3D11RenderTargetView* outputView = nullptr;
            HRESULT hr = device->CreateTexture2D(
                &desc, nullptr, &outputTexture);
            if (SUCCEEDED(hr)) {
                hr = device->CreateRenderTargetView(
                    outputTexture, nullptr, &outputView);
            }
            if (FAILED(hr) || !outputTexture || !outputView) {
                ReleaseCom(outputView);
                ReleaseCom(outputTexture);
                logger::warn(
                    "Scaleform tips: delta output unavailable (hr={:x})",
                    static_cast<unsigned>(hr));
                CancelLoadingMenuDeltaCapture();
                return;
            }

            // The current overlay still references only the plain DDS at this
            // point; a delta texture is never replaced after publication within
            // the same load.
            ReleaseVoidCom<ID3D11ShaderResourceView>(m_flatTipsProofSRV);
            ReleaseVoidCom<ID3D11RenderTargetView>(m_tipsScaleformRTV);
            ReleaseVoidCom<ID3D11Texture2D>(m_tipsScaleformTex);
            m_tipsScaleformTex = outputTexture;
            m_tipsScaleformRTV = outputView;
            m_tipsScaleformWidth = destWidth;
            m_tipsScaleformHeight = destHeight;
            m_tipsScaleformFormat =
                static_cast<unsigned int>(desc.Format);
        }

        if (!m_tipsDeltaQuery) {
            D3D11_QUERY_DESC queryDesc{};
            queryDesc.Query = D3D11_QUERY_OCCLUSION;
            ID3D11Query* query = nullptr;
            const HRESULT queryResult =
                device->CreateQuery(&queryDesc, &query);
            if (FAILED(queryResult) || !query) {
                logger::warn(
                    "Scaleform tips: non-empty proof query unavailable "
                    "(hr={:x})",
                    static_cast<unsigned>(queryResult));
                CancelLoadingMenuDeltaCapture();
                return;
            }
            m_tipsDeltaQuery = query;
        }

        const float inverseSourceWidth =
            1.0f / static_cast<float>(m_loadingMenuCaptureWidth);
        const float inverseSourceHeight =
            1.0f / static_cast<float>(m_loadingMenuCaptureHeight);
        const DeltaSourceParams sourceParams{
            static_cast<float>(capturedRegion.left) * inverseSourceWidth,
            static_cast<float>(capturedRegion.top) * inverseSourceHeight,
            static_cast<float>(sourceRegionWidth) * inverseSourceWidth,
            static_cast<float>(sourceRegionHeight) * inverseSourceHeight
        };
        auto* constantBuffer =
            static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = context->Map(
            constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(mapResult) || !mapped.pData) {
            logger::warn(
                "Scaleform tips: delta source transform map failed (hr={:x})",
                static_cast<unsigned>(mapResult));
            CancelLoadingMenuDeltaCapture();
            return;
        }
        std::memcpy(mapped.pData, &sourceParams, sizeof(sourceParams));
        context->Unmap(constantBuffer, 0);

        {
            ScopedPipelineState savedState(context);
            auto* outputView = static_cast<ID3D11RenderTargetView*>(
                m_tipsScaleformRTV);
            constexpr float transparent[4] = {
                0.0f, 0.0f, 0.0f, 0.0f
            };
            context->ClearRenderTargetView(outputView, transparent);
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(destWidth);
            viewport.Height = static_cast<float>(destHeight);
            viewport.MaxDepth = 1.0f;
            context->OMSetRenderTargets(1, &outputView, nullptr);
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            BindFullscreenVertexPipeline(
                context,
                static_cast<ID3D11VertexShader*>(m_vsFullscreen));
            context->PSSetShader(
                static_cast<ID3D11PixelShader*>(
                    m_loadingMenuCaptureFlatProof ?
                        m_psFlatTipsProof : m_psTipsDelta),
                nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &constantBuffer);
            ID3D11ShaderResourceView* sources[2] = {
                static_cast<ID3D11ShaderResourceView*>(
                    m_loadingMenuAfterSRV),
                static_cast<ID3D11ShaderResourceView*>(
                    m_loadingMenuBeforeSRV)
            };
            context->PSSetShaderResources(0, 2, sources);
            auto* sampler = static_cast<ID3D11SamplerState*>(m_sampler);
            context->PSSetSamplers(0, 1, &sampler);
            context->OMSetBlendState(
                static_cast<ID3D11BlendState*>(m_blendState),
                nullptr, 0xFFFFFFFF);
            context->RSSetState(
                static_cast<ID3D11RasterizerState*>(m_rasterState));
            context->OMSetDepthStencilState(
                static_cast<ID3D11DepthStencilState*>(m_depthState), 0);
            auto* query =
                static_cast<ID3D11Query*>(m_tipsDeltaQuery);
            context->Begin(query);
            context->Draw(3, 0);
            context->End(query);
        }

        // Retire only the armed source reference. Keep the verified geometry
        // until the asynchronous proof resolves so the success log describes
        // the exact draw that produced the published texture.
        m_loadingMenuDeltaArmed.store(false, std::memory_order_release);
        ReleaseVoidCom<ID3D11Texture2D>(m_loadingMenuCaptureSource);
        // Snapshot the delta draw's OWN target size for the coverage test. The
        // occlusion query counts samples at dest resolution (crop scaled into a
        // <=2048 viewport), so dividing by the full source-RTV area understates
        // coverage whenever the crop is smaller than the RTV - at 4096x4096
        // with a quarter-area crop the 40% guard was mathematically dead and a
        // full repaint sailed under it (the L2 black square, back again).
        // Dedicated fields, NOT m_tipsScaleformWidth/Height: those are also
        // written by CaptureScaleformRTToTips, which is gated only on
        // m_tipsCaptureComplete and can overwrite them while this query is
        // still in flight.
        m_tipsDeltaQueryWidth = destWidth;
        m_tipsDeltaQueryHeight = destHeight;
        m_tipsDeltaQueryPending.store(true, std::memory_order_release);
        TryFinalizeTipsDeltaQuery();
    }

    void D3D11Compositor::TryFinalizeTipsDeltaQuery()
    {
        if (!m_tipsDeltaQueryPending.load(std::memory_order_acquire) ||
            !m_context || !m_tipsDeltaQuery) {
            return;
        }

        UINT64 visibleSamples = 0;
        const HRESULT result =
            static_cast<ID3D11DeviceContext*>(m_context)->GetData(
                static_cast<ID3D11Query*>(m_tipsDeltaQuery),
                &visibleSamples, sizeof(visibleSamples),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) {
            return;
        }
        m_tipsDeltaQueryPending.store(false, std::memory_order_release);
        if (FAILED(result)) {
            logger::warn(
                "Scaleform tips: delta content query failed (hr={:x}); "
                "capture will retry",
                static_cast<unsigned>(result));
            CancelLoadingMenuDeltaCapture();
            return;
        }
        const bool flatProof = m_loadingMenuCaptureFlatProof;
        const UINT64 surfaceSamples =
            static_cast<UINT64>(m_tipsDeltaQueryWidth) *
            static_cast<UINT64>(m_tipsDeltaQueryHeight);
        if (surfaceSamples == 0) {
            logger::warn(
                "Scaleform tips: delta target size unavailable "
                "({}x{}); rejecting rather than publishing untested",
                m_tipsDeltaQueryWidth, m_tipsDeltaQueryHeight);
            CancelLoadingMenuDeltaCapture();
            return;
        }
        const UINT64 kMinimumTipSamples = flatProof ?
            Policy::FlatVisibleTipDeltaMinimum(surfaceSamples) : 16;
        if (visibleSamples < kMinimumTipSamples) {
            const int n = m_diagRejectInsufficient.fetch_add(
                1, std::memory_order_relaxed);
            // An insufficient (near-empty) delta breaks a full-surface run:
            // the surface is no longer repainting wholesale, so convergence is
            // back on the table.
            m_tipsFullSurfaceRun.store(0, std::memory_order_relaxed);
            // Budget: first two, then every 16th. The old shared 3-entry gate
            // hid ~52 of the ~55 attempts a failing load makes, which is why
            // four sessions could not show whether the retry ever converges.
            if (n < 2 || (n % 16) == 0) {
                logger::info(
                    "Scaleform tips: insufficient LoadingMenu delta "
                    "(samples={}, need={}, attempt {}); awaiting the next "
                    "native movie draw",
                    visibleSamples, kMinimumTipSamples, n + 1);
            }
            CancelLoadingMenuDeltaCapture();
            return;
        }
        // Upper bound. A tips delta is a few glyphs: field captures measure
        // 22k-33k samples out of a 1024x1024 surface, about 2-3%. On SHORT
        // interior loads the before-snapshot can be taken while the surface is
        // still being initialised, so the "delta" comes back as the ENTIRE
        // surface (1047552 of 1048576, 99.9%). That is not a tips delta, it is
        // a full repaint, and compositing it lays the menu's opaque black
        // backdrop over the artwork — the black square reported over the
        // loading screen when entering All Faiths Church and Publick
        // Occurrences (loads #3-#6, all 1.2-1.9 s, all samples=1047552, while
        // the longer loads #1/#2/#7 captured 22k-33k and looked correct).
        // Only a minimum was ever checked, so the pathological case passed.
        // Denominator = the delta draw's own target area, snapshotted when the
        // query was armed. Same units as the numerator (the occlusion query
        // counts dest-resolution samples); the previous full-source-RTV
        // denominator made the 40% guard porous for sub-RTV crops.
        // Fail CLOSED on a bad denominator. The dims are provably non-zero
        // today, but this guard's entire job is keeping a full repaint out of
        // the published texture; if a future edit to the dest-size maths ever
        // zeroed them, the old "!= 0 &&" form would silently stop testing and
        // publish the black square with no log line to show for it.
        const bool excessiveCoverage = flatProof ?
            !Policy::AcceptFlatVisibleTipDelta(
                visibleSamples, surfaceSamples) :
            visibleSamples * 100u >
                surfaceSamples * kMaxTipCoveragePercent;
        if (excessiveCoverage) {
            const int n = m_diagRejectFullSurface.fetch_add(
                1, std::memory_order_relaxed);
            // Consecutive full-surface rejects mean the surface is repainting
            // wholesale every draw; retrying cannot converge (field data: every
            // eligible draw of a short load rejects at exactly 1047552/1048576).
            // Declare the load hopeless and stop arming captures. Never set
            // m_tipsCaptureComplete here - that would publish the rejected
            // full-surface image and resurrect the black square.
            if (m_tipsFullSurfaceRun.fetch_add(1, std::memory_order_relaxed) +
                    1 >= kTipsHopelessConsecutiveRejects &&
                !m_tipsCaptureHopeless.exchange(
                    true, std::memory_order_acq_rel)) {
                // Elapsed is logged so a wrongly-EARLY bailout is falsifiable:
                // if this ever fires a few ms into a load that would have
                // converged later, the timestamp says so.
                const auto sinceOpen = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    std::chrono::steady_clock::time_point(
                        std::chrono::steady_clock::duration(
                            m_inLoadingScreenSinceTicks.load(
                                std::memory_order_acquire))))
                    .count();
                logger::info(
                    "Scaleform tips: capture declared hopeless after {} "
                    "consecutive full-surface rejects (+{} ms into the load, "
                    "insufficient={}); no further attempts this load "
                    "(background-only)",
                    kTipsHopelessConsecutiveRejects, sinceOpen,
                    m_diagRejectInsufficient.load(std::memory_order_relaxed));
            }
            if (n < 2 || (n % 16) == 0) {
                logger::info(
                    "Scaleform tips: full-surface delta rejected "
                    "(samples={}/{}, max {}%, attempt {}); awaiting the next "
                    "native movie draw",
                    visibleSamples, surfaceSamples,
                    flatProof ? 10 :
                        static_cast<int>(kMaxTipCoveragePercent), n + 1);
            }
            CancelLoadingMenuDeltaCapture();
            return;
        }
        if (!m_inLoadingScreen.load(std::memory_order_acquire) ||
            !m_tipsExtractEnabled.load(std::memory_order_acquire)) {
            CancelLoadingMenuDeltaCapture();
            return;
        }

        if (flatProof) {
            const auto serial = m_loadingMenuCaptureFlatSerial;
            const auto epoch = m_loadingMenuCaptureFlatLoadEpoch;
            const auto nativeContent =
                static_cast<Policy::FlatNativeLoadingContent>(
                    m_flatNativeLoadingContent.load(
                        std::memory_order_acquire));
            const bool stillOwned = serial != 0 &&
                !m_isVR.load(std::memory_order_acquire) &&
                m_flatTipDrawHooksInstalled.load(
                    std::memory_order_acquire) &&
                m_enabled.load(std::memory_order_acquire) &&
                m_inLoadingScreen.load(std::memory_order_acquire) &&
                m_flatMode.load(std::memory_order_acquire) == 3 &&
                Policy::FlatNativeContentNeedsVisibleProof(nativeContent) &&
                m_loadingTextOwner.load(
                    std::memory_order_acquire) ==
                    m_loadingMenuCaptureFlatOwner &&
                m_flatTipsReadySerial.load(
                    std::memory_order_acquire) == serial &&
                m_flatTipsLoadEpoch.load(
                    std::memory_order_acquire) == epoch;
            if (!stillOwned) {
                logger::info(
                    "Flat tips: stale visible-pixel query retired without "
                    "publication (serial={}, epoch={}, owner={:#x})",
                    serial, epoch,
                    reinterpret_cast<std::uintptr_t>(
                        m_loadingMenuCaptureFlatOwner));
                CancelLoadingMenuDeltaCapture();
                return;
            }

            ID3D11ShaderResourceView* replayView = nullptr;
            const HRESULT replayResult =
                static_cast<ID3D11Device*>(m_device)->
                    CreateShaderResourceView(
                        static_cast<ID3D11Texture2D*>(m_tipsScaleformTex),
                        nullptr, &replayView);
            if (FAILED(replayResult) || !replayView) {
                logger::warn(
                    "Flat tips: visible delta replay SRV unavailable "
                    "(hr={:x}); capture will retry",
                    static_cast<unsigned>(replayResult));
                CancelLoadingMenuDeltaCapture();
                return;
            }
            ReleaseVoidCom<ID3D11ShaderResourceView>(m_flatTipsProofSRV);
            m_flatTipsProofSRV = replayView;
            // Publish only after both pixel proof and persistent replay source
            // exist. Present uses this serial for authorization and rendering.
            m_flatTipsDrawSerial.store(serial, std::memory_order_release);
            logger::info(
                "Flat tips: owner-matched visible pixel delta proven "
                "(serial={}, epoch={}, owner={:#x}, samples={}/{})",
                serial, epoch,
                reinterpret_cast<std::uintptr_t>(
                    m_loadingMenuCaptureFlatOwner),
                visibleSamples, surfaceSamples);
        } else {
            VRNativeLoadingSelectionSnapshot currentSelection{};
            const bool stillOwned = m_loadingMenuCaptureVRProof &&
                m_isVR.load(std::memory_order_acquire) &&
                m_inLoadingScreen.load(std::memory_order_acquire) &&
                m_flatMode.load(std::memory_order_acquire) == 3 &&
                TryGetVRNativeLoadingSelection(currentSelection) &&
                currentSelection.content ==
                    Policy::NativeLoadingContent::kTipAndLevel &&
                currentSelection.owner == m_loadingMenuCaptureVROwner &&
                currentSelection.serial == m_loadingMenuCaptureVRSerial &&
                currentSelection.epoch == m_loadingMenuCaptureVREpoch;
            if (!stillOwned) {
                logger::info(
                    "VR tips: stale visible-pixel query retired without "
                    "publication (serial={}, epoch={}, owner={:#x})",
                    m_loadingMenuCaptureVRSerial,
                    m_loadingMenuCaptureVREpoch,
                    reinterpret_cast<std::uintptr_t>(
                        m_loadingMenuCaptureVROwner));
                CancelLoadingMenuDeltaCapture();
                return;
            }
            logger::info(
                "VR tips: owner-matched visible pixel delta proven "
                "(serial={}, epoch={}, owner={:#x}, samples={}/{})",
                currentSelection.serial, currentSelection.epoch,
                reinterpret_cast<std::uintptr_t>(currentSelection.owner),
                visibleSamples, surfaceSamples);
        }

        m_loadingMenuRenderFresh.store(true, std::memory_order_release);
        m_tipsCaptureComplete.store(true, std::memory_order_release);
        logger::info(
            "Scaleform tips: exact LoadingMenu delta captured "
            "(rtv={}x{} sub={} resourceFmt={} viewFmt={} srvFmt={} "
            "viewport=({:.3f},{:.3f} {:.3f}x{:.3f} z={:.3f}..{:.3f}) "
            "scissor={} [{},{},{},{}] crop=[{},{},{},{}] -> {}x{}, "
            "samples={}, controller-safe)",
            m_loadingMenuCaptureWidth, m_loadingMenuCaptureHeight,
            m_loadingMenuCaptureSubresource,
            m_loadingMenuCaptureFormat, m_loadingMenuCaptureViewFormat,
            m_loadingMenuCaptureSrvFormat,
            m_loadingMenuCaptureViewport.topLeftX,
            m_loadingMenuCaptureViewport.topLeftY,
            m_loadingMenuCaptureViewport.width,
            m_loadingMenuCaptureViewport.height,
            m_loadingMenuCaptureViewport.minDepth,
            m_loadingMenuCaptureViewport.maxDepth,
            m_loadingMenuCaptureScissor.enabled,
            m_loadingMenuCaptureScissor.left,
            m_loadingMenuCaptureScissor.top,
            m_loadingMenuCaptureScissor.right,
            m_loadingMenuCaptureScissor.bottom,
            m_loadingMenuCaptureRegion.left,
            m_loadingMenuCaptureRegion.top,
            m_loadingMenuCaptureRegion.right,
            m_loadingMenuCaptureRegion.bottom,
            m_tipsScaleformWidth, m_tipsScaleformHeight,
            visibleSamples);
    }

    void D3D11Compositor::CaptureScaleformRTToTips()
    {
        if (!m_device || !m_context || !m_psTipsKey || !m_vsFullscreen) return;
        // One-shot gate: after the first successful blit, the IVROverlay
        // holds the captured texture and shows it for free. Re-running the
        // full GPU pass at 90 Hz the entire load was the dominant VR cost.
        if (m_tipsCaptureComplete.load(std::memory_order_acquire)) return;

        void* srcTex = AcquireScaleformRTTexture();
        if (!srcTex) return;
        ComReleaseGuard srcGuard{ static_cast<IUnknown*>(
            static_cast<ID3D11Texture2D*>(srcTex)) };

        auto* src = static_cast<ID3D11Texture2D*>(srcTex);
        D3D11_TEXTURE2D_DESC srcDesc{};
        src->GetDesc(&srcDesc);

        // The submitted VR eye/framebuffer is wide (the observed runtime is
        // 4224x2304) and can already contain controllers. A Scaleform UI surface
        // is square. Refuse any non-square/full-frame resource even though it
        // came from the shared screen-space slot; false negatives retain the
        // plain background, while a false positive would bake 3D imagery.
        if (srcDesc.Width != srcDesc.Height ||
            srcDesc.Width < 512 || srcDesc.Width > 2048 ||
            srcDesc.ArraySize != 1) {
            if (m_tipsScaleformLogCount.fetch_add(
                    1, std::memory_order_relaxed) < 1) {
                logger::warn(
                    "Scaleform tips: untrusted screen-space surface "
                    "{}x{} array={} (idx={}); refusing eye/framebuffer capture",
                    srcDesc.Width, srcDesc.Height, srcDesc.ArraySize,
                    GetScaleformRTIndex());
            }
            return;
        }

        if (srcDesc.SampleDesc.Count != 1) {
            if (m_tipsScaleformLogCount.fetch_add(1, std::memory_order_relaxed) < 1) {
                logger::warn("Scaleform tips: unexpected MSAA (samples={}), skipping",
                    srcDesc.SampleDesc.Count);
            }
            return;
        }

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);

        const unsigned int destW = srcDesc.Width  * kTipsUpscale;
        const unsigned int destH = srcDesc.Height * kTipsUpscale;

        // (Re)create destination texture + RTV when dims change.
        if (!m_tipsScaleformTex
            || m_tipsScaleformWidth  != destW
            || m_tipsScaleformHeight != destH)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = destW;
            d.Height = destH;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* dst = nullptr;
            HRESULT hr = device->CreateTexture2D(&d, nullptr, &dst);
            if (FAILED(hr) || !dst) {
                logger::warn("Scaleform tips: dest tex create failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return;
            }
            ID3D11RenderTargetView* rtv = nullptr;
            hr = device->CreateRenderTargetView(dst, nullptr, &rtv);
            if (FAILED(hr) || !rtv) {
                dst->Release();
                logger::warn("Scaleform tips: dest RTV create failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return;
            }

            if (m_tipsScaleformRTV)
                static_cast<ID3D11RenderTargetView*>(m_tipsScaleformRTV)->Release();
            if (m_tipsScaleformTex)
                static_cast<ID3D11Texture2D*>(m_tipsScaleformTex)->Release();
            m_tipsScaleformTex = dst;
            m_tipsScaleformRTV = rtv;
            m_tipsScaleformWidth = destW;
            m_tipsScaleformHeight = destH;
            m_tipsScaleformFormat = static_cast<unsigned>(d.Format);

            logger::info("Scaleform tips: dest tex created {}x{} (upscale {}x)",
                destW, destH, kTipsUpscale);
        }

        // (Re)create source SRV when the game's RT pointer changes.
        if (m_tipsScaleformSrcCached != srcTex) {
            DXGI_FORMAT srvFmt = srcDesc.Format;
            if (srvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (srvFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;

            ID3D11ShaderResourceView* srv = nullptr;
            HRESULT hr = device->CreateShaderResourceView(src, &sd, &srv);
            if (FAILED(hr) || !srv) {
                logger::warn("Scaleform tips: src SRV create failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return;
            }
            if (m_tipsScaleformSrcSRV)
                static_cast<ID3D11ShaderResourceView*>(m_tipsScaleformSrcSRV)->Release();
            m_tipsScaleformSrcSRV = srv;
            m_tipsScaleformSrcCached = srcTex;
        }

        ScopedPipelineState savedState(ctx);

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(destW);
        vp.Height = static_cast<float>(destH);
        vp.MaxDepth = 1.0f;

        auto* rtv  = static_cast<ID3D11RenderTargetView*>(m_tipsScaleformRTV);
        auto* srv  = static_cast<ID3D11ShaderResourceView*>(m_tipsScaleformSrcSRV);
        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));
        // Key the UI and remove VaultTecLogo_mc while producing the persistent
        // capture. Both the normal bg+tips composite and its separate-overlay
        // fallback therefore consume a spinner-free texture.
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psTipsKey), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);

        const int logCount = m_tipsScaleformLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 4) {
            logger::info("Scaleform tips: keyed spinner-free capture {} ({}x{} → {}x{})",
                logCount + 1, srcDesc.Width, srcDesc.Height, destW, destH);
        }
        // Publish only after the draw succeeds. The render-suppression hooks
        // and overlay readiness checks consume this current-load proof, so any
        // MSAA/allocation/SRV failure above preserves the native fallback.
        m_tipsCaptureComplete.store(true, std::memory_order_release);
    }

    // ========================================================================
    // Shader compilation (dynamic d3dcompiler_47.dll)
    // ========================================================================

    bool D3D11Compositor::CompileShaders()
    {
        if (m_vsFullscreen && m_psLuminanceKey && m_psBackground &&
            m_psBlit && m_psTipsKey && m_psTipsDelta &&
            m_psFlatTipsProof) {
            return true;
        }
        if (!s_D3DCompile) {
            HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
            if (!compiler) {
                logger::error("D3D11Compositor: d3dcompiler_47.dll not found");
                return false;
            }
            s_D3DCompile = reinterpret_cast<D3DCompileFn>(
                GetProcAddress(compiler, "D3DCompile"));
            if (!s_D3DCompile) {
                logger::error("D3D11Compositor: D3DCompile not found");
                return false;
            }
        }

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto cleanupShaders = [this]() {
            ReleaseVoidCom<ID3D11VertexShader>(m_vsFullscreen);
            ReleaseVoidCom<ID3D11PixelShader>(m_psLuminanceKey);
            ReleaseVoidCom<ID3D11PixelShader>(m_psBackground);
            ReleaseVoidCom<ID3D11PixelShader>(m_psBlit);
            ReleaseVoidCom<ID3D11PixelShader>(m_psTipsKey);
            ReleaseVoidCom<ID3D11PixelShader>(m_psTipsDelta);
            ReleaseVoidCom<ID3D11PixelShader>(m_psFlatTipsProof);
            return false;
        };
        cleanupShaders();
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr;

        // Vertex shader
        hr = s_D3DCompile(VS_FULLSCREEN_SRC, strlen(VS_FULLSCREEN_SRC), "vs_fullscreen",
            nullptr, nullptr, "main", "vs_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error("VS compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11VertexShader* vs = nullptr;
        hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !vs) {
            logger::error("CreateVertexShader failed (hr={:x})", static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_vsFullscreen = vs;

        // Luminance key pixel shader (flat composite)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_LUMINANCE_KEY_SRC, strlen(PS_LUMINANCE_KEY_SRC), "ps_lumkey",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error("PS lumkey compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psLum = nullptr;
        hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psLum);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psLum) {
            logger::error("CreatePixelShader(lumkey) failed (hr={:x})", static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psLuminanceKey = psLum;

        // Background-only pixel shader (for ClearRTV mode)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_BACKGROUND_SRC, strlen(PS_BACKGROUND_SRC), "ps_bg",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error("PS bg compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psBg = nullptr;
        hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psBg);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psBg) {
            logger::error("CreatePixelShader(background) failed (hr={:x})", static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psBackground = psBg;

        // Blit pixel shader (scaleform RT → upscaled tips texture)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_BLIT_SRC, strlen(PS_BLIT_SRC), "ps_blit",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error("PS blit compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psBlit = nullptr;
        hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psBlit);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psBlit) {
            logger::error("CreatePixelShader(blit) failed (hr={:x})", static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psBlit = psBlit;

        // Maskless tips alpha-key (composite pass 2)
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(PS_TIPS_KEY_SRC, strlen(PS_TIPS_KEY_SRC), "ps_tipskey",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error("PS tipskey compile: {}", static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psTipsKey = nullptr;
        hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &psTipsKey);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psTipsKey) {
            logger::error("CreatePixelShader(tips key) failed (hr={:x})", static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psTipsKey = psTipsKey;

        // LoadingMenu before/after delta. This is the only mode-3 capture
        // shader allowed to consume a framebuffer-shaped source.
        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(
            PS_TIPS_DELTA_SRC, strlen(PS_TIPS_DELTA_SRC), "ps_tipsdelta",
            nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error(
                    "PS tips-delta compile: {}",
                    static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psTipsDelta = nullptr;
        hr = device->CreatePixelShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            &psTipsDelta);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psTipsDelta) {
            logger::error(
                "CreatePixelShader(tips delta) failed (hr={:x})",
                static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psTipsDelta = psTipsDelta;

        errors = nullptr;
        blob = nullptr;
        hr = s_D3DCompile(
            PS_FLAT_TIPS_PROOF_SRC, strlen(PS_FLAT_TIPS_PROOF_SRC),
            "ps_flattipsproof", nullptr, nullptr, "main", "ps_5_0", 0, 0,
            reinterpret_cast<void**>(&blob),
            reinterpret_cast<void**>(&errors));
        if (FAILED(hr) || !blob) {
            if (errors) {
                logger::error(
                    "PS flat tips proof compile: {}",
                    static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
            if (blob) blob->Release();
            return cleanupShaders();
        }
        ID3D11PixelShader* psFlatTipsProof = nullptr;
        hr = device->CreatePixelShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            &psFlatTipsProof);
        blob->Release();
        if (errors) errors->Release();
        if (FAILED(hr) || !psFlatTipsProof) {
            logger::error(
                "CreatePixelShader(flat tips proof) failed (hr={:x})",
                static_cast<unsigned>(hr));
            return cleanupShaders();
        }
        m_psFlatTipsProof = psFlatTipsProof;

        logger::info("D3D11Compositor: all shaders compiled");
        return m_vsFullscreen && m_psLuminanceKey && m_psBackground &&
            m_psBlit && m_psTipsKey && m_psTipsDelta &&
            m_psFlatTipsProof;
    }

    // ========================================================================
    // Background texture management
    // ========================================================================

    void D3D11Compositor::SetBackgroundTexture(void* d3dTexture)
    {
        ID3D11ShaderResourceView* newSRV = nullptr;
        unsigned int newWidth = 0;
        unsigned int newHeight = 0;

        if (d3dTexture && m_device) {
            auto* device = static_cast<ID3D11Device*>(m_device);
            auto* tex = static_cast<ID3D11Texture2D*>(d3dTexture);

            D3D11_TEXTURE2D_DESC texDesc{};
            tex->GetDesc(&texDesc);

            DXGI_FORMAT srvFormat = texDesc.Format;
            if (srvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (srvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            else if (srvFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS) srvFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = srvFormat;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = texDesc.MipLevels;

            const HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, &newSRV);
            if (FAILED(hr) || !newSRV) {
                logger::warn("D3D11Compositor: failed to create bg SRV (hr={:x})",
                    static_cast<unsigned>(hr));
                return;  // Keep the prior valid background on replacement failure.
            }
            newWidth = texDesc.Width;
            newHeight = texDesc.Height;
        }

        ID3D11ShaderResourceView* oldSRV = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_bgMutex);
            oldSRV = static_cast<ID3D11ShaderResourceView*>(m_bgSRV);
            m_bgSRV = newSRV;
            m_bgWidth = newWidth;
            m_bgHeight = newHeight;
        }
        ReleaseCom(oldSRV);
        if (newSRV) {
            logger::info("D3D11Compositor: background SRV created ({}x{})",
                newWidth, newHeight);
        }
    }

    void* D3D11Compositor::AcquireBackgroundSRV(
        unsigned int& width, unsigned int& height) const
    {
        std::lock_guard<std::mutex> lock(m_bgMutex);
        auto* srv = static_cast<ID3D11ShaderResourceView*>(m_bgSRV);
        if (srv) srv->AddRef();
        width = m_bgWidth;
        height = m_bgHeight;
        return srv;
    }

    bool D3D11Compositor::HasBackgroundTexture() const
    {
        std::lock_guard<std::mutex> lock(m_bgMutex);
        return m_bgSRV != nullptr;
    }

    void* D3D11Compositor::AcquireTipsTexture() const
    {
        std::lock_guard resourceLock(m_renderResourceMutex);
        if (!m_tipsCaptureComplete.load(std::memory_order_acquire) ||
            !m_tipsScaleformTex) {
            return nullptr;
        }
        auto* texture =
            static_cast<ID3D11Texture2D*>(m_tipsScaleformTex);
        texture->AddRef();
        return texture;
    }

    // Bake bg + captured tips into a single owned 16:9 landscape texture.
    // The captured tips texture (scaleform RT) is square, so its pass is drawn
    // into a centered square viewport to preserve text proportions rather than
    // stretching it across the widescreen background.
    //   Pass 1: bg, opaque, cover-fit (m_psBackground + bgUvScale).
    //   Pass 2: tips, SrcAlpha/InvSrcAlpha on top (m_psBlit + m_alphaBlendState).
    // Caller submits the returned tex as the bg overlay's ONLY texture — there
    // is no separate tips overlay, so the tips are truly baked into the bg.
    bool D3D11Compositor::ComputeTipsContentBounds()
    {
        // Caller holds m_renderResourceMutex and has verified m_tipsScaleformTex.
        m_tipsContentBoundsValid = false;
        if (!m_device || !m_context || !m_psBlit || !m_vsFullscreen ||
            !m_tipsSrcSRV) {
            return false;
        }
        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);

        if (!m_tipsBoundsTex || !m_tipsBoundsRTV || !m_tipsBoundsStaging) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = kTipsBoundsRes;
            desc.Height = kTipsBoundsRes;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;

            ID3D11Texture2D* boundsTex = nullptr;
            ID3D11RenderTargetView* boundsRTV = nullptr;
            ID3D11Texture2D* staging = nullptr;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &boundsTex);
            if (SUCCEEDED(hr)) {
                hr = device->CreateRenderTargetView(
                    boundsTex, nullptr, &boundsRTV);
            }
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC stagingDesc = desc;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.BindFlags = 0;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
            }
            if (FAILED(hr) || !boundsTex || !boundsRTV || !staging) {
                ReleaseCom(staging);
                ReleaseCom(boundsRTV);
                ReleaseCom(boundsTex);
                logger::warn(
                    "Tips bounds: measurement resources unavailable (hr={:x})",
                    static_cast<unsigned>(hr));
                return false;
            }
            ReleaseVoidCom<ID3D11Texture2D>(m_tipsBoundsStaging);
            ReleaseVoidCom<ID3D11RenderTargetView>(m_tipsBoundsRTV);
            ReleaseVoidCom<ID3D11Texture2D>(m_tipsBoundsTex);
            m_tipsBoundsTex = boundsTex;
            m_tipsBoundsRTV = boundsRTV;
            m_tipsBoundsStaging = staging;
        }

        {
            ScopedPipelineState savedState(ctx);
            auto* rtv = static_cast<ID3D11RenderTargetView*>(m_tipsBoundsRTV);
            constexpr float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ctx->ClearRenderTargetView(rtv, transparent);
            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(kTipsBoundsRes);
            vp.Height = static_cast<float>(kTipsBoundsRes);
            vp.MaxDepth = 1.0f;
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            BindFullscreenVertexPipeline(
                ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));
            ctx->PSSetShader(
                static_cast<ID3D11PixelShader*>(m_psBlit), nullptr, 0);
            auto* sampler = static_cast<ID3D11SamplerState*>(m_sampler);
            ctx->PSSetSamplers(0, 1, &sampler);
            auto* srv = static_cast<ID3D11ShaderResourceView*>(m_tipsSrcSRV);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->OMSetBlendState(
                static_cast<ID3D11BlendState*>(m_blendState),
                nullptr, 0xFFFFFFFF);
            ctx->RSSetState(
                static_cast<ID3D11RasterizerState*>(m_rasterState));
            ctx->OMSetDepthStencilState(
                static_cast<ID3D11DepthStencilState*>(m_depthState), 0);
            ctx->Draw(3, 0);
        }

        ctx->CopyResource(
            static_cast<ID3D11Texture2D*>(m_tipsBoundsStaging),
            static_cast<ID3D11Texture2D*>(m_tipsBoundsTex));

        // One bounded readback per capture (16 KB). It blocks until the two
        // draws above retire, which is why this runs from the frame callback
        // rather than inside Fallout's Scaleform call.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = ctx->Map(
            static_cast<ID3D11Texture2D*>(m_tipsBoundsStaging),
            0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult) || !mapped.pData) {
            logger::warn("Tips bounds: readback map failed (hr={:x})",
                static_cast<unsigned>(mapResult));
            return false;
        }

        unsigned int minX = kTipsBoundsRes;
        unsigned int minY = kTipsBoundsRes;
        unsigned int maxX = 0;
        unsigned int maxY = 0;
        // The downsample is one bilinear tap per output pixel (single-mip
        // source), not a true box average, so a thin stroke that lands near a
        // tap arrives heavily diluted. Keep the threshold low; strokes falling
        // between taps are covered by the one-block bound expansion below.
        constexpr std::uint8_t kAlphaThreshold = 6;
        const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
        for (unsigned int y = 0; y < kTipsBoundsRes; ++y) {
            const auto* row = rows + static_cast<std::size_t>(y) * mapped.RowPitch;
            for (unsigned int x = 0; x < kTipsBoundsRes; ++x) {
                if (row[x * 4 + 3] >= kAlphaThreshold) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        ctx->Unmap(static_cast<ID3D11Texture2D*>(m_tipsBoundsStaging), 0);

        if (minX > maxX || minY > maxY) {
            logger::warn(
                "Tips bounds: no glyph coverage found; using the full capture");
            return false;
        }

        // maxX/maxY are inclusive block indices, so +1 converts them to an
        // exclusive edge; the extra block on each side covers glyph coverage
        // that fell below the threshold in a partially lit edge block.
        const float inv = 1.0f / static_cast<float>(kTipsBoundsRes);
        m_tipsContentU0 = std::max(0.0f, (static_cast<float>(minX) - 1.0f) * inv);
        m_tipsContentV0 = std::max(0.0f, (static_cast<float>(minY) - 1.0f) * inv);
        m_tipsContentU1 = std::min(1.0f, (static_cast<float>(maxX) + 2.0f) * inv);
        m_tipsContentV1 = std::min(1.0f, (static_cast<float>(maxY) + 2.0f) * inv);
        // A block far narrower than the surface is not a line of tip text; it is
        // a partial fade-in frame that happened to clear the 16-sample publish
        // gate. Magnifying that to fill the background would bake a smear in for
        // the whole load, so fall back to fitting the capture as a whole.
        if (m_tipsContentU1 - m_tipsContentU0 < kTipsMinContentWidth ||
            m_tipsContentV1 - m_tipsContentV0 < 0.02f) {
            logger::warn(
                "Tips bounds: implausible glyph extent "
                "u=[{:.3f},{:.3f}] v=[{:.3f},{:.3f}]; using the full capture",
                m_tipsContentU0, m_tipsContentU1,
                m_tipsContentV0, m_tipsContentV1);
            return false;
        }
        m_tipsContentBoundsValid = true;
        logger::info(
            "Tips bounds: glyph extent u=[{:.3f},{:.3f}] v=[{:.3f},{:.3f}] "
            "({:.0f}x{:.0f} px of {}x{})",
            m_tipsContentU0, m_tipsContentU1,
            m_tipsContentV0, m_tipsContentV1,
            (m_tipsContentU1 - m_tipsContentU0) * m_tipsScaleformWidth,
            (m_tipsContentV1 - m_tipsContentV0) * m_tipsScaleformHeight,
            m_tipsScaleformWidth, m_tipsScaleformHeight);
        return true;
    }

    void* D3D11Compositor::CompositeTipsIntoBg()
    {
        std::lock_guard resourceLock(m_renderResourceMutex);
        if (!m_device || !m_context || !m_psBlit ||
            !m_psBackground || !m_vsFullscreen)
            return nullptr;
        // The allocation survives between loads. Only composite it after this
        // load's owner-scoped Scaleform capture has completed; otherwise an eye
        // fallback could accidentally bake stale tips from the previous load.
        if (!m_tipsCaptureComplete.load(std::memory_order_acquire) ||
            !m_tipsScaleformTex) return nullptr;
        if (m_isVR.load(std::memory_order_acquire)) {
            VRNativeLoadingSelectionSnapshot currentSelection{};
            const bool stillOwned = m_loadingMenuCaptureVRProof &&
                m_inLoadingScreen.load(std::memory_order_acquire) &&
                m_flatMode.load(std::memory_order_acquire) == 3 &&
                TryGetVRNativeLoadingSelection(currentSelection) &&
                currentSelection.content ==
                    Policy::NativeLoadingContent::kTipAndLevel &&
                currentSelection.owner == m_loadingMenuCaptureVROwner &&
                currentSelection.serial == m_loadingMenuCaptureVRSerial &&
                currentSelection.epoch == m_loadingMenuCaptureVREpoch;
            if (!stillOwned) {
                logger::info(
                    "VR tips: captured delta invalidated before composite "
                    "(serial={}, epoch={}, owner={:#x})",
                    m_loadingMenuCaptureVRSerial,
                    m_loadingMenuCaptureVREpoch,
                    reinterpret_cast<std::uintptr_t>(
                        m_loadingMenuCaptureVROwner));
                m_tipsCaptureComplete.store(
                    false, std::memory_order_release);
                CancelLoadingMenuDeltaCapture();
                return nullptr;
            }
        }

        unsigned int bgWidth = 0;
        unsigned int bgHeight = 0;
        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(
            AcquireBackgroundSRV(bgWidth, bgHeight));
        ComReleaseGuard bgGuard{ bgSRV };
        if (!bgSRV || bgWidth == 0 || bgHeight == 0) return nullptr;

        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* ctx    = static_cast<ID3D11DeviceContext*>(m_context);

        // True widescreen output at a fixed 2048x1152 (exact 16:9). This used to
        // follow the captured tips width, which on VR is the 1024-wide UI
        // surface, so the composite silently downsampled the 2048-wide landscape
        // art to 1024 and the background lost half its detail whenever tips were
        // present. The output resolution belongs to the background, not to the
        // surface Fallout happened to draw its UI on.
        constexpr unsigned int outputWidth = 2048;
        constexpr unsigned int outputHeight = 1152;

        bool needRecreate = !m_bgPlusTipsTex
                         || m_bgPlusTipsWidth  != outputWidth
                         || m_bgPlusTipsHeight != outputHeight;
        if (needRecreate) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width  = outputWidth;
            desc.Height = outputHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* t = nullptr;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &t);
            if (FAILED(hr) || !t) {
                logger::warn("CompositeTipsIntoBg: CreateTexture2D failed (hr={:x})", (unsigned)hr);
                return nullptr;
            }

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = desc.Format;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            hr = device->CreateRenderTargetView(t, &rtvDesc, &rtv);
            if (FAILED(hr) || !rtv) {
                t->Release();
                logger::warn("CompositeTipsIntoBg: CreateRTV failed (hr={:x})", (unsigned)hr);
                return nullptr;
            }

            if (m_bgPlusTipsRTV)
                static_cast<ID3D11RenderTargetView*>(m_bgPlusTipsRTV)->Release();
            if (m_bgPlusTipsTex)
                static_cast<ID3D11Texture2D*>(m_bgPlusTipsTex)->Release();
            m_bgPlusTipsTex = t;
            m_bgPlusTipsRTV = rtv;
            m_bgPlusTipsWidth  = outputWidth;
            m_bgPlusTipsHeight = outputHeight;
            logger::info("CompositeTipsIntoBg: created {}x{} 16:9 bg+tips RT",
                outputWidth, outputHeight);
        }

        // (Re)create SRV on m_tipsScaleformTex.
        auto* tipsTex = static_cast<ID3D11Texture2D*>(m_tipsScaleformTex);
        D3D11_TEXTURE2D_DESC tipsDesc{};
        tipsTex->GetDesc(&tipsDesc);
        {
            DXGI_FORMAT srvFmt = tipsDesc.Format;
            if (srvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (srvFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            ID3D11ShaderResourceView* s = nullptr;
            const HRESULT hr = device->CreateShaderResourceView(tipsTex, &sd, &s);
            if (FAILED(hr) || !s) {
                logger::warn("CompositeTipsIntoBg: tips SRV creation failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return nullptr;
            }
            if (m_tipsSrcSRV)
                static_cast<ID3D11ShaderResourceView*>(m_tipsSrcSRV)->Release();
            m_tipsSrcSRV = s;
        }

        // Measure where the glyphs actually are before placing them. Failure is
        // non-fatal: the legacy whole-surface fit below is still correct, just
        // smaller.
        ComputeTipsContentBounds();

        // Cover-fit UV scale for the source bg into the 16:9 output (no stretch):
        // sample a centered sub-rectangle of whichever source dimension is larger.
        CompositeParams params{};
        float bgAspect = static_cast<float>(bgWidth) / static_cast<float>(bgHeight);
        const float outputAspect =
            static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
        if (bgAspect >= outputAspect) {
            params.bgUvScaleX = outputAspect / bgAspect;
            params.bgUvScaleY = 1.0f;
        } else {
            params.bgUvScaleX = 1.0f;
            params.bgUvScaleY = bgAspect / outputAspect;
        }
        auto* cb = static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = cb
            ? ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
            : E_POINTER;
        if (FAILED(mapHr)) {
            logger::warn("CompositeTipsIntoBg: constant-buffer Map failed (hr={:x})",
                static_cast<unsigned>(mapHr));
            return nullptr;
        }
        std::memcpy(mapped.pData, &params, sizeof(params));
        ctx->Unmap(cb, 0);

        ScopedPipelineState savedState(ctx);

        auto* rtv = static_cast<ID3D11RenderTargetView*>(m_bgPlusTipsRTV);
        D3D11_VIEWPORT vp = {};
        vp.Width  = static_cast<float>(outputWidth);
        vp.Height = static_cast<float>(outputHeight);
        vp.MaxDepth = 1.0f;
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));
        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        // Pass 1: bg, opaque, cover-fit aspect via m_psBackground (reads cb b0).
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->PSSetShaderResources(0, 1, &bgSRV);
        ctx->Draw(3, 0);

        // Pass 2: tips, alpha-blended on top. The exact LoadingMenu delta pass
        // already produced authoritative alpha. Preserve it with a plain blit;
        // re-keying luminance here would turn bright but unchanged framebuffer
        // pixels (including controllers) opaque again.
        //
        // Blend: the delta texture is PREMULTIPLIED (Scaleform's native
        // output, passed through unmodified for the native glyph look), so
        // color blends One/InvSrcAlpha. Dest ALPHA stays preserved
        // (SrcBlendAlpha=ZERO, DestBlendAlpha=ONE): the generic alpha blend
        // wrote destA = srcA, zeroing the composite's alpha outside the text;
        // SteamVR honors texture alpha, so the bg displayed as transparent
        // black when the composite alpha is not preserved.
        if (!m_bgPlusTipsBlend) {
            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11BlendState* bs = nullptr;
            const HRESULT hr = device->CreateBlendState(&bd, &bs);
            if (SUCCEEDED(hr) && bs) {
                m_bgPlusTipsBlend = bs;
            } else {
                logger::warn("CompositeTipsIntoBg: alpha-preserving blend creation failed (hr={:x})",
                    static_cast<unsigned>(hr));
                return nullptr;
            }
        }
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBlit), nullptr, 0);
        ctx->OMSetBlendState(
            static_cast<ID3D11BlendState*>(m_bgPlusTipsBlend),
            nullptr, 0xFFFFFFFF);
        auto* tipsSrv = static_cast<ID3D11ShaderResourceView*>(m_tipsSrcSRV);
        ctx->PSSetShaderResources(0, 1, &tipsSrv);
        // Draw the capture at its plain contain-fit scale — the same on-screen
        // text size as the original fit — and use the measured glyph bounds
        // ONLY to slide the block toward the lower left. Scaling the content
        // block up to a share of the output width was tried and rejected: in
        // the headset the enlarged text read as far too big. The fit scale
        // also keeps the glyphs near 1:1 with the captured surface (1152 px
        // draw from a 1024 px source), which is the sharpness ceiling; the
        // 2.0.7 softness came from the 1024x576 composite, not the fit.
        const float tipsAspect =
            static_cast<float>(m_tipsScaleformWidth) /
            static_cast<float>(std::max(1u, m_tipsScaleformHeight));
        D3D11_VIEWPORT tipsVp = {};
        if (tipsAspect >= outputAspect) {
            tipsVp.Width = static_cast<float>(outputWidth);
            tipsVp.Height =
                static_cast<float>(outputWidth) / tipsAspect;
        } else {
            tipsVp.Height = static_cast<float>(outputHeight);
            tipsVp.Width =
                static_cast<float>(outputHeight) * tipsAspect;
        }
        // Widen horizontally only. Height is deliberately untouched: the size
        // itself was already right, this is the small extra width asked for.
        tipsVp.Width *= kTipsWidthScale;
        // Default: centred, exactly the pre-measurement behaviour.
        tipsVp.TopLeftX =
            (static_cast<float>(outputWidth) - tipsVp.Width) * 0.5f;
        tipsVp.TopLeftY =
            (static_cast<float>(outputHeight) - tipsVp.Height) * 0.5f;
        if (m_tipsContentBoundsValid) {
            // Anchor the measured content block's left edge at the left margin
            // and its bottom edge at the bottom line, like the native flat
            // layout. Offsets are bounded by margin + fit size (< 2.2k), so the
            // viewport always stays far inside D3D11's legal range.
            tipsVp.TopLeftX =
                static_cast<float>(outputWidth) *
                    kTipsAnchorLeftMarginFraction -
                tipsVp.Width * m_tipsContentU0;
            tipsVp.TopLeftY =
                static_cast<float>(outputHeight) *
                    kTipsAnchorBottomFraction -
                tipsVp.Height * m_tipsContentV1;
        }
        tipsVp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &tipsVp);
        ctx->Draw(3, 0);

        return m_bgPlusTipsTex;
    }

    void D3D11Compositor::SetEnabled(bool enabled)
    {
        if (enabled && !m_renderReady.load(std::memory_order_acquire)) {
            logger::warn("D3D11Compositor: enable ignored until render resources are ready");
            return;
        }
        if (enabled &&
            m_advanceMovieKilled.load(std::memory_order_acquire) &&
            !TryRestoreAdvanceMovie()) {
            // Never carry an unverified RET ownership epoch into a new load.
            // Present keeps retrying the restoration while compositing remains
            // fail-open/off.
            logger::warn(
                "D3D11Compositor: enable deferred until AdvanceMovie RET "
                "restoration can be verified");
            return;
        }
        if (enabled) {
            m_clearMatchCount.store(0, std::memory_order_relaxed);
            m_submitCompositeCount.store(0, std::memory_order_relaxed);
            m_flatPresentCount.store(0, std::memory_order_relaxed);
            // Preserve the native content choice here: SendLoadingText can
            // return immediately before MenuOpen. No draw is accepted while
            // disabled, so only the per-open draw/present proof is reset.
            m_flatTipsDrawSerial.store(0, std::memory_order_release);
            m_flatTipsPresentedSerial.store(0, std::memory_order_release);
            m_flatTipsLoadEpoch.fetch_add(1, std::memory_order_acq_rel);
            m_flatLimiterDeadlineUs.store(0, std::memory_order_relaxed);
            m_flatLimiterWaitCount.store(0, std::memory_order_relaxed);
            m_flatLimiterWaitUs.store(0, std::memory_order_relaxed);
            m_flatSyncSampled.store(false, std::memory_order_relaxed);
            m_flatIncomingSyncInterval.store(0, std::memory_order_relaxed);
            m_flatOutgoingSyncInterval.store(0, std::memory_order_relaxed);
            m_skipPresent = false;
            const int flatMode = m_flatMode.load(std::memory_order_acquire);
            m_flatBackgroundOnly.store(flatMode == 2, std::memory_order_release);
            if (!m_isVR.load(std::memory_order_acquire) &&
                flatMode == 3 &&
                !m_flatTipReadinessHookInstalled.load(
                    std::memory_order_acquire)) {
                logger::warn(
                    "Flat tips: mode 3 running without exact native selection; "
                    "AdvanceMovie will remain unmodified");
            }

            // Luminance-key threshold (only used in modes that run the lum-key
            // shader). Mode 3 uses a tighter cutoff than the 0.25 default so
            // game bg art doesn't bleed through, but Scaleform tip text is
            // anti-aliased and tends to land in the 0.4-0.6 range on the
            // backbuffer — 0.65+ was filtering the tips themselves out.
            m_luminanceThreshold.store(
                flatMode == 3 ? 0.40f : 0.25f, std::memory_order_release);

            // NG: track load start time for accurate duration measurement
            if (REL::Module::IsNG()) {
                m_ngLoadStartTime = std::chrono::steady_clock::now();
                m_ngLoadNumber++;
            }
            m_enabled.store(true, std::memory_order_release);
        } else {
            // Stop new render-hook work before restoring patched game code.
            {
                std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
                m_enabled.store(false, std::memory_order_release);
                m_inLoadingScreen.store(false, std::memory_order_release);
                m_flatBackgroundOnly.store(false, std::memory_order_release);
                m_flatNativeLoadingContent.store(
                    static_cast<std::uint8_t>(
                        Policy::FlatNativeLoadingContent::kUnknown),
                    std::memory_order_release);
                m_flatNativeModelSelection.store(
                    static_cast<std::uint8_t>(
                        Policy::NativeModelSelection::kUnknown),
                    std::memory_order_release);
                m_flatNativeInitialSelectionSerial.store(
                    0, std::memory_order_release);
                m_flatTipsDrawSerial.store(0, std::memory_order_release);
                m_flatTipsPresentedSerial.store(0, std::memory_order_release);
                m_flatTipsLoadEpoch.fetch_add(1, std::memory_order_acq_rel);
                if (!m_isVR.load(std::memory_order_acquire)) {
                    // CLOSE is the authoritative per-load invalidation boundary.
                    // The next SendLoadingText may publish before the next OPEN.
                    m_loadingTextReady.store(false, std::memory_order_release);
                    m_loadingTextOwner.store(nullptr, std::memory_order_release);
                }
                TryRestoreAdvanceMovieLocked();
            }

            if (m_mode.load(std::memory_order_acquire) == CompositeMode::ClearIntercept) {
                logger::info("D3D11Compositor: ClearRTV matched {} times this load",
                    m_clearMatchCount.load(std::memory_order_relaxed));
            } else {
                logger::info("D3D11Compositor: composited {}/{} Present calls",
                    m_submitCompositeCount.load(std::memory_order_relaxed),
                    m_flatPresentCount.load(std::memory_order_relaxed));
                if (m_flatSyncSampled.load(std::memory_order_relaxed)) {
                    logger::info(
                        "Flat loading Present policy: syncInterval {} -> {}, "
                        "350-FPS waits={} totalWait={}us",
                        m_flatIncomingSyncInterval.load(std::memory_order_relaxed),
                        m_flatOutgoingSyncInterval.load(std::memory_order_relaxed),
                        m_flatLimiterWaitCount.load(std::memory_order_relaxed),
                        m_flatLimiterWaitUs.load(std::memory_order_relaxed));
                }
            }
        }
        logger::info("D3D11Compositor: {}", enabled ? "enabled" : "disabled");
    }

    void D3D11Compositor::KillAdvanceMovie()
    {
        if (m_advanceMovieKilled.load(std::memory_order_acquire) ||
            m_isVR.load(std::memory_order_acquire)) return;
        if (!ResolveAdvanceMovie()) return;

        std::lock_guard<std::mutex> lock(m_advanceMovieMutex);
        if (m_enabled.load(std::memory_order_acquire) &&
            !m_advanceMovieKilled.load(std::memory_order_relaxed) && m_advanceMovieAddr) {
            static constexpr std::uint8_t RET = 0xC3;
            std::uint8_t current = 0;
            if (TryReadExecutableByte_SEH(m_advanceMovieAddr, &current) &&
                current == m_advanceMovieOrigByte &&
                ExecutableBytesEqual(
                    m_advanceMovieAddr, m_advanceMovieOrigBytes.data(),
                    m_advanceMovieOrigBytes.size())) {
                REL::safe_write(m_advanceMovieAddr, &RET, 1);
                m_advanceMovieKilled.store(true, std::memory_order_release);
                m_advanceMovieRestoreReadFailureLogged.store(
                    false, std::memory_order_release);
                logger::info(
                    "Flat: AdvanceMovie killed early at {:x}",
                    m_advanceMovieAddr);
            } else {
                logger::warn(
                    "Flat: AdvanceMovie kill skipped: ownership changed "
                    "(current=0x{:02x}, expected=0x{:02x})",
                    current, m_advanceMovieOrigByte);
            }
        }
    }

    // ========================================================================
    // Temp texture management
    // ========================================================================

    bool D3D11Compositor::EnsureTempTexture(unsigned int width, unsigned int height, unsigned int format)
    {
        if (m_tempTexture && m_tempSRV && m_tempWidth == width &&
            m_tempHeight == height && m_tempFormat == format) return true;

        auto* device = static_cast<ID3D11Device*>(m_device);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = static_cast<DXGI_FORMAT>(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
        if (FAILED(hr) || !tex) {
            logger::warn("D3D11Compositor: temp texture creation failed ({}x{} fmt={}, hr={:x})",
                width, height, format, static_cast<unsigned>(hr));
            return false;
        }
        // SRVs can't use TYPELESS formats — remap to matching UNORM.
        DXGI_FORMAT srvFormat = desc.Format;
        if (srvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (srvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (srvFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS) srvFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = srvFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        if (FAILED(hr) || !srv) {
            tex->Release();
            logger::warn("D3D11Compositor: temp SRV creation failed (hr={:x})",
                static_cast<unsigned>(hr));
            return false;
        }

        if (m_tempSRV) static_cast<ID3D11ShaderResourceView*>(m_tempSRV)->Release();
        if (m_tempTexture) static_cast<ID3D11Texture2D*>(m_tempTexture)->Release();
        m_tempTexture = tex;
        m_tempSRV = srv;
        m_tempWidth = width;
        m_tempHeight = height;
        m_tempFormat = format;

        logger::info("D3D11Compositor: temp texture created ({}x{} fmt={})", width, height, format);
        return true;
    }

#if 0
    // Removed runtime path retained temporarily for source-history comparison.
    // Submitted-eye extraction is deliberately not declared, compiled, or
    // callable; mode 3 accepts only the exact LoadingMenu before/after delta.
    bool D3D11Compositor::EnsureTipsExtractTextures(unsigned int width, unsigned int height, unsigned int format)
    {
        // width == per-eye width. Output is side-by-side (2x wide) so each eye
        // gets its own half; IVROverlay with SideBySide_Parallel flag samples
        // left half for left eye, right half for right eye. This eliminates
        // stereo ghosting that a single monoscopic extract would cause.
        if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2 ||
            height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return false;
        }
        if (m_tipsExtractTexture && m_tipsExtractRTV && m_tipsExtractCopy &&
            m_tipsExtractSRV && m_tipsExtractWidth == width * 2 &&
            m_tipsExtractHeight == height && m_tipsExtractFormat == format) {
            return true;
        }

        // Release old
        if (m_tipsExtractRTV) { static_cast<ID3D11RenderTargetView*>(m_tipsExtractRTV)->Release(); m_tipsExtractRTV = nullptr; }
        if (m_tipsExtractTexture) { static_cast<ID3D11Texture2D*>(m_tipsExtractTexture)->Release(); m_tipsExtractTexture = nullptr; }
        if (m_tipsExtractSRV) { static_cast<ID3D11ShaderResourceView*>(m_tipsExtractSRV)->Release(); m_tipsExtractSRV = nullptr; }
        if (m_tipsExtractCopy) { static_cast<ID3D11Texture2D*>(m_tipsExtractCopy)->Release(); m_tipsExtractCopy = nullptr; }

        auto* device = static_cast<ID3D11Device*>(m_device);

        // Output: RGBA8 with alpha from luminance. 2x wide for side-by-side stereo.
        // Needs RENDER_TARGET (we draw into it) and SHADER_RESOURCE (IVROverlay samples it).
        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = width * 2;
        outDesc.Height = height;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* outTex = nullptr;
        HRESULT hr = device->CreateTexture2D(&outDesc, nullptr, &outTex);
        if (FAILED(hr) || !outTex) {
            logger::warn("Tips extract: out tex create failed (hr={:x})", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        hr = device->CreateRenderTargetView(outTex, &rtvDesc, &rtv);
        if (FAILED(hr) || !rtv) {
            outTex->Release();
            logger::warn("Tips extract: RTV create failed (hr={:x})", static_cast<unsigned>(hr));
            return false;
        }

        // Copy texture: matches eye-tex format so CopyResource works. SRV reads it.
        D3D11_TEXTURE2D_DESC cpyDesc = {};
        cpyDesc.Width = width;
        cpyDesc.Height = height;
        cpyDesc.MipLevels = 1;
        cpyDesc.ArraySize = 1;
        cpyDesc.Format = static_cast<DXGI_FORMAT>(format);
        cpyDesc.SampleDesc.Count = 1;
        cpyDesc.Usage = D3D11_USAGE_DEFAULT;
        cpyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* cpyTex = nullptr;
        hr = device->CreateTexture2D(&cpyDesc, nullptr, &cpyTex);
        if (FAILED(hr) || !cpyTex) {
            rtv->Release();
            outTex->Release();
            logger::warn("Tips extract: copy tex create failed (hr={:x})", static_cast<unsigned>(hr));
            return false;
        }

        DXGI_FORMAT srvFmt = cpyDesc.Format;
        if (srvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (srvFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (srvFmt == DXGI_FORMAT_R10G10B10A2_TYPELESS) srvFmt = DXGI_FORMAT_R10G10B10A2_UNORM;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = srvFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView(cpyTex, &srvDesc, &srv);
        if (FAILED(hr) || !srv) {
            cpyTex->Release();
            rtv->Release();
            outTex->Release();
            logger::warn("Tips extract: copy SRV create failed (hr={:x})",
                static_cast<unsigned>(hr));
            return false;
        }

        m_tipsExtractTexture = outTex;
        m_tipsExtractRTV = rtv;
        m_tipsExtractCopy = cpyTex;
        m_tipsExtractSRV = srv;
        m_tipsExtractWidth = width * 2;  // side-by-side
        m_tipsExtractHeight = height;
        m_tipsEyeWidth = width;          // per-eye
        m_tipsExtractFormat = format;

        logger::info("Tips extract: textures created (output {}x{}, per-eye copy {}x{})",
            width * 2, height, width, height);
        return true;
    }

    void D3D11Compositor::ExtractTipsFromEye(void* eyeTexture2D, int eye)
    {
        if (!m_context || !m_device || !eyeTexture2D || !m_psTipsExtract) return;

        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* eyeTex = static_cast<ID3D11Texture2D*>(eyeTexture2D);

        D3D11_TEXTURE2D_DESC eyeDesc;
        eyeTex->GetDesc(&eyeDesc);

        if (!EnsureTipsExtractTextures(eyeDesc.Width, eyeDesc.Height, eyeDesc.Format)) return;

        // Copy eye texture to our copy so we can sample it (can't sample the
        // source directly — may be bound as RTV by the game / needs SRV binding)
        ctx->CopyResource(static_cast<ID3D11Texture2D*>(m_tipsExtractCopy), eyeTex);

        ScopedPipelineState savedState(ctx);

        // Draw only to this eye's half (left=0..eyeW, right=eyeW..2*eyeW)
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = (eye == 0) ? 0.0f : static_cast<float>(m_tipsEyeWidth);
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(m_tipsEyeWidth);
        vp.Height = static_cast<float>(m_tipsExtractHeight);
        vp.MaxDepth = 1.0f;

        auto* rtv = static_cast<ID3D11RenderTargetView*>(m_tipsExtractRTV);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psTipsExtract), nullptr, 0);

        // Constant buffer: threshold + padding
        // Moderate threshold — tips text is alpha-blended in the game's UI so
        // even "white" text reaches the eye texture around luminance 0.4-0.7.
        // Lower threshold catches faint text at the cost of letting some
        // VaultBoy animation pixels through (mitigated by the narrow ramp).
        CompositeParams params{};
        params.threshold = 0.35f;
        auto* cb = static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = cb
            ? ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
            : E_POINTER;
        if (FAILED(mapHr)) return;
        std::memcpy(mapped.pData, &params, sizeof(params));
        ctx->Unmap(cb, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        auto* srv = static_cast<ID3D11ShaderResourceView*>(m_tipsExtractSRV);
        ctx->PSSetShaderResources(0, 1, &srv);

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);
        m_tipsExtractContentFresh.store(true, std::memory_order_release);

        if (m_tipsExtractLogCount < 6) {
            m_tipsExtractLogCount++;
            logger::info("Tips extract: frame {} eye {} rendered (half {}x{})",
                m_tipsExtractLogCount, eye, m_tipsEyeWidth, m_tipsExtractHeight);
        }
    }
#endif

    // ========================================================================
    // Submit hook — intercepts VR eye texture submission (called once per eye)
    // Signature: int Submit(void* this, EVREye eye, const Texture_t* tex,
    //                       const VRTextureBounds_t* bounds, EVRSubmitFlags flags)
    // ========================================================================

    int __cdecl D3D11Compositor::HookedSubmit(void* compositor, int eye,
        const void* texture, const void* bounds, int flags)
    {
        auto* self = s_instance;

        if (self && texture) {
            std::unique_lock resourceLock(self->m_renderResourceMutex);
            auto* vrTex = static_cast<const VRTexture_t*>(texture);

            // A VR runtime/device reset can replace Fallout's eye textures
            // without re-running plugin initialization. Recover from the
            // authoritative submitted DirectX texture before touching any
            // cached device-dependent resource.
            if (vrTex->eType == 0 && vrTex->handle) {
                ID3D11Device* submittedDevice = nullptr;
                static_cast<ID3D11Texture2D*>(vrTex->handle)->GetDevice(
                    &submittedDevice);
                if (submittedDevice && submittedDevice != self->m_device) {
                    // HandleDeviceLoss notifies LoadingScreenManager after its
                    // own resource teardown. Never retain resource->state lock
                    // order here: OPEN/Update legitimately use state->resource
                    // for capture lifecycle changes.
                    resourceLock.unlock();
                    self->HandleDeviceLoss(
                        "VR eye texture belongs to a replacement device");
                    self->RebuildDeviceResources(submittedDevice);
                    resourceLock.lock();
                }
                ReleaseCom(submittedDevice);
            }

            // Track eye textures (needed for ClearRTV mode)
            if (eye == 0)
                self->m_lastLeftEye = vrTex->handle;
            else
                self->m_lastRightEye = vrTex->handle;

            // Scrub the submitted eye texture while the custom loading stack
            // (or its post-close cover) owns presentation. Fallout's renderer
            // never clears the eye buffers — the world pass normally overwrites
            // them — so with the 3D loading model and the LoadingMenu UI both
            // suppressed, they keep whatever was drawn BEFORE the load: the
            // main menu, for a title-screen save load. At load end the engine
            // re-submits those stale pixels for ~0.5 s before the world pass
            // resumes, which presented as a title-screen flash the moment the
            // black cover released (log-proven: cover released on 3 fresh
            // pairs, flash still visible). The clear is invisible behind the
            // blocker and cannot disturb the tips capture, which reads the
            // separate Scaleform UI surface mid-frame, before Submit.
            // Read what Fallout submitted BEFORE the scrub blacks it. Requested
            // only at the two release boundaries, so this costs one small
            // readback each at the end of a load and nothing during it. Order
            // matters: sampling after the clear would only ever report black.
            // BOTH eyes. Sampling only the right eye made this instrument
            // structurally incapable of observing the reported symptom: a
            // "broken stereo" image means the two eyes DISAGREE, and a
            // single-eye probe can never see that. The request is consumed on
            // the right eye (which completes a pair) so the burst still counts
            // frames, while the left eye samples opportunistically against
            // whatever tag is currently pending.
            if (self && self->m_device && self->m_context) {
                const int pendingTag = eye == 1
                    ? VRCompositorHelper::ConsumeEyeSampleRequest()
                    : VRCompositorHelper::PeekEyeSampleRequest();
                if (pendingTag != 0) {
                    self->SampleSubmittedEye(vrTex->handle, pendingTag, eye);
                }
            }

            // March never mutated Fallout's submitted eye textures. The native
            // LoadingMenu/world renderer is now allowed to advance underneath
            // the opaque overlay stack and overwrite both eyes normally.

            // Mode-3 tips are captured synchronously in HookedDisplayMenu while
            // LoadingMenu owns the Scaleform target. Never sample submitted eye
            // textures here: they can contain controllers/hands and are not an
            // ownership-safe fallback.

            if (self->m_enabled.load(std::memory_order_acquire) && self->HasBackgroundTexture() &&
                !self->m_deferredNOPApplied.load() &&
                self->m_mode.load(std::memory_order_acquire) == CompositeMode::LuminanceKey)
            {
                const int compositeCount = self->m_submitCompositeCount.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                if (compositeCount <= 4) {
                    logger::info("Submit: compositing eye {} (count={})",
                        eye, compositeCount);
                }
                self->CompositeFrame(vrTex->handle, eye);
            }
        }

        if (!s_originalSubmit) {
            logger::critical("D3D11Compositor: Submit hook has no original function");
            return 0;
        }
        int result = s_originalSubmit(compositor, eye, texture, bounds, flags);
        const bool validSubmittedTexture =
            texture &&
            static_cast<const VRTexture_t*>(texture)->handle != nullptr;
        const bool acceptedSubmit = result == 0 && validSubmittedTexture;
        if (self && acceptedSubmit) {
            // Session telemetry for a real accepted game submission. The
            // bounded fade-latch notification above owns its separate epoch.
            self->m_eyeSubmitTotal.fetch_add(1, std::memory_order_relaxed);
        }

        // Per-frame VR callback (after right eye Submit)
        if (self && eye == 1) {
            if (auto callback = self->m_frameCallback.load(std::memory_order_acquire)) {
                callback();
            }
        }

        // March v1.0 speed path: after the next right-eye Submit returns, apply
        // the NOP. OpenVR may report AlreadySubmitted even though Fallout has
        // completed the frame handoff; making result==0 or a separately tracked
        // left eye prerequisites was the regression that made the speed patch
        // arrive late or never. Current tip capture is best-effort and cannot
        // delay this boundary.
        if (self && eye == 1 &&
            self->m_deferredNOPPending.load(std::memory_order_acquire)) {
            // This mutex owns the complete freeze lifecycle transition. Close's
            // ResetDeferredState takes the same lock, so it can never reset
            // between ownership validation and patch publication.
            std::lock_guard<std::mutex> lock(self->m_deferredNOPMutex);
            if (self->m_deferredNOPPending.load(std::memory_order_relaxed)) {
                const auto patchAddress = self->m_deferredNOPAddress;
                const auto patchSize = self->m_deferredNOPSize;
                const auto armSerial = self->m_deferredNOPArmSerial;
                const auto loadGeneration = self->m_deferredNOPLoadGeneration;

                if (!self->m_inLoadingScreen.load(std::memory_order_acquire)) {
                    self->m_deferredNOPPending.store(
                        false, std::memory_order_release);
                    logger::info(
                        "Deferred NOP cancelled after LoadingMenu CLOSE "
                        "(arm={}, loadGeneration={})",
                        armSerial, loadGeneration);
                } else if (!patchAddress || patchSize == 0 ||
                    patchSize > sizeof(self->m_deferredNOPBytes) ||
                    !self->m_deferredNOPOriginalValid) {
                    self->m_deferredNOPPending.store(false, std::memory_order_release);
                    logger::error("Deferred NOP rejected invalid payload (address={:x}, size={})",
                        patchAddress, patchSize);
                } else if (!ExecutableBytesEqual(
                               patchAddress,
                               self->m_deferredNOPOriginalBytes,
                               patchSize)) {
                    // Another plugin or the engine changed the site after the
                    // request was armed. Fail closed; never overwrite a foreign
                    // hook/prologue and never claim ownership for restoration.
                    self->m_deferredNOPPending.store(
                        false, std::memory_order_release);
                    self->m_deferredNOPOriginalValid = false;
                    logger::warn(
                        "Deferred NOP skipped at {:x}: exact expected bytes "
                        "no longer own the site",
                        patchAddress);
                } else {
                    // The v1.0 speed path applied immediately after the real
                    // right-eye Submit. SteamVR retains that completed frame.
                    // Do not issue duplicate Submit calls without owning a new
                    // WaitGetPoses frame; OpenVR rejects those as
                    // AlreadySubmitted and cross-thread frame ownership races
                    // Bethesda's compositor loop.

                    // March made the compositor SCENE layer opaque BEFORE it
                    // froze Fallout's loop (924a5f79 D3D11Compositor.cpp).
                    // Preserve that order so there is no instruction-sized
                    // interval in which a stale or stereo-mismatched game eye
                    // can sit underneath an already-frozen producer. Overlays
                    // composite above scene black, so loading art is unchanged.
                    VRCompositorHelper::ApplySceneFade();
                    REL::safe_write(
                        patchAddress, self->m_deferredNOPBytes, patchSize);
                    self->m_deferredNOPPending.store(
                        false, std::memory_order_release);
                    self->m_deferredNOPApplied.store(
                        true, std::memory_order_release);
                    logger::info(
                        "Deferred NOP applied at {:x} after right-eye Submit "
                        "(OpenVR result={}, validTexture={}, arm={}, "
                        "loadGeneration={}); March timing owns presentation",
                        patchAddress, result, validSubmittedTexture,
                        armSerial, loadGeneration);

                    // The blocker/background overlay owns custom presentation.
                    // Do not replace or swallow Bethesda's FadeToColor/FadeGrid
                    // calls: their values and timing remain the game defaults.
                }
            }
        }

        return result;
    }

    void D3D11Compositor::RequestDeferredNOP(
        std::uintptr_t address, const std::uint8_t* expectedBytes,
        const std::uint8_t* patchBytes, std::size_t size,
        std::uint64_t loadGeneration)
    {
        if (!m_renderReady.load(std::memory_order_acquire)) {
            logger::warn("D3D11Compositor: deferred NOP ignored because render resources are unavailable");
            return;
        }
        if (!address || !expectedBytes || !patchBytes || size == 0 ||
            loadGeneration == 0 ||
            size > sizeof(m_deferredNOPBytes)) {
            logger::error(
                "D3D11Compositor: rejected invalid deferred patch "
                "(address={:x}, size={}, loadGeneration={})",
                address, size, loadGeneration);
            return;
        }
        if (!ExecutableBytesEqual(address, expectedBytes, size)) {
            logger::warn(
                "D3D11Compositor: deferred patch refused at {:x}; "
                "exact expected bytes do not own the site",
                address);
            return;
        }

        std::lock_guard<std::mutex> lock(m_deferredNOPMutex);
        if (m_deferredNOPPending.load(std::memory_order_acquire) ||
            m_deferredNOPApplied.load(std::memory_order_acquire)) {
            logger::warn("D3D11Compositor: deferred patch already pending/applied; duplicate ignored");
            return;
        }
        m_deferredNOPAddress = address;
        std::memcpy(m_deferredNOPBytes, patchBytes, size);
        std::memcpy(m_deferredNOPOriginalBytes, expectedBytes, size);
        m_deferredNOPSize = size;
        m_deferredNOPOriginalValid = true;
        ++m_deferredNOPArmSerial;
        if (m_deferredNOPArmSerial == 0) {
            ++m_deferredNOPArmSerial;
        }
        m_deferredNOPLoadGeneration = loadGeneration;
        m_deferredNOPApplied.store(false, std::memory_order_relaxed);
        // Release publishes the non-atomic payload to the Submit hook, whose
        // acquire load occurs before it reads the address/bytes/size.
        m_deferredNOPPending.store(true, std::memory_order_release);
        logger::info(
            "Deferred NOP requested at {:x} ({} bytes, arm={}, "
            "loadGeneration={})",
            address, size, m_deferredNOPArmSerial, loadGeneration);
    }

    void D3D11Compositor::ResetDeferredState()
    {
        m_inLoadingScreen.store(false, std::memory_order_release);
        {
            // Serialize the inverse transition with HookedSubmit's patch. Restore
            // only while the live site still equals our exact patch; a foreign
            // owner that changed it after application must never be overwritten
            // with the startup snapshot.
            std::lock_guard<std::mutex> lock(m_deferredNOPMutex);
            const bool wasApplied =
                m_deferredNOPApplied.load(std::memory_order_acquire);
            if (wasApplied && m_deferredNOPOriginalValid && m_deferredNOPAddress &&
                m_deferredNOPSize > 0 &&
                m_deferredNOPSize <= sizeof(m_deferredNOPOriginalBytes)) {
                if (ExecutableBytesEqual(
                        m_deferredNOPAddress, m_deferredNOPBytes,
                        m_deferredNOPSize)) {
                    REL::safe_write(
                        m_deferredNOPAddress, m_deferredNOPOriginalBytes,
                        m_deferredNOPSize);
                } else {
                    logger::warn(
                        "Deferred NOP restore skipped at {:x}: "
                        "patch ownership was lost",
                        m_deferredNOPAddress);
                }
            }
            m_deferredNOPPending.store(false, std::memory_order_release);
            m_deferredNOPApplied.store(false, std::memory_order_release);
            m_deferredNOPAddress = 0;
            m_deferredNOPSize = 0;
            m_deferredNOPOriginalValid = false;
            m_deferredNOPLoadGeneration = 0;

        }
        m_frozen.store(false, std::memory_order_release);
        logger::info("D3D11Compositor: deferred state reset (unfrozen)");
    }

    // ========================================================================
    // Flat luminance-key compositing (same image to both eyes)
    // ========================================================================

    void D3D11Compositor::CompositeFrame(void* eyeTexture2D, int eye)
    {
        (void)eye;
        if (!m_context || !m_device || !eyeTexture2D) return;

        auto* ctx = static_cast<ID3D11DeviceContext*>(m_context);
        auto* device = static_cast<ID3D11Device*>(m_device);
        auto* eyeTex = static_cast<ID3D11Texture2D*>(eyeTexture2D);

        unsigned int bgWidth = 0;
        unsigned int bgHeight = 0;
        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(
            AcquireBackgroundSRV(bgWidth, bgHeight));
        ComReleaseGuard bgGuard{ bgSRV };
        if (!bgSRV) return;

        // Get eye texture dimensions and flags
        D3D11_TEXTURE2D_DESC eyeDesc;
        eyeTex->GetDesc(&eyeDesc);

        ScopedPipelineState savedState(ctx);
        ID3D11RenderTargetView* temporaryRTV = nullptr;
        ID3D11RenderTargetView* rtv = savedState.FindRenderTargetView(eyeTex);
        if (!rtv) {
            savedState.CaptureHazardBindings(eyeTex);
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = eyeDesc.Format;
            rtvDesc.ViewDimension = eyeDesc.SampleDesc.Count > 1
                ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                : D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = device->CreateRenderTargetView(eyeTex, &rtvDesc, &temporaryRTV);
            if (FAILED(hr) || !temporaryRTV) return;
            rtv = temporaryRTV;
        }

        // Fullscreen blit of background image (mono — identical both eyes)
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(eyeDesc.Width);
        vp.Height = static_cast<float>(eyeDesc.Height);
        vp.MaxDepth = 1.0f;

        // Compute aspect-correct UV scales for VR eye texture
        CompositeParams params{};
        if (bgWidth > 0 && bgHeight > 0) {
            float texAspect = static_cast<float>(bgWidth) / static_cast<float>(bgHeight);
            float eyeAspect = static_cast<float>(eyeDesc.Width) / static_cast<float>(eyeDesc.Height);
            if (texAspect > eyeAspect) {
                params.bgUvScaleX = eyeAspect / texAspect;
                params.bgUvScaleY = 1.0f;
            } else {
                params.bgUvScaleX = 1.0f;
                params.bgUvScaleY = texAspect / eyeAspect;
            }
        } else {
            params.bgUvScaleX = 1.0f;
            params.bgUvScaleY = 1.0f;
        }

        // Transient pre-freeze eye composite. The persistent loading background
        // is world-locked independently by IVROverlay.
        bool useLumKey = m_psLuminanceKey != nullptr;

        // Lumkey paths need a copy of the eye texture to sample as gameTex (can't
        // sample from a texture while it's bound as RTV, so copy BEFORE bind).
        if (useLumKey) {
            if (!EnsureTempTexture(eyeDesc.Width, eyeDesc.Height, eyeDesc.Format)) {
                useLumKey = false;
            } else {
                ctx->CopyResource(static_cast<ID3D11Texture2D*>(m_tempTexture), eyeTex);
            }
        }

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));

        if (useLumKey) {
            params.threshold = 0.15f;
            ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psLuminanceKey), nullptr, 0);
        } else {
            ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);
        }
        auto* cb = static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = cb
            ? ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
            : E_POINTER;
        if (FAILED(mapHr)) {
            ReleaseCom(temporaryRTV);
            return;
        }
        std::memcpy(mapped.pData, &params, sizeof(params));
        ctx->Unmap(cb, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        if (useLumKey) {
            // Lumkey: t0 = game eye copy, t1 = background
            ID3D11ShaderResourceView* srvs[2] = {
                static_cast<ID3D11ShaderResourceView*>(m_tempSRV),
                bgSRV
            };
            ctx->PSSetShaderResources(0, 2, srvs);
        } else {
            ctx->PSSetShaderResources(0, 1, &bgSRV);
        }

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        ctx->Draw(3, 0);

        ReleaseCom(temporaryRTV);
    }

    // ========================================================================
    // ClearRTV hook — intercepts render target clears to inject background
    // ========================================================================

    void __stdcall D3D11Compositor::HookedClearRTV(void* context, void* rtv,
        const float color[4])
    {
        // Always call original clear first
        if (!s_originalClearRTV) return;
        s_originalClearRTV(context, rtv, color);

        auto* self = s_instance;
        if (!self || !self->m_enabled.load(std::memory_order_acquire) ||
            !self->HasBackgroundTexture()) return;
        if (self->m_mode.load(std::memory_order_acquire) != CompositeMode::ClearIntercept) return;

        // Check if this RTV belongs to one of the eye textures
        auto* d3dRTV = static_cast<ID3D11RenderTargetView*>(rtv);
        ID3D11Resource* resource = nullptr;
        d3dRTV->GetResource(&resource);
        if (!resource) return;

        bool isEyeTexture = (resource == self->m_lastLeftEye || resource == self->m_lastRightEye);
        resource->Release();

        if (isEyeTexture) {
            const int count = self->m_clearMatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 4) {
                logger::info("ClearRTV: matched eye texture (count={})", count);
            }
            self->DrawBackgroundOnRTV(context, rtv);
        }
    }

    void D3D11Compositor::DrawBackgroundOnRTV(void* context, void* rtv)
    {
        if (!m_vsFullscreen || !m_psBackground || !context || !rtv) return;

        unsigned int bgWidth = 0;
        unsigned int bgHeight = 0;
        auto* bgSRV = static_cast<ID3D11ShaderResourceView*>(
            AcquireBackgroundSRV(bgWidth, bgHeight));
        ComReleaseGuard bgGuard{ bgSRV };
        if (!bgSRV) return;

        auto* ctx = static_cast<ID3D11DeviceContext*>(context);
        auto* d3dRTV = static_cast<ID3D11RenderTargetView*>(rtv);

        // Get dimensions from RTV's resource
        ID3D11Resource* resource = nullptr;
        d3dRTV->GetResource(&resource);
        if (!resource) return;

        ID3D11Texture2D* tex = nullptr;
        const HRESULT qiHr = resource->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        resource->Release();
        if (FAILED(qiHr) || !tex) return;

        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        tex->Release();

        ScopedPipelineState savedState(ctx);

        // Set pipeline for background draw
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(desc.Width);
        vp.Height = static_cast<float>(desc.Height);
        vp.MaxDepth = 1.0f;

        ctx->OMSetRenderTargets(1, &d3dRTV, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);

        BindFullscreenVertexPipeline(
            ctx, static_cast<ID3D11VertexShader*>(m_vsFullscreen));
        ctx->PSSetShader(static_cast<ID3D11PixelShader*>(m_psBackground), nullptr, 0);

        // Compute aspect-correct UV scales for ClearRTV target
        CompositeParams params{};
        if (bgWidth > 0 && bgHeight > 0) {
            float texAspect = static_cast<float>(bgWidth) / static_cast<float>(bgHeight);
            float rtvAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            if (texAspect > rtvAspect) {
                params.bgUvScaleX = rtvAspect / texAspect;
                params.bgUvScaleY = 1.0f;
            } else {
                params.bgUvScaleX = 1.0f;
                params.bgUvScaleY = texAspect / rtvAspect;
            }
        } else {
            params.bgUvScaleX = 1.0f;
            params.bgUvScaleY = 1.0f;
        }

        auto* cb = static_cast<ID3D11Buffer*>(m_constantBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapHr = cb
            ? ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
            : E_POINTER;
        if (FAILED(mapHr)) return;
        std::memcpy(mapped.pData, &params, sizeof(params));
        ctx->Unmap(cb, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        ctx->PSSetShaderResources(0, 1, &bgSRV);

        auto* samp = static_cast<ID3D11SamplerState*>(m_sampler);
        ctx->PSSetSamplers(0, 1, &samp);

        ctx->OMSetBlendState(static_cast<ID3D11BlendState*>(m_blendState), nullptr, 0xFFFFFFFF);
        ctx->RSSetState(static_cast<ID3D11RasterizerState*>(m_rasterState));
        ctx->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(m_depthState), 0);

        // Draw fullscreen triangle
        ctx->Draw(3, 0);

    }
}
