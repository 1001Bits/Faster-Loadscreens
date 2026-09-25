#include "PCH.h"
#include "DoorPrefetch.h"
#include "PreloadDiagnostics.h"
#include "RuntimePolicy.h"
#include "WorldspacePreload.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace VRLoadingScreens
{
    namespace
    {
        // PickRefUpdateEvent payload (RE'd from PlayerCharacter::UpdatePickRef's
        // Notify site). Crosshair / wand activate-pick — fires only on change.
        struct PickRefUpdateEvent
        {
            RE::ObjectRefHandle pickRef;          // 0x00
            bool                refChanged;       // 0x04
            bool                pickableChanged;  // 0x05
            bool                unk06;            // 0x06
        };

        // Event-source offsets within PlayerCharacter (RE-verified, three
        // consecutive BSTEventSource bases Δ0x58 on VR):
        //   flat        : +0x5D0  (single PickRefUpdateEvent)
        //   VR primary  : +0x5D8  (UPrimaryPickRefUpdateEvent  — primary wand)
        //   VR secondary: +0x630  (USecondaryPickRefUpdateEvent — off-hand wand)
        //   VR center   : +0x688  (UPickRefUpdateEvent — head/gaze "where you look")
        // Register all three on VR so whichever caster is aimed at a door fires.
        constexpr std::uintptr_t kPickSrcFlat     = 0x5D0;
        constexpr std::uintptr_t kPickSrcVRPrim   = 0x5D8;
        constexpr std::uintptr_t kPickSrcVRSec    = 0x630;
        constexpr std::uintptr_t kPickSrcVRCenter = 0x688;

        // Use Fallout's high-level exterior entry point. Calls must pass the exact
        // runtime's live g_TES value; speculative interior submission is retired.
        using PreloadWorldFn =
            void (*)(void* /*TES*/, RE::TESWorldSpace*, int, int, bool /*queueOnly*/);
        using LoadedTeleportVisitorFn =
            bool (*)(RE::TESObjectREFR*, std::intptr_t);
        using EnumTeleportDoorsFn = void (*)(
            void* /*BGSLoadedReferenceCollection*/,
            RE::NiPoint3&,
            float,
            RE::NiPoint3&,
            float,
            LoadedTeleportVisitorFn,
            std::intptr_t);
        PreloadWorldFn    s_preloadWorld = nullptr;
        void**            s_tesSingletonPtr = nullptr;  // global holding TES*
        EnumTeleportDoorsFn s_enumTeleportDoors = nullptr;
        void** s_loadedReferenceCollectionPtr = nullptr;
        void** s_ngPlayerSingletonPtr = nullptr;

        // Official Fallout 4 NG Address Library IDs shared by the verified
        // 1.11.221.0 and 1.11.240.0 version databases.
        constexpr std::uint64_t kBhkPickDataCtorIDNG = 2230668;
        constexpr std::uint64_t kBhkPickDataSetStartEndIDNG = 2236622;
        constexpr std::uint64_t kCellPickIDNG = 2200263;
        constexpr std::uint64_t kFindReferenceFor3DIDNG = 2201082;
        constexpr std::uint64_t kPlayerCameraGlobalIDNG = 4796065;
        constexpr std::uint64_t kPreloadWorldIDNG = 2192104;
        constexpr std::uint64_t kGameTESGlobalIDNG = 2698044;
        constexpr std::uint64_t kEnumTeleportDoorsIDNG = 2199588;
        constexpr std::uint64_t kLoadedReferencesGlobalIDNG = 4798035;
        constexpr std::uint64_t kPlayerCharacterGlobalIDNG = 2698073;
        constexpr std::uint64_t kObjectRefHandleGetIDNG = 2188681;
        constexpr std::uint64_t kTESObjectREFRGetHandleIDNG = 2201196;
        constexpr std::uint64_t kTESObjectCELLGetDataXIDNG = 2200213;
        constexpr std::uint64_t kTESObjectCELLGetDataYIDNG = 2200214;

        std::atomic<bool> s_exteriorPreloadResolved{ false };
        std::atomic<bool> s_exteriorResolutionAttempted{ false };
        std::atomic<bool> s_gateEnumerationResolved{ false };
        std::atomic<bool> s_gateEnumerationResolutionAttempted{ false };
        // The engine exposes two PreloadWorld modes, not an arbitrary-radius API:
        // queueOnly=true requests the arrival cell; false runs the engine's live
        // grid-switch machinery for the uGridsToLoad square. RETIRED 2026-07-31:
        // queueOnly MUST always be true. The false path mutates live grid state
        // and is only legal in the engine's own transition contexts; calling it
        // from this plugin's exterior-origin poller produced a reproducible CTD
        // at the next real transition (Diamond City exit gate, benchmark run
        // b-all-run1-crash). Every config selector now clamps to single-cell.
        std::atomic<bool> s_exteriorFullGrid{ false };

        // Transition barrier. FlushQueuedLoads publishes true before it drains the
        // plugin's call mutex, so no poller task, pick callback, or direct preload
        // path can race a new engine call against the transition handoff.
        std::atomic<bool> s_transitionActive{ false };
        std::atomic<std::uint64_t> s_transitionStartedMs{ 0 };
        std::atomic<std::uint64_t> s_postTransitionQuietUntilMs{ 0 };
        std::atomic<bool> s_forceGateRescan{ true };
        std::atomic<bool> s_forceRayOriginReset{ true };
        std::mutex s_transitionStateMutex;
        constexpr std::uint64_t kPostTransitionQuietMs = 5000;
        constexpr std::uint64_t kTransitionFailSafeMs = 120000;

        [[nodiscard]] bool IsTransitionBarrierActive() noexcept;
        [[nodiscard]] bool IsPostTransitionQuiet() noexcept;
        [[nodiscard]] bool IsPreloadSuppressed() noexcept;

        // Serializes the final suppression check and both engine preload calls with
        // transition handoff. A published transition barrier therefore drains a
        // call already in flight and prevents a not-yet-started one.
        std::mutex s_submissionMutex;
        [[nodiscard]] bool IsStableAttachedExteriorSource(
            RE::TESObjectCELL* a_cell,
            RE::TESWorldSpace* a_sourceWorld) noexcept;

        [[nodiscard]] bool IsCommittedReadableRegion(
            const void* a_address, std::size_t a_size) noexcept
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!a_address || a_size == 0 ||
                VirtualQuery(
                    a_address, std::addressof(region), sizeof(region)) !=
                    sizeof(region) ||
                region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }
            const auto protection = region.Protect & 0xFF;
            const bool readable =
                protection == PAGE_READONLY ||
                protection == PAGE_READWRITE ||
                protection == PAGE_WRITECOPY ||
                protection == PAGE_EXECUTE_READ ||
                protection == PAGE_EXECUTE_READWRITE ||
                protection == PAGE_EXECUTE_WRITECOPY;
            if (!readable) {
                return false;
            }
            const auto address =
                reinterpret_cast<std::uintptr_t>(a_address);
            const auto start =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (address < start) {
                return false;
            }
            const auto offset = address - start;
            return offset <= region.RegionSize &&
                a_size <= region.RegionSize - offset;
        }

        [[nodiscard]] bool IsCommittedExecutableRegion(
            const void* a_address, std::size_t a_size) noexcept
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!a_address || a_size == 0 ||
                VirtualQuery(
                    a_address, std::addressof(region), sizeof(region)) !=
                    sizeof(region) ||
                region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }
            const auto protection = region.Protect & 0xFF;
            const bool executable =
                protection == PAGE_EXECUTE ||
                protection == PAGE_EXECUTE_READ ||
                protection == PAGE_EXECUTE_READWRITE ||
                protection == PAGE_EXECUTE_WRITECOPY;
            if (!executable) {
                return false;
            }
            const auto address =
                reinterpret_cast<std::uintptr_t>(a_address);
            const auto start =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (address < start) {
                return false;
            }
            const auto offset = address - start;
            return offset <= region.RegionSize &&
                a_size <= region.RegionSize - offset;
        }

        [[nodiscard]] bool IsValidatedDataPointerSlot(
            const void* a_slot) noexcept
        {
            if (!a_slot ||
                !IsCommittedReadableRegion(a_slot, sizeof(void*))) {
                return false;
            }
            const auto data =
                REL::Module::get().segment(REL::Segment::data);
            const auto address =
                reinterpret_cast<std::uintptr_t>(a_slot);
            const auto start = data.address();
            if (address < start || data.size() < sizeof(void*)) {
                return false;
            }
            const auto offset = address - start;
            return offset <= data.size() &&
                sizeof(void*) <= data.size() - offset;
        }

        template <class T>
        [[nodiscard]] T* TryReadPointerSlot(void** a_slot) noexcept
        {
            if (!IsValidatedDataPointerSlot(a_slot)) {
                return nullptr;
            }
            void* value = nullptr;
            __try {
                value = *a_slot;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                value = nullptr;
            }
            return static_cast<T*>(value);
        }

        // RE::PlayerCharacter::GetSingleton() is UNSAFE on NG. Its address-library
        // ID (2690919) is ABSENT from the verified 1.11.221/1.11.240 version bins,
        // and the CROSS_VR build skips exact-ID matching on non-VR runtimes
        // (IDDB::id2offset only verifies it->id == a_id when Module::IsVR()), so
        // lower_bound silently returns the nearest neighbour's offset → a garbage
        // "player" pointer → CTD the instant it is dereferenced (this was the
        // world-entry crash). Resolve the player through the official shared
        // g_PlayerCharacter Address Library ID instead.
        // OG keeps the typed singleton (ID 303410 is present); VR keeps it too (VR
        // does exact matching and reports loudly on a miss, so it can't silently
        // corrupt). Struct member offsets like the +0x5D0 pick-ref source are
        // compile-time and shared OG/NG, so only the singleton lookup is affected.
        RE::PlayerCharacter* GetPlayerForPrefetch()
        {
            if (REL::Module::IsNG()) {
                return TryReadPointerSlot<RE::PlayerCharacter>(
                    s_ngPlayerSingletonPtr);
            }
            return RE::PlayerCharacter::GetSingleton();
        }

        [[nodiscard]] bool IsExactSupportedRuntime() noexcept
        {
            const auto version = REL::Module::get().version();
            if (REL::Module::IsVR()) {
                return version[0] == 1 && version[1] == 2 &&
                    version[2] == 72 && version[3] == 0;
            }
            if (REL::Module::IsNG()) {
                return version[0] == 1 && version[1] == 11 &&
                    (version[2] == 221 || version[2] == 240) &&
                    version[3] == 0;
            }
            return version[0] == 1 && version[1] == 10 &&
                version[2] == 163 && version[3] == 0;
        }

        [[nodiscard]] std::uintptr_t ExpectedNGRva(
            std::uintptr_t a_221,
            std::uintptr_t a_240) noexcept
        {
            const auto version = REL::Module::get().version();
            if (version[0] != 1 || version[1] != 11 || version[3] != 0) {
                return 0;
            }
            if (version[2] == 221) {
                return a_221;
            }
            if (version[2] == 240) {
                return a_240;
            }
            return 0;
        }

        [[nodiscard]] bool MatchesExpectedNGRva(
            std::uintptr_t a_address,
            std::uintptr_t a_221,
            std::uintptr_t a_240) noexcept
        {
            const auto expected = ExpectedNGRva(a_221, a_240);
            const auto base = REL::Module::get().base();
            return expected != 0 && a_address >= base &&
                a_address - base == expected;
        }

        [[nodiscard]] bool MatchesExpectedNGRelocation(
            std::uint64_t a_id,
            std::uintptr_t a_221,
            std::uintptr_t a_240) noexcept
        {
            try {
                const auto address = REL::Relocation<std::uintptr_t>{
                    REL::ID(a_id)
                }.address();
                return MatchesExpectedNGRva(address, a_221, a_240);
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] bool IsResidentOrBusy(RE::TESObjectCELL* a_cell) noexcept
        {
            return a_cell &&
                (a_cell->loadedData != nullptr ||
                 a_cell->cellState != RE::TESObjectCELL::CELL_STATE::kNotLoaded);
        }

        constexpr std::size_t kCooldownPruneInterval = 64;
        // PreloadWorld is void, so one returned call is submission evidence rather
        // than completion evidence. Keep one bounded revalidation opportunity, but
        // only after the player is materially closer or inside the near-door band.
        // This prevents a stationary gate from resubmitting every old 8-second
        // cooldown while retaining a late, high-confidence retry before activation.
        constexpr int kExteriorRetryMinMs = 2000;
        constexpr int kExteriorStateLifetimeMs = 120000;
        constexpr int kExteriorResidentEvidenceMs = 5000;
        constexpr float kExteriorNearDoorDistance = 768.0F;
        constexpr float kExteriorMaterialCloserUnits = 512.0F;
        constexpr float kExteriorMaterialCloserRatio = 0.70F;
        constexpr std::uint8_t kExteriorMaxSubmissions = 2;
        // Presentation evidence is separate from the per-transition submission
        // cooldown. A player can visit one or more interiors before using an
        // already-preloaded exterior gate (Diamond City is the field-proven
        // example), so clearing this at every LoadingMenu OPEN creates a false
        // negative. Source-world reconciliation and a hard entry cap bound the
        // proof without imposing a timer that would fail after a long interior.
        constexpr std::size_t kExteriorPresentationEvidenceLimit = 64;

        struct ExteriorDestinationKey
        {
            std::uint32_t worldFormID = 0;
            int gridX = 0;
            int gridY = 0;
            bool fullGrid = false;

            bool operator==(const ExteriorDestinationKey&) const noexcept =
                default;
        };

        struct ExteriorDestinationKeyHash
        {
            std::size_t operator()(
                const ExteriorDestinationKey& a_key) const noexcept
            {
                // hash-combine the engine-owned world identity, exact XTEL
                // arrival grid, and requested engine mode.
                std::size_t value =
                    std::hash<std::uint32_t>{}(a_key.worldFormID);
                const auto combine = [&value](std::size_t a_component) {
                    value ^= a_component + 0x9E3779B97F4A7C15ULL +
                        (value << 6) + (value >> 2);
                };
                combine(std::hash<int>{}(a_key.gridX));
                combine(std::hash<int>{}(a_key.gridY));
                combine(std::hash<bool>{}(a_key.fullGrid));
                return value;
            }
        };

        struct ExteriorCooldownState
        {
            std::chrono::steady_clock::time_point lastSubmitted{};
            std::chrono::steady_clock::time_point updated{};
            std::chrono::steady_clock::time_point residentObserved{};
            float nearestSubmittedDistance =
                std::numeric_limits<float>::infinity();
            std::uint8_t submissions = 0;
            bool residentOrPending = false;
            bool duplicateLogged = false;
        };

        struct ExteriorPresentationEvidence
        {
            std::uint32_t sourceWorldFormID = 0;
            std::uint64_t sequence = 0;
            bool exactLiveArrivalObserved = false;
        };

        enum class ExteriorSubmissionDisposition : std::uint8_t
        {
            kInitial,
            kRevalidation,
            kRecent,
            kAwaitingCloser,
            kRetryLimit,
            kResidentOrPending
        };

        struct ExteriorSubmissionEvaluation
        {
            ExteriorSubmissionDisposition disposition =
                ExteriorSubmissionDisposition::kInitial;
            std::uint8_t submissions = 0;
        };

        std::mutex s_seenMutex;
        // Exterior cooldowns use the actual engine request identity, so two
        // different doors leading to the same world/grid cannot duplicate work.
        std::unordered_map<
            ExteriorDestinationKey,
            ExteriorCooldownState,
            ExteriorDestinationKeyHash> s_exteriorSeen;
        // Causal proof for LoadingMenu policy. It deliberately outlives ordinary
        // transition cooldown generations so an intervening interior round trip
        // cannot erase the exact exterior request. The target is consumed on its
        // first PositionPlayerJob arrival and session boundaries clear everything.
        std::unordered_map<
            ExteriorDestinationKey,
            ExteriorPresentationEvidence,
            ExteriorDestinationKeyHash> s_exteriorPresentationEvidence;
        // A PositionPlayerJob Show can race the tail of a plugin PreloadWorld
        // call before FlushQueuedLoads publishes/drains its barrier. Tombstoning
        // the exact arrival prevents that late return from re-arming stale proof.
        std::array<
            ExteriorDestinationKey,
            kExteriorPresentationEvidenceLimit>
            s_exteriorPresentationArrivals{};
        std::size_t s_exteriorPresentationArrivalCount = 0;
        bool s_exteriorPresentationArrivalWorldPending = false;
        std::uint32_t s_exteriorPresentationArrivalWorldFormID = 0;
        std::uint64_t s_exteriorPresentationSequence = 0;
        std::size_t s_exteriorSeenSincePrune = 0;
        std::atomic<bool> s_registered{ false };
        std::atomic<bool> s_registrationTaskQueued{ false };
        std::atomic<bool> s_shuttingDown{ false };
        // GameDataReady happens while Fallout is still in the main menu. Keep the
        // shared poller hard-parked until the existing kPostLoadGame/kNewGame
        // lifecycle publishes a genuine gameplay session. MainMenu OPEN clears
        // this again, making already-queued tasks inert before they touch engine
        // state; the next activation forces a game-thread cache rebuild.
        std::atomic<bool> s_gameSessionActive{ false };
        std::atomic<DoorPrefetch::SessionLostCallback>
            s_sessionLostCallback{ nullptr };
        // Independent detection-source switches. The historical SetEnabled API now
        // controls only the event-driven crosshair / VR-wand pick source.
        std::atomic<bool> s_crosshairPickEnabled{ false };
        // Product-policy gate for every plugin-issued engine PreloadWorld request,
        // regardless of whether the candidate came from proximity, a ray, or a pick.
        std::atomic<bool> s_exteriorPreloadEnabled{ false };
        // Runtime capability, not a user preference. Published only after the
        // exact PositionPlayerJob -> ShowLoadingMenu bytes have been verified.
        std::atomic<bool> s_exteriorPresentationCorrectionReady{ false };
        std::atomic<bool> s_gateProximity{ false };
        std::atomic<float> s_gateProximityDistance{ 4096.0F };

        [[nodiscard]] bool HasExteriorPresentationArrivalLocked(
            const ExteriorDestinationKey& a_destination) noexcept
        {
            return std::find(
                       s_exteriorPresentationArrivals.begin(),
                       s_exteriorPresentationArrivals.begin() +
                           s_exteriorPresentationArrivalCount,
                       a_destination) !=
                s_exteriorPresentationArrivals.begin() +
                    s_exteriorPresentationArrivalCount;
        }

        void TombstoneExteriorPresentationArrivalLocked(
            const ExteriorDestinationKey& a_destination) noexcept
        {
            if (HasExteriorPresentationArrivalLocked(a_destination)) {
                return;
            }
            if (s_exteriorPresentationArrivalCount >=
                s_exteriorPresentationArrivals.size()) {
                // More than 64 exact exterior Shows without one synchronized
                // Flush is not a valid engine sequence. Fail bounded: retain
                // the newest exact arrival and sacrifice only older race proof.
                std::move(
                    s_exteriorPresentationArrivals.begin() + 1,
                    s_exteriorPresentationArrivals.end(),
                    s_exteriorPresentationArrivals.begin());
                --s_exteriorPresentationArrivalCount;
            }
            s_exteriorPresentationArrivals[
                s_exteriorPresentationArrivalCount++] = a_destination;
        }

        void ResetDestinationCooldowns(
            const char* a_reason,
            bool a_clearPresentationEvidence) noexcept
        {
            std::size_t exteriorCount = 0;
            std::size_t evidenceBefore = 0;
            std::size_t evidenceCleared = 0;
            std::size_t evidenceRetained = 0;
            std::size_t arrivalsCleared = 0;
            {
                std::lock_guard lock(s_seenMutex);
                exteriorCount = s_exteriorSeen.size();
                evidenceBefore = s_exteriorPresentationEvidence.size();
                arrivalsCleared =
                    s_exteriorPresentationArrivalCount;
                s_exteriorSeen.clear();
                s_exteriorSeenSincePrune = 0;

                if (a_clearPresentationEvidence) {
                    evidenceCleared = evidenceBefore;
                    s_exteriorPresentationEvidence.clear();
                    s_exteriorPresentationSequence = 0;
                } else {
                    // Consume records the exact arrived world without allocating
                    // or freeing. Reconcile only after the submission mutex drain
                    // so no in-flight request from the departed world can survive
                    // by stamping around the first-Show boundary.
                    if (s_exteriorPresentationArrivalWorldPending) {
                        for (auto it =
                                 s_exteriorPresentationEvidence.begin();
                             it !=
                                 s_exteriorPresentationEvidence.end();) {
                            if (!Policy::ShouldRetainExteriorPresentationEvidence(
                                    it->second.sourceWorldFormID,
                                    s_exteriorPresentationArrivalWorldFormID)) {
                                it = s_exteriorPresentationEvidence.erase(it);
                                ++evidenceCleared;
                            } else {
                                it->second.sequence =
                                    ++s_exteriorPresentationSequence;
                                ++it;
                            }
                        }
                    }
                    // Consume is normally earlier than this drain. Erase every
                    // tombstoned key again after the submission mutex has drained
                    // to close the small Show-vs-PreloadWorld return race.
                    for (std::size_t i = 0;
                         i < s_exteriorPresentationArrivalCount; ++i) {
                        evidenceCleared +=
                            s_exteriorPresentationEvidence.erase(
                                s_exteriorPresentationArrivals[i]);
                    }
                }
                s_exteriorPresentationArrivalCount = 0;
                s_exteriorPresentationArrivalWorldPending = false;
                s_exteriorPresentationArrivalWorldFormID = 0;
                evidenceRetained = s_exteriorPresentationEvidence.size();
            }
            if (exteriorCount != 0 || evidenceBefore != 0 ||
                evidenceCleared != 0 || evidenceRetained != 0 ||
                arrivalsCleared != 0) {
                logger::info(
                    "DoorPrefetch: destination cooldown generation reset "
                    "(reason={}, exterior={}, presentationEvidenceCleared={}, "
                    "presentationEvidenceRetained={}, arrivalsCleared={})",
                    a_reason ? a_reason : "unspecified",
                    exteriorCount, evidenceCleared, evidenceRetained,
                    arrivalsCleared);
            }
        }

        struct ResolvedDoorDestination
        {
            RE::TESObjectCELL* transitionCell = nullptr;
            RE::NiPointer<RE::TESObjectREFR> linkedDoor;
            RE::TESObjectCELL* linkedCell = nullptr;
            bool linkedCellFromSaveParent = false;
            RE::TESObjectCELL* cell = nullptr;
            RE::TESWorldSpace* world = nullptr;
            RE::NiPoint3 xtelPosition{};
            bool hasTeleportData = false;
        };

        [[nodiscard]] bool ResolveDoorDestination(
            RE::TESObjectREFR* a_door,
            ResolvedDoorDestination& a_out)
        {
            a_out = {};
            if (!a_door || !a_door->extraList) {
                return false;
            }
            auto* teleport = a_door->extraList->GetByType<RE::ExtraTeleport>();
            if (!teleport || !teleport->teleportData) {
                return false;
            }

            auto* data = teleport->teleportData;
            a_out.hasTeleportData = true;
            a_out.transitionCell = data->transitionCell;
            a_out.linkedDoor = data->linkedDoor.get();
            a_out.linkedCell =
                a_out.linkedDoor ? a_out.linkedDoor->GetParentCell() : nullptr;
            if (!a_out.linkedCell && a_out.linkedDoor) {
                // Persistent exterior doors can be detached from a live parent
                // cell while their destination world is still represented by the
                // reference's ExtraPersistentCell. Fallout's virtual
                // GetSaveParentCell follows that engine-owned fallback without
                // requiring the persistent cell to be attached or its reference
                // array to be walked.
                a_out.linkedCell = a_out.linkedDoor->GetSaveParentCell();
                a_out.linkedCellFromSaveParent =
                    a_out.linkedCell != nullptr;
            }
            // Prefer the linked reference's engine-resolved parent. Keep
            // transitionCell only as a fail-closed fallback for malformed or
            // temporarily unresolved handles.
            a_out.cell =
                a_out.linkedCell ? a_out.linkedCell : a_out.transitionCell;
            if (a_out.cell && !a_out.cell->IsInterior()) {
                a_out.world = a_out.cell->worldSpace;
            }
            if (!a_out.world &&
                a_out.transitionCell &&
                !a_out.transitionCell->IsInterior()) {
                a_out.world = a_out.transitionCell->worldSpace;
            }
            // XTEL stores the actual arrival position. Native linked exterior
            // preload derives its world-grid center from this position, never from
            // transitionCell::GetDataX/Y (persistent cells commonly report 0,0).
            a_out.xtelPosition = data->position;
            return a_out.cell != nullptr;
        }

        [[nodiscard]] bool TryGetExteriorGridCenter(
            float a_x,
            float a_y,
            int& a_gridX,
            int& a_gridY) noexcept
        {
            const double x = static_cast<double>(a_x);
            const double y = static_cast<double>(a_y);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                return false;
            }
            const double gridX = std::floor(x / 4096.0);
            const double gridY = std::floor(y / 4096.0);
            constexpr double minInt =
                static_cast<double>(std::numeric_limits<int>::min());
            constexpr double maxInt =
                static_cast<double>(std::numeric_limits<int>::max());
            if (gridX < minInt || gridX > maxInt ||
                gridY < minInt || gridY > maxInt) {
                return false;
            }
            a_gridX = static_cast<int>(gridX);
            a_gridY = static_cast<int>(gridY);
            return true;
        }

        [[nodiscard]] bool TryGetExteriorGridCenter(
            const ResolvedDoorDestination& a_destination,
            int& a_gridX,
            int& a_gridY) noexcept
        {
            return a_destination.hasTeleportData &&
                TryGetExteriorGridCenter(
                    a_destination.xtelPosition.x,
                    a_destination.xtelPosition.y,
                    a_gridX,
                    a_gridY);
        }

        [[nodiscard]] bool TryReadWorldFormID_SEH(
            void* a_world,
            std::uint32_t* a_formID) noexcept
        {
            if (!a_world || !a_formID) {
                return false;
            }
            *a_formID = 0;
            __try {
                const auto formID =
                    static_cast<RE::TESWorldSpace*>(a_world)->GetFormID();
                if (formID == 0) {
                    return false;
                }
                *a_formID = formID;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *a_formID = 0;
                return false;
            }
        }

        void PruneCooldownsLocked(
            std::chrono::steady_clock::time_point a_now)
        {
            // Expired exterior state has no semantic value, so prune it in
            // bounded batches rather than retaining every visited destination.
            if (++s_exteriorSeenSincePrune >= kCooldownPruneInterval) {
                const auto exteriorCutoff =
                    a_now - std::chrono::milliseconds(
                        kExteriorStateLifetimeMs);
                for (auto it = s_exteriorSeen.begin();
                     it != s_exteriorSeen.end();) {
                    if (it->second.updated <= exteriorCutoff) {
                        it = s_exteriorSeen.erase(it);
                    } else {
                        ++it;
                    }
                }
                s_exteriorSeenSincePrune = 0;
            }
        }

        [[nodiscard]] bool IsKnownDistance(float a_distance) noexcept
        {
            return std::isfinite(a_distance) && a_distance >= 0.0F;
        }

        [[nodiscard]] bool IsMeaningfullyCloserForRetry(
            float a_distance,
            float a_nearestSubmittedDistance) noexcept
        {
            if (!IsKnownDistance(a_distance)) {
                return false;
            }
            if (a_distance <= kExteriorNearDoorDistance) {
                return true;
            }
            return IsKnownDistance(a_nearestSubmittedDistance) &&
                a_nearestSubmittedDistance - a_distance >=
                    kExteriorMaterialCloserUnits &&
                a_distance <=
                    a_nearestSubmittedDistance *
                        kExteriorMaterialCloserRatio;
        }

        ExteriorSubmissionEvaluation EvaluateExteriorSubmission(
            const ExteriorDestinationKey& a_destination,
            float a_distance)
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(s_seenMutex);
            PruneCooldownsLocked(now);
            auto it = s_exteriorSeen.find(a_destination);
            if (it == s_exteriorSeen.end()) {
                return {
                    ExteriorSubmissionDisposition::kInitial,
                    0
                };
            }

            auto& state = it->second;
            if (state.updated.time_since_epoch().count() == 0 ||
                now - state.updated >=
                    std::chrono::milliseconds(
                        kExteriorStateLifetimeMs)) {
                s_exteriorSeen.erase(it);
                return {
                    ExteriorSubmissionDisposition::kInitial,
                    0
                };
            }

            if (state.residentOrPending) {
                if (state.residentObserved.time_since_epoch().count() != 0 &&
                    now - state.residentObserved <
                        std::chrono::milliseconds(
                            kExteriorResidentEvidenceMs)) {
                    return {
                        ExteriorSubmissionDisposition::kResidentOrPending,
                        state.submissions
                    };
                }
                state.residentOrPending = false;
            }
            if (state.submissions == 0) {
                return {
                    ExteriorSubmissionDisposition::kInitial,
                    0
                };
            }
            if (state.submissions >= kExteriorMaxSubmissions) {
                return {
                    ExteriorSubmissionDisposition::kRetryLimit,
                    state.submissions
                };
            }
            if (now - state.lastSubmitted <
                std::chrono::milliseconds(kExteriorRetryMinMs)) {
                return {
                    ExteriorSubmissionDisposition::kRecent,
                    state.submissions
                };
            }
            if (IsMeaningfullyCloserForRetry(
                    a_distance, state.nearestSubmittedDistance)) {
                return {
                    ExteriorSubmissionDisposition::kRevalidation,
                    state.submissions
                };
            }
            return {
                ExteriorSubmissionDisposition::kAwaitingCloser,
                state.submissions
            };
        }

        [[nodiscard]] bool ExteriorSubmissionReady(
            const ExteriorSubmissionEvaluation& a_evaluation) noexcept
        {
            return a_evaluation.disposition ==
                    ExteriorSubmissionDisposition::kInitial ||
                a_evaluation.disposition ==
                    ExteriorSubmissionDisposition::kRevalidation;
        }

        [[nodiscard]] const char* ExteriorDispositionName(
            ExteriorSubmissionDisposition a_disposition) noexcept
        {
            switch (a_disposition) {
            case ExteriorSubmissionDisposition::kInitial:
                return "initial";
            case ExteriorSubmissionDisposition::kRevalidation:
                return "distance-revalidation";
            case ExteriorSubmissionDisposition::kRecent:
                return "retry-minimum-delay";
            case ExteriorSubmissionDisposition::kAwaitingCloser:
                return "awaiting-materially-closer";
            case ExteriorSubmissionDisposition::kRetryLimit:
                return "bounded-retry-limit";
            case ExteriorSubmissionDisposition::kResidentOrPending:
                return "live-resident-or-pending";
            default:
                return "unknown";
            }
        }

        void MakeRoomForExteriorPresentationEvidenceLocked()
        {
            if (s_exteriorPresentationEvidence.size() <
                kExteriorPresentationEvidenceLimit) {
                return;
            }
            const auto oldest = std::min_element(
                s_exteriorPresentationEvidence.begin(),
                s_exteriorPresentationEvidence.end(),
                [](const auto& a_left, const auto& a_right) {
                    return a_left.second.sequence < a_right.second.sequence;
                });
            if (oldest != s_exteriorPresentationEvidence.end()) {
                s_exteriorPresentationEvidence.erase(oldest);
            }
        }

        std::uint8_t StampExteriorSubmission(
            const ExteriorDestinationKey& a_destination,
            std::uint32_t a_sourceWorldFormID,
            bool a_armPresentationEvidence,
            float a_distance)
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(s_seenMutex);
            PruneCooldownsLocked(now);
            auto& state = s_exteriorSeen[a_destination];
            state.lastSubmitted = now;
            state.updated = now;
            if (state.submissions < kExteriorMaxSubmissions) {
                ++state.submissions;
            }
            if (a_armPresentationEvidence && a_sourceWorldFormID != 0 &&
                !HasExteriorPresentationArrivalLocked(a_destination)) {
                if (!s_exteriorPresentationEvidence.contains(a_destination)) {
                    MakeRoomForExteriorPresentationEvidenceLocked();
                }
                s_exteriorPresentationEvidence[a_destination] = {
                    a_sourceWorldFormID,
                    ++s_exteriorPresentationSequence,
                    false
                };
            }
            if (IsKnownDistance(a_distance)) {
                state.nearestSubmittedDistance = std::min(
                    state.nearestSubmittedDistance, a_distance);
            }
            state.duplicateLogged = false;
            return state.submissions;
        }

        bool MarkExteriorResidentOrPending(
            const ExteriorDestinationKey& a_destination,
            bool a_exactLiveArrivalObserved)
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(s_seenMutex);
            PruneCooldownsLocked(now);
            auto& state = s_exteriorSeen[a_destination];
            const bool firstObservation = !state.residentOrPending;
            state.residentOrPending = true;
            state.residentObserved = now;
            state.updated = now;
            // A verified live linked cell refreshes only proof that originated
            // in an actual earlier plugin submission. It can never manufacture
            // presentation evidence for a naturally resident destination.
            if (a_exactLiveArrivalObserved &&
                !HasExteriorPresentationArrivalLocked(a_destination)) {
                const auto evidence =
                    s_exteriorPresentationEvidence.find(a_destination);
                if (evidence != s_exteriorPresentationEvidence.end()) {
                    evidence->second.exactLiveArrivalObserved = true;
                    evidence->second.sequence =
                        ++s_exteriorPresentationSequence;
                }
            }
            return firstObservation;
        }

        bool ConsumeExteriorSuppressionLog(
            const ExteriorDestinationKey& a_destination)
        {
            if (!PreloadDiagnostics::IsConfigured()) {
                return false;
            }
            std::lock_guard lock(s_seenMutex);
            const auto it = s_exteriorSeen.find(a_destination);
            if (it == s_exteriorSeen.end() ||
                it->second.duplicateLogged) {
                return false;
            }
            it->second.duplicateLogged = true;
            return true;
        }

        [[nodiscard]] bool HasMatchingLiveResidentDestination(
            const ResolvedDoorDestination& a_destination,
            const ExteriorDestinationKey& a_key) noexcept
        {
            // A live linked-door parent is engine-owned evidence for the exact
            // arrival cell. A save-parent/persistent fallback is useful for
            // resolving the world but cannot prove that the XTEL grid is resident.
            // Likewise, a resident center cell does not prove a full uGrids square.
            auto* cell = a_destination.linkedCell;
            return !a_key.fullGrid &&
                cell != nullptr &&
                !a_destination.linkedCellFromSaveParent &&
                !cell->IsInterior() &&
                cell->worldSpace != nullptr &&
                cell->worldSpace->GetFormID() == a_key.worldFormID &&
                cell->GetDataX() == a_key.gridX &&
                cell->GetDataY() == a_key.gridY &&
                IsResidentOrBusy(cell);
        }

        void ObserveResidentExteriorGateDestination(
            RE::TESObjectREFR* a_door,
            const ResolvedDoorDestination* a_preResolved = nullptr) noexcept
        {
            if (!a_door ||
                s_exteriorFullGrid.load(std::memory_order_relaxed)) {
                return;
            }
            ResolvedDoorDestination destination =
                a_preResolved ? *a_preResolved : ResolvedDoorDestination{};
            if ((!a_preResolved &&
                 !ResolveDoorDestination(a_door, destination)) ||
                !destination.cell ||
                !destination.world ||
                destination.cell->IsInterior()) {
                return;
            }
            int centerX = 0;
            int centerY = 0;
            if (!TryGetExteriorGridCenter(
                    destination, centerX, centerY)) {
                return;
            }
            const ExteriorDestinationKey key{
                destination.world->GetFormID(),
                centerX,
                centerY,
                false
            };
            if (!HasMatchingLiveResidentDestination(destination, key)) {
                return;
            }
            if (MarkExteriorResidentOrPending(key, true)) {
                logger::info(
                    "DoorPrefetch: exact arrival already resident/pending "
                    "door={:08X} linkedDoor={:08X} destinationCell={:08X} "
                    "destinationWorld={:08X} center=({}, {}) "
                    "evidence=live-linked-cell",
                    a_door->GetFormID(),
                    destination.linkedDoor
                        ? destination.linkedDoor->GetFormID()
                        : 0,
                    destination.linkedCell
                        ? destination.linkedCell->GetFormID()
                        : 0,
                    destination.world->GetFormID(),
                    centerX,
                    centerY);
            }
        }

        void LogExteriorPreloadDecision(
            const char* a_trigger,
            const char* a_decision,
            RE::TESObjectREFR* a_door,
            RE::TESObjectCELL* a_sourceCell,
            RE::TESWorldSpace* a_sourceWorld,
            const ResolvedDoorDestination& a_destination,
            int a_gridX,
            int a_gridY,
            bool a_fullGrid,
            bool a_engineCallReturned) noexcept
        {
            if (!PreloadDiagnostics::IsConfigured()) {
                return;
            }
            logger::info(
                "PreloadDiag event=exterior-preload-decision "
                "source=plugin-door-prefetch trigger={} decision={} "
                "door={:08X} linkedDoor={:08X} sourceCell={:08X} "
                "sourceWorld={:08X} destCell={:08X} destWorld={:08X} "
                "center=({}, {}) mode={} destinationKey="
                "({:08X},{},{},{}) cooldownMs={} retryMinMs={} "
                "retryNearDistance={:.0f} maxSubmissions={} stateTtlMs={} "
                "engineCallReturned={} "
                "engineOwnsResidentPendingDedup=true "
                "pluginLiveResidentEvidence=true completionClaim=false",
                a_trigger ? a_trigger : "<null>",
                a_decision ? a_decision : "<null>",
                a_door ? a_door->GetFormID() : 0,
                a_destination.linkedDoor
                    ? a_destination.linkedDoor->GetFormID()
                    : 0,
                a_sourceCell ? a_sourceCell->GetFormID() : 0,
                a_sourceWorld ? a_sourceWorld->GetFormID() : 0,
                a_destination.cell
                    ? a_destination.cell->GetFormID()
                    : 0,
                a_destination.world
                    ? a_destination.world->GetFormID()
                    : 0,
                a_gridX,
                a_gridY,
                a_fullGrid ? "native-uGrids" : "single-arrival-cell",
                a_destination.world
                    ? a_destination.world->GetFormID()
                    : 0,
                a_gridX,
                a_gridY,
                a_fullGrid,
                kExteriorRetryMinMs,
                kExteriorRetryMinMs,
                kExteriorNearDoorDistance,
                kExteriorMaxSubmissions,
                kExteriorStateLifetimeMs,
                a_engineCallReturned);
        }

        // Stable result codes. Keep historical values analyzable. Code 16 now means
        // only that the engine PreloadWorld call returned; it never claims that any
        // requested cell was accepted, completed, attached, or later visited.
        enum class PreRes : int {
            kNullRef = 0, kNotDoor = 1, kDestLoaded = 2,
            kRetiredLegacyInteriorFired = 3,
            kRetiredLegacyInteriorNoFn = 4,
            kRetiredLegacyInteriorCooldown = 5,
            kExtNoFn = 6, kNoDestWS = 7,
            kSameWS = 8, kExtCooldown = 9, kFiredExteriorLegacy = 10,
            kExtNoLayout = 11, kTransitionActive = 12,
            kPlayerContextUnavailable = 13, kInteriorOriginUnsupported = 14,
            kNoColdWork = 15, kExteriorEngineIssued = 16,
            kSourceWorldMissing = 17, kPostTransitionQuiet = 18,
            kExteriorDisabled = 19, kExteriorPositionInvalid = 20,
            kInteriorRetired = 21,
            kWorldspaceNotPrepared = 22,
            kDestinationChanged = 23
        };

        [[nodiscard]] const char* PreResName(PreRes a_result) noexcept
        {
            switch (a_result) {
            case PreRes::kNullRef: return "null-ref";
            case PreRes::kNotDoor: return "not-door";
            case PreRes::kDestLoaded: return "destination-loaded";
            case PreRes::kRetiredLegacyInteriorFired:
                return "retired-legacy-interior-fired-code";
            case PreRes::kRetiredLegacyInteriorNoFn:
                return "retired-legacy-interior-no-function-code";
            case PreRes::kRetiredLegacyInteriorCooldown:
                return "retired-legacy-interior-cooldown-code";
            case PreRes::kExtNoFn: return "exterior-engine-unavailable";
            case PreRes::kNoDestWS: return "destination-worldspace-missing";
            case PreRes::kSameWS: return "same-worldspace";
            case PreRes::kExtCooldown: return "exterior-cooldown";
            case PreRes::kFiredExteriorLegacy: return "fired-exterior-legacy";
            case PreRes::kExtNoLayout: return "legacy-cell-map-layout-unavailable";
            case PreRes::kTransitionActive: return "transition-active";
            case PreRes::kPlayerContextUnavailable: return "player-context-unavailable";
            case PreRes::kInteriorOriginUnsupported: return "interior-origin-unsupported";
            case PreRes::kNoColdWork: return "legacy-no-cold-work";
            case PreRes::kExteriorEngineIssued: return "fired-exterior-engine";
            case PreRes::kSourceWorldMissing: return "source-worldspace-missing";
            case PreRes::kPostTransitionQuiet: return "post-transition-quiet";
            case PreRes::kExteriorDisabled: return "exterior-disabled";
            case PreRes::kExteriorPositionInvalid: return "exterior-position-invalid";
            case PreRes::kInteriorRetired: return "interior-preload-retired";
            case PreRes::kWorldspaceNotPrepared: return "worldspace-not-prepared";
            case PreRes::kDestinationChanged: return "destination-changed";
            default: return "unknown";
            }
        }

        PreRes TryPreloadFromRef(
            RE::TESObjectREFR* a_refr,
            const char* a_trigger,
            float a_distance,
            const ResolvedDoorDestination* a_preResolved = nullptr)
        {
            RE::TESObjectCELL* cell = nullptr;
            ResolvedDoorDestination destination =
                a_preResolved ? *a_preResolved : ResolvedDoorDestination{};
            auto finish = [&](PreRes a_result) {
                PreloadDiagnostics::NoteCrosshairCandidate(
                    a_refr, cell, a_trigger, a_distance,
                    static_cast<int>(a_result), PreResName(a_result));
                return a_result;
            };

            if (IsTransitionBarrierActive()) {
                return finish(PreRes::kTransitionActive);
            }
            if (IsPostTransitionQuiet()) {
                return finish(PreRes::kPostTransitionQuiet);
            }
            if (!a_refr) {
                return finish(PreRes::kNullRef);
            }
            if ((!a_preResolved &&
                 !ResolveDoorDestination(a_refr, destination)) ||
                !destination.cell) {
                return finish(PreRes::kNotDoor);
            }
            cell = destination.cell;
            if (cell->IsInterior()) {
                // Hard retirement boundary. Native-only linked interior preload
                // reproduced the same structural freeze as direct speculative
                // interior work. Picks, rays, and gates therefore never submit it.
                return finish(PreRes::kInteriorRetired);
            }

            // Exterior destination (building exit / city or vault gate). Delegate
            // the request to TES::PreloadWorld exactly as Fallout does. Never inspect
            // TESWorldSpace::cellMap and never call ExteriorCellLoader directly.
            // Keep plugin look-ahead cross-worldspace only; same-world streaming
            // remains wholly owned by the engine's live-grid logic.
            if (!s_exteriorPreloadEnabled.load(std::memory_order_acquire)) {
                return finish(PreRes::kExteriorDisabled);
            }
            if (!s_preloadWorld || !s_tesSingletonPtr) {
                return finish(PreRes::kExtNoFn);
            }
            auto* destWS = destination.world;
            if (!destWS) {
                return finish(PreRes::kNoDestWS);
            }
            auto* player = GetPlayerForPrefetch();
            if (!player) {
                return finish(PreRes::kPlayerContextUnavailable);
            }
            auto* playerCell = player->GetParentCell();
            if (!playerCell) {
                return finish(PreRes::kPlayerContextUnavailable);
            }
            if (playerCell->IsInterior()) {
                // Only exterior-to-exterior operation has been validated. Keep
                // interior origins out of this speculative exterior path.
                return finish(PreRes::kInteriorOriginUnsupported);
            }
            auto* playerWS = playerCell->worldSpace;
            if (!playerWS) {
                return finish(PreRes::kSourceWorldMissing);
            }
            if (!IsStableAttachedExteriorSource(playerCell, playerWS)) {
                return finish(PreRes::kPlayerContextUnavailable);
            }
            if (destWS == playerWS) {
                return finish(PreRes::kSameWS);
            }
            void* tes = TryReadPointerSlot<void>(s_tesSingletonPtr);
            if (!tes) {
                return finish(PreRes::kExtNoFn);
            }
            int centerX = 0;
            int centerY = 0;
            if (!TryGetExteriorGridCenter(destination, centerX, centerY)) {
                return finish(PreRes::kExteriorPositionInvalid);
            }
            // Always the single-arrival-cell mode. The native-uGrids request
            // (queueOnly=false) is retired: it rewrites live grid state and
            // CTDs at the next real transition when issued from an exterior
            // origin (field-confirmed at the Diamond City exit gate).
            constexpr bool fullGrid = false;
            const ExteriorDestinationKey destinationKey{
                destWS->GetFormID(),
                centerX,
                centerY,
                fullGrid
            };
            const auto suppressExterior =
                [&](const ExteriorSubmissionEvaluation& a_evaluation) {
                if (a_evaluation.disposition ==
                    ExteriorSubmissionDisposition::kResidentOrPending) {
                    return finish(PreRes::kDestLoaded);
                }
                if (ConsumeExteriorSuppressionLog(destinationKey)) {
                    LogExteriorPreloadDecision(
                        a_trigger,
                        "recent-plugin-request",
                        a_refr,
                        playerCell,
                        playerWS,
                        destination,
                        centerX,
                        centerY,
                        fullGrid,
                        false);
                    logger::info(
                        "DoorPrefetch: exact destination request suppressed "
                        "world={:08X} center=({}, {}) mode={} policy={} "
                        "submissions={}/{}",
                        destinationKey.worldFormID,
                        centerX,
                        centerY,
                        fullGrid ? "native-uGrids" : "single-arrival-cell",
                        ExteriorDispositionName(a_evaluation.disposition),
                        a_evaluation.submissions,
                        kExteriorMaxSubmissions);
                }
                return finish(PreRes::kExtCooldown);
            };

            if (HasMatchingLiveResidentDestination(
                    destination, destinationKey)) {
                MarkExteriorResidentOrPending(destinationKey, true);
            }
            auto exteriorEvaluation =
                EvaluateExteriorSubmission(destinationKey, a_distance);
            if (!ExteriorSubmissionReady(exteriorEvaluation)) {
                return suppressExterior(exteriorEvaluation);
            }

            ExteriorSubmissionDisposition submittedAs =
                ExteriorSubmissionDisposition::kInitial;
            std::uint8_t submissionCount = 0;
            bool presentationEvidenceArmed = false;
            {
                // Serialize the final state checks and the engine call with
                // FlushQueuedLoads. Re-read g_TES under the lock because the
                // singleton is lifecycle-owned by the engine.
                std::lock_guard submissionLock(s_submissionMutex);
                if (!s_gameSessionActive.load(
                        std::memory_order_acquire)) {
                    return finish(PreRes::kPlayerContextUnavailable);
                }
                if (IsPreloadSuppressed()) {
                    return finish(IsTransitionBarrierActive()
                        ? PreRes::kTransitionActive
                        : PreRes::kPostTransitionQuiet);
                }
                if (!s_exteriorPreloadEnabled.load(std::memory_order_acquire)) {
                    return finish(PreRes::kExteriorDisabled);
                }
                auto* currentPlayer = GetPlayerForPrefetch();
                if (!currentPlayer ||
                    currentPlayer->GetParentCell() != playerCell ||
                    !IsStableAttachedExteriorSource(playerCell, playerWS)) {
                    return finish(PreRes::kPlayerContextUnavailable);
                }
                // Refresh the door target at the final submission boundary.
                // The prepared gate snapshot can be up to one poll old, and a
                // linked reference may have gained its live parent meanwhile.
                // Only that exact live linked cell may suppress cross-world
                // prediction; no presentation/tip state participates here.
                ResolvedDoorDestination currentDestination{};
                int currentX = 0;
                int currentY = 0;
                if (!ResolveDoorDestination(a_refr, currentDestination) ||
                    currentDestination.world != destWS ||
                    currentDestination.cell->IsInterior() ||
                    !TryGetExteriorGridCenter(currentDestination, currentX, currentY) ||
                    currentX != centerX || currentY != centerY) {
                    return finish(PreRes::kDestinationChanged);
                }
                if (HasMatchingLiveResidentDestination(
                        currentDestination, destinationKey)) {
                    MarkExteriorResidentOrPending(destinationKey, true);
                }
                exteriorEvaluation =
                    EvaluateExteriorSubmission(destinationKey, a_distance);
                if (!ExteriorSubmissionReady(exteriorEvaluation)) {
                    return suppressExterior(exteriorEvaluation);
                }
                tes = TryReadPointerSlot<void>(s_tesSingletonPtr);
                if (!tes) {
                    return finish(PreRes::kExtNoFn);
                }

                // The common PreloadWorld diagnostic hook records the actual engine
                // arguments as event=preload-world/source=plugin-door-prefetch.
                // queueOnly is hard-coded true: the false path is the engine's
                // live grid switch, not a preload hint, and CTDs from here.
                // Do not consult TES::AllCellsInGridLoaded here: its active-grid
                // fast path is valid inside PositionPlayerJob's aligned handoff,
                // but is not destination-world proof for this ambient poller.
                // Fallout owns loaded/pending dedup for this queue-only request.
                PreloadDiagnostics::PluginCrosshairScope sourceScope;
                // AddMultiBoundRef runs before Load3D's late world initializer.
                // Preparation and submission share the native teardown lock;
                // the graph lease then outlives the asynchronous engine work.
                if (!WorldspacePreload::Submit(tes, destWS, centerX, centerY, s_preloadWorld)) {
                    return finish(PreRes::kWorldspaceNotPrepared);
                }
                presentationEvidenceArmed =
                    Policy::ShouldArmExteriorPresentationEvidence(
                        s_exteriorPresentationCorrectionReady.load(
                            std::memory_order_acquire),
                        true);
                submittedAs = exteriorEvaluation.disposition;
                submissionCount =
                    StampExteriorSubmission(
                        destinationKey,
                        playerWS->GetFormID(),
                        presentationEvidenceArmed,
                        a_distance);

                // Stamp before observing. If PreloadWorld materialized the linked
                // reference synchronously, this fresh game-thread resolution can
                // attach exact-arrival proof immediately; asynchronous work is
                // picked up by the existing 120 ms poll without any extra scan.
                ResolvedDoorDestination postSubmissionDestination{};
                if (ResolveDoorDestination(
                        a_refr, postSubmissionDestination)) {
                    ObserveResidentExteriorGateDestination(
                        a_refr, &postSubmissionDestination);
                }
            }
            LogExteriorPreloadDecision(
                a_trigger,
                "engine-call-returned",
                a_refr,
                playerCell,
                playerWS,
                destination,
                centerX,
                centerY,
                fullGrid,
                true);
            logger::info(
                "DoorPrefetch: TES::PreloadWorld returned trigger={} "
                "door={:08X} linkedDoor={:08X} sourceWorld={:08X} "
                "destinationCell={:08X} destinationWorld={:08X} "
                "center=({}, {}) centerSource=xtel-position mode={} "
                "destinationDedup=exact-world-grid-mode+live-linked-resident"
                "+bounded-distance-revalidation submission={} "
                "submissionCount={}/{} presentationEvidenceArmed={} "
                "completionClaim=false",
                a_trigger ? a_trigger : "<null>",
                a_refr ? a_refr->GetFormID() : 0,
                destination.linkedDoor
                    ? destination.linkedDoor->GetFormID()
                    : 0,
                playerWS->GetFormID(),
                cell->GetFormID(),
                destWS->GetFormID(),
                centerX,
                centerY,
                fullGrid ? "native-uGrids" : "single-arrival-cell",
                ExteriorDispositionName(submittedAs),
                submissionCount,
                kExteriorMaxSubmissions,
                presentationEvidenceArmed);
            return finish(PreRes::kExteriorEngineIssued);
        }

        // ===================================================================
        // Extended-range door DETECTION — adapted from Skyrim's long-ray test.
        // An independent Havok ray cast from the camera at (base reach * mult),
        // PRELOAD-ONLY: it never writes the engine pick and never changes the
        // player's activation reach. It just preloads a targeted load-door's
        // destination from far away so the load starts earlier. Runs on the GAME
        // thread (F4SE task), reusing TryPreloadFromRef's door/exterior/cooldown
        // gating. Complements the close-range event pick (PickSink).
        //
        // Works on OG + NG + VR. The fork's Havok wrappers (RE::bhkPickData /
        // cell->Pick) resolve via bare REL::ID with OG ids that are ABSENT from the
        // NG bin (→ garbage → CTD on NG), so we resolve each engine function
        // ourselves: OG/VR retain their existing REL::ID path; NG uses official
        // Address Library IDs shared by the verified 1.11.221 and 1.11.240 bins.
        // ===================================================================
        std::atomic<bool>  s_extendedRay{ false };
        std::atomic<float> s_rangeMult{ 2.0f };
        std::atomic<bool>  s_rayPollerRunning{ false };
        std::atomic<bool>  s_rayTaskQueued{ false };
        std::mutex s_rayWakeMutex;
        std::condition_variable s_rayWakeCv;
        // Owning thread: never detached. std::jthread also gives process-shutdown
        // cleanup a stop+join fallback if the explicit Shutdown path is missed.
        std::jthread s_rayPollerThread;

        [[nodiscard]] bool IsTransitionBarrierActive() noexcept
        {
            if (!s_transitionActive.load(std::memory_order_acquire)) {
                return false;
            }
            const auto now = static_cast<std::uint64_t>(GetTickCount64());
            const auto started = s_transitionStartedMs.load(std::memory_order_acquire);
            if (started == 0 || now - started < kTransitionFailSafeMs) {
                return true;
            }

            {
                std::lock_guard stateLock(s_transitionStateMutex);
                if (!s_transitionActive.load(std::memory_order_acquire)) {
                    return false;
                }
                const auto currentStart =
                    s_transitionStartedMs.load(std::memory_order_acquire);
                if (currentStart == 0 || now - currentStart < kTransitionFailSafeMs) {
                    return true;
                }

                // Publish the quiet barrier before clearing transition-active so
                // another thread can never observe both suppression gates as false.
                s_postTransitionQuietUntilMs.store(
                    now + kPostTransitionQuietMs, std::memory_order_release);
                s_transitionStartedMs.store(0, std::memory_order_release);
                s_forceGateRescan.store(true, std::memory_order_release);
                s_forceRayOriginReset.store(true, std::memory_order_release);
                s_transitionActive.store(false, std::memory_order_release);
                logger::warn(
                    "DoorPrefetch: transition barrier exceeded {}ms; "
                    "fail-safe entered {}ms quiet period",
                    kTransitionFailSafeMs,
                    kPostTransitionQuietMs);
            }
            s_rayWakeCv.notify_all();
            return false;
        }

        [[nodiscard]] bool IsPostTransitionQuiet() noexcept
        {
            const auto until =
                s_postTransitionQuietUntilMs.load(std::memory_order_acquire);
            return until != 0 &&
                static_cast<std::uint64_t>(GetTickCount64()) < until;
        }

        [[nodiscard]] bool IsPreloadSuppressed() noexcept
        {
            return IsTransitionBarrierActive() || IsPostTransitionQuiet();
        }

        using BhkCtorFn  = void (*)(void* /*bhkPickData*/);
        using SetSEFn    = void (*)(void* /*bhkPickData*/, const RE::NiPoint3*, const RE::NiPoint3*);
        using CellPickFn = RE::NiAVObject* (*)(RE::TESObjectCELL*, void* /*bhkPickData&*/);
        using FindRefFn  = RE::TESObjectREFR* (*)(RE::NiAVObject*);

        BhkCtorFn         s_bhkCtor       = nullptr;
        SetSEFn           s_setStartEnd   = nullptr;
        CellPickFn        s_cellPick      = nullptr;
        FindRefFn         s_findRef       = nullptr;
        RE::TESCamera**   s_gPlayerCamera = nullptr;  // POINTER global — deref once
        std::atomic<bool> s_havokResolved{ false };
        std::atomic<bool> s_havokResolutionAttempted{ false };

        [[nodiscard]] bool MatchesExecutableBytes(
            std::uintptr_t a_address,
            const std::uint8_t* a_expected,
            std::size_t a_size) noexcept
        {
            if (!a_expected ||
                !IsCommittedExecutableRegion(
                    reinterpret_cast<const void*>(a_address), a_size)) {
                return false;
            }
            bool matches = false;
            __try {
                matches = std::memcmp(
                    reinterpret_cast<const void*>(a_address),
                    a_expected,
                    a_size) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matches = false;
            }
            return matches;
        }

        // Resolve the Havok pick path for the running build. OG/VR retain the same
        // IDs the fork's wrappers use (present in the OG bin / VR CSV); NG uses the
        // official IDs mapped by both verified 1.11.221 and 1.11.240 version bins.
        // The exact-build gate and entry-byte checks keep resolution fail closed.
        bool ResolveHavokRay()
        {
            s_havokResolved.store(false, std::memory_order_release);
            s_bhkCtor = nullptr;
            s_setStartEnd = nullptr;
            s_cellPick = nullptr;
            s_findRef = nullptr;
            s_gPlayerCamera = nullptr;
            try {
                if (!IsExactSupportedRuntime()) {
                    return false;
                }
                const auto base = REL::Module::get().base();
                const auto ver  = REL::Module::get().version();
                const bool ng   = REL::Module::IsNG();
                const bool vr   = REL::Module::IsVR();
                if (!vr) {
                    if (ng) {
                        if (!(ver[1] == 11 &&
                              (ver[2] == 221 || ver[2] == 240))) {
                            return false;
                        }
                    } else if (!(ver[1] == 10 && ver[2] == 163)) {
                        return false;
                    }
                }
                auto R = [&](std::uint64_t ogId,
                             std::uint64_t ngId) -> std::uintptr_t {
                    return REL::Relocation<std::uintptr_t>{
                        REL::ID(ng ? ngId : ogId)
                    }.address();
                };
                const auto bhkCtor = R(526783, kBhkPickDataCtorIDNG);
                const auto setStartEnd = R(
                    747470, kBhkPickDataSetStartEndIDNG);
                const auto cellPick = R(434717, kCellPickIDNG);
                const auto findRef = R(766937, kFindReferenceFor3DIDNG);
                if (ng &&
                    (!MatchesExpectedNGRva(
                         bhkCtor, 0xCBB7B0, 0xCBBB40) ||
                     !MatchesExpectedNGRva(
                         setStartEnd, 0xE234C0, 0xE23850) ||
                     !MatchesExpectedNGRva(
                         cellPick, 0x4CA840, 0x4CAB60) ||
                     !MatchesExpectedNGRva(
                         findRef, 0x510170, 0x510490))) {
                    logger::error(
                        "DoorPrefetch: AE Havok Address Library mapping "
                        "does not match the exact runtime RVA set; "
                        "extended ray disabled");
                    return false;
                }
                const bool executable =
                    IsCommittedExecutableRegion(
                        reinterpret_cast<const void*>(bhkCtor), 16) &&
                    IsCommittedExecutableRegion(
                        reinterpret_cast<const void*>(setStartEnd), 8) &&
                    IsCommittedExecutableRegion(
                        reinterpret_cast<const void*>(cellPick), 16) &&
                    IsCommittedExecutableRegion(
                        reinterpret_cast<const void*>(findRef), 16);
                if (!executable) {
                    logger::error(
                        "DoorPrefetch: Havok ray entry is not in committed "
                        "executable memory; extended ray disabled");
                    return false;
                }

                if (ng) {
                    static constexpr std::uint8_t kBhkCtorBytes[]{
                        0x33, 0xD2, 0xB8, 0xFF, 0xFF, 0x00, 0x00, 0x66,
                        0x89, 0x41, 0x08, 0x0F, 0x57, 0xC0, 0x89, 0x51
                    };
                    static constexpr std::uint8_t kSetStartEndBytes[]{
                        0x48, 0x8B, 0xC4, 0x48, 0x83, 0xEC, 0x48, 0xF3
                    };
                    static constexpr std::uint8_t kCellPickBytes[]{
                        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
                    };
                    static constexpr std::uint8_t kFindRefBytes[]{
                        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33
                    };
                    if (!MatchesExecutableBytes(
                            bhkCtor, kBhkCtorBytes, sizeof(kBhkCtorBytes)) ||
                        !MatchesExecutableBytes(
                            setStartEnd,
                            kSetStartEndBytes,
                            sizeof(kSetStartEndBytes)) ||
                        !MatchesExecutableBytes(
                            cellPick,
                            kCellPickBytes,
                            sizeof(kCellPickBytes)) ||
                        !MatchesExecutableBytes(
                            findRef,
                            kFindRefBytes,
                            sizeof(kFindRefBytes))) {
                        logger::error(
                            "DoorPrefetch: AE 1.11.221/1.11.240 Havok ray signature "
                            "mismatch; extended ray disabled");
                        return false;
                    }
                }

                // g_PlayerCamera is a POINTER global (deref once), not a
                // function. OG/VR retain their raw RVAs; NG uses shared
                // Address Library ID 4796065.
                const std::uintptr_t cameraAddress = ng
                    ? REL::Relocation<std::uintptr_t>{
                        REL::ID(kPlayerCameraGlobalIDNG)
                    }.address()
                    : base + (vr ? 0x5930608 : 0x58CEB28);
                if (ng && !MatchesExpectedNGRva(
                              cameraAddress, 0x30DBDD8, 0x30E6E58)) {
                    logger::error(
                        "DoorPrefetch: AE g_PlayerCamera mapping does not "
                        "match the exact runtime RVA; extended ray disabled");
                    return false;
                }
                auto** cameraSlot =
                    reinterpret_cast<RE::TESCamera**>(cameraAddress);
                if (!IsValidatedDataPointerSlot(cameraSlot)) {
                    logger::error(
                        "DoorPrefetch: player-camera pointer slot failed "
                        ".data/readability validation; extended ray disabled");
                    return false;
                }
                s_bhkCtor = reinterpret_cast<BhkCtorFn>(bhkCtor);
                s_setStartEnd = reinterpret_cast<SetSEFn>(setStartEnd);
                s_cellPick = reinterpret_cast<CellPickFn>(cellPick);
                s_findRef = reinterpret_cast<FindRefFn>(findRef);
                s_gPlayerCamera = cameraSlot;
            } catch (...) {
                return false;
            }
            s_havokResolved.store(true, std::memory_order_release);
            return true;
        }

        // Movement-direction ray + gate-proximity state. The camera-forward ray only
        // catches doors the crosshair lands on; the movement ray adds doors you walk
        // at (with line-of-sight); gate proximity adds the cross-worldspace GATES you
        // approach without aiming (down stairs, round a corner). Gates are persistent
        // refs in the worldspace's persistent cell — NOT the player's grid cell — so
        // the gate cache is per-worldspace, rebuilt only on worldspace change.
        RE::NiPoint3       s_prevOrigin{};
        struct DoorEntry
        {
            std::uint32_t doorFormID = 0;
            RE::NiPoint3 pos{};
            int lastRes = -1;
            // Operational native-enumeration entries carry an engine handle so
            // later polls never use the NG-unsafe global form lookup. The
            // diagnostics-only legacy scanners may leave this empty.
            RE::ObjectRefHandle doorHandle{};
        };

        struct GateScanStats
        {
            std::size_t references = 0;
            std::size_t teleportDoors = 0;
            std::size_t disabledOrDeleted = 0;
            std::size_t noDestination = 0;
            std::size_t interiorDestination = 0;
            std::size_t noDestinationWorld = 0;
            std::size_t sameWorld = 0;
            std::size_t accepted = 0;
        };

        std::atomic<std::uint64_t> s_gateScanSequence{ 0 };

        [[nodiscard]] std::uint32_t FormIDOrZero(
            const RE::TESForm* a_form) noexcept
        {
            return a_form ? a_form->GetFormID() : 0;
        }

        [[nodiscard]] const char* EditorIDOrNone(
            const RE::TESForm* a_form) noexcept
        {
            if (!a_form) {
                return "<null>";
            }
            const char* editor = a_form->GetFormEditorID();
            return editor && editor[0] ? editor : "<none>";
        }

        void LogGateScanSource(
            std::uint64_t a_scan,
            const char* a_scope,
            RE::TESObjectCELL* a_cell,
            RE::TESWorldSpace* a_sourceWorld,
            bool a_stable) noexcept
        {
            if (!PreloadDiagnostics::IsInstalled()) {
                return;
            }
            auto* cellWorld =
                a_cell && !a_cell->IsInterior() ? a_cell->worldSpace : nullptr;
            logger::info(
                "PreloadDiag event=gate-scan-source scan={} scope={} stable={} "
                "sourceCellPtr={:#x} sourceCell={:08X} sourceEditor={} "
                "sourceInterior={} sourceState={} loadedData={:#x} "
                "cellWorld={:08X} cellWorldEditor={} expectedWorld={:08X} "
                "expectedWorldEditor={}",
                a_scan,
                a_scope,
                a_stable,
                reinterpret_cast<std::uintptr_t>(a_cell),
                FormIDOrZero(a_cell),
                EditorIDOrNone(a_cell),
                a_cell ? a_cell->IsInterior() : false,
                a_cell ? a_cell->cellState.underlying() : 0,
                reinterpret_cast<std::uintptr_t>(
                    a_cell ? a_cell->loadedData : nullptr),
                FormIDOrZero(cellWorld),
                EditorIDOrNone(cellWorld),
                FormIDOrZero(a_sourceWorld),
                EditorIDOrNone(a_sourceWorld));
        }

        void LogGateScanCandidate(
            std::uint64_t a_scan,
            const char* a_scope,
            RE::TESObjectCELL* a_sourceCell,
            RE::TESWorldSpace* a_sourceWorld,
            RE::TESObjectREFR* a_door,
            RE::TESObjectCELL* a_transitionCell,
            RE::TESObjectREFR* a_linkedDoor,
            RE::TESObjectCELL* a_linkedCell,
            RE::TESObjectCELL* a_destination,
            const char* a_chosenBy,
            const char* a_decision) noexcept
        {
            if (!PreloadDiagnostics::IsInstalled()) {
                return;
            }
            const bool destinationInterior =
                a_destination && a_destination->IsInterior();
            auto* destinationWorld =
                a_destination && !destinationInterior
                    ? a_destination->worldSpace
                    : nullptr;
            const auto& position = a_door->data.location;
            logger::info(
                "PreloadDiag event=gate-scan-candidate scan={} scope={} "
                "sourceCell={:08X} sourceWorld={:08X} "
                "door={:08X} doorEditor={} pos=({:.1f},{:.1f},{:.1f}) "
                "transitionCell={:08X} transitionEditor={} "
                "linkedDoor={:08X} linkedDoorEditor={} "
                "linkedCell={:08X} linkedCellEditor={} chosenBy={} "
                "destination={:08X} destinationEditor={} "
                "destinationInterior={} destinationWorld={:08X} "
                "destinationWorldEditor={} decision={}",
                a_scan,
                a_scope,
                FormIDOrZero(a_sourceCell),
                FormIDOrZero(a_sourceWorld),
                FormIDOrZero(a_door),
                EditorIDOrNone(a_door),
                position.x,
                position.y,
                position.z,
                FormIDOrZero(a_transitionCell),
                EditorIDOrNone(a_transitionCell),
                FormIDOrZero(a_linkedDoor),
                EditorIDOrNone(a_linkedDoor),
                FormIDOrZero(a_linkedCell),
                EditorIDOrNone(a_linkedCell),
                a_chosenBy,
                FormIDOrZero(a_destination),
                EditorIDOrNone(a_destination),
                destinationInterior,
                FormIDOrZero(destinationWorld),
                EditorIDOrNone(destinationWorld),
                a_decision);
        }

        void LogGateScanSummary(
            std::uint64_t a_scan,
            const char* a_scope,
            const GateScanStats& a_stats) noexcept
        {
            if (!PreloadDiagnostics::IsInstalled()) {
                return;
            }
            logger::info(
                "PreloadDiag event=gate-scan-summary scan={} scope={} "
                "references={} teleportDoors={} disabledOrDeleted={} "
                "noDestination={} interiorDestination={} "
                "noDestinationWorld={} sameWorld={} accepted={}",
                a_scan,
                a_scope,
                a_stats.references,
                a_stats.teleportDoors,
                a_stats.disabledOrDeleted,
                a_stats.noDestination,
                a_stats.interiorDestination,
                a_stats.noDestinationWorld,
                a_stats.sameWorld,
                a_stats.accepted);
        }

        struct GateEnumStats
        {
            std::size_t callbacks = 0;
            std::size_t nullReference = 0;
            std::size_t disabledOrDeleted = 0;
            std::size_t missingTeleportData = 0;
            std::size_t sourceWorldMissing = 0;
            std::size_t sourceWorldMismatch = 0;
            std::size_t noDestination = 0;
            std::size_t interiorDestination = 0;
            std::size_t noDestinationWorld = 0;
            std::size_t sameWorld = 0;
            std::size_t accepted = 0;
            std::size_t handleUnavailable = 0;
            std::size_t duplicate = 0;
            std::size_t capacityDropped = 0;
            std::size_t candidateLogsDropped = 0;
        };

        // Do not let the diagnostic record cap change traversal correctness.
        // The engine has already radius-bounded this query; retain a generous,
        // bounded retained-reference snapshot, then cap only qualifying
        // cross-world gates.
        constexpr std::size_t kMaxLoadedGateCandidates = 4096;
        constexpr std::size_t kMaxLoadedGates = 64;
        constexpr std::size_t kMaxGateCandidateLogs = 64;

        struct LoadedGateCandidate
        {
            std::uint32_t formID = 0;
            RE::NiPointer<RE::TESObjectREFR> door{};
        };

        struct GateEnumContext
        {
            std::uint64_t scan = 0;
            RE::TESObjectCELL* playerCell = nullptr;
            RE::TESWorldSpace* sourceWorld = nullptr;
            RE::NiPoint3 origin{};
            float radius = 0.0F;
            std::vector<LoadedGateCandidate>* candidates = nullptr;
            std::vector<DoorEntry>* output = nullptr;
            GateEnumStats stats{};
            std::size_t candidateLogs = 0;
        };

        void LogGateEnumSource(
            const GateEnumContext& a_context,
            void* a_collection,
            bool a_resolved) noexcept
        {
            if (!PreloadDiagnostics::IsConfigured()) {
                return;
            }
            logger::info(
                "PreloadDiag event=gate-enum-source scan={} "
                "backend=native-loaded-reference-collection resolved={} "
                "collectionPtr={:#x} sourceCellPtr={:#x} sourceCell={:08X} "
                "sourceEditor={} sourceState={} loadedData={:#x} "
                "sourceWorld={:08X} sourceWorldEditor={} "
                "origin=({:.1f},{:.1f},{:.1f}) radius={:.1f}",
                a_context.scan,
                a_resolved,
                reinterpret_cast<std::uintptr_t>(a_collection),
                reinterpret_cast<std::uintptr_t>(a_context.playerCell),
                FormIDOrZero(a_context.playerCell),
                EditorIDOrNone(a_context.playerCell),
                a_context.playerCell
                    ? a_context.playerCell->cellState.underlying()
                    : 0,
                reinterpret_cast<std::uintptr_t>(
                    a_context.playerCell
                        ? a_context.playerCell->loadedData
                        : nullptr),
                FormIDOrZero(a_context.sourceWorld),
                EditorIDOrNone(a_context.sourceWorld),
                a_context.origin.x,
                a_context.origin.y,
                a_context.origin.z,
                a_context.radius);
        }

        void LogGateEnumCandidate(
            const GateEnumContext& a_context,
            RE::TESObjectREFR* a_door,
            RE::TESObjectCELL* a_sourceCell,
            RE::TESWorldSpace* a_sourceWorld,
            const ResolvedDoorDestination& a_destination,
            float a_distance,
            const char* a_decision) noexcept
        {
            if (!PreloadDiagnostics::IsConfigured() || !a_door) {
                return;
            }

            int computedGridX = 0;
            int computedGridY = 0;
            const bool gridValid = TryGetExteriorGridCenter(
                a_destination, computedGridX, computedGridY);
            const int transitionGridX = a_destination.transitionCell
                ? a_destination.transitionCell->GetDataX()
                : 0;
            const int transitionGridY = a_destination.transitionCell
                ? a_destination.transitionCell->GetDataY()
                : 0;
            const auto linkedPosition = a_destination.linkedDoor
                ? a_destination.linkedDoor->data.location
                : RE::NiPoint3{};
            auto* linkedWorld =
                a_destination.linkedCell &&
                !a_destination.linkedCell->IsInterior()
                    ? a_destination.linkedCell->worldSpace
                    : nullptr;
            logger::info(
                "PreloadDiag event=gate-enum-candidate scan={} "
                "backend=native-loaded-reference-collection "
                "door={:08X} doorEditor={} sourceCell={:08X} "
                "sourceCellEditor={} sourceWorld={:08X} "
                "sourceWorldEditor={} distance={:.1f} "
                "transitionCell={:08X} transitionEditor={} "
                "transitionGrid=({}, {}) linkedDoor={:08X} "
                "linkedDoorEditor={} linkedCell={:08X} linkedCellEditor={} "
                "linkedCellSource={} linkedWorld={:08X} linkedWorldEditor={} "
                "xtelPos=({:.1f},{:.1f},{:.1f}) "
                "linkedPos=({:.1f},{:.1f},{:.1f}) "
                "computedGrid=({}, {}) gridValid={} "
                "centerSource=xtel-position decision={}",
                a_context.scan,
                FormIDOrZero(a_door),
                EditorIDOrNone(a_door),
                FormIDOrZero(a_sourceCell),
                EditorIDOrNone(a_sourceCell),
                FormIDOrZero(a_sourceWorld),
                EditorIDOrNone(a_sourceWorld),
                a_distance,
                FormIDOrZero(a_destination.transitionCell),
                EditorIDOrNone(a_destination.transitionCell),
                transitionGridX,
                transitionGridY,
                FormIDOrZero(a_destination.linkedDoor.get()),
                EditorIDOrNone(a_destination.linkedDoor.get()),
                FormIDOrZero(a_destination.linkedCell),
                EditorIDOrNone(a_destination.linkedCell),
                a_destination.linkedCellFromSaveParent
                    ? "save-parent"
                    : (a_destination.linkedCell ? "live-parent" : "none"),
                FormIDOrZero(linkedWorld),
                EditorIDOrNone(linkedWorld),
                a_destination.xtelPosition.x,
                a_destination.xtelPosition.y,
                a_destination.xtelPosition.z,
                linkedPosition.x,
                linkedPosition.y,
                linkedPosition.z,
                computedGridX,
                computedGridY,
                gridValid,
                a_decision);
        }

        void LogGateEnumSummary(
            const GateEnumContext& a_context,
            void* a_collection,
            bool a_resolved) noexcept
        {
            if (!PreloadDiagnostics::IsConfigured()) {
                return;
            }
            const auto& stats = a_context.stats;
            logger::info(
                "PreloadDiag event=gate-enum-summary scan={} "
                "backend=native-loaded-reference-collection resolved={} "
                "collectionPtr={:#x} callbacks={} nullReference={} "
                "disabledOrDeleted={} missingTeleportData={} "
                "sourceWorldMissing={} sourceWorldMismatch={} "
                "noDestination={} interiorDestination={} "
                "noDestinationWorld={} sameWorld={} accepted={} "
                "handleUnavailable={} duplicate={} "
                "capacityDropped={} candidateLogs={} candidateLogsDropped={} "
                "cached={}",
                a_context.scan,
                a_resolved,
                reinterpret_cast<std::uintptr_t>(a_collection),
                stats.callbacks,
                stats.nullReference,
                stats.disabledOrDeleted,
                stats.missingTeleportData,
                stats.sourceWorldMissing,
                stats.sourceWorldMismatch,
                stats.noDestination,
                stats.interiorDestination,
                stats.noDestinationWorld,
                stats.sameWorld,
                stats.accepted,
                stats.handleUnavailable,
                stats.duplicate,
                stats.capacityDropped,
                a_context.candidateLogs,
                stats.candidateLogsDropped,
                a_context.output ? a_context.output->size() : 0);
        }

        // Operational gate discovery uses Fallout's loaded-reference spatial
        // collection. The legacy current/persistent cell scans below remain only
        // as diagnostics-on comparison evidence and never feed a preload request.
        RE::TESWorldSpace* s_loadedGateWorldSpace = nullptr;
        std::vector<DoorEntry> s_loadedGates;
        std::chrono::steady_clock::time_point s_lastLoadedGateScan{};
        constexpr auto kLoadedGateRescanInterval = std::chrono::seconds(2);

        void MaybeLogGateEnumCandidate(
            GateEnumContext& a_context,
            RE::TESObjectREFR* a_door,
            RE::TESObjectCELL* a_sourceCell,
            RE::TESWorldSpace* a_sourceWorld,
            const ResolvedDoorDestination& a_destination,
            float a_distance,
            const char* a_decision) noexcept
        {
            if (!PreloadDiagnostics::IsConfigured()) {
                return;
            }
            if (a_context.candidateLogs >= kMaxGateCandidateLogs) {
                ++a_context.stats.candidateLogsDropped;
                return;
            }
            LogGateEnumCandidate(
                a_context,
                a_door,
                a_sourceCell,
                a_sourceWorld,
                a_destination,
                a_distance,
                a_decision);
            ++a_context.candidateLogs;
        }

        // Called synchronously while Fallout's loaded-reference collection holds
        // its own read lock. Do not follow references, inspect ExtraTeleport, log,
        // or issue preload work here. Retain only a bounded FormID + NiPointer
        // snapshot. Taking the smart reference while the collection read lock is
        // held keeps each candidate alive until classification after return.
        bool CollectLoadedTeleportDoorCandidate(
            RE::TESObjectREFR* a_door,
            std::intptr_t a_contextValue)
        {
            auto* context =
                reinterpret_cast<GateEnumContext*>(a_contextValue);
            if (!context) {
                // RunEnumRefsFunc treats a non-zero visitor result as STOP.
                // With no valid destination for the snapshot, fail closed.
                return true;
            }
            auto& stats = context->stats;
            ++stats.callbacks;
            if (!a_door) {
                ++stats.nullReference;
                return false;
            }
            const auto formID = a_door->GetFormID();
            if (!context->candidates ||
                context->candidates->size() >=
                    kMaxLoadedGateCandidates) {
                ++stats.capacityDropped;
                // The bounded snapshot is full. Stop under the engine's read
                // lock instead of traversing references we cannot retain.
                return true;
            }
            context->candidates->push_back({
                formID,
                RE::NiPointer<RE::TESObjectREFR>{ a_door }
            });
            return false;  // false = continue; true = stop
        }

        void ClassifyLoadedTeleportDoorCandidates(
            GateEnumContext& a_context)
        {
            if (!a_context.candidates || !a_context.output) {
                return;
            }
            for (const auto& candidate : *a_context.candidates) {
                const auto formID = candidate.formID;
                auto* door = candidate.door.get();
                if (!door) {
                    ++a_context.stats.nullReference;
                    continue;
                }

                const auto& doorPosition = door->data.location;
                const float dx = doorPosition.x - a_context.origin.x;
                const float dy = doorPosition.y - a_context.origin.y;
                const float dz = doorPosition.z - a_context.origin.z;
                const float distance =
                    std::sqrt(dx * dx + dy * dy + dz * dz);
                auto* sourceCell = door->GetParentCell();
                if (!sourceCell) {
                    sourceCell = door->GetSaveParentCell();
                }
                auto* sourceWorld =
                    sourceCell && !sourceCell->IsInterior()
                        ? sourceCell->worldSpace
                        : nullptr;
                ResolvedDoorDestination destination{};

                if (door->IsDisabled() || door->IsDeleted()) {
                    ++a_context.stats.disabledOrDeleted;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "disabled-or-deleted");
                    continue;
                }
                auto* teleport = door->extraList
                    ? door->extraList->GetByType<RE::ExtraTeleport>()
                    : nullptr;
                if (!teleport || !teleport->teleportData) {
                    ++a_context.stats.missingTeleportData;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "missing-teleport-data");
                    continue;
                }
                if (!sourceWorld) {
                    ++a_context.stats.sourceWorldMissing;
                    (void)ResolveDoorDestination(door, destination);
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "source-worldspace-missing");
                    continue;
                }
                if (sourceWorld != a_context.sourceWorld) {
                    ++a_context.stats.sourceWorldMismatch;
                    (void)ResolveDoorDestination(door, destination);
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "source-worldspace-mismatch");
                    continue;
                }
                if (!ResolveDoorDestination(door, destination)) {
                    ++a_context.stats.noDestination;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "no-destination");
                    continue;
                }
                if (destination.cell->IsInterior()) {
                    ++a_context.stats.interiorDestination;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "interior-preload-retired");
                    continue;
                }
                if (!destination.world) {
                    ++a_context.stats.noDestinationWorld;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "destination-worldspace-missing");
                    continue;
                }
                if (destination.world == a_context.sourceWorld) {
                    ++a_context.stats.sameWorld;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "same-worldspace");
                    continue;
                }
                const auto doorHandle = door->GetHandle();
                if (!doorHandle) {
                    ++a_context.stats.handleUnavailable;
                    MaybeLogGateEnumCandidate(
                        a_context, door, sourceCell, sourceWorld, destination,
                        distance, "handle-unavailable");
                    continue;
                }
                a_context.output->push_back({
                    formID,
                    RE::NiPoint3{
                        doorPosition.x,
                        doorPosition.y,
                        doorPosition.z },
                    -1,
                    doorHandle
                });
                ++a_context.stats.accepted;
                MaybeLogGateEnumCandidate(
                    a_context, door, sourceCell, sourceWorld, destination,
                    distance, "accepted-cross-world-exterior");
            }
        }

        void EnumerateLoadedTeleportGates(
            RE::TESObjectCELL* a_playerCell,
            RE::TESWorldSpace* a_sourceWorld,
            const RE::NiPoint3& a_origin,
            float a_radius,
            std::vector<DoorEntry>& a_out)
        {
            a_out.clear();
            a_out.reserve(kMaxLoadedGates);
            std::vector<LoadedGateCandidate> candidates;
            // Allocate the full bounded snapshot before entering Fallout's
            // collection read lock. The visitor then performs only GetFormID +
            // NiPointer retention + append; sorting, deduplication and reference
            // inspection happen after the native enumerator releases its lock.
            candidates.reserve(kMaxLoadedGateCandidates);
            GateEnumContext context{};
            context.scan =
                s_gateScanSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            context.playerCell = a_playerCell;
            context.sourceWorld = a_sourceWorld;
            context.origin = a_origin;
            context.radius = a_radius;
            context.candidates = &candidates;
            context.output = &a_out;

            void* collection =
                TryReadPointerSlot<void>(
                    s_loadedReferenceCollectionPtr);
            const bool resolved =
                s_gateEnumerationResolved.load(std::memory_order_acquire) &&
                s_enumTeleportDoors != nullptr &&
                s_loadedReferenceCollectionPtr != nullptr &&
                collection != nullptr;
            LogGateEnumSource(context, collection, resolved);
            if (resolved && a_playerCell && a_sourceWorld &&
                std::isfinite(a_radius) && a_radius > 0.0F) {
                // The native caller supplies the same player-centered sphere to
                // both filters. Preserve that engine-owned selection path exactly.
                RE::NiPoint3 firstCenter = a_origin;
                RE::NiPoint3 secondCenter = a_origin;
                s_enumTeleportDoors(
                    collection,
                    firstCenter,
                    a_radius,
                    secondCenter,
                    a_radius,
                    &CollectLoadedTeleportDoorCandidate,
                    reinterpret_cast<std::intptr_t>(&context));
                // The native method has returned and released its collection lock.
                // It is now safe to deduplicate, inspect retained doors, follow
                // XTEL links, and log. Avoiding the old O(n^2) duplicate search
                // inside the callback shortens Fallout's read-lock hold time.
                std::sort(
                    candidates.begin(),
                    candidates.end(),
                    [](const LoadedGateCandidate& a_left,
                       const LoadedGateCandidate& a_right) {
                        return a_left.formID < a_right.formID;
                    });
                const auto uniqueEnd = std::unique(
                    candidates.begin(),
                    candidates.end(),
                    [](const LoadedGateCandidate& a_left,
                       const LoadedGateCandidate& a_right) {
                        return a_left.formID == a_right.formID;
                    });
                context.stats.duplicate += static_cast<std::size_t>(
                    std::distance(uniqueEnd, candidates.end()));
                candidates.erase(uniqueEnd, candidates.end());
                if (context.stats.capacityDropped == 0) {
                    // Classify the complete bounded snapshot first. Capping while
                    // FormIDs were numerically sorted allowed an arbitrary first
                    // 64 to hide a much nearer gate (including Diamond City's).
                    a_out.reserve(candidates.size());
                    ClassifyLoadedTeleportDoorCandidates(context);
                    const auto distanceSquared =
                        [&a_origin](const DoorEntry& a_entry) {
                            const float dx = a_entry.pos.x - a_origin.x;
                            const float dy = a_entry.pos.y - a_origin.y;
                            const float dz = a_entry.pos.z - a_origin.z;
                            return dx * dx + dy * dy + dz * dz;
                        };
                    std::sort(
                        a_out.begin(),
                        a_out.end(),
                        [&distanceSquared](
                            const DoorEntry& a_left,
                            const DoorEntry& a_right) {
                            const float leftDistance =
                                distanceSquared(a_left);
                            const float rightDistance =
                                distanceSquared(a_right);
                            return leftDistance < rightDistance ||
                                (leftDistance == rightDistance &&
                                 a_left.doorFormID < a_right.doorFormID);
                        });
                    if (a_out.size() > kMaxLoadedGates) {
                        context.stats.capacityDropped +=
                            a_out.size() - kMaxLoadedGates;
                        a_out.resize(kMaxLoadedGates);
                    }
                } else {
                    // Once the collection snapshot overflows, it can no longer
                    // prove that the retained set contains the globally nearest
                    // gate. Fail this scan closed instead of issuing an arbitrary
                    // preload; the next radius-bounded poll can try again.
                    a_out.clear();
                    logger::warn(
                        "DoorPrefetch: loaded gate snapshot exceeded {} "
                        "references; scan discarded",
                        kMaxLoadedGateCandidates);
                }
            }
            LogGateEnumSummary(context, collection, resolved);
        }

        // Diagnostics-only legacy comparison caches.
        RE::TESObjectCELL* s_cachedCell = nullptr;
        std::vector<DoorEntry> s_cellGates;
        std::chrono::steady_clock::time_point s_lastCellGateScan{};
        RE::TESWorldSpace* s_cachedWorldSpace = nullptr;
        std::vector<DoorEntry> s_wsGates;
        std::chrono::steady_clock::time_point s_lastPersistentGateScan{};
        constexpr auto kCellGateRescanInterval = std::chrono::seconds(2);
        constexpr auto kPersistentGateRescanInterval = std::chrono::seconds(5);
        // One Havok ray from origin along dir; preload whatever load-door it hits.
        // Construct bhkPickData on a generous zeroed buffer (the ctor sets the query
        // params; the real struct is < 0x200). SetStartEnd builds the scaled Havok
        // ray; cell->Pick returns the hit node (null = miss).
        [[nodiscard]] bool CastDoorRay(
            RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_origin,
            const RE::NiPoint3& a_dir, float a_reach, const char* a_trigger)
        {
            if (!s_extendedRay.load(std::memory_order_relaxed) ||
                IsPreloadSuppressed()) {
                return false;
            }
            const RE::NiPoint3 end = a_origin + a_dir * a_reach;
            alignas(16) std::byte buf[0x200];
            std::memset(buf, 0, sizeof(buf));
            s_bhkCtor(buf);
            s_setStartEnd(buf, &a_origin, &end);
            RE::NiAVObject* hit = s_cellPick(a_cell, buf);
            if (!hit) {
                return false;
            }
            if (auto* refr = s_findRef(hit)) {
                if (IsPreloadSuppressed()) {
                    return false;
                }
                const auto& pos = refr->data.location;
                const float dx = pos.x - a_origin.x;
                const float dy = pos.y - a_origin.y;
                const float dz = pos.z - a_origin.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const auto result = TryPreloadFromRef(
                    refr, a_trigger, distance);
                return result == PreRes::kExteriorEngineIssued;
            }
            return false;
        }

        [[nodiscard]] bool IsStableAttachedExteriorSource(
            RE::TESObjectCELL* a_cell,
            RE::TESWorldSpace* a_sourceWorld) noexcept
        {
            return a_cell &&
                !a_cell->IsInterior() &&
                a_sourceWorld != nullptr &&
                a_cell->worldSpace == a_sourceWorld &&
                a_cell->loadedData != nullptr &&
                a_cell->cellState == RE::TESObjectCELL::CELL_STATE::kAttached;
        }

        // Snapshot one attached exterior cell's references while holding the cell's
        // own spin lock, then inspect the retaining NiPointers after releasing it.
        // This avoids racing reference-array mutation without holding an engine lock
        // across teleport-extra inspection and logging.
        void ScanGatesInto(
            RE::TESObjectCELL* a_cell,
            RE::TESWorldSpace* a_sourceWorld,
            std::vector<DoorEntry>& a_out,
            int& a_skipInterior,
            const char* a_scope)
        {
            const auto scan =
                s_gateScanSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const bool stable =
                IsStableAttachedExteriorSource(a_cell, a_sourceWorld);
            LogGateScanSource(
                scan, a_scope, a_cell, a_sourceWorld, stable);
            if (!stable) {
                return;
            }

            std::vector<RE::NiPointer<RE::TESObjectREFR>> references;
            {
                std::lock_guard cellLock(a_cell->spinLock);
                if (!IsStableAttachedExteriorSource(a_cell, a_sourceWorld)) {
                    return;
                }
                references.reserve(a_cell->references.size());
                for (const auto& refPtr : a_cell->references) {
                    references.push_back(refPtr);
                }
            }

            GateScanStats stats{};
            for (const auto& refPtr : references) {
                ++stats.references;
                RE::TESObjectREFR* ref = refPtr.get();
                if (!ref || !ref->extraList) {
                    continue;
                }
                auto* tp = ref->extraList->GetByType<RE::ExtraTeleport>();
                if (!tp || !tp->teleportData) {
                    continue;  // not a load door (cheap filter — most refs skip here)
                }
                ++stats.teleportDoors;
                auto* transitionCell = tp->teleportData->transitionCell;
                auto linkedDoorPtr = tp->teleportData->linkedDoor.get();
                auto* linkedDoor = linkedDoorPtr.get();
                auto* linkedCell =
                    linkedDoor ? linkedDoor->GetParentCell() : nullptr;
                bool linkedCellFromSaveParent = false;
                if (!linkedCell && linkedDoor) {
                    linkedCell = linkedDoor->GetSaveParentCell();
                    linkedCellFromSaveParent = linkedCell != nullptr;
                }
                // Match the operational resolver and Fallout's own
                // GetTeleportWorldSpace path: the linked reference's live/save
                // parent is authoritative; transitionCell is only a fallback.
                auto* dest = linkedCell ? linkedCell : transitionCell;
                const char* chosenBy = linkedCell
                    ? (linkedCellFromSaveParent
                        ? "linked-door-save-parent"
                        : "linked-door-parent")
                    : (transitionCell ? "transition-cell" : "none");
                const char* decision = "accepted-cross-world-exterior";

                if (ref->IsDisabled() || ref->IsDeleted()) {
                    ++stats.disabledOrDeleted;
                    decision = "disabled-or-deleted";
                    LogGateScanCandidate(
                        scan, a_scope, a_cell, a_sourceWorld, ref,
                        transitionCell, linkedDoor, linkedCell, dest,
                        chosenBy, decision);
                    continue;
                }
                if (!dest) {
                    ++stats.noDestination;
                    decision = "no-destination";
                    LogGateScanCandidate(
                        scan, a_scope, a_cell, a_sourceWorld, ref,
                        transitionCell, linkedDoor, linkedCell, dest,
                        chosenBy, decision);
                    continue;
                }
                if (dest->IsInterior()) {
                    ++stats.interiorDestination;
                    ++a_skipInterior;
                    decision = "interior-preload-retired";
                    LogGateScanCandidate(
                        scan, a_scope, a_cell, a_sourceWorld, ref,
                        transitionCell, linkedDoor, linkedCell, dest,
                        chosenBy, decision);
                    continue;
                }
                // Gate proximity is deliberately exterior and cross-worldspace
                // only. Same-world exterior doors belong to the live grid and
                // must never enter the speculative exterior loader.
                auto* destWorld = dest->worldSpace;
                if (!destWorld) {
                    ++stats.noDestinationWorld;
                    decision = "destination-worldspace-missing";
                    LogGateScanCandidate(
                        scan, a_scope, a_cell, a_sourceWorld, ref,
                        transitionCell, linkedDoor, linkedCell, dest,
                        chosenBy, decision);
                    continue;
                }
                if (destWorld == a_sourceWorld) {
                    ++stats.sameWorld;
                    decision = "same-worldspace";
                    LogGateScanCandidate(
                        scan, a_scope, a_cell, a_sourceWorld, ref,
                        transitionCell, linkedDoor, linkedCell, dest,
                        chosenBy, decision);
                    continue;
                }
                const auto& loc = ref->data.location;
                a_out.push_back({ ref->GetFormID(), RE::NiPoint3{ loc.x, loc.y, loc.z } });
                ++stats.accepted;
                LogGateScanCandidate(
                    scan, a_scope, a_cell, a_sourceWorld, ref,
                    transitionCell, linkedDoor, linkedCell, dest,
                    chosenBy, decision);
            }
            LogGateScanSummary(scan, a_scope, stats);
        }

        // Scan only the engine-owned, currently attached player cell. Cross-cell
        // discovery through TESWorldSpace::cellMap was removed because that table has
        // no proven reader lock while exterior streaming can mutate it. Persistent
        // gates are scanned separately from TESWorldSpace::persistentCell below.
        void ScanGatesInCurrentCell(
            RE::TESObjectCELL* a_cell,
            std::vector<DoorEntry>& a_out)
        {
            a_out.clear();
            if (!a_cell || a_cell->IsInterior() || !a_cell->worldSpace) {
                return;
            }
            int skipInterior = 0;
            RE::TESWorldSpace* ws = a_cell->worldSpace;
            ScanGatesInto(
                a_cell, ws, a_out, skipInterior, "player-current-cell");
            if (!a_out.empty() || skipInterior) {
                logger::info(
                    "DoorPrefetch: current-cell scan — {} exterior gate(s), "
                    "{} interior-dest skipped",
                    a_out.size(),
                    skipInterior);
            }
        }

        void EnterTransitionBarrier() noexcept
        {
            // This store must remain the first operation. FlushQueuedLoads relies
            // on it to close every producer before inspecting plugin bookkeeping.
            s_transitionActive.store(true, std::memory_order_release);
            std::lock_guard stateLock(s_transitionStateMutex);
            // Reassert while serialized with EndTransition/fail-safe in case a
            // concurrent close completed between the first store and this lock.
            s_transitionActive.store(true, std::memory_order_release);
            s_transitionStartedMs.store(
                static_cast<std::uint64_t>(GetTickCount64()),
                std::memory_order_release);
            s_postTransitionQuietUntilMs.store(0, std::memory_order_release);
            s_forceRayOriginReset.store(true, std::memory_order_release);
            s_rayWakeCv.notify_all();
        }

        bool LeaveTransitionBarrierWithQuiet() noexcept
        {
            std::lock_guard stateLock(s_transitionStateMutex);
            if (!s_transitionActive.load(std::memory_order_acquire)) {
                return false;
            }
            const auto now = static_cast<std::uint64_t>(GetTickCount64());
            s_forceGateRescan.store(true, std::memory_order_release);
            s_forceRayOriginReset.store(true, std::memory_order_release);
            // Publish quiet before clearing active; suppression remains continuous.
            s_postTransitionQuietUntilMs.store(
                now + kPostTransitionQuietMs, std::memory_order_release);
            s_transitionStartedMs.store(0, std::memory_order_release);
            s_transitionActive.store(false, std::memory_order_release);
            s_rayWakeCv.notify_all();
            return true;
        }

        [[nodiscard]] bool PollerSourceEnabled() noexcept
        {
            return s_extendedRay.load(std::memory_order_relaxed) ||
                s_gateProximity.load(std::memory_order_relaxed);
        }

        [[nodiscard]] bool PollerSessionEnabled() noexcept
        {
            return s_gameSessionActive.load(std::memory_order_acquire);
        }

        void ParkMissingGameSession(const char* a_reason) noexcept
        {
            if (s_gameSessionActive.exchange(
                    false, std::memory_order_acq_rel)) {
                s_forceGateRescan.store(true, std::memory_order_release);
                s_forceRayOriginReset.store(true, std::memory_order_release);
                // Match explicit session deactivation: publish inactive first,
                // then drain a producer which may already have crossed its last
                // session check before discarding all old-world provenance.
                {
                    std::lock_guard submissionLock(s_submissionMutex);
                    ResetDestinationCooldowns("game-session-lost", true);
                }
                logger::info(
                    "DoorPrefetch: no live game session ({}) — "
                    "shared poller parked",
                    a_reason ? a_reason : "unknown");
                if (const auto callback =
                        s_sessionLostCallback.load(
                            std::memory_order_acquire)) {
                    callback();
                }
            }
            s_rayWakeCv.notify_all();
        }

        void TryConfiguredDoorPrefetch()
        {
            if (!s_gameSessionActive.load(std::memory_order_acquire) ||
                IsPreloadSuppressed()) {
                return;
            }
            const bool runRays =
                s_extendedRay.load(std::memory_order_relaxed) &&
                s_havokResolved.load(std::memory_order_acquire);
            const bool runGateProximity =
                s_gateProximity.load(std::memory_order_relaxed);
            if ((!runRays && !runGateProximity) || !s_preloadWorld ||
                !s_tesSingletonPtr ||
                IsPreloadSuppressed()) {
                return;
            }
            auto* player = GetPlayerForPrefetch();
            if (!player) {
                // NG has no reliable MenuOpenCloseEvent registration. Treat a
                // destroyed player as its return-to-main-menu fail-safe and wait
                // for the next kPostLoadGame/kNewGame activation.
                ParkMissingGameSession("player-unavailable");
                return;
            }
            auto* cell = player->GetParentCell();
            if (!cell || !cell->loadedData) {
                // Transition tasks cannot reach here because suppression parks
                // the worker. Outside a transition, no attached cell means there
                // is no valid gameplay session to poll.
                ParkMissingGameSession("attached-cell-unavailable");
                return;
            }
            if (runRays) {
                bool submittedThisEpisode = false;
                RE::TESCamera* cam =
                    TryReadPointerSlot<RE::TESCamera>(
                        reinterpret_cast<void**>(s_gPlayerCamera));
                if (cam && cam->cameraRoot) {
                    const auto& xf = cam->cameraRoot->world;
                    const RE::NiPoint3 rayOrigin = xf.translate;
                    const bool resetRayOrigin =
                        s_forceRayOriginReset.exchange(
                            false, std::memory_order_acq_rel);
                    const float reach =
                        180.0f * s_rangeMult.load(std::memory_order_relaxed);

                    // Camera-forward ray (Bethesda forward = rotation matrix +Y column).
                    RE::NiPoint3 camFwd{
                        xf.rotate.entry[0].y,
                        xf.rotate.entry[1].y,
                        xf.rotate.entry[2].y
                    };
                    const float fm = std::sqrt(
                        camFwd.x * camFwd.x +
                        camFwd.y * camFwd.y +
                        camFwd.z * camFwd.z);
                    if (fm > 1.0e-4f) {
                        camFwd *= (1.0f / fm);
                        submittedThisEpisode = CastDoorRay(
                            cell, rayOrigin, camFwd, reach, "camera-ray");
                    }

                    // Movement direction (horizontal), normalised. Valid only
                    // while actually moving; skip first tick and teleport jumps.
                    RE::NiPoint3 move{
                        rayOrigin.x - s_prevOrigin.x,
                        rayOrigin.y - s_prevOrigin.y,
                        0.0f
                    };
                    s_prevOrigin = rayOrigin;
                    const float mm =
                        std::sqrt(move.x * move.x + move.y * move.y);
                    const bool moving =
                        !resetRayOrigin && (mm > 4.0f && mm < reach);
                    if (moving && !submittedThisEpisode) {
                        move *= (1.0f / mm);
                        submittedThisEpisode = CastDoorRay(
                            cell, rayOrigin, move, reach, "movement-ray");
                    }
                }
                if (submittedThisEpisode) {
                    // Camera, movement and proximity share one polling episode.
                    // Once any source submits new engine work, defer every other
                    // candidate to the next single-flight game-thread poll.
                    return;
                }
            }

            if (!runGateProximity) {
                return;
            }
            if (IsPreloadSuppressed() || cell->IsInterior() ||
                !IsStableAttachedExteriorSource(cell, cell->worldSpace)) {
                return;
            }

            // Exterior gate proximity is independent of Havok and ray settings.
            // Player and door reference positions share the current cell/world
            // coordinate system, so this remains available when ray resolution
            // fails closed on an unsupported runtime.
            const auto& playerLocation = player->data.location;
            const RE::NiPoint3 proximityOrigin{
                playerLocation.x, playerLocation.y, playerLocation.z
            };

            // Ask Fallout's own loaded-reference spatial collection which teleport
            // doors are near the player. This covers persistent gates without
            // reading TESWorldSpace::cellMap or assuming the persistent cell is
            // attached. The callback only retains bounded smart references; it
            // creates durable engine handles and makes preload decisions after
            // native enumeration has returned.
            const auto now = std::chrono::steady_clock::now();
            const float gateDistance =
                s_gateProximityDistance.load(std::memory_order_relaxed);
            const bool forceRescan =
                s_forceGateRescan.exchange(false, std::memory_order_acq_rel);
            if (forceRescan) {
                s_loadedGateWorldSpace = nullptr;
                s_lastLoadedGateScan = {};
                s_loadedGates.clear();
                s_cachedCell = nullptr;
                s_cachedWorldSpace = nullptr;
                s_lastCellGateScan = {};
                s_lastPersistentGateScan = {};
                s_cellGates.clear();
                s_wsGates.clear();
            }
            RE::TESWorldSpace* ws = cell->worldSpace;
            if (ws != s_loadedGateWorldSpace ||
                s_lastLoadedGateScan.time_since_epoch().count() == 0 ||
                now - s_lastLoadedGateScan >= kLoadedGateRescanInterval) {
                EnumerateLoadedTeleportGates(
                    cell,
                    ws,
                    proximityOrigin,
                    gateDistance,
                    s_loadedGates);
                s_loadedGateWorldSpace = ws;
                s_lastLoadedGateScan = now;
                logger::info(
                    "DoorPrefetch: native loaded-reference gate refresh — "
                    "{} gate(s)",
                    s_loadedGates.size());
            }

            // Keep the old current-cell and persistent-cell walkers only when
            // diagnostics are installed. Their results explain what those legacy
            // strategies can and cannot see, but never feed the operational loop.
            if (PreloadDiagnostics::IsInstalled()) {
                if (cell != s_cachedCell ||
                    s_lastCellGateScan.time_since_epoch().count() == 0 ||
                    now - s_lastCellGateScan >= kCellGateRescanInterval) {
                    ScanGatesInCurrentCell(cell, s_cellGates);
                    s_cachedCell = cell;
                    s_lastCellGateScan = now;
                }
                if (ws != s_cachedWorldSpace ||
                    s_lastPersistentGateScan.time_since_epoch().count() == 0 ||
                    now - s_lastPersistentGateScan >=
                        kPersistentGateRescanInterval) {
                    s_wsGates.clear();
                    int wsSkip = 0;
                    ScanGatesInto(
                        ws ? ws->persistentCell : nullptr,
                        ws,
                        s_wsGates,
                        wsSkip,
                        "world-persistent-cell");
                    s_cachedWorldSpace = ws;
                    s_lastPersistentGateScan = now;
                    logger::info(
                        "DoorPrefetch: diagnostics-only persistent refresh — "
                        "{} gate(s)",
                        s_wsGates.size());
                }
            }

            // Resolve each in-range gate exactly once for this game-thread poll,
            // sort nearest-first, collect resident evidence for every destination,
            // then allow at most one engine PreloadWorld submission. A later poll
            // may select the next destination; exact world/grid/mode cooldowns and
            // bounded revalidation continue to suppress duplicate work.
            {
                const float reachSq = gateDistance * gateDistance;
                struct PreparedGate
                {
                    DoorEntry* entry = nullptr;
                    RE::NiPointer<RE::TESObjectREFR> door{};
                    ResolvedDoorDestination destination{};
                    float distanceSq = 0.0F;
                };
                std::vector<PreparedGate> prepared;
                prepared.reserve(s_loadedGates.size());
                for (auto& gate : s_loadedGates) {
                    if (!gate.doorFormID) {
                        continue;
                    }
                    const float dx = gate.pos.x - proximityOrigin.x;
                    const float dy = gate.pos.y - proximityOrigin.y;
                    const float dz = gate.pos.z - proximityOrigin.z;
                    const float distanceSq = dx * dx + dy * dy + dz * dz;
                    if (distanceSq >= reachSq) {
                        continue;
                    }
                    auto door = gate.doorHandle.get();
                    if (!door || door->IsDisabled() || door->IsDeleted() ||
                        IsPreloadSuppressed()) {
                        continue;
                    }
                    ResolvedDoorDestination destination{};
                    if (!ResolveDoorDestination(door.get(), destination)) {
                        const int result = static_cast<int>(PreRes::kNotDoor);
                        if (result != gate.lastRes) {
                            gate.lastRes = result;
                            logger::info(
                                "DoorPrefetch: gate door={:08X} "
                                "(dist {:.0f}) preload result = {}",
                                gate.doorFormID,
                                std::sqrt(distanceSq),
                                result);
                        }
                        continue;
                    }
                    prepared.push_back({
                        &gate,
                        std::move(door),
                        std::move(destination),
                        distanceSq
                    });
                }
                std::sort(
                    prepared.begin(),
                    prepared.end(),
                    [](const PreparedGate& a_left,
                       const PreparedGate& a_right) {
                        return a_left.distanceSq < a_right.distanceSq;
                    });

                // First collect engine-owned resident/pending evidence for every
                // nearby gate. Two different doors can share one arrival grid while
                // only one linked reference currently has a live parent cell (the
                // Diamond City main gate/elevator pair is one such case). Doing
                // this pass before selection makes the result independent of
                // native enumeration order without resolving XTEL twice.
                for (const auto& gate : prepared) {
                    ObserveResidentExteriorGateDestination(
                        gate.door.get(), &gate.destination);
                }

                for (auto& gate : prepared) {
                    if (IsPreloadSuppressed()) {
                        break;
                    }
                    const float distance = std::sqrt(gate.distanceSq);
                    const auto result = TryPreloadFromRef(
                        gate.door.get(),
                        "gate-proximity",
                        distance,
                        &gate.destination);
                    const int resultCode = static_cast<int>(result);
                    if (resultCode != gate.entry->lastRes) {
                        gate.entry->lastRes = resultCode;
                        logger::info("DoorPrefetch: gate door={:08X} "
                            "(dist {:.0f}) preload result = {} "
                            "(2=resident/busy 7=noDestWS 8=sameWS 9=cooldown "
                            "12=transition 13=noPlayer 14=interiorOrigin "
                            "16=engineReturned 17=noSourceWS 18=quiet "
                            "19=exteriorOff 20=invalidXTEL 21=interiorRetired)",
                            gate.entry->doorFormID, distance, resultCode);
                    }
                    if (result == PreRes::kExteriorEngineIssued) {
                        break;  // at most one new engine submission this poll
                    }
                }
            }
        }

        void ProbeEngineSession()
        {
            if (!s_gameSessionActive.load(std::memory_order_acquire) ||
                IsPreloadSuppressed()) {
                return;
            }
            auto* player = GetPlayerForPrefetch();
            if (!player) {
                ParkMissingGameSession("player-unavailable");
                return;
            }
            auto* cell = player->GetParentCell();
            if (!cell || !cell->loadedData) {
                ParkMissingGameSession("attached-cell-unavailable");
            }
        }

        // ~8 Hz cadence shared by the optional extended rays and the independent
        // exterior-gate proximity source. Only AddTask runs here; all engine
        // access stays on the game thread. Single-flight prevents a mid-load
        // game-thread stall from accumulating a task backlog.
        void ExtendedRayPoller(std::stop_token a_stopToken)
        {
            while (!a_stopToken.stop_requested() &&
                   s_rayPollerRunning.load(std::memory_order_acquire)) {
                std::unique_lock wakeLock(s_rayWakeMutex);
                s_rayWakeCv.wait(wakeLock, [&]() {
                    return a_stopToken.stop_requested() ||
                        !s_rayPollerRunning.load(std::memory_order_acquire) ||
                        PollerSessionEnabled();
                });
                if (a_stopToken.stop_requested() ||
                    !s_rayPollerRunning.load(std::memory_order_acquire)) break;

                if (IsTransitionBarrierActive()) {
                    // Park without scheduling game work. A one-second bounded wait
                    // lets the 120s missed-close fail-safe make progress even if no
                    // lifecycle notification arrives.
                    s_rayWakeCv.wait_for(
                        wakeLock, std::chrono::seconds(1), [&]() {
                            return a_stopToken.stop_requested() ||
                                !s_rayPollerRunning.load(std::memory_order_acquire) ||
                                !PollerSessionEnabled() ||
                                !s_transitionActive.load(std::memory_order_acquire);
                        });
                    continue;
                }
                if (IsPostTransitionQuiet()) {
                    const auto now =
                        static_cast<std::uint64_t>(GetTickCount64());
                    const auto until =
                        s_postTransitionQuietUntilMs.load(std::memory_order_acquire);
                    const auto remaining = until > now ? until - now : 1;
                    s_rayWakeCv.wait_for(
                        wakeLock, std::chrono::milliseconds(remaining), [&]() {
                            return a_stopToken.stop_requested() ||
                                !s_rayPollerRunning.load(std::memory_order_acquire) ||
                                !PollerSessionEnabled() ||
                                s_transitionActive.load(std::memory_order_acquire);
                        });
                    continue;
                }

                // With a prediction source enabled, keep the normal ~8 Hz
                // cadence. With every source off, retain only a one-Hz
                // engine-session probe so AE can publish return-to-main-menu and
                // park Papyrus/manager state despite lacking MenuOpenCloseEvent.
                const bool predictionEnabled = PollerSourceEnabled();
                const auto cadence = predictionEnabled
                    ? std::chrono::milliseconds(120)
                    : std::chrono::milliseconds(1000);
                const bool interrupted = s_rayWakeCv.wait_for(
                    wakeLock, cadence, [&]() {
                        return a_stopToken.stop_requested() ||
                            !s_rayPollerRunning.load(std::memory_order_acquire) ||
                            !PollerSessionEnabled() ||
                            IsTransitionBarrierActive() ||
                            PollerSourceEnabled() != predictionEnabled;
                    });
                if (interrupted) {
                    continue;
                }
                wakeLock.unlock();

                bool expected = false;
                if (!s_rayTaskQueued.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    continue;  // exactly one queued or executing task at a time
                }
                if (auto* task = F4SE::GetTaskInterface()) {
                    task->AddTask([]() {
                        struct ResetQueuedFlag
                        {
                            ~ResetQueuedFlag()
                            {
                                s_rayTaskQueued.store(false, std::memory_order_release);
                            }
                        } reset;

                        // This lambda is the sole owner of engine/camera/Havok
                        // access. Keep the single-flight flag set until all work
                        // completes, including early returns and exceptions.
                        if (!s_shuttingDown.load(std::memory_order_acquire) &&
                            PollerSessionEnabled() &&
                            !IsPreloadSuppressed()) {
                            if (PollerSourceEnabled()) {
                                TryConfiguredDoorPrefetch();
                            } else {
                                ProbeEngineSession();
                            }
                        }
                    });
                } else {
                    s_rayTaskQueued.store(false, std::memory_order_release);
                }
            }
        }

        class PickSink : public RE::BSTEventSink<PickRefUpdateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const PickRefUpdateEvent& a_event,
                RE::BSTEventSource<PickRefUpdateEvent>*) override
            {
                if (s_crosshairPickEnabled.load(std::memory_order_relaxed) &&
                    s_gameSessionActive.load(std::memory_order_acquire) &&
                    a_event.refChanged &&
                    !IsPreloadSuppressed()) {
                    if (auto refr = a_event.pickRef.get()) {
                        if (IsPreloadSuppressed()) {
                            return RE::BSEventNotifyControl::kContinue;
                        }
                        TryPreloadFromRef(
                            refr.get(),
                            REL::Module::IsVR() ? "vr-pick" : "crosshair-pick",
                            -1.0F);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
            static PickSink* GetSingleton()
            {
                static PickSink instance;
                return &instance;
            }
        };

        // This guard is declared after every poller-owned cache, so it is
        // destroyed first. Stop/join while all state touched by the worker (or
        // by an already-queued, now-inert game task) is still alive.
        struct PollerLifetimeGuard
        {
            ~PollerLifetimeGuard()
            {
                s_shuttingDown.store(true, std::memory_order_release);
                s_transitionActive.store(true, std::memory_order_release);
                s_crosshairPickEnabled.store(false, std::memory_order_relaxed);
                s_gameSessionActive.store(false, std::memory_order_release);
                s_exteriorPreloadEnabled.store(false, std::memory_order_relaxed);
                s_extendedRay.store(false, std::memory_order_relaxed);
                s_gateProximity.store(false, std::memory_order_relaxed);
                s_rayPollerRunning.store(false, std::memory_order_release);
                s_rayWakeCv.notify_all();
                if (s_rayPollerThread.joinable()) {
                    s_rayPollerThread.request_stop();
                    s_rayPollerThread.join();
                }
            }
        } s_pollerLifetimeGuard;

        struct EventArrayHeader
        {
            void*         data;
            std::uint32_t capacity;
            std::uint32_t pad0C;
            std::uint32_t size;
            std::uint32_t pad14;
        };
        static_assert(sizeof(EventArrayHeader) == 0x18);

        bool IsPlausibleEventArray(const EventArrayHeader& a_array)
        {
            constexpr std::uint32_t kMaxPlausibleSinks = 4096;
            if (a_array.size > a_array.capacity ||
                a_array.capacity > kMaxPlausibleSinks) {
                return false;
            }
            return a_array.capacity == 0 || a_array.data != nullptr;
        }

        bool RegisterAt(RE::PlayerCharacter* a_pc, std::uintptr_t a_offset)
        {
            const auto srcAddr = reinterpret_cast<std::uintptr_t>(a_pc) + a_offset;
            // BSTEventSource is NOT polymorphic: +0x00 is its BSSpinLock, not a
            // vtable. The old "vtable must be in the EXE" guard therefore rejected
            // every valid, normally-unlocked source (first qword == 0) and made
            // crosshair preload effectively OFF despite its config being ON.
            //
            // Validate all three embedded BSTArray headers instead. A valid source
            // is 0x58 bytes: lock @00, arrays @08/@20/@38, notifying byte @50.
            // Wrong offsets fail closed without writing through RegisterSink.
            const auto* arrays = reinterpret_cast<const EventArrayHeader*>(srcAddr + 0x08);
            const auto notifying = *reinterpret_cast<const std::uint8_t*>(srcAddr + 0x50);
            if (!IsPlausibleEventArray(arrays[0]) ||
                !IsPlausibleEventArray(arrays[1]) ||
                !IsPlausibleEventArray(arrays[2]) ||
                notifying > 32) {
                logger::warn("DoorPrefetch: event source @+{:#x} failed layout validation — skip",
                    a_offset);
                return false;
            }
            reinterpret_cast<RE::BSTEventSource<PickRefUpdateEvent>*>(srcAddr)
                ->RegisterSink(PickSink::GetSingleton());
            return true;
        }

        enum class EntryValidation
        {
            kInvalid,
            kOriginal,
            kMinHookDetour
        };

        [[nodiscard]] EntryValidation ValidateEntry(
            const void* a_address,
            const std::uint8_t* a_expected,
            const char* a_mask,
            std::size_t a_size) noexcept
        {
            if (!a_address || !a_expected || !a_mask || a_size == 0) {
                return EntryValidation::kInvalid;
            }
            __try {
                const auto* bytes =
                    static_cast<const std::uint8_t*>(a_address);
                bool matches = true;
                for (std::size_t i = 0; i < a_size; ++i) {
                    if (a_mask[i] == 'x' && bytes[i] != a_expected[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return EntryValidation::kOriginal;
                }

                // PreloadDiagnostics is installed before DoorPrefetch and uses
                // MinHook on these same exact entries. Accept only MinHook's
                // canonical x64 entry stubs; that hook already
                // validated the untouched prologue before creating the detour.
                const bool minHookRelative = bytes[0] == 0xE9;
                const bool minHookAbsolute =
                    bytes[0] == 0xFF && bytes[1] == 0x25 &&
                    bytes[2] == 0x00 && bytes[3] == 0x00 &&
                    bytes[4] == 0x00 && bytes[5] == 0x00;
                if (!REL::Module::IsNG() &&
                    PreloadDiagnostics::IsInstalled() &&
                    (minHookRelative || minHookAbsolute)) {
                    return EntryValidation::kMinHookDetour;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return EntryValidation::kInvalid;
            }
            return EntryValidation::kInvalid;
        }

        bool ResolvePreloadFns()
        {
            s_preloadWorld = nullptr;
            s_tesSingletonPtr = nullptr;
            s_enumTeleportDoors = nullptr;
            s_loadedReferenceCollectionPtr = nullptr;
            s_ngPlayerSingletonPtr = nullptr;
            s_exteriorPreloadResolved.store(false, std::memory_order_release);
            s_gateEnumerationResolved.store(false, std::memory_order_release);

            try {
                if (!IsExactSupportedRuntime()) {
                    return false;
                }
                const auto base = REL::Module::get().base();
                const auto version = REL::Module::get().version();
                std::uintptr_t worldRva = 0;
                std::uintptr_t tesGlobalRva = 0;
                std::uintptr_t enumTeleportDoorsRva = 0;
                std::uintptr_t loadedReferenceCollectionGlobalRva = 0;
                if (REL::Module::IsVR()) {                          // 1.2.72.0
                    worldRva = 0xFC0A0;
                    tesGlobalRva = 0x5B042C0;
                    enumTeleportDoorsRva = 0x360F50;
                    loadedReferenceCollectionGlobalRva = 0x5A379F8;
                } else if (!REL::Module::IsNG()) {
                    worldRva = 0xFC0D0;
                    tesGlobalRva = 0x5AA4288;
                    enumTeleportDoorsRva = 0x37A860;
                    loadedReferenceCollectionGlobalRva = 0x59D64C8;
                }

                void* world = nullptr;
                void* enumTeleportDoors = nullptr;
                void** tesSingletonSlot = nullptr;
                void** loadedCollectionSlot = nullptr;
                void** ngPlayerSlot = nullptr;
                if (REL::Module::IsNG()) {
                    // Official IDs are shared by the verified 1.11.221 and
                    // 1.11.240 Address Library bins; no build-specific raw RVA
                    // is retained on NG.
                    world = reinterpret_cast<void*>(
                        REL::Relocation<std::uintptr_t>{
                            REL::ID(kPreloadWorldIDNG)
                        }.address());
                    enumTeleportDoors = reinterpret_cast<void*>(
                        REL::Relocation<std::uintptr_t>{
                            REL::ID(kEnumTeleportDoorsIDNG)
                        }.address());
                    tesSingletonSlot = reinterpret_cast<void**>(
                        REL::Relocation<std::uintptr_t>{
                            REL::ID(kGameTESGlobalIDNG)
                        }.address());
                    loadedCollectionSlot = reinterpret_cast<void**>(
                        REL::Relocation<std::uintptr_t>{
                            REL::ID(kLoadedReferencesGlobalIDNG)
                        }.address());
                    ngPlayerSlot = reinterpret_cast<void**>(
                        REL::Relocation<std::uintptr_t>{
                            REL::ID(kPlayerCharacterGlobalIDNG)
                        }.address());
                    if (!MatchesExpectedNGRva(
                            reinterpret_cast<std::uintptr_t>(world),
                            0x2D4F20,
                            0x2D5240) ||
                        !MatchesExpectedNGRva(
                            reinterpret_cast<std::uintptr_t>(
                                enumTeleportDoors),
                            0x49A2F0,
                            0x49A610) ||
                        !MatchesExpectedNGRva(
                            reinterpret_cast<std::uintptr_t>(
                                tesSingletonSlot),
                            0x32D20C8,
                            0x32DD158) ||
                        !MatchesExpectedNGRva(
                            reinterpret_cast<std::uintptr_t>(
                                loadedCollectionSlot),
                            0x31E2398,
                            0x31ED418) ||
                        !MatchesExpectedNGRva(
                            reinterpret_cast<std::uintptr_t>(ngPlayerSlot),
                            0x32D22E0,
                            0x32DD370)) {
                        logger::error(
                            "DoorPrefetch: AE exterior-preload Address "
                            "Library mapping does not match the exact runtime "
                            "RVA set; exterior preloading disabled fail closed");
                        return false;
                    }
                    // These CommonLib inline wrappers are active in the native
                    // gate path. This fork uses a lower-bound fallback for a
                    // missing NG ID, so prove each helper's exact mapping before
                    // any later GetHandle/get/GetDataX/GetDataY call can run.
                    if (!MatchesExpectedNGRelocation(
                            kObjectRefHandleGetIDNG,
                            0x22CD40,
                            0x22D060) ||
                        !MatchesExpectedNGRelocation(
                            kTESObjectREFRGetHandleIDNG,
                            0x5191A0,
                            0x5194C0) ||
                        !MatchesExpectedNGRelocation(
                            kTESObjectCELLGetDataXIDNG,
                            0x4C6AB0,
                            0x4C6DD0) ||
                        !MatchesExpectedNGRelocation(
                            kTESObjectCELLGetDataYIDNG,
                            0x4C6AD0,
                            0x4C6DF0)) {
                        logger::error(
                            "DoorPrefetch: AE handle/cell helper Address "
                            "Library mapping mismatch; exterior preloading "
                            "disabled fail closed");
                        return false;
                    }
                } else {
                    // Preserve the verified OG/VR raw paths unchanged.
                    world = reinterpret_cast<void*>(base + worldRva);
                    enumTeleportDoors = reinterpret_cast<void*>(
                        base + enumTeleportDoorsRva);
                    tesSingletonSlot = reinterpret_cast<void**>(
                        base + tesGlobalRva);
                    loadedCollectionSlot = reinterpret_cast<void**>(
                        base + loadedReferenceCollectionGlobalRva);
                }
                constexpr std::uint8_t kWorldBytes[]{
                    0x48, 0x85, 0xD2, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
                    0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57
                };
                constexpr char kWorldMask[] = "xxxxx????xxxxxxxxxx";
                constexpr std::uint8_t kEnumTeleportDoorsLegacyBytes[]{
                    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B,
                    0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0xF3, 0x0F,
                    0x10, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48,
                    0x8D, 0x99, 0x50, 0x01, 0x00, 0x00
                };
                constexpr std::uint8_t kEnumTeleportDoorsNGBytes[]{
                    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B,
                    0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8D,
                    0x99, 0x50, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10,
                    0x84, 0x24, 0x80, 0x00, 0x00, 0x00
                };
                constexpr char kEnumTeleportDoorsMask[] =
                    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

                const auto worldValidation = ValidateEntry(
                    world,
                    kWorldBytes,
                    kWorldMask,
                    sizeof(kWorldBytes));
                const auto enumTeleportDoorsValidation = ValidateEntry(
                    enumTeleportDoors,
                    REL::Module::IsNG()
                        ? kEnumTeleportDoorsNGBytes
                        : kEnumTeleportDoorsLegacyBytes,
                    kEnumTeleportDoorsMask,
                    sizeof(kEnumTeleportDoorsLegacyBytes));
                // Diagnostics never hook this native enumerator. Require the
                // exact untouched prologue.
                const bool tesSlotValid =
                    IsValidatedDataPointerSlot(tesSingletonSlot);
                const bool loadedCollectionSlotValid =
                    IsValidatedDataPointerSlot(loadedCollectionSlot);
                const bool ngPlayerSlotValid =
                    !REL::Module::IsNG() ||
                    IsValidatedDataPointerSlot(ngPlayerSlot);
                if (REL::Module::IsNG() && ngPlayerSlotValid) {
                    s_ngPlayerSingletonPtr = ngPlayerSlot;
                } else if (REL::Module::IsNG()) {
                    logger::error(
                        "DoorPrefetch: AE g_player pointer slot failed "
                        ".data/readability validation; player-dependent "
                        "preloading disabled fail closed");
                }

                if (enumTeleportDoorsValidation == EntryValidation::kOriginal &&
                    loadedCollectionSlotValid) {
                    s_enumTeleportDoors =
                        reinterpret_cast<EnumTeleportDoorsFn>(
                            enumTeleportDoors);
                    s_loadedReferenceCollectionPtr = loadedCollectionSlot;
                    s_gateEnumerationResolved.store(
                        true, std::memory_order_release);
                } else {
                    logger::error(
                        "DoorPrefetch: "
                        "BGSLoadedReferenceCollection::"
                        "EnumTeleportDoorsCloseToPoint signature or global "
                        "slot validation failed — "
                        "gate proximity disabled fail closed");
                }
                if (worldValidation != EntryValidation::kInvalid &&
                    tesSlotValid) {
                    s_preloadWorld =
                        reinterpret_cast<PreloadWorldFn>(world);
                    s_tesSingletonPtr = tesSingletonSlot;
                    s_exteriorPreloadResolved.store(
                        true, std::memory_order_release);
                } else {
                    logger::error(
                        "DoorPrefetch: PreloadWorld signature or "
                        "g_TES slot validation failed — "
                        "exterior preloading disabled fail closed");
                }
                logger::info(
                    "DoorPrefetch: engine entries validated "
                    "(interior=retired, exterior={}, residencyProof={}, "
                    "gateEnumeration={}, runtime={})",
                    worldValidation == EntryValidation::kMinHookDetour
                        ? "diagnostic-detour"
                        : (worldValidation == EntryValidation::kOriginal
                            ? "original"
                            : "unavailable"),
                    "exact-live-linked-cell",
                    enumTeleportDoorsValidation == EntryValidation::kOriginal
                        ? "original"
                        : "unavailable",
                    REL::Module::IsNG()
                        ? (version[2] == 240
                            ? "NG-1.11.240"
                            : "NG-1.11.221")
                        : (REL::Module::IsVR()
                            ? "VR-1.2.72"
                            : "OG-1.10.163"));
            } catch (...) {
                s_preloadWorld = nullptr;
                s_tesSingletonPtr = nullptr;
                s_enumTeleportDoors = nullptr;
                s_loadedReferenceCollectionPtr = nullptr;
                s_ngPlayerSingletonPtr = nullptr;
                s_exteriorPreloadResolved.store(
                    false, std::memory_order_release);
                s_gateEnumerationResolved.store(
                    false, std::memory_order_release);
            }
            return s_preloadWorld != nullptr && s_tesSingletonPtr != nullptr;
        }
    }

    bool DoorPrefetch::ConsumeExteriorSubmissionEvidence(
        void* a_world,
        float a_x,
        float a_y,
        bool a_allowConsume) noexcept
    {
        std::uint32_t worldFormID = 0;
        int gridX = 0;
        int gridY = 0;
        if (!TryReadWorldFormID_SEH(a_world, &worldFormID) ||
            !TryGetExteriorGridCenter(a_x, a_y, gridX, gridY)) {
            return false;
        }

        const ExteriorDestinationKey key{
            worldFormID, gridX, gridY, false
        };
        bool matched = false;
        std::uint32_t matchedSourceWorldFormID = 0;
        {
            std::lock_guard lock(s_seenMutex);

            // Test the inbound destination before source reconciliation: a
            // cross-world request normally has source != arrived target. Leave
            // the node allocated here; the synchronized Flush erases it after
            // Bethesda's Show. A repeated pre-Flush call is one-shot because
            // the fixed tombstone makes it ineligible.
            if (a_allowConsume &&
                !HasExteriorPresentationArrivalLocked(key)) {
                const auto exact =
                    s_exteriorPresentationEvidence.find(key);
                if (Policy::ShouldConsumeExteriorPresentationEvidence(
                        exact != s_exteriorPresentationEvidence.end(),
                        exact != s_exteriorPresentationEvidence.end() &&
                            exact->second.exactLiveArrivalObserved) &&
                    (!s_exteriorPresentationArrivalWorldPending ||
                     exact->second.sourceWorldFormID ==
                         s_exteriorPresentationArrivalWorldFormID)) {
                    matched = true;
                    matchedSourceWorldFormID =
                        exact->second.sourceWorldFormID;
                }
            }

            // Keep the arrival retired until FlushQueuedLoads has published its
            // transition barrier and drained any already-running engine call.
            // Source-world reconciliation is intentionally deferred to that
            // drained boundary, keeping iteration/deallocation out of this exact
            // first-Show hook.
            if (a_allowConsume) {
                TombstoneExteriorPresentationArrivalLocked(key);
            }
            s_exteriorPresentationArrivalWorldPending = true;
            s_exteriorPresentationArrivalWorldFormID = worldFormID;
        }

        if (matched) {
            logger::info(
                "DoorPrefetch: exterior presentation evidence arrival "
                "targetWorld={:08X} center=({}, {}) matched=true "
                "matchedSourceWorld={:08X} exactLiveArrivalObserved=true",
                worldFormID, gridX, gridY,
                matchedSourceWorldFormID);
        }
        return matched;
    }

    void DoorPrefetch::ClearExteriorPresentationEvidence(
        const char* a_reason) noexcept
    {
        std::size_t evidenceCleared = 0;
        std::size_t arrivalsCleared = 0;
        // Match the established submissionMutex -> seenMutex order. A plugin
        // call which already crossed its final gate must finish stamping before
        // a save/session replacement discards the old provenance.
        std::lock_guard submissionLock(s_submissionMutex);
        {
            std::lock_guard seenLock(s_seenMutex);
            evidenceCleared = s_exteriorPresentationEvidence.size();
            arrivalsCleared = s_exteriorPresentationArrivalCount;
            s_exteriorPresentationEvidence.clear();
            s_exteriorPresentationArrivalCount = 0;
            s_exteriorPresentationArrivalWorldPending = false;
            s_exteriorPresentationArrivalWorldFormID = 0;
            s_exteriorPresentationSequence = 0;
        }
        if (evidenceCleared != 0 || arrivalsCleared != 0) {
            logger::info(
                "DoorPrefetch: exterior presentation evidence cleared "
                "(reason={}, evidence={}, arrivals={})",
                a_reason ? a_reason : "unspecified",
                evidenceCleared, arrivalsCleared);
        }
    }

    void DoorPrefetch::FlushQueuedLoads(
        bool a_clearPresentationEvidence)
    {
        // EnterTransitionBarrier's first operation publishes the fail-closed gate.
        // Then wait for a call already inside an engine preload entry to return.
        // We never cancel, wait on, or otherwise manipulate the global cell loader;
        // work accepted by the engine remains entirely engine-owned.
        EnterTransitionBarrier();
        std::lock_guard submissionLock(s_submissionMutex);
        // The drain above prevents an older plugin submission from stamping a
        // destination after this boundary reset. Native linked-area throttling is
        // reset here as well so a new load/session never inherits the prior
        // world's suppression generation.
        ResetDestinationCooldowns(
            "load-transition", a_clearPresentationEvidence);
    }

    void DoorPrefetch::EndTransition()
    {
        if (s_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        if (LeaveTransitionBarrierWithQuiet()) {
            logger::info(
                "DoorPrefetch: transition ended; prediction quiet={}ms, "
                "then native gate cache rescan (immediate reverse uses exact-grid "
                "live linked-cell residency; unresolved void submissions get at "
                "most one materially-closer revalidation)",
                kPostTransitionQuietMs);
        }
    }

    void DoorPrefetch::Install()
    {
        s_shuttingDown.store(false, std::memory_order_release);
        s_gameSessionActive.store(false, std::memory_order_release);
        s_exteriorResolutionAttempted.store(true, std::memory_order_release);
        s_gateEnumerationResolutionAttempted.store(
            true, std::memory_order_release);
        if (!IsExactSupportedRuntime()) {
            s_exteriorPreloadEnabled.store(false, std::memory_order_release);
            s_gateProximity.store(false, std::memory_order_relaxed);
            logger::warn("DoorPrefetch: unsupported runtime — disabled fail closed");
            return;
        }
        if (!ResolvePreloadFns() || !WorldspacePreload::Install()) {
            s_exteriorPreloadEnabled.store(false, std::memory_order_release);
            s_gateProximity.store(false, std::memory_order_relaxed);
            logger::warn("DoorPrefetch: exterior preload entry or worldspace lifetime contract unresolved — disabled");
            return;
        }
        s_transitionActive.store(false, std::memory_order_release);
        s_transitionStartedMs.store(0, std::memory_order_release);
        s_postTransitionQuietUntilMs.store(0, std::memory_order_release);
        s_forceGateRescan.store(true, std::memory_order_release);
        s_forceRayOriginReset.store(true, std::memory_order_release);
        ResetDestinationCooldowns("install", true);

        const bool exteriorReady =
            s_exteriorPreloadResolved.load(std::memory_order_acquire) &&
            s_preloadWorld != nullptr &&
            s_tesSingletonPtr != nullptr;
        const bool gateEnumerationReady =
            s_gateEnumerationResolved.load(std::memory_order_acquire) &&
            s_enumTeleportDoors != nullptr &&
            s_loadedReferenceCollectionPtr != nullptr;
        if (!exteriorReady) {
            s_exteriorPreloadEnabled.store(false, std::memory_order_release);
            s_gateProximity.store(false, std::memory_order_relaxed);
            logger::warn(
                "DoorPrefetch: TES::PreloadWorld unavailable — "
                "exterior preload disabled");
        } else if (!gateEnumerationReady) {
            s_gateProximity.store(false, std::memory_order_relaxed);
            logger::warn(
                "DoorPrefetch: native loaded-reference gate enumeration "
                "unavailable — gate proximity disabled; targeted exterior "
                "pick/ray preloading remains available");
        }
        logger::info(
            "DoorPrefetch: exterior-only engine preload path ready "
            "(exterior={}, interior=retired, gateEnumeration={}, runtime={})",
            exteriorReady ? "single-arrival-cell" : "unavailable",
            gateEnumerationReady ? "native" : "unavailable",
            REL::Module::IsNG()
                ? "NG"
                : (REL::Module::IsVR() ? "VR" : "OG"));

        // Resolve the optional Havok path, but start the shared poller regardless:
        // exterior-gate proximity does not use Havok and remains available when
        // ray resolution fails closed.
        const bool havokReady = ResolveHavokRay();
        s_havokResolutionAttempted.store(true, std::memory_order_release);
        if (!havokReady) {
            s_extendedRay.store(false, std::memory_order_relaxed);
            logger::warn("DoorPrefetch: extended ray unavailable on this build (Havok path unresolved)");
        }
        if (!s_rayPollerRunning.exchange(true, std::memory_order_acq_rel)) {
            try {
                s_rayPollerThread = std::jthread(ExtendedRayPoller);
                logger::info(
                    "DoorPrefetch: shared door poller started "
                    "(extendedRay={}, havokReady={}, gateProximity={}, "
                    "gateDistance={:.0f}, exteriorMode={}, {})",
                    s_extendedRay.load(std::memory_order_relaxed),
                    havokReady,
                    s_gateProximity.load(std::memory_order_relaxed),
                    s_gateProximityDistance.load(std::memory_order_relaxed),
                    s_exteriorFullGrid.load(std::memory_order_relaxed)
                        ? "native-uGrids"
                        : "single-arrival-cell",
                    REL::Module::IsNG() ? "NG" : (REL::Module::IsVR() ? "VR" : "OG"));
            } catch (const std::system_error& e) {
                s_rayPollerRunning.store(false, std::memory_order_release);
                logger::error("DoorPrefetch: failed to start shared door poller: {}", e.what());
            }
        }
        s_rayWakeCv.notify_all();
    }

    void DoorPrefetch::SetSessionLostCallback(
        SessionLostCallback a_callback) noexcept
    {
        s_sessionLostCallback.store(
            a_callback, std::memory_order_release);
    }

    void DoorPrefetch::SetGameSessionActive(bool a_active)
    {
        if (s_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        bool changed = false;
        if (a_active) {
            if (!s_gameSessionActive.load(std::memory_order_acquire)) {
                // No poller work is possible while the flag is false. Reset the
                // old generation before publishing the new live session.
                ResetDestinationCooldowns("game-session-activate", true);
                changed = !s_gameSessionActive.exchange(
                    true, std::memory_order_acq_rel);
            }
        } else {
            changed = s_gameSessionActive.exchange(
                false, std::memory_order_acq_rel);
            if (changed) {
                // Publish inactive first, then drain a plugin preload which may
                // already have crossed its final check before clearing state.
                std::lock_guard submissionLock(s_submissionMutex);
                ResetDestinationCooldowns("game-session-deactivate", true);
            }
        }
        if (changed) {
            // Caches remain game-thread-owned. Publish invalidation only; the first
            // poll after activation performs the actual clear/rebuild safely.
            s_forceGateRescan.store(true, std::memory_order_release);
            s_forceRayOriginReset.store(true, std::memory_order_release);
            logger::info(
                "DoorPrefetch: game session {} — shared poller {}",
                a_active ? "active" : "inactive",
                a_active ? "resumed" : "parked");
        }
        s_rayWakeCv.notify_all();
        if (a_active) {
            EnsureRegistered();
        }
    }

    void DoorPrefetch::SetEnabled(bool a_enabled)
    {
        s_crosshairPickEnabled.store(a_enabled, std::memory_order_relaxed);
    }

    void DoorPrefetch::SetExteriorPresentationCorrectionReady(
        bool a_ready) noexcept
    {
        s_exteriorPresentationCorrectionReady.store(
            a_ready, std::memory_order_release);
    }

    void DoorPrefetch::SetExtendedRay(bool a_enabled, float a_rangeMult)
    {
        const bool resolutionAttempted =
            s_havokResolutionAttempted.load(std::memory_order_acquire);
        const bool supported =
            !resolutionAttempted ||
            s_havokResolved.load(std::memory_order_acquire);
        s_extendedRay.store(a_enabled && supported, std::memory_order_relaxed);
        if (std::isfinite(a_rangeMult) && a_rangeMult > 0.0f) {
            s_rangeMult.store(std::clamp(a_rangeMult, 1.0f, 10.0f),
                std::memory_order_relaxed);
        }
        s_rayWakeCv.notify_all();
    }

    void DoorPrefetch::SetExteriorGateProximity(
        bool a_enabled, float a_distanceUnits, int a_gridSelector)
    {
        bool distanceChanged = false;
        if (std::isfinite(a_distanceUnits) && a_distanceUnits > 0.0F) {
            const float nextDistance =
                std::clamp(a_distanceUnits, 1.0F, 32768.0F);
            const float priorDistance = s_gateProximityDistance.exchange(
                nextDistance, std::memory_order_acq_rel);
            distanceChanged = priorDistance != nextDistance;
        }
        // The native-uGrids selector (>= 1) is retired: its queueOnly=false
        // engine call mutates live grid state and produced a reproducible CTD
        // at the next real transition. Every selector now clamps to the
        // single-arrival-cell mode; the config value is preserved in the INI
        // so older files keep loading, it just no longer selects the mode.
        if (a_gridSelector >= 1) {
            static std::atomic<bool> s_fullGridRetirementLogged{ false };
            if (!s_fullGridRetirementLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                logger::warn(
                    "DoorPrefetch: iExteriorGridRadius={} requested the "
                    "retired native-uGrids exterior mode; clamped to the "
                    "single-arrival-cell engine path (queueOnly=false CTDs "
                    "from an exterior origin)",
                    a_gridSelector);
            }
        }
        s_exteriorFullGrid.store(false, std::memory_order_relaxed);
        const bool exteriorResolutionAttempted =
            s_exteriorResolutionAttempted.load(std::memory_order_acquire);
        const bool exteriorSupported =
            !exteriorResolutionAttempted ||
            (s_exteriorPreloadResolved.load(std::memory_order_acquire) &&
             s_preloadWorld != nullptr &&
             s_tesSingletonPtr != nullptr);
        const bool gateResolutionAttempted =
            s_gateEnumerationResolutionAttempted.load(
                std::memory_order_acquire);
        const bool gateEnumerationSupported =
            !gateResolutionAttempted ||
            (s_gateEnumerationResolved.load(std::memory_order_acquire) &&
             s_enumTeleportDoors != nullptr &&
             s_loadedReferenceCollectionPtr != nullptr);
        const bool exteriorEnabled = a_enabled && exteriorSupported;
        s_exteriorPreloadEnabled.store(
            exteriorEnabled, std::memory_order_release);
        const bool gateEnabled =
            exteriorEnabled && gateEnumerationSupported;
        const bool gateChanged =
            s_gateProximity.exchange(
                gateEnabled, std::memory_order_acq_rel) != gateEnabled;
        if (distanceChanged || gateChanged) {
            // The loaded-reference cache was filtered using the previous radius
            // and enable state. Invalidate it now so a live MCM change cannot
            // reuse that result for the normal two-second scan interval.
            s_forceGateRescan.store(true, std::memory_order_release);
        }
        s_rayWakeCv.notify_all();
    }

    void DoorPrefetch::EnsureRegistered()
    {
        // Callers include NG's Present callback, so never touch the player or event
        // sources here. Marshal registration to the game task queue just like the
        // extended ray path.
        if (s_shuttingDown.load(std::memory_order_acquire) ||
            !s_gameSessionActive.load(std::memory_order_acquire) ||
            !s_crosshairPickEnabled.load(std::memory_order_relaxed) ||
            s_registered.load(std::memory_order_acquire) || !s_preloadWorld ||
            !s_tesSingletonPtr) {
            return;
        }
        bool expected = false;
        if (!s_registrationTaskQueued.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        if (auto* task = F4SE::GetTaskInterface()) {
            task->AddTask([]() {
                struct ResetQueuedFlag
                {
                    ~ResetQueuedFlag()
                    {
                        s_registrationTaskQueued.store(false, std::memory_order_release);
                    }
                } reset;

                if (s_shuttingDown.load(std::memory_order_acquire) ||
                    !s_gameSessionActive.load(std::memory_order_acquire) ||
                    !s_crosshairPickEnabled.load(std::memory_order_relaxed) ||
                    s_registered.load(std::memory_order_acquire) || !s_preloadWorld ||
                    !s_tesSingletonPtr) {
                    return;
                }
                auto* pc = GetPlayerForPrefetch();
                if (!pc) {
                    return;  // retry on the next load
                }
                bool ok = false;
                if (REL::Module::IsVR()) {
                    // Both wands + the center/gaze pick.
                    ok |= RegisterAt(pc, kPickSrcVRPrim);
                    ok |= RegisterAt(pc, kPickSrcVRSec);
                    ok |= RegisterAt(pc, kPickSrcVRCenter);
                } else {
                    ok = RegisterAt(pc, kPickSrcFlat);
                }
                if (!ok) {
                    return;  // validation failed — retry on the next load
                }
                s_registered.store(true, std::memory_order_release);
                logger::info("DoorPrefetch: pick sink registered ({})",
                    REL::Module::IsVR() ? "VR wands + center" : "crosshair");
            });
        } else {
            s_registrationTaskQueued.store(false, std::memory_order_release);
        }
    }

    void DoorPrefetch::Shutdown()
    {
        s_shuttingDown.store(true, std::memory_order_release);
        s_transitionActive.store(true, std::memory_order_release);
        s_gameSessionActive.store(false, std::memory_order_release);
        s_crosshairPickEnabled.store(false, std::memory_order_relaxed);
        s_exteriorPreloadEnabled.store(false, std::memory_order_release);
        s_extendedRay.store(false, std::memory_order_relaxed);
        s_gateProximity.store(false, std::memory_order_relaxed);
        s_rayPollerRunning.store(false, std::memory_order_release);
        s_rayWakeCv.notify_all();
        if (s_rayPollerThread.joinable()) {
            s_rayPollerThread.request_stop();
            s_rayPollerThread.join();
        }
        // Drain only a plugin exterior call already executing. Any asynchronous
        // work accepted by PreloadWorld remains owned by Fallout.
        std::lock_guard submissionLock(s_submissionMutex);
        ResetDestinationCooldowns("shutdown", true);

        // The event sink remains attached for the process lifetime; the disabled
        // crosshair-pick switch makes callbacks inert. Do not pretend it was
        // unregistered, which could duplicate RegisterSink on a later install.
        logger::info("DoorPrefetch: shutdown complete");
    }
}
