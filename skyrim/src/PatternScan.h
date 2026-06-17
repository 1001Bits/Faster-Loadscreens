#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>

// Header-only masked-signature scanner over the main module's executable
// sections. Shared by LoadingLoopHook and DoorPrefetchHook.
namespace FasterLoadscreens::scan
{
    struct Pattern
    {
        std::vector<std::uint8_t> bytes;
        std::vector<bool> mask;  // true = byte must match, false = wildcard (??)
    };

    // Parse a "48 8B 05 ?? ?? ?? ?? C3"-style string.
    inline Pattern Parse(const char* a_sig)
    {
        Pattern p;
        for (const char* c = a_sig; *c;) {
            if (*c == ' ') {
                ++c;
                continue;
            }
            if (c[0] == '?') {
                p.bytes.push_back(0);
                p.mask.push_back(false);
                c += (c[1] == '?') ? 2 : 1;
            } else {
                auto hex = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    return 0;
                };
                p.bytes.push_back(static_cast<std::uint8_t>(hex(c[0]) * 16 + hex(c[1])));
                p.mask.push_back(true);
                c += 2;
            }
        }
        return p;
    }

    inline bool MatchAt(const std::uint8_t* a_mem, const Pattern& a_p)
    {
        for (std::size_t i = 0; i < a_p.bytes.size(); ++i) {
            if (a_p.mask[i] && a_mem[i] != a_p.bytes[i]) {
                return false;
            }
        }
        return true;
    }

    // All matches across the main module's executable sections.
    inline std::vector<std::uintptr_t> FindAll(const Pattern& a_p)
    {
        std::vector<std::uintptr_t> out;
        if (a_p.bytes.empty()) {
            return out;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        const bool firstConcrete = a_p.mask[0];
        const std::uint8_t first = a_p.bytes[0];

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
                continue;
            }
            const auto* begin = reinterpret_cast<const std::uint8_t*>(base + sec[i].VirtualAddress);
            const std::size_t size = sec[i].Misc.VirtualSize;
            if (size < a_p.bytes.size()) {
                continue;
            }
            for (std::size_t off = 0; off + a_p.bytes.size() <= size; ++off) {
                if (firstConcrete && begin[off] != first) {
                    continue;
                }
                if (MatchAt(begin + off, a_p)) {
                    out.push_back(reinterpret_cast<std::uintptr_t>(begin + off));
                }
            }
        }
        return out;
    }

    // Single unique match, or 0 when not found / ambiguous. Logs the outcome.
    inline std::uintptr_t FindUnique(const Pattern& a_p, const char* a_tag)
    {
        const auto matches = FindAll(a_p);
        if (matches.empty()) {
            logger::warn("{}: signature not found in this runtime — feature disabled", a_tag);
            return 0;
        }
        if (matches.size() > 1) {
            logger::warn("{}: signature ambiguous ({} matches) — disabling", a_tag, matches.size());
            return 0;
        }
        return matches.front();
    }
}
