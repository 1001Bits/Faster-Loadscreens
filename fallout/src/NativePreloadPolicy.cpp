#include "PCH.h"
#include "NativePreloadPolicy.h"

#include <F4SE/Trampoline.h>
#include <MinHook.h>

#include <array>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace VRLoadingScreens
{
    namespace
    {
        using PreloadWorldFn =
            void (*)(void*, RE::TESWorldSpace*, int, int, bool);

        // The instruction immediately before the native linked-area call to
        // TES::PreloadWorld writes its fifth (queueOnly) argument at [rsp+20].
        // OG/VR use an immediate byte, so the policy can toggle only that byte
        // atomically even while the game is running.
        constexpr std::uintptr_t kArgumentWriteOG = 0xEC5645;
        constexpr std::uintptr_t kArgumentWriteVR = 0xF36265;
        constexpr std::uintptr_t kPreloadWorldCallOG = 0xEC5652;
        constexpr std::uintptr_t kPreloadWorldCallVR = 0xF36272;
        constexpr std::uintptr_t kPreloadWorldReturnOG = 0xEC5657;
        constexpr std::uintptr_t kPreloadWorldReturnVR = 0xF36277;
        constexpr std::uintptr_t kPreloadWorldOG = 0xFC0D0;
        constexpr std::uintptr_t kPreloadWorldVR = 0xFC0A0;

        constexpr std::array<std::uint8_t, 5> kOriginalOGVR{
            0xC6, 0x44, 0x24, 0x20, 0x00
        };
        constexpr std::uint8_t kFullGrid = 0x00;
        constexpr std::uint8_t kSingleArrivalCell = 0x01;

        // Fallout 4 AE/NG 1.11.221.0 and 1.11.240.0, verified in the Combined
        // Ghidra project. Address Library IDs resolve the two function entries;
        // all interior offsets below are invariant between those builds:
        //
        // ID 2233254 PlayerCharacter::PreloadLinkedAreas:
        //   1.11.221 RVA D7BC10; 1.11.240 RVA D7BFA0
        // ID 2192104 TES::PreloadWorld:
        //   1.11.221 RVA 2D4F20; 1.11.240 RVA 2D5240
        //
        // PlayerCharacter::PreloadLinkedAreas + 0x2B0:
        //   caller+2B0  40 88 7c 24 20       mov [rsp+20],dil
        //   ...
        //   caller+2C2  e8 ?? ?? ?? ??       call TES::PreloadWorld
        //   caller+2C7                         native return address
        //
        // Rewriting MOV [rsp+20],dil is unsafe as a live five-byte patch. Hook
        // TES::PreloadWorld instead and change queueOnly only when _ReturnAddress
        // is this one exact native linked-area call. All other callers preserve
        // their original fifth argument.
        constexpr std::uint64_t kNativeLinkedCallerIDNG = 2233254;
        constexpr std::uint64_t kPreloadWorldIDNG = 2192104;
        constexpr std::uintptr_t kArgumentWriteOffsetNG = 0x2B0;
        constexpr std::uintptr_t kPreloadWorldCallOffsetNG = 0x2C2;
        constexpr std::uintptr_t kPreloadWorldReturnOffsetNG = 0x2C7;

        // Stop before the first RIP-relative displacement. The entry's first 16
        // bytes are invariant, while later control-flow displacements move
        // between 1.11.221 and 1.11.240.
        constexpr std::array<std::uint8_t, 16> kNativeLinkedCallerNGBytes{
            0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0x6B, 0xA1,
            0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00, 0x80
        };
        constexpr std::array<std::uint8_t, 5> kArgumentWriteNGBytes{
            0x40, 0x88, 0x7C, 0x24, 0x20
        };
        constexpr std::array<std::uint8_t, 24> kPreloadWorldNGBytes{
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0x54, 0x02, 0x00,
            0x00, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x41,
            0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80
        };

        constexpr std::uint64_t kThrottleCooldownMs = 1000;
        constexpr std::uint64_t kThrottleSummaryIntervalMs = 5000;
        constexpr std::size_t kThrottleCapacity = 32;

        struct NativeDestinationKey
        {
            std::uintptr_t world = 0;
            int gridX = 0;
            int gridY = 0;
            bool queueOnly = false;

            [[nodiscard]] bool operator==(
                const NativeDestinationKey&) const noexcept = default;
        };

        struct ThrottleEntry
        {
            bool valid = false;
            NativeDestinationKey key{};
            std::uint64_t lastForwardedMs = 0;
        };

        struct ThrottleDecision
        {
            bool forward = true;
            bool emitSummary = false;
            std::uint64_t summaryWindowMs = 0;
            std::uint64_t summaryForwarded = 0;
            std::uint64_t summarySuppressed = 0;
        };

        enum class RuntimePolicy : std::uint8_t
        {
            kNone,
            kLegacyByte,
            kNGHook
        };

        std::mutex s_mutex;
        // Fail closed until full mode opts in. In an active startup, Install()
        // places the verified pass-through caller hook once; runtime policy
        // changes never create/remove that hook.
        std::atomic<bool> s_wanted{ false };
        RuntimePolicy s_runtime = RuntimePolicy::kNone;
        bool s_installed = false;
        bool s_patched = false;
        std::uintptr_t s_address = 0;
        std::uint8_t s_original = kFullGrid;

        bool s_legacyCallsiteInstalled = false;
        std::uintptr_t s_legacyCallsiteAddress = 0;
        std::uintptr_t s_legacyExpectedPreloadWorld = 0;
        std::uintptr_t s_legacyExpectedReturnAddress = 0;
        std::array<std::uint8_t, 5> s_legacyOriginalCall{};
        std::array<std::uint8_t, 5> s_legacyOwnedCall{};
        std::atomic<PreloadWorldFn> s_legacyPreloadWorld{ nullptr };

        bool s_ngHookCreated = false;
        bool s_ngHookEnabled = false;
        bool s_ngHookEverCallable = false;
        bool s_ngHookRetained = false;
        void* s_ngHookTarget = nullptr;
        std::uintptr_t s_ngNativeCallerAddress = 0;
        std::atomic<std::uintptr_t> s_ngNativeReturnAddress{ 0 };
        PreloadWorldFn s_originalPreloadWorld = nullptr;

        std::mutex s_throttleMutex;
        std::array<ThrottleEntry, kThrottleCapacity> s_throttleEntries{};
        std::uint64_t s_throttleSummaryStartedMs = 0;
        std::uint64_t s_throttleForwarded = 0;
        std::uint64_t s_throttleSuppressed = 0;

        template <std::size_t N>
        [[nodiscard]] bool ReadBytes(
            std::uintptr_t a_address,
            std::array<std::uint8_t, N>& a_output) noexcept
        {
            bool ok = false;
            __try {
                std::memcpy(
                    a_output.data(),
                    reinterpret_cast<const void*>(a_address),
                    a_output.size());
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        template <std::size_t N>
        [[nodiscard]] bool HasExactBytes(
            std::uintptr_t a_address,
            const std::array<std::uint8_t, N>& a_expected) noexcept
        {
            std::array<std::uint8_t, N> observed{};
            return ReadBytes(a_address, observed) &&
                observed == a_expected;
        }

        [[nodiscard]] bool DecodeDirectCallTarget(
            std::uintptr_t a_callAddress,
            std::uintptr_t& a_target,
            std::uintptr_t& a_returnAddress) noexcept
        {
            std::array<std::uint8_t, 5> call{};
            if (!ReadBytes(a_callAddress, call) || call[0] != 0xE8) {
                return false;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
            a_returnAddress = a_callAddress + call.size();
            a_target = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(a_returnAddress) +
                static_cast<std::intptr_t>(displacement));
            return true;
        }

        void ResetThrottleState(const char* a_reason = "reset") noexcept
        {
            std::uint64_t windowMs = 0;
            std::uint64_t forwarded = 0;
            std::uint64_t suppressed = 0;
            {
                std::lock_guard lock(s_throttleMutex);
                const auto now =
                    static_cast<std::uint64_t>(GetTickCount64());
                windowMs = s_throttleSummaryStartedMs != 0
                    ? now - s_throttleSummaryStartedMs
                    : 0;
                forwarded = s_throttleForwarded;
                suppressed = s_throttleSuppressed;
                s_throttleEntries = {};
                s_throttleSummaryStartedMs = 0;
                s_throttleForwarded = 0;
                s_throttleSuppressed = 0;
            }
            if (suppressed != 0) {
                logger::info(
                    "NativePreloadPolicy: native-linked PreloadWorld throttle "
                    "boundary summary reason={} windowMs={} forwarded={} "
                    "suppressed={} cooldownMs={}",
                    a_reason ? a_reason : "reset",
                    windowMs,
                    forwarded,
                    suppressed,
                    kThrottleCooldownMs);
            }
        }

        [[nodiscard]] ThrottleDecision CheckNativeThrottle(
            const NativeDestinationKey& a_key) noexcept
        {
            ThrottleDecision decision{};
            const auto now = static_cast<std::uint64_t>(GetTickCount64());

            {
                std::lock_guard lock(s_throttleMutex);
                if (s_throttleSummaryStartedMs == 0) {
                    s_throttleSummaryStartedMs = now;
                }

                ThrottleEntry* matching = nullptr;
                ThrottleEntry* replacement = nullptr;
                std::uint64_t replacementAge = 0;
                for (auto& entry : s_throttleEntries) {
                    if (entry.valid && entry.key == a_key) {
                        matching = &entry;
                        break;
                    }
                    if (!entry.valid) {
                        replacement = &entry;
                        replacementAge = UINT64_MAX;
                        continue;
                    }
                    if (replacementAge != UINT64_MAX) {
                        const auto age = now - entry.lastForwardedMs;
                        if (!replacement || age > replacementAge) {
                            replacement = &entry;
                            replacementAge = age;
                        }
                    }
                }

                if (matching &&
                    now - matching->lastForwardedMs < kThrottleCooldownMs) {
                    decision.forward = false;
                    ++s_throttleSuppressed;
                } else {
                    auto* entry = matching ? matching : replacement;
                    if (entry) {
                        // Publish the timestamp while holding the cache lock, then
                        // release it before entering Fallout. Concurrent callers for
                        // the same destination therefore cannot both submit.
                        entry->valid = true;
                        entry->key = a_key;
                        entry->lastForwardedMs = now;
                    }
                    ++s_throttleForwarded;
                }

                const auto summaryWindow =
                    now - s_throttleSummaryStartedMs;
                if (summaryWindow >= kThrottleSummaryIntervalMs &&
                    s_throttleSuppressed != 0) {
                    decision.emitSummary = true;
                    decision.summaryWindowMs = summaryWindow;
                    decision.summaryForwarded = s_throttleForwarded;
                    decision.summarySuppressed = s_throttleSuppressed;
                    s_throttleSummaryStartedMs = now;
                    s_throttleForwarded = 0;
                    s_throttleSuppressed = 0;
                }
            }
            return decision;
        }

        void LogThrottleSummary(
            const ThrottleDecision& a_decision) noexcept
        {
            if (!a_decision.emitSummary) {
                return;
            }
            logger::info(
                "NativePreloadPolicy: native-linked PreloadWorld throttle "
                "summary windowMs={} forwarded={} suppressed={} cooldownMs={}",
                a_decision.summaryWindowMs,
                a_decision.summaryForwarded,
                a_decision.summarySuppressed,
                kThrottleCooldownMs);
        }

        [[nodiscard]] bool ValidateCurrentInstruction(
            std::uint8_t a_expectedImmediate) noexcept
        {
            if (s_address < 4) {
                return false;
            }
            auto expected = kOriginalOGVR;
            expected[4] = a_expectedImmediate;
            std::array<std::uint8_t, 5> observed{};
            return ReadBytes(s_address - 4, observed) &&
                observed == expected;
        }

        [[nodiscard]] bool WriteByte(
            std::uint8_t a_expectedCurrent,
            std::uint8_t a_byte) noexcept
        {
            if (!s_address ||
                !ValidateCurrentInstruction(a_expectedCurrent)) {
                return false;
            }
            bool ok = false;
            __try {
                REL::safe_write(s_address, &a_byte, sizeof(a_byte));
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(s_address),
                    sizeof(a_byte));
                ok = ValidateCurrentInstruction(a_byte);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool ResolveNGAddresses(
            std::uintptr_t& a_caller,
            std::uintptr_t& a_preloadWorld) noexcept
        {
            a_caller = 0;
            a_preloadWorld = 0;
            try {
                REL::Relocation<std::uintptr_t> caller{
                    REL::ID(kNativeLinkedCallerIDNG)
                };
                REL::Relocation<std::uintptr_t> preloadWorld{
                    REL::ID(kPreloadWorldIDNG)
                };
                a_caller = caller.address();
                a_preloadWorld = preloadWorld.address();
            } catch (...) {
                logger::error(
                    "NativePreloadPolicy: AE/NG Address Library resolution "
                    "failed (caller ID {}, target ID {}); no hook placed",
                    kNativeLinkedCallerIDNG,
                    kPreloadWorldIDNG);
                return false;
            }
            if (!a_caller || !a_preloadWorld) {
                logger::error(
                    "NativePreloadPolicy: AE/NG Address Library returned a "
                    "null address (caller={:#x}, target={:#x}); no hook placed",
                    a_caller,
                    a_preloadWorld);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidateNGSites(
            std::uintptr_t a_caller,
            std::uintptr_t a_preloadWorld) noexcept
        {
            if (!a_caller || !a_preloadWorld) {
                return false;
            }
            const auto argument = a_caller + kArgumentWriteOffsetNG;
            const auto call = a_caller + kPreloadWorldCallOffsetNG;
            const auto expectedReturn =
                a_caller + kPreloadWorldReturnOffsetNG;

            if (!HasExactBytes(a_caller, kNativeLinkedCallerNGBytes)) {
                logger::error(
                    "NativePreloadPolicy: AE/NG native-linked caller "
                    "signature mismatch at {:#x}; no hook placed",
                    a_caller);
                return false;
            }
            if (!HasExactBytes(argument, kArgumentWriteNGBytes)) {
                logger::error(
                    "NativePreloadPolicy: AE/NG queueOnly argument write "
                    "mismatch at {:#x}; no hook placed",
                    argument);
                return false;
            }

            std::uintptr_t decodedTarget = 0;
            std::uintptr_t decodedReturn = 0;
            if (!DecodeDirectCallTarget(call, decodedTarget, decodedReturn) ||
                decodedTarget != a_preloadWorld ||
                decodedReturn != expectedReturn) {
                logger::error(
                    "NativePreloadPolicy: AE/NG native-linked CALL "
                    "validation failed at {:#x} "
                    "(decoded target={:#x}, return={:#x}); no hook placed",
                    call, decodedTarget, decodedReturn);
                return false;
            }

            // Install and re-enable call this only while our hook is absent or
            // disabled, so the engine entry must be pristine in both cases.
            if (!HasExactBytes(a_preloadWorld, kPreloadWorldNGBytes)) {
                logger::error(
                    "NativePreloadPolicy: AE/NG TES::PreloadWorld "
                    "signature mismatch at {:#x}; no hook placed",
                    a_preloadWorld);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidateLegacyCallsite() noexcept
        {
            if (!s_legacyCallsiteAddress ||
                !s_legacyExpectedPreloadWorld ||
                !s_legacyExpectedReturnAddress) {
                return false;
            }

            std::uintptr_t decodedTarget = 0;
            std::uintptr_t decodedReturn = 0;
            if (!DecodeDirectCallTarget(
                    s_legacyCallsiteAddress,
                    decodedTarget,
                    decodedReturn) ||
                decodedTarget != s_legacyExpectedPreloadWorld ||
                decodedReturn != s_legacyExpectedReturnAddress) {
                logger::error(
                    "NativePreloadPolicy: OG/VR native-linked CALL "
                    "validation failed at {:#x} "
                    "(decoded target={:#x}, return={:#x}); no hook placed",
                    s_legacyCallsiteAddress,
                    decodedTarget,
                    decodedReturn);
                return false;
            }
            return true;
        }

        void HookedLegacyPreloadWorld(
            void* a_tes,
            RE::TESWorldSpace* a_world,
            int a_x,
            int a_y,
            bool a_queueOnly)
        {
            // The legacy immediate is retained as the authority for queueOnly.
            // Gate only the patched single-cell form so a policy-off/full-grid
            // call is always a byte-for-byte pass-through.
            if (a_world && a_queueOnly &&
                s_wanted.load(std::memory_order_acquire)) {
                const auto decision = CheckNativeThrottle({
                    reinterpret_cast<std::uintptr_t>(a_world),
                    a_x,
                    a_y,
                    a_queueOnly
                });
                LogThrottleSummary(decision);
                if (!decision.forward) {
                    return;
                }
            }

            // Published before write_call. This is a raw engine function (not a
            // disposable trampoline), so retain it for process lifetime. A thread
            // can cross the patched CALL immediately before shutdown restores the
            // five bytes and reach this load only afterward.
            const auto original =
                s_legacyPreloadWorld.load(std::memory_order_acquire);
            if (original) {
                original(a_tes, a_world, a_x, a_y, a_queueOnly);
            }
        }

        [[nodiscard]] bool InstallLegacyCallsiteLocked() noexcept
        {
            if (s_legacyCallsiteInstalled) {
                return true;
            }
            if (!ValidateLegacyCallsite()) {
                return false;
            }
            if (!ReadBytes(
                    s_legacyCallsiteAddress,
                    s_legacyOriginalCall)) {
                logger::error(
                    "NativePreloadPolicy: failed to save OG/VR native-linked "
                    "CALL bytes at {:#x}; no hook placed",
                    s_legacyCallsiteAddress);
                return false;
            }

            auto& trampoline = F4SE::GetTrampoline();
            // CommonLib's five-byte CALL always allocates one 14-byte absolute
            // branch island. Check capacity first because its allocator treats
            // exhaustion as fatal rather than returning a recoverable error.
            constexpr std::size_t kBranchIslandBytes = 14;
            if (trampoline.free_size() < kBranchIslandBytes) {
                logger::error(
                    "NativePreloadPolicy: F4SE trampoline has only {} byte(s) "
                    "free; need {} for OG/VR native-linked CALL hook",
                    trampoline.free_size(),
                    kBranchIslandBytes);
                return false;
            }

            s_legacyPreloadWorld.store(
                reinterpret_cast<PreloadWorldFn>(
                    s_legacyExpectedPreloadWorld),
                std::memory_order_release);

            const auto originalTarget = trampoline.write_call<5>(
                s_legacyCallsiteAddress,
                reinterpret_cast<std::uintptr_t>(
                    &HookedLegacyPreloadWorld));
            if (originalTarget != s_legacyExpectedPreloadWorld) {
                logger::error(
                    "NativePreloadPolicy: OG/VR native-linked CALL hook "
                    "returned unexpected original target {:#x} "
                    "(expected {:#x}); restoring CALL bytes",
                    originalTarget,
                    s_legacyExpectedPreloadWorld);
                REL::safe_write(
                    s_legacyCallsiteAddress,
                    s_legacyOriginalCall.data(),
                    s_legacyOriginalCall.size());
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(
                        s_legacyCallsiteAddress),
                    s_legacyOriginalCall.size());
                return false;
            }

            s_legacyCallsiteInstalled = true;
            if (!ReadBytes(
                    s_legacyCallsiteAddress,
                    s_legacyOwnedCall) ||
                s_legacyOwnedCall == s_legacyOriginalCall) {
                logger::error(
                    "NativePreloadPolicy: OG/VR native-linked CALL hook "
                    "ownership bytes could not be captured; restoring original");
                REL::safe_write(
                    s_legacyCallsiteAddress,
                    s_legacyOriginalCall.data(),
                    s_legacyOriginalCall.size());
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(
                        s_legacyCallsiteAddress),
                    s_legacyOriginalCall.size());
                s_legacyCallsiteInstalled = false;
                s_legacyOwnedCall = {};
                return false;
            }
            logger::info(
                "NativePreloadPolicy: OG/VR native-linked PreloadWorld "
                "callsite throttle installed (call={:#x}, target={:#x}, "
                "cooldownMs={}, capacity={})",
                s_legacyCallsiteAddress,
                s_legacyExpectedPreloadWorld,
                kThrottleCooldownMs,
                kThrottleCapacity);
            return true;
        }

        [[nodiscard]] bool RestoreLegacyCallsiteLocked() noexcept
        {
            if (!s_legacyCallsiteInstalled) {
                return true;
            }

            std::array<std::uint8_t, 5> observedOwned{};
            if (!ReadBytes(
                    s_legacyCallsiteAddress,
                    observedOwned) ||
                observedOwned != s_legacyOwnedCall) {
                logger::error(
                    "NativePreloadPolicy: OG/VR CALL ownership lost at "
                    "{:#x}; refusing to overwrite another patch",
                    s_legacyCallsiteAddress);
                return false;
            }

            bool restored = false;
            __try {
                REL::safe_write(
                    s_legacyCallsiteAddress,
                    s_legacyOriginalCall.data(),
                    s_legacyOriginalCall.size());
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(
                        s_legacyCallsiteAddress),
                    s_legacyOriginalCall.size());
                std::array<std::uint8_t, 5> observed{};
                restored =
                    ReadBytes(s_legacyCallsiteAddress, observed) &&
                    observed == s_legacyOriginalCall;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                restored = false;
            }

            if (!restored) {
                logger::error(
                    "NativePreloadPolicy: failed to restore OG/VR "
                    "native-linked CALL bytes at {:#x}",
                    s_legacyCallsiteAddress);
                return false;
            }

            s_legacyCallsiteInstalled = false;
            // Do not clear s_legacyPreloadWorld. An invocation which crossed the
            // old CALL before the restore can still be inside our detour, and the
            // engine target remains valid for the lifetime of Fallout.
            s_legacyOwnedCall = {};
            return true;
        }

        void HookedPreloadWorld(
            void* a_tes,
            RE::TESWorldSpace* a_world,
            int a_x,
            int a_y,
            bool a_queueOnly)
        {
            const auto returnAddress =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if (returnAddress ==
                    s_ngNativeReturnAddress.load(std::memory_order_acquire) &&
                s_wanted.load(std::memory_order_acquire)) {
                a_queueOnly = true;
                if (a_world) {
                    const auto decision = CheckNativeThrottle({
                        reinterpret_cast<std::uintptr_t>(a_world),
                        a_x,
                        a_y,
                        a_queueOnly
                    });
                    LogThrottleSummary(decision);
                    if (!decision.forward) {
                        return;
                    }
                }
            }

            // Publish before MH_EnableHook. Once the hook has ever been callable,
            // retain this pointer with MinHook's disabled relay for process
            // lifetime so a thread already committed to that relay stays safe.
            const auto original = s_originalPreloadWorld;
            if (original) {
                original(a_tes, a_world, a_x, a_y, a_queueOnly);
            }
        }

        [[nodiscard]] bool SetNGHookEnabledLocked(bool a_enable) noexcept
        {
            if (!s_ngHookTarget) {
                return false;
            }
            if (a_enable && s_ngHookRetained) {
                logger::error(
                    "NativePreloadPolicy: refusing to re-enable retained "
                    "AE/NG TES::PreloadWorld hook");
                return false;
            }
            if (a_enable == s_ngHookEnabled) {
                if (!a_enable && s_ngHookEverCallable) {
                    s_ngHookRetained = true;
                }
                return true;
            }

            if (a_enable) {
                if (!ValidateNGSites(
                        s_ngNativeCallerAddress,
                        reinterpret_cast<std::uintptr_t>(s_ngHookTarget))) {
                    return false;
                }

                if (!s_ngHookCreated) {
                    const auto initStatus = MH_Initialize();
                    if (initStatus != MH_OK &&
                        initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                        logger::error(
                            "NativePreloadPolicy: AE/NG MH_Initialize "
                            "failed: {}",
                            MH_StatusToString(initStatus));
                        return false;
                    }

                    const auto createStatus = MH_CreateHook(
                        s_ngHookTarget,
                        reinterpret_cast<void*>(&HookedPreloadWorld),
                        reinterpret_cast<void**>(&s_originalPreloadWorld));
                    if (createStatus != MH_OK ||
                        !s_originalPreloadWorld) {
                        logger::error(
                            "NativePreloadPolicy: AE/NG "
                            "MH_CreateHook(TES::PreloadWorld) failed: {}",
                            MH_StatusToString(createStatus));
                        if (createStatus == MH_OK) {
                            (void)MH_RemoveHook(s_ngHookTarget);
                        }
                        s_originalPreloadWorld = nullptr;
                        return false;
                    }
                    s_ngHookCreated = true;
                }

                const auto enableStatus = MH_EnableHook(s_ngHookTarget);
                if (enableStatus != MH_OK) {
                    logger::error(
                        "NativePreloadPolicy: AE/NG "
                        "MH_EnableHook(TES::PreloadWorld) failed: {}",
                        MH_StatusToString(enableStatus));
                    return false;
                }
                s_ngHookEnabled = true;
                s_ngHookEverCallable = true;
                logger::info(
                    "NativePreloadPolicy: AE/NG native-linked exterior "
                    "destination mode=engine-single-arrival-cell "
                    "(TES::PreloadWorld hook={:#x}, caller return={:#x})",
                    reinterpret_cast<std::uintptr_t>(s_ngHookTarget),
                    s_ngNativeReturnAddress.load(std::memory_order_acquire));
                return true;
            }

            const auto disableStatus = MH_DisableHook(s_ngHookTarget);
            if (disableStatus != MH_OK) {
                logger::error(
                    "NativePreloadPolicy: AE/NG "
                    "MH_DisableHook(TES::PreloadWorld) failed: {}",
                    MH_StatusToString(disableStatus));
                return false;
            }
            s_ngHookEnabled = false;
            if (s_ngHookEverCallable) {
                // A thread can already have followed MinHook's patched entry or
                // relay without having reached our detour body yet. There is no
                // race-free in-detour counter which can prove that path idle.
                // Keep MinHook's disabled relay/trampoline and the original
                // function pointer alive for the rest of the process.
                s_ngHookRetained = true;
            }
            logger::info(
                "NativePreloadPolicy: AE/NG native-linked exterior "
                "destination mode=engine-native-uGrids");
            return true;
        }

        [[nodiscard]] bool ApplyWantedLocked() noexcept
        {
            if (!s_installed) {
                return true;
            }
            const bool want = s_wanted.load(std::memory_order_acquire);
            if (s_runtime == RuntimePolicy::kNGHook) {
                // The verified hook is installed and enabled exactly once during
                // startup. Runtime policy changes are a pass-through atomic in
                // HookedPreloadWorld, never a hot executable-code rewrite.
                return s_ngHookEnabled;
            }
            if (s_runtime != RuntimePolicy::kLegacyByte) {
                return false;
            }
            // The five-byte CALL is likewise installed only during Install().
            // Runtime changes below touch only the verified one-byte immediate.
            if (!s_legacyCallsiteInstalled) return false;
            if (want == s_patched) {
                return true;
            }
            const auto current = s_patched
                ? kSingleArrivalCell
                : s_original;
            const auto byte = want ? kSingleArrivalCell : s_original;
            if (!WriteByte(current, byte)) {
                logger::error(
                    "NativePreloadPolicy: instruction validation/write failed "
                    "at {:#x}; "
                    "state unchanged",
                    s_address);
                return false;
            }
            s_patched = want;
            logger::info(
                "NativePreloadPolicy: native linked exterior-destination mode={} "
                "(address={:#x})",
                want ? "engine-single-arrival-cell" : "engine-native-uGrids",
                s_address);
            return true;
        }

        struct LifetimeGuard
        {
            ~LifetimeGuard()
            {
                NativePreloadPolicy::Shutdown();
            }
        } s_lifetimeGuard;
    }

    bool NativePreloadPolicy::Install() noexcept
    {
        std::lock_guard lock(s_mutex);
        if (s_installed) {
            return true;
        }
        if (s_ngHookRetained || s_ngHookCreated) {
            logger::error(
                "NativePreloadPolicy: retained AE/NG hook object already "
                "exists; refusing to install or re-enable it");
            return false;
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
        const bool exactNG221 =
            !REL::Module::IsVR() && REL::Module::IsNG() &&
            version[0] == 1 && version[1] == 11 &&
            version[2] == 221 && version[3] == 0;
        const bool exactNG240 =
            !REL::Module::IsVR() && REL::Module::IsNG() &&
            version[0] == 1 && version[1] == 11 &&
            version[2] == 240 && version[3] == 0;
        const bool exactNG = exactNG221 || exactNG240;
        if (!exactOG && !exactVR && !exactNG) {
            logger::warn(
                "NativePreloadPolicy: unsupported runtime "
                "{}.{}.{}.{}; no code changed",
                version[0], version[1], version[2], version[3]);
            return false;
        }

        const auto base = REL::Module::get().base();
        if (exactNG) {
            std::uintptr_t nativeLinkedCaller = 0;
            std::uintptr_t preloadWorld = 0;
            if (!ResolveNGAddresses(
                    nativeLinkedCaller,
                    preloadWorld) ||
                !ValidateNGSites(nativeLinkedCaller, preloadWorld)) {
                return false;
            }
            s_runtime = RuntimePolicy::kNGHook;
            s_ngNativeCallerAddress = nativeLinkedCaller;
            s_ngHookTarget = reinterpret_cast<void*>(preloadWorld);
            s_ngNativeReturnAddress.store(
                nativeLinkedCaller + kPreloadWorldReturnOffsetNG,
                std::memory_order_release);
            s_installed = true;
            // Keep the filtered hook enabled even when the policy starts off;
            // HookedPreloadWorld is then a byte-for-byte pass-through controlled
            // solely by s_wanted.
            if (!SetNGHookEnabledLocked(true) ||
                !ApplyWantedLocked()) {
                if (s_ngHookEnabled) {
                    if (!SetNGHookEnabledLocked(false)) {
                        logger::error(
                            "NativePreloadPolicy: AE/NG startup rollback "
                            "could not disable owned hook");
                        return false;
                    }
                }
                if (s_ngHookCreated) {
                    if (s_ngHookEverCallable) {
                        s_ngHookRetained = true;
                    } else {
                        const auto removeStatus =
                            MH_RemoveHook(s_ngHookTarget);
                        if (removeStatus != MH_OK) {
                            logger::error(
                                "NativePreloadPolicy: AE/NG startup rollback "
                                "could not remove never-enabled owned hook: {}",
                                MH_StatusToString(removeStatus));
                            return false;
                        }
                        s_ngHookCreated = false;
                        s_originalPreloadWorld = nullptr;
                    }
                }
                s_installed = false;
                s_runtime = RuntimePolicy::kNone;
                if (!s_ngHookRetained) {
                    s_ngHookTarget = nullptr;
                    s_ngNativeCallerAddress = 0;
                    s_ngNativeReturnAddress.store(
                        0, std::memory_order_release);
                }
                return false;
            }
            logger::info(
                "NativePreloadPolicy: AE/NG {}.{}.{}.{} native-linked "
                "callsite validated via Address Library IDs "
                "(caller ID {}={:#x}, call={:#x}, target ID {}={:#x})",
                version[0], version[1], version[2], version[3],
                kNativeLinkedCallerIDNG,
                nativeLinkedCaller,
                nativeLinkedCaller + kPreloadWorldCallOffsetNG,
                kPreloadWorldIDNG,
                preloadWorld);
            return true;
        }

        const auto instructionAddress =
            base + (exactVR ? kArgumentWriteVR : kArgumentWriteOG);
        const auto callsiteAddress =
            base + (exactVR ? kPreloadWorldCallVR : kPreloadWorldCallOG);
        const auto expectedPreloadWorld =
            base + (exactVR ? kPreloadWorldVR : kPreloadWorldOG);
        const auto expectedReturnAddress =
            base + (exactVR
                ? kPreloadWorldReturnVR
                : kPreloadWorldReturnOG);
        std::array<std::uint8_t, 5> observed{};
        if (!ReadBytes(instructionAddress, observed) ||
            observed != kOriginalOGVR) {
            logger::error(
                "NativePreloadPolicy: native callsite validation failed "
                "at {:#x}; no code changed",
                instructionAddress);
            s_address = 0;
            return false;
        }

        s_legacyCallsiteAddress = callsiteAddress;
        s_legacyExpectedPreloadWorld = expectedPreloadWorld;
        s_legacyExpectedReturnAddress = expectedReturnAddress;
        if (!ValidateLegacyCallsite()) {
            s_legacyCallsiteAddress = 0;
            s_legacyExpectedPreloadWorld = 0;
            s_legacyExpectedReturnAddress = 0;
            return false;
        }

        // Only the immediate operand changes (00 <-> 01). A one-byte aligned
        // memory write cannot expose a partially rewritten instruction.
        s_address = instructionAddress + 4;
        s_original = kFullGrid;
        s_runtime = RuntimePolicy::kLegacyByte;
        s_installed = true;
        if (!InstallLegacyCallsiteLocked() ||
            !ApplyWantedLocked()) {
            if (s_legacyCallsiteInstalled &&
                !RestoreLegacyCallsiteLocked()) {
                // Retain ownership state if rollback cannot prove it restored
                // the exact original CALL.
                return false;
            }
            s_installed = false;
            s_runtime = RuntimePolicy::kNone;
            s_address = 0;
            s_original = kFullGrid;
            s_legacyCallsiteAddress = 0;
            s_legacyExpectedPreloadWorld = 0;
            s_legacyExpectedReturnAddress = 0;
            return false;
        }
        logger::info(
            "NativePreloadPolicy: {} native-linked callsite validated "
            "(call={:#x}, target={:#x}, return={:#x}, throttle={})",
            exactVR ? "Fallout4VR 1.2.72" : "Fallout4 1.10.163",
            callsiteAddress,
            expectedPreloadWorld,
            expectedReturnAddress,
            s_legacyCallsiteInstalled ? "installed" : "deferred");
        return true;
    }

    void NativePreloadPolicy::SetExteriorArrivalCellOnly(
        bool a_enabled) noexcept
    {
        const bool previous =
            s_wanted.exchange(a_enabled, std::memory_order_acq_rel);
        if (previous != a_enabled) {
            ResetThrottleState("mode-change");
        }
        std::lock_guard lock(s_mutex);
        (void)ApplyWantedLocked();
    }

    void NativePreloadPolicy::ResetForLoadBoundary() noexcept
    {
        // This is deliberately independent of the executable-byte / MinHook
        // lifecycle mutex. The throttle lock is released before every engine
        // call, so a transition can advance generations without touching or
        // waiting on Fallout's loader.
        ResetThrottleState("load-boundary");
    }

    void NativePreloadPolicy::Shutdown() noexcept
    {
        // Publish pass-through before restoring any executable bytes. A caller
        // already inside either detour will not suppress an engine request.
        s_wanted.store(false, std::memory_order_release);
        std::lock_guard lock(s_mutex);
        if (!s_installed) {
            ResetThrottleState("shutdown-uninstalled");
            return;
        }

        if (s_runtime == RuntimePolicy::kNGHook) {
            if (s_ngHookEnabled) {
                if (!SetNGHookEnabledLocked(false)) {
                    // Keep every target/trampoline value alive if MinHook could
                    // not restore the entry. Clearing them while the detour may
                    // still be callable would turn a recoverable shutdown error
                    // into an invalid jump/call.
                    return;
                }
            }
            if (s_ngHookCreated) {
                if (s_ngHookEverCallable) {
                    // Disable restores the engine entry, but a thread can already
                    // be in MinHook's relay on its way to HookedPreloadWorld.
                    // Never remove/free that relay or clear the original pointer.
                    s_ngHookRetained = true;
                } else {
                    const auto removeStatus =
                        MH_RemoveHook(s_ngHookTarget);
                    if (removeStatus != MH_OK) {
                        logger::error(
                            "NativePreloadPolicy: AE/NG failed to remove "
                            "never-enabled TES::PreloadWorld hook: {}",
                            MH_StatusToString(removeStatus));
                        return;
                    }
                    s_ngHookCreated = false;
                    s_originalPreloadWorld = nullptr;
                }
            }
            s_ngHookEnabled = false;
            if (!s_ngHookRetained) {
                s_ngHookTarget = nullptr;
                s_ngNativeCallerAddress = 0;
                s_ngNativeReturnAddress.store(
                    0, std::memory_order_release);
            }
        } else if (s_runtime == RuntimePolicy::kLegacyByte) {
            if (s_patched) {
                if (WriteByte(kSingleArrivalCell, s_original)) {
                    s_patched = false;
                } else {
                    logger::error(
                        "NativePreloadPolicy: failed to restore original "
                        "queueOnly byte at shutdown");
                    return;
                }
            }
            if (!RestoreLegacyCallsiteLocked()) {
                // Keep the target pointer and all callsite state alive if the
                // original CALL could not be restored.
                return;
            }
        }
        s_installed = false;
        s_runtime = RuntimePolicy::kNone;
        s_address = 0;
        s_original = kFullGrid;
        s_legacyCallsiteAddress = 0;
        s_legacyExpectedPreloadWorld = 0;
        s_legacyExpectedReturnAddress = 0;
        s_legacyOriginalCall = {};
        s_legacyOwnedCall = {};
        ResetThrottleState("shutdown");
    }
}
