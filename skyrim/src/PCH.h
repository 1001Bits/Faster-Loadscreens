#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Fix CommonLib's REL::Version formatter for fmt v12 (requires const format()).
template <>
struct fmt::formatter<REL::Version> : fmt::formatter<std::string>
{
    auto format(const REL::Version& a_ver, fmt::format_context& a_ctx) const
    {
        return fmt::formatter<std::string>::format(
            fmt::format("{}.{}.{}.{}", a_ver[0], a_ver[1], a_ver[2], a_ver[3]), a_ctx);
    }
};

namespace logger = SKSE::log;

using namespace std::literals;

#define DLLEXPORT __declspec(dllexport)
