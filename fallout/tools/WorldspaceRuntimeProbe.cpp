// Private OG runtime probe. Explicit target only; never linked into or packaged
// with LoadingScreens. Calls the exact release preparation entry from its map.
#include "PCH.h"
#include <array>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace
{
    std::filesystem::path s_directory;
    std::mutex s_logMutex;
    std::atomic<bool> s_loaded{};
    std::uintptr_t s_prepareRva{};
    std::array<std::uint8_t, 24> s_prepareBytes{}, s_consoleBytes{};

    void Log(const std::string& a_text)
    {
        std::lock_guard lock(s_logMutex);
        std::ofstream(s_directory / "probe.log", std::ios::app) <<
            GetTickCount64() << ' ' << a_text << '\n';
    }

    bool PrivateDesktop()
    {
        wchar_t expected[256]{}, current[256]{}, input[256]{};
        const auto length = GetEnvironmentVariableW(L"FLS_PROBE_DESKTOP", expected, 256);
        if (!length || length >= 256 || std::wcsncmp(expected, L"FLSIssue2_", 10)) return false;
        DWORD size{};
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                current, sizeof(current), &size) || std::wcscmp(expected, current)) return false;
        auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!desktop) return false;
        const bool named = GetUserObjectInformationW(desktop, UOI_NAME, input, sizeof(input), &size) != FALSE;
        CloseDesktop(desktop);
        return named && std::wcscmp(current, input) != 0;
    }

    bool Token(const std::string& a_value)
    {
        return !a_value.empty() && a_value.size() <= 240 &&
            a_value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") ==
                std::string::npos;
    }

    void Inspect()
    {
        auto* world = RE::TESForm::GetFormByID<RE::TESWorldSpace>(0x03000B0F);
        auto* marker = RE::TESForm::GetFormByID<RE::TESObjectREFR>(0x030180DE);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        std::ostringstream out;
        out << "STATE loaded=" << s_loaded << " playerCell=" << std::hex << (cell ? cell->GetFormID() : 0)
            << " playerWorld=" << (cell && cell->worldSpace ? cell->worldSpace->GetFormID() : 0);
        if (world) {
            out << " farHarborGraph=" << world->portalGraph.get()
                << " forwardMap=" << world->multiboundRefMap << " reverseMap=" << world->refMultiboundMap;
            if (world->portalGraph) {
                out << " graphRefs=" << std::dec << reinterpret_cast<RE::NiRefObject*>(world->portalGraph.get())->refCount;
            }
        }
        auto* markerCell = marker ? marker->GetParentCell() : nullptr;
        out << " marker=" << marker << " markerCell=" << std::hex << (markerCell ? markerCell->GetFormID() : 0)
            << " cellState=" << std::dec << (markerCell ? static_cast<int>(markerCell->cellState.get()) : -1);
        // Read the native registration only after cell state >= LOADED (4),
        // which is published after CreateLoadedData/AddMultiBoundRef finishes.
        if (world && world->multiboundRefMap && world->refMultiboundMap && markerCell &&
            static_cast<int>(markerCell->cellState.get()) >= 4) {
            auto handle = marker->GetHandle();
            RE::BSMultiBoundNode* bound = nullptr;
            // This CommonLib pin's const handle comparison is ill-formed when
            // instantiated. Observe the native handle bits without changing it.
            for (auto& entry : *world->multiboundRefMap) {
                std::uint32_t bits{};
                static_assert(sizeof(entry.first) == sizeof(bits));
                std::memcpy(&bits, &entry.first, sizeof(bits));
                if (bits == handle.native_handle()) bound = entry.second.get();
            }
            const bool registered = bound != nullptr;
            bool reverse = false;
            if (registered) {
                for (auto& entry : *world->refMultiboundMap) {
                    std::uint32_t bits{};
                    std::memcpy(&bits, &entry.second, sizeof(bits));
                    if (entry.first == bound && bits == handle.native_handle()) reverse = true;
                }
            }
            out << " markerRegistered=" << registered << " reverseRegistered=" << reverse;
        }
        Log(out.str());
    }

    void Execute(const std::string& a_command)
    {
        if (!PrivateDesktop()) { Log("FAIL desktop mismatch"); return; }
        if (a_command == "inspect") { Inspect(); return; }
        if (a_command == "preload-far-harbor") {
            if (!s_loaded) { Log("FAIL gameplay not loaded"); return; }
            auto* world = RE::TESForm::GetFormByID<RE::TESWorldSpace>(0x03000B0F);
            auto* module = GetModuleHandleW(L"LoadingScreens.dll");
            if (!module || !world) { Log("FAIL missing plugin/Far Harbor"); return; }
            const auto address = reinterpret_cast<std::uintptr_t>(module) + s_prepareRva;
            if (std::memcmp(reinterpret_cast<void*>(address), s_prepareBytes.data(), s_prepareBytes.size())) {
                Log("FAIL preparation entry differs from tested artifact"); return;
            }
            Inspect();
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            auto* tes = *reinterpret_cast<void**>(base + 0x5AA4288);
            if (!tes) { Log("FAIL missing TES"); return; }
            using Preload = void (*)(void*, RE::TESWorldSpace*, int, int, bool);
            using Submit = bool (*)(void*, RE::TESWorldSpace*, int, int, Preload);
            const bool ready = reinterpret_cast<Submit>(address)(tes, world, 14, 2,
                reinterpret_cast<Preload>(base + 0xFC0D0));
            Log(ready ? "QUEUED FarHarbor (14,2) queueOnly=1 through release Submit" : "REJECTED FarHarbor submission");
            return;
        }
        if (a_command == "full-reset" || a_command == "return-to-menu") {
            auto* main = RE::Main::GetSingleton();
            DWORD owner{};
            if (!main || !main->hwnd ||
                !GetWindowThreadProcessId(reinterpret_cast<HWND>(main->hwnd), &owner) ||
                owner != GetCurrentProcessId()) {
                Log("FAIL main/window ownership mismatch"); return;
            }
            s_loaded = false;
            // Request the engine's normal reset on its next main-loop iteration.
            // Do not call reset directly from an F4SE task-pool callback.
            main->fullReset = a_command == "full-reset";
            MemoryBarrier();
            main->resetGame = true;
            Log(main->fullReset ? "REQUESTED native full reset" : "REQUESTED native menu reset");
            return;
        }
        if (a_command == "confirm-save-warning") {
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu<RE::MessageBoxMenu>() : nullptr;
            auto* message = menu ? menu->currentMessage : nullptr;
            const std::string body = message ? message->bodyText.c_str() : "";
            Log("WARNING body=" + body);
            const bool expected = body == "$LoadVanillaSaveWithMods" ||
                (body.find("Mods are currently loaded") != std::string::npos &&
                    body.find("Achievements are disabled") != std::string::npos);
            if (!expected || !message || !message->callback || message->buttonText.empty() ||
                (std::string_view(message->buttonText[0].c_str()) != "Yes" &&
                    std::string_view(message->buttonText[0].c_str()) != "$Yes")) {
                Log("FAIL unexpected save warning"); return;
            }
            auto callback = message->callback;
            message->callback.reset();
            RE::UIMessageQueue::GetSingleton()->AddMessage(RE::MessageBoxMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
            (*callback)(0);
            return;
        }
        std::string native;
        if (a_command.starts_with("load ") && Token(a_command.substr(5))) {
            const auto saves = std::filesystem::path(L"C:/Users/Noud/Documents/My Games/Fallout4/Saves");
            if (!std::filesystem::is_regular_file(saves / (a_command.substr(5) + ".fos"))) {
                Log("FAIL save missing"); return;
            }
            native = "LoadGame " + a_command.substr(5);
        } else if (a_command == "visit-far-harbor") native = "cow DLC03FarHarbor 14 2";
        else if (a_command == "visit-commonwealth") native = "cow Commonwealth -4 -8";
        else if (a_command == "quit-game") native = "qqq";
        else { Log("FAIL invalid command"); return; }
        const auto address = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + 0x125B4A0;
        if (std::memcmp(reinterpret_cast<void*>(address), s_consoleBytes.data(), s_consoleBytes.size())) {
            Log("FAIL console entry differs from pinned executable"); return;
        }
        Log("COMMAND " + a_command);
        reinterpret_cast<void (*)(const char*)>(address)(native.c_str());
        Log("COMMAND returned");
    }

    void Message(F4SE::MessagingInterface::Message* a_message)
    {
        if (!a_message) return;
        if (a_message->type == F4SE::MessagingInterface::kGameDataReady) Log("DATA READY");
        if (a_message->type == F4SE::MessagingInterface::kPreLoadGame) {
            s_loaded = false; Log("PRELOAD");
        }
        if (a_message->type == F4SE::MessagingInterface::kPostLoadGame) {
            s_loaded = a_message->data != nullptr;
            Log(s_loaded ? "POSTLOAD success" : "POSTLOAD failed");
        }
    }
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Query(
    const F4SE::QueryInterface* a_query, F4SE::PluginInfo* a_info)
{
    a_info->infoVersion = 1;
    a_info->name = "FLSWorldspaceProbe";
    a_info->version = 1;
    return a_query && a_query->RuntimeVersion() == REL::Version(1, 10, 163, 0);
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const F4SE::LoadInterface* a_load)
{
    if (!PrivateDesktop()) return false;
    wchar_t directory[1024]{};
    const auto length = GetEnvironmentVariableW(L"FLS_PROBE_DIRECTORY", directory, 1024);
    if (!length || length >= 1024) return false;
    s_directory = directory;
    std::ifstream addressFile(s_directory / "prepare-rva.txt");
    addressFile >> std::hex >> s_prepareRva;
    if (!s_prepareRva || s_prepareRva >= 0x200000) return false;
    for (const auto& [name, bytes] : {
             std::pair{ "prepare-entry.bin", &s_prepareBytes },
             std::pair{ "console-entry.bin", &s_consoleBytes } }) {
        std::ifstream file(s_directory / name, std::ios::binary);
        file.read(reinterpret_cast<char*>(bytes->data()), bytes->size());
        if (file.gcount() != bytes->size()) return false;
    }
    F4SE::Init(a_load);
    F4SE::GetMessagingInterface()->RegisterListener(Message);
    Log("ARMED private OG worldspace probe");
    std::thread([] {
        std::string last;
        for (;;) {
            Sleep(100);
            std::ifstream file(s_directory / "command.txt");
            std::string line;
            std::getline(file, line);
            if (line.empty() || line == last) continue;
            last = line;
            const auto space = line.find(' ');
            if (space == std::string::npos) continue;
            auto command = line.substr(space + 1);
            F4SE::GetTaskInterface()->AddTask([command] { Execute(command); });
        }
    }).detach();
    return true;
}
