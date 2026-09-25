#pragma once

#include <cstddef>

namespace VRLoadingScreens
{
    // Read-only save-file cache warmer for confirmation dialogs.
    //
    // Fallout deliberately does not open the selected .fos until after some
    // confirmation dialogs are accepted.  This component captures the selected
    // path on the UI thread, then performs a byte/time-bounded, cancellable buffered
    // read of a canonical local .fos only while MessageBoxMenu is open. It never
    // parses a save or changes save/load state; the eventual engine load opens the
    // same file through its normal path. Real-load handoff waits only a bounded
    // interval; the owned worker retains any pending OVERLAPPED until Windows has
    // completed cancellation.
    class SaveGamePrefetch
    {
    public:
        // Exact-version MainMenu hook. Supported on OG 1.10.163 and Fallout 4
        // VR 1.2.72. NG currently lacks the required MenuOpenCloseEvent path,
        // so it fails closed rather than arming a reader that can never start.
        static bool Install() noexcept;
        static void Shutdown() noexcept;

        // Hot configuration switch. Disabling also cancels pending/active reads.
        static void SetEnabled(bool a_enabled) noexcept;

        // Menu lifecycle, forwarded from the existing game-thread MenuWatcher.
        static void OnMessageBoxMenu(bool a_opening) noexcept;
        static void OnMainMenu(bool a_opening) noexcept;
        static void OnLoadingMenu(bool a_opening) noexcept;

        // F4SE fallback for warnings that appear only after an initial failed
        // load attempt. kPreLoadGame owns the filename bytes; copy them here.
        static void OnPreLoadGame(
            const void* a_fileName, std::size_t a_length) noexcept;
        static void OnPostLoadGame(bool a_succeeded) noexcept;
    };
}
