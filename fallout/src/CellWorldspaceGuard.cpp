#include "PCH.h"
#include "CellWorldspaceGuard.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>

namespace VRLoadingScreens
{
    namespace
    {
        // TESWorldSpace::UnregisterCell-ish. See the header for the full
        // provenance of this address and why the null check belongs here.
        constexpr std::uintptr_t kCellUnregister_Offset_VR = 0x47B9B0;

        // TESObjectCELL fields, confirmed against the CommonLibF4 this builds
        // against (TESForms.h) and against three inlined GetWorldSpace idioms
        // in the binary (0x47B9F3, 0x1008FE, 0xF04533).
        constexpr std::uintptr_t kCellFlags = 0x40;   // u16, bit 0 = interior
        constexpr std::uintptr_t kCellState = 0x44;   // u8
        constexpr std::uintptr_t kCellWorldSpace = 0xC8;  // union w/ tempDataOffset

        using CellUnregister_t = void(__fastcall*)(void*, void*);
        CellUnregister_t s_original = nullptr;
        bool s_installed = false;
        std::atomic<int> s_intercepts{ 0 };

        // The cell is by definition suspect here, so every read of it is
        // guarded. A guard that crashes while reporting a crash is worthless.
        [[nodiscard]] bool TryReadPointer(
            const void* a_at, std::uintptr_t& a_out) noexcept
        {
            __try {
                a_out = *reinterpret_cast<const std::uintptr_t*>(a_at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        [[nodiscard]] bool TryReadU16(
            const void* a_at, std::uint16_t& a_out) noexcept
        {
            __try {
                a_out = *reinterpret_cast<const std::uint16_t*>(a_at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        [[nodiscard]] bool TryReadU8(
            const void* a_at, std::uint8_t& a_out) noexcept
        {
            __try {
                a_out = *reinterpret_cast<const std::uint8_t*>(a_at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void LogOffendingCell(void* a_cell)
        {
            const auto* base = static_cast<const std::uint8_t*>(a_cell);
            std::uintptr_t vtable = 0;
            std::uintptr_t worldSpace = 0;
            std::uint16_t flags = 0;
            std::uint8_t state = 0;
            const bool vtableOk = a_cell && TryReadPointer(base, vtable);
            const bool wsOk =
                a_cell && TryReadPointer(base + kCellWorldSpace, worldSpace);
            const bool flagsOk =
                a_cell && TryReadU16(base + kCellFlags, flags);
            const bool stateOk =
                a_cell && TryReadU8(base + kCellState, state);
            logger::error(
                "CellWorldspaceGuard: blocked null-worldspace unregister "
                "(cell={}, vtable={}, flags={}, state={}, worldSpace={}) "
                "- crash averted",
                fmt::ptr(a_cell),
                vtableOk ? fmt::format("{:#x}", vtable) : "unreadable",
                flagsOk ? fmt::format("{:#x}", flags) : "unreadable",
                stateOk ? fmt::format("{:#x}", state) : "unreadable",
                wsOk ? fmt::format("{:#x}", worldSpace) : "unreadable");
            // Classify by WHERE the vtable points, not merely whether it is
            // non-null. A real TESObjectCELL vtable lives in the Fallout4VR.exe
            // image; a heap address there means the block is recycled or
            // uninitialised memory that merely happens to be non-zero. The
            // first field capture showed vtable=0x273adf95100 for a cell at
            // 0x273adf95600 - a heap pointer 0x500 bytes below the object, i.e.
            // adjacent heap data, not a vtable at all. An earlier version of
            // this message called that "a live object" and was wrong.
            if (vtableOk) {
                // .rdata is where MSVC emits vtables, so this is a tighter and
                // more meaningful test than "somewhere in the module".
                const auto rdata =
                    REL::Module::get().segment(REL::Segment::rdata);
                const auto rdataStart = rdata.address();
                const auto rdataEnd = rdataStart + rdata.size();
                const bool inImage =
                    rdataStart != 0 &&
                    vtable >= rdataStart && vtable < rdataEnd;
                logger::error(
                    "CellWorldspaceGuard: vtable {:#x} is {} -> {}",
                    vtable,
                    vtable == 0 ? "NULL"
                        : inImage ? "in .rdata (a real vtable)"
                                  : "a HEAP address (not a vtable)",
                    (vtable == 0 || !inImage)
                        ? "recycled/uninitialised memory - this is not a live "
                          "cell; TES->attachedCells holds a stale or freed slot"
                        : "a live object registered without a worldspace "
                          "(initialisation-order bug)");
            }
        }

        void __fastcall HookedCellUnregister(void* a_worldSpace, void* a_cell)
        {
            if (!a_worldSpace) {
                // Exactly what the engine does at its own guard-failure label
                // (0x47BA6E): nothing. Unregistering a cell from a worldspace
                // it was never in has no work to do.
                const int seen =
                    s_intercepts.fetch_add(1, std::memory_order_relaxed);
                if (seen < 5) {
                    LogOffendingCell(a_cell);
                }
                return;
            }
            if (s_original) {
                s_original(a_worldSpace, a_cell);
            }
        }
    }

    int CellWorldspaceGuard::InterceptCount()
    {
        return s_intercepts.load(std::memory_order_relaxed);
    }

    bool CellWorldspaceGuard::Install()
    {
        if (s_installed) return true;
        // VR-only: the address is a hard 1.2.72 offset. Flat builds resolve a
        // different function here and must never be patched with it.
        if (!REL::Module::IsVR()) {
            return false;
        }

        const MH_STATUS initSt = MH_Initialize();
        if (initSt != MH_OK && initSt != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error(
                "CellWorldspaceGuard: MH_Initialize failed: {}", int(initSt));
            return false;
        }

        REL::Relocation<void*> target{
            REL::Offset(kCellUnregister_Offset_VR)
        };
        void* targetAddr = reinterpret_cast<void*>(target.address());

        // Verify the first bytes match the function we analysed before
        // redirecting it. A wrong address here would corrupt cell teardown,
        // which is far worse than the crash being guarded.
        //   0x47B9B0  mov [rsp+8], rbx    48 89 5C 24 08
        //   0x47B9B5  mov [rsp+10h], rbp  48 89 6C 24 10
        static constexpr std::uint8_t kExpected[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10
        };
        const auto* actual = static_cast<const std::uint8_t*>(targetAddr);
        for (std::size_t i = 0; i < sizeof(kExpected); ++i) {
            if (actual[i] != kExpected[i]) {
                logger::error(
                    "CellWorldspaceGuard: prologue mismatch at {:x} "
                    "(byte {} = {:#x}, expected {:#x}); NOT hooking",
                    target.address(), i, actual[i], kExpected[i]);
                return false;
            }
        }

        MH_STATUS st = MH_CreateHook(
            targetAddr,
            reinterpret_cast<void*>(&HookedCellUnregister),
            reinterpret_cast<void**>(&s_original));
        if (st != MH_OK) {
            logger::error(
                "CellWorldspaceGuard: MH_CreateHook failed: {}", int(st));
            return false;
        }
        if (!s_original) {
            logger::error(
                "CellWorldspaceGuard: no original trampoline returned");
            MH_RemoveHook(targetAddr);
            return false;
        }
        st = MH_EnableHook(targetAddr);
        if (st != MH_OK) {
            logger::error(
                "CellWorldspaceGuard: MH_EnableHook failed: {}", int(st));
            MH_RemoveHook(targetAddr);
            return false;
        }

        s_installed = true;
        logger::info(
            "CellWorldspaceGuard: installed at {:x} (null-worldspace cell "
            "unregister; guards the vanilla VR worldspace-transition CTD)",
            target.address());
        return true;
    }
}
