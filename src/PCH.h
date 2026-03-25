#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <REL/Relocation.h>

#include <Windows.h>

using namespace std::literals;

// Logging — CommonLibF4 sets up spdlog; use the standard logger macro
namespace logger = F4SE::log;

#define DLLEXPORT __declspec(dllexport)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <random>
#include <string>
#include <thread>
#include <vector>
