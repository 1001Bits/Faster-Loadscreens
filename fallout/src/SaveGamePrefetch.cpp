#include "PCH.h"
#include "SaveGamePrefetch.h"

#include <MinHook.h>

#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>

namespace VRLoadingScreens
{
    namespace
    {
        using MainMenuDoLoadGameFn = void (*)(void*, std::int32_t);
        using GetFullPathFn =
            void (*)(RE::BGSSaveLoadManager*, const char*, char*, bool);

        constexpr std::uintptr_t kMainMenuDoLoadGameOG = 0x12A2280;
        constexpr std::uintptr_t kMainMenuDoLoadGameVR = 0x13213E0;
        constexpr std::uintptr_t kGetFullPathOG = 0xCECB70;
        constexpr std::uintptr_t kGetFullPathVR = 0xD36250;

        constexpr std::array<std::uint8_t, 15> kMainMenuPrologue{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9
        };
        constexpr std::array<std::uint8_t, 13> kGetFullPathPrologue{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
            0x81, 0xEC, 0x40, 0x02, 0x00, 0x00
        };

        // Combined Ghidra: BGSSaveLoadFileEntry is 0x68 bytes on OG/VR.
        // We inspect only the first pointer and never retain the engine object.
        struct SaveEntryView
        {
            const char* fileName;       // 00
            const char* playerName;     // 08
            const char* playerTitle;    // 10
            const char* location;       // 18
            const char* playTime;       // 20
            const char* raceName;       // 28
            std::uint32_t version;       // 30
            std::uint32_t saveNumber;    // 34
            std::uint32_t playerLevel;   // 38
            float levelProgress;         // 3C
            float levelThreshold;        // 40
            std::uint32_t screenshotW;   // 44
            std::uint32_t screenshotH;   // 48
            std::uint32_t screenshotOff; // 4C
            FILETIME fileTime;           // 50
            FILETIME saveTime;           // 58
            std::int32_t deviceID;        // 60
            bool loaded;                  // 64
            bool corrupt;                 // 65
            bool needsSync;               // 66
            std::byte pad67;              // 67
        };
        static_assert(sizeof(SaveEntryView) == 0x68);
        static_assert(offsetof(SaveEntryView, deviceID) == 0x60);
        static_assert(offsetof(RE::BGSSaveLoadManager, queuedEntryToLoad) == 0x48);

        constexpr std::size_t kEnginePathCapacity = 0x104;
        constexpr std::size_t kReadChunkSize = 256 * 1024;
        constexpr std::uint64_t kMaxPrefetchBytes =
            128ULL * 1024ULL * 1024ULL;
        constexpr auto kSelectionWindow = std::chrono::seconds(5);
        constexpr auto kCancelPoll = std::chrono::milliseconds(20);
        constexpr auto kMaximumWarmTime = std::chrono::milliseconds(2500);
        constexpr auto kGameThreadQuiesceWait =
            std::chrono::milliseconds(250);

        std::atomic<bool> s_enabled{ true };
        std::atomic<bool> s_installed{ false };
        std::atomic<bool> s_shuttingDown{ false };
        std::atomic<bool> s_popupOpen{ false };
        std::atomic<bool> s_mainMenuOpen{ false };
        std::atomic<bool> s_quiescing{ false };
        std::atomic<std::uint64_t> s_generation{ 0 };

        std::mutex s_stateMutex;
        std::condition_variable_any s_workCv;
        std::condition_variable s_idleCv;
        std::mutex s_quiesceMutex;
        std::optional<std::string> s_pendingPath;
        std::optional<std::string> s_attemptPath;
        std::chrono::steady_clock::time_point s_pendingSince{};
        std::uint64_t s_pendingGeneration = 0;
        bool s_workReady = false;
        bool s_workerActive = false;
        std::jthread s_worker;

        std::mutex s_hookMutex;
        std::condition_variable s_hookIdleCv;
        std::uint32_t s_activeHookCalls = 0;
        void* s_mainMenuHookTarget = nullptr;
        MainMenuDoLoadGameFn s_originalMainMenuDoLoadGame = nullptr;
        GetFullPathFn s_getFullPath = nullptr;

        [[nodiscard]] bool Matches(
            const void* a_address,
            const std::uint8_t* a_expected,
            std::size_t a_size) noexcept
        {
            if (!a_address || !a_expected || a_size == 0) {
                return false;
            }
            bool matches = false;
            __try {
                matches =
                    std::memcmp(a_address, a_expected, a_size) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matches = false;
            }
            return matches;
        }

        [[nodiscard]] bool CopyEntryFileName(
            const RE::BGSSaveLoadFileEntry* a_entry,
            char (&a_output)[kEnginePathCapacity]) noexcept
        {
            bool ok = false;
            __try {
                const auto* entry =
                    reinterpret_cast<const SaveEntryView*>(a_entry);
                if (entry && entry->fileName && entry->fileName[0] != '\0') {
                    ok = strcpy_s(
                        a_output, kEnginePathCapacity, entry->fileName) == 0;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool ResolveEnginePath(
            RE::BGSSaveLoadManager* a_manager,
            const char* a_fileName,
            char (&a_output)[kEnginePathCapacity]) noexcept
        {
            bool ok = false;
            __try {
                if (a_manager && a_fileName && a_fileName[0] != '\0' &&
                    s_getFullPath) {
                    s_getFullPath(a_manager, a_fileName, a_output, false);
                    ok = a_output[0] != '\0';
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool IsSavePath(const std::string& a_path) noexcept
        {
            try {
                const std::filesystem::path path(a_path);
                auto extension = path.extension().string();
                std::transform(
                    extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                return extension == ".fos";
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] std::optional<std::string> NormalizeLocalSavePath(
            const std::string& a_path) noexcept
        {
            try {
                if (a_path.empty() ||
                    a_path.find('\0') != std::string::npos) {
                    return std::nullopt;
                }
                std::filesystem::path path(a_path);
                if (!path.is_absolute()) {
                    return std::nullopt;
                }
                path = path.lexically_normal();
                const auto native = path.string();
                // Speculative warming is an optimization, so fail closed for UNC
                // paths instead of risking a blocked UI/load handoff on a remote
                // redirect. Local OneDrive paths remain ordinary drive paths.
                if (native.starts_with("\\\\") ||
                    native.starts_with("//") ||
                    !IsSavePath(native)) {
                    return std::nullopt;
                }
                return native;
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] bool ValidateOpenedSaveHandle(
            HANDLE a_file,
            std::string& a_finalPath) noexcept
        {
            if (a_file == INVALID_HANDLE_VALUE || a_file == nullptr ||
                GetFileType(a_file) != FILE_TYPE_DISK) {
                return false;
            }

            // This runs only on the below-normal worker after the handle is open.
            // Resolve reparse points to the kernel's final path and reject a UNC
            // destination even if the engine supplied a drive-letter alias.
            std::array<char, 32768> finalPath{};
            const DWORD length = GetFinalPathNameByHandleA(
                a_file,
                finalPath.data(),
                static_cast<DWORD>(finalPath.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length == 0 || length >= finalPath.size()) {
                return false;
            }

            try {
                a_finalPath.assign(finalPath.data(), length);
            } catch (...) {
                return false;
            }
            if (a_finalPath.starts_with("\\\\?\\UNC\\") ||
                a_finalPath.starts_with("\\\\UNC\\") ||
                a_finalPath.starts_with("\\\\Device\\") ||
                !IsSavePath(a_finalPath)) {
                return false;
            }
            return true;
        }

        void CancelAndClear(
            const char* a_reason,
            bool a_clearAttemptPath = true) noexcept
        {
            const auto generation =
                s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            {
                std::lock_guard lock(s_stateMutex);
                s_pendingPath.reset();
                if (a_clearAttemptPath) {
                    s_attemptPath.reset();
                }
                s_pendingGeneration = generation;
                s_workReady = false;
            }
            s_workCv.notify_all();
            logger::debug(
                "SaveGamePrefetch: cancelled/cleared ({}, generation={})",
                a_reason ? a_reason : "unspecified", generation);
        }

        void CancelAndQuiesce(const char* a_reason) noexcept
        {
            // kPreLoadGame is immediately followed by Fallout's real read. Close
            // the cancellation gate, request cancellation, and give the worker a
            // bounded opportunity to retire its OVERLAPPED and close its handle.
            // OnPostLoadGame reopens the gate only after Fallout's entire attempt
            // has returned, so no speculative arm can restart during the real load.
            std::lock_guard quiesceLock(s_quiesceMutex);
            s_quiescing.store(true, std::memory_order_release);
            // A new real attempt supersedes any older failed-attempt path.
            CancelAndClear(a_reason, true);
            bool idle = false;
            {
                std::unique_lock lock(s_stateMutex);
                idle = s_idleCv.wait_for(
                    lock,
                    kGameThreadQuiesceWait,
                    [] {
                        return !s_workerActive;
                    });
            }
            if (!idle) {
                // Never abandon or destroy the worker's stack OVERLAPPED. The
                // generation cancellation prevents another chunk, while the
                // worker retains its file/event/OVERLAPPED until Windows reports
                // completion. Bound only the game-thread handoff so a faulty
                // filesystem driver cannot indefinitely block the real load.
                logger::warn(
                    "SaveGamePrefetch: speculative reader did not quiesce "
                    "within {}ms; real load continues while the owned worker "
                    "finishes cancellation",
                    kGameThreadQuiesceWait.count());
            }
        }

        void ArmPath(std::string a_path, const char* a_source) noexcept
        {
            if (!s_enabled.load(std::memory_order_acquire) ||
                s_shuttingDown.load(std::memory_order_acquire) ||
                s_quiescing.load(std::memory_order_acquire)) {
                return;
            }
            auto normalized = NormalizeLocalSavePath(a_path);
            if (!normalized) {
                return;
            }
            a_path = std::move(*normalized);

            std::uint64_t generation = 0;
            bool startNow = false;
            {
                std::lock_guard lock(s_stateMutex);
                // Recheck while serialized with CancelAndClear.  A hook callback
                // may have passed the fast check just before kPreLoadGame closed
                // the quiescence gate.
                if (!s_enabled.load(std::memory_order_acquire) ||
                    s_shuttingDown.load(std::memory_order_acquire) ||
                    s_quiescing.load(std::memory_order_acquire)) {
                    return;
                }
                startNow = s_popupOpen.load(std::memory_order_acquire);
                generation =
                    s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                s_pendingPath = std::move(a_path);
                s_pendingSince = std::chrono::steady_clock::now();
                s_pendingGeneration = generation;
                // MenuOpenCloseEvent may be dispatched synchronously inside the
                // original MainMenu::DoLoadGame.  In that ordering the popup is
                // already open by the time the post-original hook captures the
                // selected entry, so begin immediately instead of waiting for an
                // OPEN event that has already happened.
                s_workReady = startNow;
            }
            s_workCv.notify_all();
            logger::info(
                "SaveGamePrefetch: selected save armed "
                "(source={}, generation={}, popupOpen={})",
                a_source ? a_source : "unknown", generation, startNow);
        }

        [[nodiscard]] bool StillCurrent(std::uint64_t a_generation) noexcept
        {
            return !s_shuttingDown.load(std::memory_order_acquire) &&
                s_enabled.load(std::memory_order_acquire) &&
                s_generation.load(std::memory_order_acquire) == a_generation;
        }

        struct ScopedHandle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~ScopedHandle()
            {
                if (value != INVALID_HANDLE_VALUE && value != nullptr) {
                    CloseHandle(value);
                }
            }
        };

        void CancelAndDrainPendingRead(
            HANDLE a_file, OVERLAPPED& a_overlapped) noexcept
        {
            // OVERLAPPED belongs to the caller's stack.  It must remain alive
            // until the kernel has completed or cancelled the request, including
            // unusual wait-failure paths.
            if (!CancelIoEx(a_file, &a_overlapped)) {
                const DWORD cancelError = GetLastError();
                if (cancelError != ERROR_NOT_FOUND) {
                    logger::warn(
                        "SaveGamePrefetch: CancelIoEx failed (error={})",
                        cancelError);
                }
            }

            DWORD ignored = 0;
            if (!GetOverlappedResult(
                    a_file, &a_overlapped, &ignored, TRUE)) {
                const DWORD resultError = GetLastError();
                if (resultError != ERROR_OPERATION_ABORTED &&
                    resultError != ERROR_HANDLE_EOF) {
                    logger::warn(
                        "SaveGamePrefetch: cancelled read completion failed "
                        "(error={})",
                        resultError);
                }
            }
        }

        void WarmFileCache(
            const std::string& a_path,
            std::uint64_t a_generation,
            std::stop_token a_stopToken) noexcept
        {
            const auto start = std::chrono::steady_clock::now();
            ScopedHandle file{
                CreateFileA(
                    a_path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr)
            };
            if (file.value == INVALID_HANDLE_VALUE) {
                logger::warn(
                    "SaveGamePrefetch: could not open selected save "
                    "(error={})",
                    GetLastError());
                return;
            }

            std::string finalPath;
            if (!ValidateOpenedSaveHandle(file.value, finalPath)) {
                logger::warn(
                    "SaveGamePrefetch: selected save resolved outside the "
                    "supported local .fos path policy; cache warm skipped");
                return;
            }

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0) {
                logger::warn(
                    "SaveGamePrefetch: selected save has no readable size "
                    "(error={})",
                    GetLastError());
                return;
            }

            ScopedHandle event{
                CreateEventW(nullptr, TRUE, FALSE, nullptr)
            };
            if (!event.value) {
                logger::warn(
                    "SaveGamePrefetch: CreateEvent failed (error={})",
                    GetLastError());
                return;
            }

            std::vector<std::byte> buffer;
            try {
                buffer.resize(kReadChunkSize);
            } catch (const std::bad_alloc&) {
                logger::warn(
                    "SaveGamePrefetch: unable to allocate read buffer");
                return;
            }
            std::uint64_t total = 0;
            bool cancelled = false;
            bool timeLimitReached = false;
            DWORD failure = ERROR_SUCCESS;
            const auto sourceBytes =
                static_cast<std::uint64_t>(size.QuadPart);
            const auto targetBytes =
                std::min(sourceBytes, kMaxPrefetchBytes);
            const bool byteLimited = sourceBytes > targetBytes;
            const auto deadline = start + kMaximumWarmTime;

            while (total < targetBytes) {
                if (a_stopToken.stop_requested() ||
                    !StillCurrent(a_generation)) {
                    cancelled = true;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    timeLimitReached = true;
                    break;
                }

                const auto remaining = targetBytes - total;
                const DWORD request = static_cast<DWORD>(
                    std::min<std::uint64_t>(buffer.size(), remaining));
                OVERLAPPED overlapped{};
                overlapped.Offset = static_cast<DWORD>(total);
                overlapped.OffsetHigh = static_cast<DWORD>(total >> 32);
                overlapped.hEvent = event.value;
                ResetEvent(event.value);

                DWORD bytesRead = 0;
                const BOOL started = ReadFile(
                    file.value, buffer.data(), request, &bytesRead, &overlapped);
                if (!started) {
                    failure = GetLastError();
                    if (failure != ERROR_IO_PENDING) {
                        break;
                    }

                    for (;;) {
                        const DWORD wait = WaitForSingleObject(
                            event.value,
                            static_cast<DWORD>(kCancelPoll.count()));
                        if (wait == WAIT_OBJECT_0) {
                            failure = ERROR_SUCCESS;
                            break;
                        }
                        if (wait != WAIT_TIMEOUT) {
                            failure = wait == WAIT_FAILED
                                ? GetLastError()
                                : ERROR_GEN_FAILURE;
                            CancelAndDrainPendingRead(
                                file.value, overlapped);
                            break;
                        }
                        if (a_stopToken.stop_requested() ||
                            !StillCurrent(a_generation)) {
                            cancelled = true;
                            CancelAndDrainPendingRead(
                                file.value, overlapped);
                            break;
                        }
                        if (std::chrono::steady_clock::now() >= deadline) {
                            timeLimitReached = true;
                            CancelAndDrainPendingRead(
                                file.value, overlapped);
                            break;
                        }
                    }
                    if (cancelled || timeLimitReached ||
                        failure != ERROR_SUCCESS) {
                        break;
                    }
                    if (!GetOverlappedResult(
                            file.value, &overlapped, &bytesRead, FALSE)) {
                        failure = GetLastError();
                        if (failure == ERROR_OPERATION_ABORTED) {
                            cancelled = true;
                        }
                        break;
                    }
                }

                if (bytesRead == 0) {
                    break;
                }
                total += bytesRead;
            }

            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
            if (cancelled || !StillCurrent(a_generation)) {
                logger::info(
                    "SaveGamePrefetch: cache warm cancelled after {} bytes "
                    "({}ms)",
                    total, elapsed.count());
            } else if (timeLimitReached) {
                logger::info(
                    "SaveGamePrefetch: cache warm bounded "
                    "(bytes={}, sourceBytes={}, timeLimitReached=true, "
                    "byteLimitReached={}, maxBytes={}, maxTimeMs={}, {}ms)",
                    total,
                    sourceBytes,
                    byteLimited,
                    kMaxPrefetchBytes,
                    kMaximumWarmTime.count(),
                    elapsed.count());
            } else if (failure != ERROR_SUCCESS) {
                logger::warn(
                    "SaveGamePrefetch: cache warm stopped after {} bytes "
                    "(error={}, {}ms)",
                    total, failure, elapsed.count());
            } else if (byteLimited) {
                logger::info(
                    "SaveGamePrefetch: cache warm bounded "
                    "(bytes={}, sourceBytes={}, timeLimitReached=false, "
                    "byteLimitReached=true, maxBytes={}, maxTimeMs={}, {}ms)",
                    total,
                    sourceBytes,
                    kMaxPrefetchBytes,
                    kMaximumWarmTime.count(),
                    elapsed.count());
            } else {
                logger::info(
                    "SaveGamePrefetch: cache warm completed "
                    "(bytes={}, {}ms)",
                    total, elapsed.count());
            }
        }

        void Worker(std::stop_token a_stopToken)
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

            while (!a_stopToken.stop_requested()) {
                std::string path;
                std::uint64_t generation = 0;
                try {
                    std::unique_lock lock(s_stateMutex);
                    s_workCv.wait(lock, a_stopToken, [] {
                        return s_workReady ||
                            s_shuttingDown.load(std::memory_order_acquire);
                    });
                    if (a_stopToken.stop_requested() ||
                        s_shuttingDown.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (!s_workReady || !s_pendingPath) {
                        continue;
                    }

                    path = *s_pendingPath;
                    generation = s_pendingGeneration;
                    s_workReady = false;
                    s_workerActive = true;
                } catch (const std::bad_alloc&) {
                    {
                        std::lock_guard lock(s_stateMutex);
                        s_workReady = false;
                    }
                    logger::warn(
                        "SaveGamePrefetch: unable to copy selected path");
                    continue;
                }

                if (StillCurrent(generation)) {
                    WarmFileCache(path, generation, a_stopToken);
                }
                {
                    std::lock_guard lock(s_stateMutex);
                    s_workerActive = false;
                }
                s_idleCv.notify_all();
            }
        }

        void CaptureQueuedSave() noexcept
        {
            auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager || !manager->queuedEntryToLoad) {
                CancelAndClear("selected-entry-unavailable");
                return;
            }

            char fileName[kEnginePathCapacity]{};
            char fullPath[kEnginePathCapacity]{};
            if (!CopyEntryFileName(manager->queuedEntryToLoad, fileName) ||
                !ResolveEnginePath(manager, fileName, fullPath)) {
                CancelAndClear("selected-path-unavailable");
                return;
            }
            ArmPath(fullPath, "main-menu-selection");
        }

        struct HookInvocation
        {
            HookInvocation()
            {
                std::lock_guard lock(s_hookMutex);
                ++s_activeHookCalls;
            }

            ~HookInvocation()
            {
                {
                    std::lock_guard lock(s_hookMutex);
                    --s_activeHookCalls;
                }
                s_hookIdleCv.notify_all();
            }
        };

        void HookedMainMenuDoLoadGame(void* a_self, std::int32_t a_index)
        {
            HookInvocation invocation;
            const auto original = s_originalMainMenuDoLoadGame;
            if (!original) {
                return;
            }

            original(a_self, a_index);
            if (s_installed.load(std::memory_order_acquire) &&
                s_enabled.load(std::memory_order_acquire) &&
                !s_shuttingDown.load(std::memory_order_acquire)) {
                CaptureQueuedSave();
            }
        }

        void StopOwnedWorker(const char* a_reason) noexcept
        {
            CancelAndClear(a_reason ? a_reason : "worker-stop");
            if (s_worker.joinable()) {
                s_worker.request_stop();
                s_workCv.notify_all();
                s_worker.join();
            }
        }

        struct LifetimeGuard
        {
            ~LifetimeGuard()
            {
                SaveGamePrefetch::Shutdown();
            }
        } s_lifetimeGuard;
    }

    bool SaveGamePrefetch::Install() noexcept
    {
        if (s_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (s_mainMenuHookTarget || s_worker.joinable()) {
            // Retry cleanup from an earlier fail-safe shutdown before touching
            // another MinHook record. A disable failure deliberately leaves the
            // original/trampoline alive.
            Shutdown();
            if (s_mainMenuHookTarget || s_worker.joinable()) {
                logger::error(
                    "SaveGamePrefetch: previous hook state could not be "
                    "retired; reinstall refused");
                return false;
            }
        }

        const auto version = REL::Module::get().version();
        const bool exactVR =
            REL::Module::IsVR() &&
            version[0] == 1 && version[1] == 2 &&
            version[2] == 72 && version[3] == 0;
        const bool exactOG =
            !REL::Module::IsVR() && !REL::Module::IsNG() &&
            version[0] == 1 && version[1] == 10 &&
            version[2] == 163 && version[3] == 0;
        if (!exactOG && !exactVR) {
            logger::info(
                "SaveGamePrefetch: popup cache warm unsupported on "
                "runtime {}.{}.{}.{}; disabled fail closed",
                version[0], version[1], version[2], version[3]);
            return false;
        }

        const auto base = REL::Module::get().base();
        auto* mainMenu = reinterpret_cast<void*>(
            base + (exactVR ? kMainMenuDoLoadGameVR : kMainMenuDoLoadGameOG));
        auto* getFullPath = reinterpret_cast<void*>(
            base + (exactVR ? kGetFullPathVR : kGetFullPathOG));
        if (!Matches(
                mainMenu, kMainMenuPrologue.data(),
                kMainMenuPrologue.size()) ||
            !Matches(
                getFullPath, kGetFullPathPrologue.data(),
                kGetFullPathPrologue.size())) {
            logger::error(
                "SaveGamePrefetch: engine signature mismatch; "
                "disabled fail closed");
            return false;
        }
        s_getFullPath = reinterpret_cast<GetFullPathFn>(getFullPath);

        const auto initStatus = MH_Initialize();
        if (initStatus != MH_OK &&
            initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error(
                "SaveGamePrefetch: MH_Initialize failed: {}",
                MH_StatusToString(initStatus));
            s_getFullPath = nullptr;
            return false;
        }

        s_popupOpen.store(false, std::memory_order_release);
        s_mainMenuOpen.store(false, std::memory_order_release);
        s_quiescing.store(false, std::memory_order_release);
        s_shuttingDown.store(false, std::memory_order_release);
        try {
            // Create the owned worker before enabling any detour. There is now no
            // rollback path where a callable hook exists without its consumer.
            s_worker = std::jthread(Worker);
        } catch (const std::system_error& e) {
            logger::error(
                "SaveGamePrefetch: worker creation failed: {}", e.what());
            s_getFullPath = nullptr;
            return false;
        }

        const auto createStatus = MH_CreateHook(
            mainMenu,
            reinterpret_cast<void*>(&HookedMainMenuDoLoadGame),
            reinterpret_cast<void**>(&s_originalMainMenuDoLoadGame));
        if (createStatus != MH_OK || !s_originalMainMenuDoLoadGame) {
            logger::error(
                "SaveGamePrefetch: MainMenu hook creation failed: {}",
                MH_StatusToString(createStatus));
            StopOwnedWorker("hook-create-failed");
            if (createStatus == MH_OK) {
                s_mainMenuHookTarget = mainMenu;
                const auto removeStatus = MH_RemoveHook(mainMenu);
                if (removeStatus == MH_OK ||
                    removeStatus == MH_ERROR_NOT_CREATED) {
                    s_mainMenuHookTarget = nullptr;
                    s_originalMainMenuDoLoadGame = nullptr;
                    s_getFullPath = nullptr;
                } else {
                    logger::error(
                        "SaveGamePrefetch: incomplete created-hook rollback: {}; "
                        "disabled state retained",
                        MH_StatusToString(removeStatus));
                }
            } else {
                s_originalMainMenuDoLoadGame = nullptr;
                s_getFullPath = nullptr;
            }
            return false;
        }
        s_mainMenuHookTarget = mainMenu;
        const auto enableStatus = MH_EnableHook(mainMenu);
        if (enableStatus != MH_OK) {
            logger::error(
                "SaveGamePrefetch: MainMenu hook enable failed: {}",
                MH_StatusToString(enableStatus));
            s_shuttingDown.store(true, std::memory_order_release);
            StopOwnedWorker("hook-enable-failed");

            const auto disableStatus = MH_DisableHook(mainMenu);
            const bool safelyDisabled =
                disableStatus == MH_OK ||
                disableStatus == MH_ERROR_DISABLED ||
                disableStatus == MH_ERROR_NOT_CREATED;
            if (!safelyDisabled) {
                logger::error(
                    "SaveGamePrefetch: failed-enable hook could not be "
                    "proven disabled: {}; retaining original/trampoline",
                    MH_StatusToString(disableStatus));
                return false;
            }
            const auto removeStatus = MH_RemoveHook(mainMenu);
            if (removeStatus == MH_OK ||
                removeStatus == MH_ERROR_NOT_CREATED) {
                s_mainMenuHookTarget = nullptr;
                s_originalMainMenuDoLoadGame = nullptr;
                s_getFullPath = nullptr;
            } else {
                logger::error(
                    "SaveGamePrefetch: failed-enable hook removal failed: {}; "
                    "disabled state retained",
                    MH_StatusToString(removeStatus));
            }
            return false;
        }

        s_installed.store(true, std::memory_order_release);
        logger::info(
            "SaveGamePrefetch: popup cache warmer installed "
            "(runtime={}, readOnly=true, cancellable=true, maxBytes={}, "
            "maxTimeMs={}, quiesceWaitMs={}, localPathsOnly=true)",
            exactVR ? "VR" : "OG",
            kMaxPrefetchBytes,
            kMaximumWarmTime.count(),
            kGameThreadQuiesceWait.count());
        return true;
    }

    void SaveGamePrefetch::Shutdown() noexcept
    {
        if (!s_installed.load(std::memory_order_acquire) &&
            !s_worker.joinable() && !s_mainMenuHookTarget) {
            return;
        }
        // Detours become pass-through immediately, before MinHook restoration.
        s_installed.store(false, std::memory_order_release);
        s_shuttingDown.store(true, std::memory_order_release);
        s_popupOpen.store(false, std::memory_order_release);
        s_mainMenuOpen.store(false, std::memory_order_release);

        // Stop new detour entries first, then wait for any call which had
        // already entered our hook to return before removing its trampoline or
        // clearing the original-function pointer.
        void* const hookTarget = s_mainMenuHookTarget;
        bool safelyDisabled = hookTarget == nullptr;
        if (hookTarget) {
            const auto disableStatus = MH_DisableHook(hookTarget);
            safelyDisabled =
                disableStatus == MH_OK ||
                disableStatus == MH_ERROR_DISABLED ||
                disableStatus == MH_ERROR_NOT_CREATED;
            if (!safelyDisabled) {
                logger::error(
                    "SaveGamePrefetch: MainMenu hook disable failed: {}; "
                    "retaining target and original as safe pass-through",
                    MH_StatusToString(disableStatus));
            }
        }

        if (safelyDisabled) {
            std::unique_lock lock(s_hookMutex);
            s_hookIdleCv.wait(lock, [] {
                return s_activeHookCalls == 0;
            });
        }

        StopOwnedWorker("shutdown");

        if (!safelyDisabled) {
            // New entries can still arrive, so the trampoline/original and target
            // metadata must remain valid. The hook only calls Fallout now because
            // s_installed is false and s_shuttingDown is true.
            logger::warn(
                "SaveGamePrefetch: shutdown left a safe pass-through hook "
                "installed for process lifetime");
            return;
        }

        bool removed = hookTarget == nullptr;
        if (hookTarget) {
            const auto removeStatus = MH_RemoveHook(hookTarget);
            removed =
                removeStatus == MH_OK ||
                removeStatus == MH_ERROR_NOT_CREATED;
            if (!removed) {
                logger::error(
                    "SaveGamePrefetch: MainMenu hook removal failed: {}; "
                    "disabled target/original state retained",
                    MH_StatusToString(removeStatus));
            }
        }
        if (removed) {
            s_mainMenuHookTarget = nullptr;
            s_originalMainMenuDoLoadGame = nullptr;
            s_getFullPath = nullptr;
            logger::info("SaveGamePrefetch: shutdown complete");
        }
    }

    void SaveGamePrefetch::SetEnabled(bool a_enabled) noexcept
    {
        s_enabled.store(a_enabled, std::memory_order_release);
        if (!a_enabled) {
            CancelAndClear("disabled");
        }
    }

    void SaveGamePrefetch::OnMessageBoxMenu(bool a_opening) noexcept
    {
        if (!s_installed.load(std::memory_order_acquire)) {
            return;
        }
        s_popupOpen.store(a_opening, std::memory_order_release);
        if (!a_opening) {
            bool hadPending = false;
            bool workerActive = false;
            bool workReady = false;
            {
                std::lock_guard lock(s_stateMutex);
                hadPending = s_pendingPath.has_value();
                workerActive = s_workerActive;
                workReady = s_workReady;
            }
            if (s_mainMenuOpen.load(std::memory_order_acquire) ||
                hadPending || workerActive || workReady) {
                logger::info(
                    "SaveGamePrefetch: MessageBox CLOSE observed; "
                    "cancelling speculative read "
                    "(pending={}, queued={}, active={}, quiescing={})",
                    hadPending,
                    workReady,
                    workerActive,
                    s_quiescing.load(std::memory_order_acquire));
            }
            CancelAndClear(
                "confirmation-closed",
                !s_quiescing.load(std::memory_order_acquire));
            return;
        }

        bool armed = false;
        bool hadPending = false;
        bool generationCurrent = false;
        bool withinSelectionWindow = false;
        bool workerActive = false;
        std::int64_t selectionAgeMs = -1;
        const bool enabled =
            s_enabled.load(std::memory_order_acquire);
        const bool shuttingDown =
            s_shuttingDown.load(std::memory_order_acquire);
        {
            std::lock_guard lock(s_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            hadPending = s_pendingPath.has_value();
            workerActive = s_workerActive;
            if (hadPending) {
                selectionAgeMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - s_pendingSince)
                        .count();
                withinSelectionWindow =
                    now - s_pendingSince <= kSelectionWindow;
                generationCurrent =
                    s_pendingGeneration ==
                    s_generation.load(std::memory_order_acquire);
            }
            if (enabled && !shuttingDown && hadPending &&
                withinSelectionWindow && generationCurrent) {
                s_workReady = true;
                armed = true;
            }
        }

        if (s_mainMenuOpen.load(std::memory_order_acquire) ||
            hadPending || workerActive) {
            logger::info(
                "SaveGamePrefetch: MessageBox OPEN observed "
                "(enabled={}, pending={}, ageMs={}, generationCurrent={}, "
                "withinWindow={}, active={}, startWarm={})",
                enabled && !shuttingDown,
                hadPending,
                selectionAgeMs,
                generationCurrent,
                withinSelectionWindow,
                workerActive,
                armed);
        }
        if (armed) {
            s_workCv.notify_all();
        }
    }

    void SaveGamePrefetch::OnMainMenu(bool a_opening) noexcept
    {
        if (!s_installed.load(std::memory_order_acquire)) {
            return;
        }
        s_mainMenuOpen.store(a_opening, std::memory_order_release);
        if (!a_opening) {
            s_popupOpen.store(false, std::memory_order_release);
            CancelAndClear(
                "main-menu-closed",
                !s_quiescing.load(std::memory_order_acquire));
        }
    }

    void SaveGamePrefetch::OnLoadingMenu(bool a_opening) noexcept
    {
        if (a_opening &&
            s_installed.load(std::memory_order_acquire)) {
            s_popupOpen.store(false, std::memory_order_release);
            // Preserve the owned kPre path until kPost. LoadingMenu normally
            // opens between those messages, including attempts which later
            // fail into a warning popup.
            CancelAndClear(
                "engine-loading-menu-open",
                !s_quiescing.load(std::memory_order_acquire));
        }
    }

    void SaveGamePrefetch::OnPreLoadGame(
        const void* a_fileName, std::size_t a_length) noexcept
    {
        if (!s_installed.load(std::memory_order_acquire)) {
            return;
        }

        bool hadPending = false;
        bool workerActive = false;
        bool workReady = false;
        std::int64_t selectionAgeMs = -1;
        {
            std::lock_guard lock(s_stateMutex);
            hadPending = s_pendingPath.has_value();
            workerActive = s_workerActive;
            workReady = s_workReady;
            if (hadPending) {
                selectionAgeMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        s_pendingSince)
                        .count();
            }
        }
        logger::info(
            "SaveGamePrefetch: engine load starting; "
            "draining speculative reader "
            "(pending={}, ageMs={}, queued={}, active={}, popupOpen={})",
            hadPending,
            selectionAgeMs,
            workReady,
            workerActive,
            s_popupOpen.load(std::memory_order_acquire));

        // The real loader is about to read. Stop any speculative read first so
        // it cannot compete. Retain an owned copy only as a fallback for a
        // subsequent failed-attempt warning popup.
        CancelAndQuiesce("engine-load-start");
        if (!s_enabled.load(std::memory_order_acquire) ||
            !a_fileName || a_length == 0) {
            return;
        }

        const auto* fileName = static_cast<const char*>(a_fileName);
        const auto* terminator = static_cast<const char*>(
            std::memchr(fileName, '\0', a_length));
        const std::size_t pathLength = terminator
            ? static_cast<std::size_t>(terminator - fileName)
            : a_length;
        if (pathLength == 0 || pathLength >= kEnginePathCapacity) {
            return;
        }

        std::string path;
        try {
            path.assign(fileName, pathLength);
        } catch (...) {
            return;
        }

        // F4SE documents this as the complete save path. Do not call engine
        // path helpers from its load worker; the MainMenu hook is the UI-thread
        // source for pre-popup cases.
        auto normalized = NormalizeLocalSavePath(path);
        if (!normalized) {
            return;
        }

        std::lock_guard lock(s_stateMutex);
        s_attemptPath = std::move(*normalized);
    }

    void SaveGamePrefetch::OnPostLoadGame(bool a_succeeded) noexcept
    {
        if (!s_installed.load(std::memory_order_acquire)) {
            return;
        }
        // Fallout's real load attempt has returned.  Failed attempts may now
        // re-arm the same path while their confirmation popup remains open.
        s_quiescing.store(false, std::memory_order_release);
        if (a_succeeded) {
            CancelAndClear("engine-load-finished");
            return;
        }

        bool armed = false;
        bool startNow = false;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(s_stateMutex);
            if (s_attemptPath &&
                s_enabled.load(std::memory_order_acquire) &&
                !s_shuttingDown.load(std::memory_order_acquire)) {
                // F4SE emits kPost(false) before the outer engine function
                // creates its missing-content MessageBox. Publish the fallback
                // atomically here, then let that subsequent OPEN event start it.
                // A concurrent CLOSE uses this same lock and either clears the
                // published path afterward or wins first and removes the
                // attempt, so cancellation can never be resurrected.
                generation =
                    s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                s_pendingPath = std::move(s_attemptPath);
                s_pendingSince = std::chrono::steady_clock::now();
                s_pendingGeneration = generation;
                startNow = s_popupOpen.load(std::memory_order_acquire);
                s_workReady = startNow;
                armed = true;
            }
            s_attemptPath.reset();
        }
        if (armed) {
            logger::info(
                "SaveGamePrefetch: failed save attempt armed "
                "(generation={}, popupOpen={})",
                generation,
                startNow);
            s_workCv.notify_all();
        }
    }
}
