#pragma once

#include <cstdint>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
}

namespace VRLoadingScreens::PreloadDiagnostics
{
    // Diagnostics are opt-in.  Install() still reports the effective native INI
    // values when disabled, but it does not place any hooks.
    void Configure(bool a_enabled) noexcept;

    // Installs read-only observer hooks on the verified preload entry points and
    // the interior-buffer add/remove functions.
    // Returns false (without patching anything) on an unsupported build, a prologue
    // mismatch, or any MinHook failure.
    [[nodiscard]] bool Install() noexcept;

    // Associates subsequent records with the plugin's load counter.
    void SetLoading(bool a_loading, std::uint64_t a_loadNo) noexcept;

    // Bracket a plugin-originated preload call so the common engine hooks can
    // distinguish it from native linked-area and other engine requests.
    void SetPluginCrosshairContext(bool a_active) noexcept;

    class PluginCrosshairScope final
    {
    public:
        PluginCrosshairScope() noexcept;
        ~PluginCrosshairScope() noexcept;

        PluginCrosshairScope(const PluginCrosshairScope&) = delete;
        PluginCrosshairScope& operator=(const PluginCrosshairScope&) = delete;
        PluginCrosshairScope(PluginCrosshairScope&&) = delete;
        PluginCrosshairScope& operator=(PluginCrosshairScope&&) = delete;
    };

    // Counts every crosshair/proximity/ray decision, including attempts which do
    // not reach an engine preload function. The first unique destination/result is
    // logged in full; bounded periodic summaries report exact additional counts so
    // a stationary door does not cause one synchronous log write every poll.
    // result is the caller's stable result code (DoorPrefetch uses PreRes).
    void NoteCrosshairCandidate(
        RE::TESObjectREFR* a_door,
        RE::TESObjectCELL* a_destination,
        const char* a_trigger,
        float a_distance,
        int a_result,
        const char* a_resultName) noexcept;

    // Configuration intent is useful to safe diagnostics that need no observer
    // hooks (for example DoorPrefetch's own gate enumeration on AE/NG).
    [[nodiscard]] bool IsConfigured() noexcept;

    // True only after the complete observer hook set was installed successfully.
    // Configuration intent alone is insufficient because an unsupported runtime
    // or signature mismatch can make installation fail.
    [[nodiscard]] bool IsInstalled() noexcept;

    // Snapshot the player's current cell without loading, attaching, or looking up
    // any destination.
    void ObservePlayerCell(const char* a_reason) noexcept;

    // Removes only this module's hooks.  It deliberately does not uninitialize the
    // process-wide MinHook library, which is shared by the other plugin modules.
    void Shutdown() noexcept;
}
