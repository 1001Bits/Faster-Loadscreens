#include "PCH.h"

#include "Config.h"
#include "DoorPrefetchHook.h"
#include "GameSettingTweaks.h"
#include "LoadingLoopHook.h"
#include "SceneReadyHold.h"
#include "ScriptSettle.h"

#include <Windows.h>
#include <ShlObj.h>  // SHGetKnownFolderPath / FOLDERID_Documents
#pragma comment(lib, "Shell32.lib")

#include <atomic>
#include <thread>

using namespace FasterLoadscreens;

namespace
{
    // Resolve the SKSE log folder explicitly. We deliberately do NOT use
    // logger::log_directory() here: on this 1.6.1170 setup CommonLibSSE-NG
    // resolves it to My Games\Skyrim.INI\SKSE (a folder-name quirk), while the
    // game's real documents folder — where saves and the other plugins' logs
    // live — is "Skyrim Special Edition". Build the standard path ourselves so
    // our log lands beside everything else.
    std::filesystem::path ResolveLogDir()
    {
        std::filesystem::path dir;
        PWSTR docs = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
            dir = docs;
        }
        if (docs) {
            ::CoTaskMemFree(docs);
        }
        if (dir.empty()) {
            return {};
        }
        dir /= "My Games";

        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring fname = std::filesystem::path(exe).filename().wstring();
        const bool isVR = fname.find(L"VR") != std::wstring::npos ||
                          fname.find(L"vr") != std::wstring::npos;

        dir /= isVR ? "Skyrim VR" : "Skyrim Special Edition";
        dir /= "SKSE";
        return dir;
    }

    void InitializeLog()
    {
        auto dir = ResolveLogDir();
        if (dir.empty()) {
            SKSE::stl::report_and_fail("Failed to resolve log directory.");
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        auto path = dir / fmt::format("{}.log", SKSE::PluginDeclaration::GetSingleton()->GetName());
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);

        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [%l] %v");
    }

    // ========================================================================
    // Black-screen-to-playable measurement.
    //
    // Our load timer stops at the loading-menu CLOSE (kHide). After that the
    // engine fades in from black and only THEN restores player control — the
    // "long black wait" before you can actually see/play, which the CLOSE time
    // does NOT capture. This measures that tail: loading-menu CLOSE -> movement
    // controls restored. Runs in ALL modes (incl. baseline iMode=0) so vanilla
    // and mod are directly comparable. A background thread polls ControlMap on
    // the game thread (via the SKSE task queue) so the engine read is race-free.
    // ========================================================================
    std::atomic<bool> s_awaitVisible{ false };
    std::atomic<int>  s_visibleLoad{ 0 };
    std::chrono::steady_clock::time_point s_hideTime{};  // written + read on the game thread only
    std::atomic<bool> s_visiblePollRun{ false };

    void BeginVisibleMeasure(int a_loadNum)
    {
        s_hideTime = std::chrono::steady_clock::now();
        s_visibleLoad.store(a_loadNum, std::memory_order_relaxed);
        s_awaitVisible.store(true, std::memory_order_relaxed);
    }

    void VisiblePollerThread()
    {
        using namespace std::chrono_literals;
        while (s_visiblePollRun.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(33ms);  // ~30 Hz
            if (!s_awaitVisible.load(std::memory_order_relaxed)) {
                continue;
            }
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                continue;
            }
            task->AddTask([]() {
                if (!s_awaitVisible.load(std::memory_order_relaxed)) {
                    return;
                }
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - s_hideTime)
                                    .count();
                auto* cm = RE::ControlMap::GetSingleton();
                if (cm && cm->IsMovementControlsEnabled()) {
                    s_awaitVisible.store(false, std::memory_order_relaxed);
                    logger::info("Playable: {:.2f}s after load #{} closed (CLOSE -> controls restored)",
                        ms / 1000.0, s_visibleLoad.load(std::memory_order_relaxed));
                } else if (ms > 30000) {
                    s_awaitVisible.store(false, std::memory_order_relaxed);  // abandon (menu/idle) — no garbage log
                }
            });
        }
    }

    // ========================================================================
    // Loading Menu watcher: per-load timing + process priority boost.
    //
    // The priority boost lives here (not in the loop hook) because freeze
    // mode exits DisplayLoadingScreen long before the load finishes — the
    // menu close event is the reliable end-of-load signal.
    // ========================================================================
    class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuWatcher* GetSingleton()
        {
            static MenuWatcher instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->menuName != RE::LoadingMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->opening) {
                m_loadStart = std::chrono::steady_clock::now();
                m_haveOpen = true;
                ++m_loadCount;

                if (Config::Get().benchmarkMode != 0 && Config::Get().boostPriority && !m_boosted) {
                    m_savedPriority = ::GetPriorityClass(::GetCurrentProcess());
                    if (!m_savedPriority) {
                        m_savedPriority = NORMAL_PRIORITY_CLASS;
                    }
                    ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);
                    m_boosted = true;
                }
                // Raise the object-finalize budget for the duration of the
                // load only. (Restored on close — leaving it raised caused a
                // multi-second post-load FPS tank.)
                GameSettingTweaks::OnLoadScreenOpen();
                logger::info("Loading Menu OPEN (load #{})", m_loadCount);
            } else {
                if (m_boosted) {
                    ::SetPriorityClass(::GetCurrentProcess(), m_savedPriority);
                    m_boosted = false;
                }
                // The startup loading screen opens before this sink is
                // registered, so its open was never seen — don't print a bogus
                // duration for it.
                if (m_haveOpen) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_loadStart).count();
                    logger::info("Loading Menu CLOSE (load #{}) — {:.2f}s", m_loadCount, ms / 1000.0);
                    m_haveOpen = false;
                    BeginVisibleMeasure(m_loadCount);  // time the post-close black/fade until controls return
                } else {
                    logger::info("Loading Menu CLOSE (startup — open not tracked)");
                }

                // Re-assert our setting values in case another mod or the
                // engine rewrote them during the load, then drop the finalize
                // budget back to its baseline so leftover queued refs drain
                // gently instead of eating whole frames.
                GameSettingTweaks::Apply();
                GameSettingTweaks::OnLoadScreenClose();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        std::chrono::steady_clock::time_point m_loadStart{};
        int m_loadCount = 0;
        DWORD m_savedPriority = NORMAL_PRIORITY_CLASS;
        bool m_boosted = false;
        bool m_haveOpen = false;
    };

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            GameSettingTweaks::Apply();
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink(MenuWatcher::GetSingleton());
                logger::info("MenuWatcher registered");
            } else {
                logger::warn("RE::UI unavailable — per-load timing/boost disabled");
            }
            DoorPrefetchHook::Install();
            SceneReadyHold::Install();
            if (!s_visiblePollRun.exchange(true)) {
                // Stop the detached poller at CRT teardown — a detached thread
                // calling AddTask/logging during static destruction is a
                // crash-on-exit source.
                std::atexit([]() { s_visiblePollRun.store(false, std::memory_order_relaxed); });
                std::thread(VisiblePollerThread).detach();
                logger::info("Black-screen-to-playable measure: poller started");
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            // A save load is starting — release any scene-ready hold left from
            // the previous load so it can't straddle into this one.
            SceneReadyHold::CancelPendingHold();
            break;
        case SKSE::MessagingInterface::kNewGame:
            SceneReadyHold::CancelPendingHold();
            GameSettingTweaks::Apply();
            // Fresh save: give the mod-initialization script burst real VM
            // time, and (optionally) force the SkyUI MCM registration sweep.
            ScriptSettle::OnGameLoaded(true);
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // Re-read config first: MCM Helper writes user changes to
            // Data\MCM\Settings\FasterLoadscreens.ini, and reloading here is
            // what makes MCM edits take effect without a game restart (all
            // consumers read Config::Get() live). Then re-assert settings —
            // fPostLoadUpdateTimeMS etc. can be reset around save load.
            Config::Get().Load();
            GameSettingTweaks::Apply();
            ScriptSettle::OnGameLoaded(false);
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    // SKSE::Init first: it wires up the interfaces (messaging, task, trampoline)
    // that everything below may touch. Log init follows immediately so any
    // failure after this point is still captured.
    SKSE::Init(a_skse);
    InitializeLog();

    const auto plugin = SKSE::PluginDeclaration::GetSingleton();
    logger::info("{} v{} loading", plugin->GetName(), plugin->GetVersion());

    const auto ver = REL::Module::get().version();
    logger::info("Runtime: Skyrim{} v{}", REL::Module::IsVR() ? " VR" : "", ver.string());

    Config::Get().Load();

    // Install the loading-loop hooks immediately: they are pure code hooks
    // with no game-state dependency, and installing this early also catches
    // the initial startup loading screen.
    LoadingLoopHook::Install();

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logger::critical("Failed to register message listener");
        return false;
    }

    logger::info("{} loaded", plugin->GetName());
    return true;
}
