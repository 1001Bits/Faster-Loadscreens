#include "PCH.h"
#include "PreloadDiagnostics.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace VRLoadingScreens::PreloadDiagnostics
{
    namespace
    {
        using PreloadInteriorFn = void (*)(void*, RE::TESObjectCELL*);
        using PreloadWorldFn = void (*)(void*, RE::TESWorldSpace*, int, int, bool);
        using NativeLinkedHelperFn = void (*)(void*, RE::TESObjectCELL*);
        using NativeLinkedOuterFn = std::uint64_t (*)(void*, void*);
        using MenuPreloadGridFn =
            void (*)(std::uint32_t, int, int, RE::TESWorldSpace*, int);
        using InteriorBufferFn = void (*)(void*, RE::TESObjectCELL*);

        constexpr std::uintptr_t kPreloadInteriorOG = 0xFC340;
        constexpr std::uintptr_t kPreloadInteriorVR = 0xFC310;
        constexpr std::uintptr_t kPreloadWorldOG = 0xFC0D0;
        constexpr std::uintptr_t kPreloadWorldVR = 0xFC0A0;
        constexpr std::uintptr_t kNativeLinkedHelperOG = 0xEC5440;
        constexpr std::uintptr_t kNativeLinkedHelperVR = 0xF36060;
        constexpr std::uintptr_t kNativeLinkedOuterOG = 0xEC5570;
        constexpr std::uintptr_t kNativeLinkedOuterVR = 0xF36190;
        constexpr std::uintptr_t kMenuPreloadGridOG = 0x35DB90;
        constexpr std::uintptr_t kMenuPreloadGridVR = 0x3441F0;
        constexpr std::uintptr_t kAddInteriorBufferOG = 0x100520;
        constexpr std::uintptr_t kAddInteriorBufferVR = 0x1004F0;
        constexpr std::uintptr_t kRemoveInteriorBufferOG = 0xFBD00;
        constexpr std::uintptr_t kRemoveInteriorBufferVR = 0xFBCD0;

        // The two native visitor functions run from PlayerCharacter::Update
        // (roughly once per frame). Per-visitor logging produced more than
        // 104,000 lines in a two-minute route and materially contaminated the
        // diagnostic run. The nested preload-interior/preload-world hooks already
        // record every engine call with the same TLS attempt/origin context, so
        // keep rejected/provisional visitor spam off in normal diagnostics.
        constexpr bool kVerboseNativeVisitorLogging = false;

        struct MaskedSignature
        {
            const std::uint8_t* bytes;
            const char* mask;
            std::size_t size;
        };

        // The wildcard bytes are relative branch/RIP displacements.  The remaining
        // opcode sequence was verified against Fallout 4 1.10.163.  VR uses the
        // corresponding compiler-identical entries after Steam has decrypted .text;
        // any divergence is intentionally rejected before MinHook sees the address.
        constexpr std::array<std::uint8_t, 12> kPreloadInteriorBytes{
            0x48, 0x83, 0xEC, 0x48, 0x48, 0x85, 0xD2, 0x74, 0x00, 0x48, 0x8B, 0x0D
        };
        constexpr char kPreloadInteriorMask[] = "xxxxxxxx?xxx";

        constexpr std::array<std::uint8_t, 19> kPreloadWorldBytes{
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x48,
            0x89, 0x6C, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57
        };
        constexpr char kPreloadWorldMask[] = "xxxxx????xxxxxxxxxx";

        constexpr std::array<std::uint8_t, 20> kNativeLinkedBytes{
            0x48, 0x85, 0xD2, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x48,
            0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57
        };
        constexpr char kNativeLinkedMask[] = "xxxxx????xxxxxxxxxxx";

        constexpr std::array<std::uint8_t, 29> kNativeLinkedOuterBytes{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x30,
            0x48, 0x8B, 0x01, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xD9, 0x80,
            0x38, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00, 0x48
        };
        constexpr char kNativeLinkedOuterMask[] =
            "xxxxxxxxxxxxxxxxxxxxxxxx????x";

        constexpr std::array<std::uint8_t, 20> kMenuPreloadGridBytes{
            0x48, 0x83, 0xEC, 0x38, 0x8B, 0x44, 0x24, 0x60, 0x89, 0x44,
            0x24, 0x28, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x45, 0x8B, 0xC8
        };
        constexpr char kMenuPreloadGridMask[] = "xxxxxxxxxxxxxxxxxxxx";

        constexpr std::array<std::uint8_t, 21> kAddInteriorBufferBytes{
            0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20,
            0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xD2, 0x74, 0x00
        };
        constexpr char kAddInteriorBufferMask[] = "xxxxxxxxxxxxxxxxxxxx?";

        constexpr std::array<std::uint8_t, 31> kRemoveInteriorBufferBytes{
            0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
            0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF2, 0x48, 0x8B,
            0xE9, 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
            0x8B
        };
        constexpr char kRemoveInteriorBufferMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxx????x";

        struct HookTarget
        {
            const char* name;
            void* address;
            void* detour;
            void** original;
            MaskedSignature signature;
            bool created;
            bool enabled;
        };

        struct CellSnapshot
        {
            bool valid;
            bool interior;
            bool hasLoadedData;
            std::uint8_t state;
            std::uint32_t formID;
            const char* editorID;
            std::uintptr_t address;
            std::uintptr_t loadedData;
            std::uintptr_t worldAddress;
            std::uint32_t worldFormID;
            const char* worldEditorID;
            int gridX;
            int gridY;
        };

        struct FormSnapshot
        {
            bool valid;
            std::uint32_t formID;
            const char* editorID;
            std::uintptr_t address;
        };

        enum class ObservationSource : std::uint8_t
        {
            kEngineOther,
            kNativeLinked,
            kPluginDoorPrefetch
        };

        constexpr std::uint64_t kAggregateIntervalMs = 5000;
        constexpr std::size_t kWorldAggregateCapacity = 64;
        constexpr std::size_t kCandidateAggregateCapacity = 128;
        constexpr std::size_t kSealedBatchCapacity = 16;
        constexpr std::size_t kTriggerCapacity = 32;
        constexpr std::uint64_t kWorldDetailBudget = 4;
        constexpr std::uint64_t kCandidateDetailBudget = 8;
        constexpr std::uint64_t kPreloadInteriorDetailBudget = 4;
        constexpr std::uint64_t kInteriorBufferDetailBudget = 2;
        constexpr std::uint64_t kMenuGridDetailBudget = 2;

        struct WorldAggregateKey
        {
            std::uintptr_t world = 0;
            std::uintptr_t originCell = 0;
            std::uint64_t loadNo = 0;
            int gridX = 0;
            int gridY = 0;
            ObservationSource source = ObservationSource::kEngineOther;
            bool queueOnly = false;

            [[nodiscard]] bool operator==(
                const WorldAggregateKey&) const noexcept = default;
        };

        struct WorldAggregateEntry
        {
            bool valid = false;
            WorldAggregateKey key{};
            std::uint64_t windowCallCount = 0;
            std::uint64_t firstOrdinal = 0;
            std::uint64_t lastOrdinal = 0;
            std::uint64_t lastMs = 0;
            bool detailEmitted = false;
        };

        struct CandidateAggregateKey
        {
            std::uintptr_t door = 0;
            std::uintptr_t destination = 0;
            std::uint64_t loadNo = 0;
            int result = 0;
            std::array<char, kTriggerCapacity> trigger{};

            [[nodiscard]] bool operator==(
                const CandidateAggregateKey&) const noexcept = default;
        };

        struct CandidateAggregateEntry
        {
            bool valid = false;
            CandidateAggregateKey key{};
            std::uint64_t windowCallCount = 0;
            std::uint64_t firstOrdinal = 0;
            std::uint64_t lastOrdinal = 0;
            std::uint64_t lastMs = 0;
            float minimumDistance = 0.0F;
            float maximumDistance = 0.0F;
            bool detailEmitted = false;
        };

        struct WorldAggregateSummary
        {
            WorldAggregateKey key{};
            std::uint64_t windowCallCount = 0;
            std::uint64_t firstOrdinal = 0;
            std::uint64_t lastOrdinal = 0;
            bool detailEmitted = false;
        };

        struct CandidateAggregateSummary
        {
            CandidateAggregateKey key{};
            std::uint64_t windowCallCount = 0;
            std::uint64_t firstOrdinal = 0;
            std::uint64_t lastOrdinal = 0;
            float minimumDistance = 0.0F;
            float maximumDistance = 0.0F;
            bool detailEmitted = false;
        };

        struct AggregateRecordResult
        {
            bool logDetail = false;
            bool flushDue = false;
            std::uint64_t loadNo = 0;
            bool loading = false;
        };

        struct DetailWindowCounter
        {
            std::uint64_t calls = 0;
            std::uint64_t detailsEmitted = 0;
            std::uint64_t detailOverflow = 0;
        };

        struct AggregateWindowCounters
        {
            std::uint64_t startMs = 0;
            DetailWindowCounter world{};
            std::uint64_t worldFirstKeyCalls = 0;
            std::uint64_t worldRepeatedCalls = 0;
            std::uint64_t worldCapacityEvictions = 0;
            std::uint64_t worldEvictedCalls = 0;
            DetailWindowCounter candidate{};
            std::uint64_t candidateFirstKeyCalls = 0;
            std::uint64_t candidateRepeatedCalls = 0;
            std::uint64_t candidateCapacityEvictions = 0;
            std::uint64_t candidateEvictedCalls = 0;
            DetailWindowCounter preloadInterior{};
            DetailWindowCounter interiorBufferAdd{};
            DetailWindowCounter interiorBufferRemove{};
            DetailWindowCounter menuGrid{};
        };

        enum class DetailStream : std::uint8_t
        {
            kPreloadInterior,
            kInteriorBufferAdd,
            kInteriorBufferRemove,
            kMenuGrid
        };

        struct DetailRecordResult
        {
            bool logDetail = false;
            bool flushDue = false;
            std::uint64_t loadNo = 0;
            bool loading = false;
        };

        enum class AggregateSealReason : std::uint8_t
        {
            kPeriodic,
            kLoadStart,
            kLoadEnd,
            kLoadSwitch,
            kShutdown
        };

        struct SealedAggregateBatch
        {
            bool valid = false;
            AggregateSealReason reason = AggregateSealReason::kPeriodic;
            std::uint64_t loadNo = 0;
            bool loading = false;
            std::uint64_t endMs = 0;
            std::array<WorldAggregateSummary, kWorldAggregateCapacity> world{};
            std::size_t worldCount = 0;
            std::size_t nextWorld = 0;
            std::array<CandidateAggregateSummary, kCandidateAggregateCapacity>
                candidate{};
            std::size_t candidateCount = 0;
            std::size_t nextCandidate = 0;
            AggregateWindowCounters window{};
            bool windowEmitted = false;
        };

        struct DroppedBatchTotals
        {
            std::uint64_t batches = 0;
            std::uint64_t firstLoadNo = 0;
            std::uint64_t lastLoadNo = 0;
            std::uint64_t worldCalls = 0;
            std::uint64_t candidateCalls = 0;
            std::uint64_t preloadInteriorCalls = 0;
            std::uint64_t interiorBufferAddCalls = 0;
            std::uint64_t interiorBufferRemoveCalls = 0;
            std::uint64_t menuGridCalls = 0;
        };

        std::atomic<bool> s_configured{ false };
        std::atomic<bool> s_supported{ false };
        std::atomic<bool> s_installed{ false };
        std::atomic<bool> s_loading{ false };
        std::atomic<std::uint64_t> s_loadNo{ 0 };
        std::atomic<std::uint64_t> s_sequence{ 0 };
        std::atomic<std::uint64_t> s_nativeAttemptSequence{ 0 };
        std::atomic<std::uint64_t> s_worldCallSequence{ 0 };
        std::atomic<std::uint64_t> s_candidateSequence{ 0 };
        std::atomic<unsigned> s_uGridsToLoad{ 5 };
        std::atomic<unsigned> s_uInteriorCellBuffer{ 3 };
        std::uint64_t s_epochMs = 0;
        std::mutex s_installMutex;

        thread_local unsigned s_nativeLinkedDepth = 0;
        thread_local unsigned s_pluginCrosshairDepth = 0;

        std::mutex s_aggregateMutex;
        std::array<WorldAggregateEntry, kWorldAggregateCapacity>
            s_worldAggregates{};
        std::array<CandidateAggregateEntry, kCandidateAggregateCapacity>
            s_candidateAggregates{};
        AggregateWindowCounters s_windowCounters{};
        std::uint64_t s_windowLoadNo = 0;
        bool s_windowLoading = false;
        std::uint64_t s_nextAggregateFlushMs = 0;
        std::atomic<bool> s_summaryFlushPending{ false };
        std::atomic<bool> s_loadStartRequested{ false };
        std::mutex s_summaryWakeMutex;
        std::condition_variable s_summaryWakeCv;
        std::mutex s_summaryEmissionMutex;
        std::mutex s_sealedBatchMutex;
        std::array<SealedAggregateBatch, kSealedBatchCapacity>
            s_sealedBatches{};
        std::size_t s_sealedBatchHead = 0;
        std::size_t s_sealedBatchCount = 0;
        std::atomic<std::size_t> s_queuedBatchCount{ 0 };
        DroppedBatchTotals s_droppedBatchTotals{};
        std::atomic<bool> s_droppedBatchPending{ false };
        std::jthread s_summaryWorker;

        std::atomic<std::uint32_t> s_activeHookCalls{ 0 };
        std::mutex s_hookIdleMutex;
        std::condition_variable s_hookIdleCv;

        PreloadInteriorFn s_originalPreloadInterior = nullptr;
        PreloadWorldFn s_originalPreloadWorld = nullptr;
        NativeLinkedHelperFn s_originalNativeLinkedHelper = nullptr;
        NativeLinkedOuterFn s_originalNativeLinkedOuter = nullptr;
        MenuPreloadGridFn s_originalMenuPreloadGrid = nullptr;
        InteriorBufferFn s_originalAddInteriorBuffer = nullptr;
        InteriorBufferFn s_originalRemoveInteriorBuffer = nullptr;

        std::array<HookTarget, 7> s_targets{};
        bool s_hooksRetainedForLifetime = false;

        [[nodiscard]] std::uint64_t NowMs() noexcept
        {
            const auto now = static_cast<std::uint64_t>(GetTickCount64());
            return now >= s_epochMs ? now - s_epochMs : 0;
        }

        [[nodiscard]] std::uint64_t NextSequence() noexcept
        {
            return s_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        [[nodiscard]] const char* CellStateName(std::uint8_t a_state) noexcept
        {
            switch (static_cast<RE::TESObjectCELL::CELL_STATE>(a_state)) {
            case RE::TESObjectCELL::CELL_STATE::kNotLoaded:
                return "not-loaded";
            case RE::TESObjectCELL::CELL_STATE::kUnloading:
                return "unloading";
            case RE::TESObjectCELL::CELL_STATE::kLoadingData:
                return "loading-data";
            case RE::TESObjectCELL::CELL_STATE::kLoading:
                return "loading";
            case RE::TESObjectCELL::CELL_STATE::kLoaded:
                return "loaded";
            case RE::TESObjectCELL::CELL_STATE::kDetaching:
                return "detaching";
            case RE::TESObjectCELL::CELL_STATE::kAttachQueued:
                return "attach-queued";
            case RE::TESObjectCELL::CELL_STATE::kAttaching:
                return "attaching";
            case RE::TESObjectCELL::CELL_STATE::kAttached:
                return "attached";
            default:
                return "invalid";
            }
        }

        [[nodiscard]] CellSnapshot SnapshotCell(RE::TESObjectCELL* a_cell) noexcept
        {
            CellSnapshot result{};
            result.address = reinterpret_cast<std::uintptr_t>(a_cell);
            result.editorID = "<null>";
            result.worldEditorID = "<none>";
            if (!a_cell) {
                return result;
            }

            __try {
                result.formID = a_cell->GetFormID();
                const char* editor = a_cell->GetFormEditorID();
                result.editorID = editor && editor[0] ? editor : "<none>";
                result.interior = a_cell->IsInterior();
                result.state = a_cell->cellState.underlying();
                result.loadedData = reinterpret_cast<std::uintptr_t>(a_cell->loadedData);
                result.hasLoadedData = a_cell->loadedData != nullptr;
                if (!result.interior) {
                    auto* world = a_cell->worldSpace;
                    result.worldAddress = reinterpret_cast<std::uintptr_t>(world);
                    if (world) {
                        result.worldFormID = world->GetFormID();
                        const char* worldEditor = world->GetFormEditorID();
                        result.worldEditorID =
                            worldEditor && worldEditor[0] ? worldEditor : "<none>";
                    }
                    result.gridX = a_cell->GetDataX();
                    result.gridY = a_cell->GetDataY();
                }
                result.valid = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                result.valid = false;
            }
            return result;
        }

        [[nodiscard]] RE::TESObjectCELL* ObservePlayerCellPointer() noexcept
        {
            RE::TESObjectCELL* cell = nullptr;
            __try {
                // Diagnostics installs only on exact OG 1.10.163 / VR 1.2.72,
                // where this typed singleton is verified.
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    cell = player->GetParentCell();
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                cell = nullptr;
            }
            return cell;
        }

        [[nodiscard]] const char* DirectionName(
            const CellSnapshot& a_origin,
            const CellSnapshot& a_destination) noexcept
        {
            if (!a_origin.valid || !a_destination.valid) {
                return "unknown";
            }
            if (a_origin.interior && !a_destination.interior) {
                return "interior-to-exterior";
            }
            if (!a_origin.interior && a_destination.interior) {
                return "exterior-to-interior";
            }
            if (a_origin.interior && a_destination.interior) {
                return "interior-to-interior";
            }
            return "exterior-to-exterior";
        }

        [[nodiscard]] const char* WorldDirectionName(
            const CellSnapshot& a_origin) noexcept
        {
            if (!a_origin.valid) {
                return "unknown-to-exterior";
            }
            return a_origin.interior
                ? "interior-to-exterior"
                : "exterior-to-exterior";
        }

        [[nodiscard]] FormSnapshot SnapshotForm(RE::TESForm* a_form) noexcept
        {
            FormSnapshot result{};
            result.address = reinterpret_cast<std::uintptr_t>(a_form);
            result.editorID = "<null>";
            if (!a_form) {
                return result;
            }
            __try {
                result.formID = a_form->GetFormID();
                const char* editor = a_form->GetFormEditorID();
                result.editorID = editor && editor[0] ? editor : "<none>";
                result.valid = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                result.valid = false;
            }
            return result;
        }

        [[nodiscard]] ObservationSource CurrentObservationSource() noexcept
        {
            if (s_pluginCrosshairDepth != 0) {
                return ObservationSource::kPluginDoorPrefetch;
            }
            if (s_nativeLinkedDepth != 0) {
                return ObservationSource::kNativeLinked;
            }
            return ObservationSource::kEngineOther;
        }

        [[nodiscard]] const char* ObservationSourceName(
            ObservationSource a_source) noexcept
        {
            switch (a_source) {
            case ObservationSource::kNativeLinked:
                return "native-linked";
            case ObservationSource::kPluginDoorPrefetch:
                return "plugin-door-prefetch";
            default:
                return "engine-other";
            }
        }

        [[nodiscard]] const char* CurrentSource() noexcept
        {
            return ObservationSourceName(CurrentObservationSource());
        }

        [[nodiscard]] bool ObserverLoggingActive() noexcept
        {
            return s_installed.load(std::memory_order_acquire) &&
                s_configured.load(std::memory_order_relaxed);
        }

        struct HookInvocation
        {
            HookInvocation() noexcept
            {
                s_activeHookCalls.fetch_add(1, std::memory_order_acq_rel);
            }

            ~HookInvocation()
            {
                if (s_activeHookCalls.fetch_sub(
                        1, std::memory_order_acq_rel) == 1) {
                    s_hookIdleCv.notify_all();
                }
            }
        };

        void CopyTrigger(
            std::array<char, kTriggerCapacity>& a_output,
            const char* a_trigger) noexcept
        {
            a_output.fill('\0');
            const char* source = a_trigger ? a_trigger : "<null>";
            (void)strncpy_s(
                a_output.data(),
                a_output.size(),
                source,
                a_output.size() - 1);
        }

        [[nodiscard]] WorldAggregateSummary MakeWorldSummary(
            const WorldAggregateEntry& a_entry) noexcept
        {
            return {
                a_entry.key,
                a_entry.windowCallCount,
                a_entry.firstOrdinal,
                a_entry.lastOrdinal,
                a_entry.detailEmitted
            };
        }

        [[nodiscard]] CandidateAggregateSummary MakeCandidateSummary(
            const CandidateAggregateEntry& a_entry) noexcept
        {
            return {
                a_entry.key,
                a_entry.windowCallCount,
                a_entry.firstOrdinal,
                a_entry.lastOrdinal,
                a_entry.minimumDistance,
                a_entry.maximumDistance,
                a_entry.detailEmitted
            };
        }

        void LogWorldAggregateSummary(
            const WorldAggregateSummary& a_summary,
            const char* a_reason) noexcept
        {
            const auto aggregateCalls =
                a_summary.windowCallCount -
                (a_summary.detailEmitted ? 1U : 0U);
            if (aggregateCalls == 0) {
                return;
            }
            logger::info(
                "PreloadDiag event=preload-world-aggregate seq={} tMs={} "
                "load={} source={} worldPtr={:#x} center=({}, {}) "
                "boolFlag={} branch={} originCellPtr={:#x} "
                "windowEngineCalls={} detailEmitted={} aggregateEngineCalls={} "
                "firstCallOrdinal={} lastCallOrdinal={} reason={} "
                "countingRule=exact-window-summary",
                NextSequence(),
                NowMs(),
                a_summary.key.loadNo,
                ObservationSourceName(a_summary.key.source),
                a_summary.key.world,
                a_summary.key.gridX,
                a_summary.key.gridY,
                a_summary.key.queueOnly,
                a_summary.key.queueOnly
                    ? "queue-single-cell"
                    : "full-arrival-grid",
                a_summary.key.originCell,
                a_summary.windowCallCount,
                a_summary.detailEmitted,
                aggregateCalls,
                a_summary.firstOrdinal,
                a_summary.lastOrdinal,
                a_reason ? a_reason : "periodic");
        }

        void LogCandidateAggregateSummary(
            const CandidateAggregateSummary& a_summary,
            const char* a_reason) noexcept
        {
            const auto aggregateCalls =
                a_summary.windowCallCount -
                (a_summary.detailEmitted ? 1U : 0U);
            if (aggregateCalls == 0) {
                return;
            }
            logger::info(
                "PreloadDiag event=crosshair-candidate-aggregate seq={} tMs={} "
                "load={} source=plugin-door-prefetch trigger={} result={} "
                "doorPtr={:#x} destPtr={:#x} windowObservations={} "
                "detailEmitted={} aggregateObservations={} "
                "firstOrdinal={} lastOrdinal={} "
                "distanceMin={:.3f} distanceMax={:.3f} reason={} "
                "countingRule=exact-window-summary",
                NextSequence(),
                NowMs(),
                a_summary.key.loadNo,
                a_summary.key.trigger.data(),
                a_summary.key.result,
                a_summary.key.door,
                a_summary.key.destination,
                a_summary.windowCallCount,
                a_summary.detailEmitted,
                aggregateCalls,
                a_summary.firstOrdinal,
                a_summary.lastOrdinal,
                a_summary.minimumDistance,
                a_summary.maximumDistance,
                a_reason ? a_reason : "periodic");
        }

        void LogWindowSummary(
            const AggregateWindowCounters& a_summary,
            std::uint64_t a_endMs,
            const char* a_reason,
            std::uint64_t a_loadNo,
            bool a_loading) noexcept
        {
            const auto bufferDetails =
                a_summary.interiorBufferAdd.detailsEmitted +
                a_summary.interiorBufferRemove.detailsEmitted;
            const auto bufferOverflow =
                a_summary.interiorBufferAdd.detailOverflow +
                a_summary.interiorBufferRemove.detailOverflow;
            logger::info(
                "PreloadDiag event=detail-window-summary seq={} tMs={} "
                "load={} loading={} reason={} windowStartMs={} windowEndMs={} "
                "windowDurationMs={} "
                "worldCalls={} worldFirstKeyCalls={} worldRepeatedCalls={} "
                "worldDetails={} worldDetailOverflow={} "
                "worldCapacityEvictions={} worldEvictedCalls={} "
                "candidateCalls={} candidateFirstKeyCalls={} "
                "candidateRepeatedCalls={} candidateDetails={} "
                "candidateDetailOverflow={} candidateCapacityEvictions={} "
                "candidateEvictedCalls={} "
                "preloadInteriorCalls={} preloadInteriorDetails={} "
                "preloadInteriorDetailOverflow={} "
                 "interiorBufferAddCalls={} interiorBufferRemoveCalls={} "
                 "interiorBufferDetails={} interiorBufferDetailOverflow={} "
                 "menuGridCalls={} menuGridDetails={} "
                 "menuGridDetailOverflow={} "
                "countingRule=exact-window-totals",
                NextSequence(),
                a_endMs,
                a_loadNo,
                a_loading,
                a_reason ? a_reason : "periodic",
                a_summary.startMs,
                a_endMs,
                a_endMs >= a_summary.startMs
                    ? a_endMs - a_summary.startMs
                    : 0,
                a_summary.world.calls,
                a_summary.worldFirstKeyCalls,
                a_summary.worldRepeatedCalls,
                a_summary.world.detailsEmitted,
                a_summary.world.detailOverflow,
                a_summary.worldCapacityEvictions,
                a_summary.worldEvictedCalls,
                a_summary.candidate.calls,
                a_summary.candidateFirstKeyCalls,
                a_summary.candidateRepeatedCalls,
                a_summary.candidate.detailsEmitted,
                a_summary.candidate.detailOverflow,
                a_summary.candidateCapacityEvictions,
                a_summary.candidateEvictedCalls,
                a_summary.preloadInterior.calls,
                a_summary.preloadInterior.detailsEmitted,
                a_summary.preloadInterior.detailOverflow,
                a_summary.interiorBufferAdd.calls,
                 a_summary.interiorBufferRemove.calls,
                 bufferDetails,
                 bufferOverflow,
                 a_summary.menuGrid.calls,
                 a_summary.menuGrid.detailsEmitted,
                 a_summary.menuGrid.detailOverflow);
        }

        [[nodiscard]] const char* AggregateSealReasonName(
            AggregateSealReason a_reason) noexcept
        {
            switch (a_reason) {
            case AggregateSealReason::kLoadStart:
                return "load-start";
            case AggregateSealReason::kLoadEnd:
                return "load-end";
            case AggregateSealReason::kLoadSwitch:
                return "load-switch";
            case AggregateSealReason::kShutdown:
                return "shutdown";
            default:
                return "periodic-worker";
            }
        }

        [[nodiscard]] bool HasAggregateWindowActivityLocked() noexcept
        {
            return s_windowCounters.world.calls != 0 ||
                s_windowCounters.candidate.calls != 0 ||
                s_windowCounters.preloadInterior.calls != 0 ||
                s_windowCounters.interiorBufferAdd.calls != 0 ||
                s_windowCounters.interiorBufferRemove.calls != 0 ||
                s_windowCounters.menuGrid.calls != 0;
        }

        // Caller owns s_aggregateMutex. This copies into fixed storage, rotates
        // the live window, and never allocates or logs.
        [[nodiscard]] bool SealAggregateWindowLocked(
            SealedAggregateBatch& a_output,
            AggregateSealReason a_reason,
            bool a_clearEntries,
            std::uint64_t a_nextLoadNo,
            bool a_nextLoading,
            std::uint64_t a_endMs) noexcept
        {
            a_output = {};
            const bool hasActivity = HasAggregateWindowActivityLocked();
            if (hasActivity) {
                a_output.valid = true;
                a_output.reason = a_reason;
                a_output.loadNo = s_windowLoadNo;
                a_output.loading = s_windowLoading;
                a_output.endMs = a_endMs;
                a_output.window = s_windowCounters;
            }

            for (auto& entry : s_worldAggregates) {
                if (hasActivity && entry.valid &&
                    entry.windowCallCount > 0 &&
                    a_output.worldCount < a_output.world.size()) {
                    a_output.world[a_output.worldCount++] =
                        MakeWorldSummary(entry);
                }
                if (a_clearEntries) {
                    entry = {};
                } else {
                    entry.windowCallCount = 0;
                    entry.firstOrdinal = 0;
                    entry.lastOrdinal = 0;
                    entry.detailEmitted = false;
                }
            }
            for (auto& entry : s_candidateAggregates) {
                if (hasActivity && entry.valid &&
                    entry.windowCallCount > 0 &&
                    a_output.candidateCount < a_output.candidate.size()) {
                    a_output.candidate[a_output.candidateCount++] =
                        MakeCandidateSummary(entry);
                }
                if (a_clearEntries) {
                    entry = {};
                } else {
                    entry.windowCallCount = 0;
                    entry.firstOrdinal = 0;
                    entry.lastOrdinal = 0;
                    entry.minimumDistance = 0.0F;
                    entry.maximumDistance = 0.0F;
                    entry.detailEmitted = false;
                }
            }

            s_windowCounters = {};
            s_windowCounters.startMs = a_endMs;
            s_windowLoadNo = a_nextLoadNo;
            s_windowLoading = a_nextLoading;
            s_nextAggregateFlushMs = a_endMs + kAggregateIntervalMs;
            return hasActivity;
        }

        void AccumulateDroppedBatchLocked(
            const SealedAggregateBatch& a_batch) noexcept
        {
            if (s_droppedBatchTotals.batches == 0) {
                s_droppedBatchTotals.firstLoadNo = a_batch.loadNo;
            }
            ++s_droppedBatchTotals.batches;
            s_droppedBatchTotals.lastLoadNo = a_batch.loadNo;
            s_droppedBatchTotals.worldCalls += a_batch.window.world.calls;
            s_droppedBatchTotals.candidateCalls +=
                a_batch.window.candidate.calls;
            s_droppedBatchTotals.preloadInteriorCalls +=
                a_batch.window.preloadInterior.calls;
            s_droppedBatchTotals.interiorBufferAddCalls +=
                a_batch.window.interiorBufferAdd.calls;
            s_droppedBatchTotals.interiorBufferRemoveCalls +=
                a_batch.window.interiorBufferRemove.calls;
            s_droppedBatchTotals.menuGridCalls +=
                a_batch.window.menuGrid.calls;
            s_droppedBatchPending.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool EnqueueSealedBatch(
            const SealedAggregateBatch& a_batch) noexcept
        {
            if (!a_batch.valid) {
                return true;
            }

            std::lock_guard lock(s_sealedBatchMutex);
            if (s_sealedBatchCount >= s_sealedBatches.size()) {
                // Never block a load boundary on diagnostic output. Preserve
                // exact totals for a later bounded overflow record if a
                // pathological chain fills all sealed slots.
                AccumulateDroppedBatchLocked(a_batch);
                return false;
            }
            const auto tail =
                (s_sealedBatchHead + s_sealedBatchCount) %
                s_sealedBatches.size();
            s_sealedBatches[tail] = a_batch;
            ++s_sealedBatchCount;
            s_queuedBatchCount.store(
                s_sealedBatchCount, std::memory_order_release);
            return true;
        }

        void LogDroppedBatchTotals(
            const DroppedBatchTotals& a_dropped) noexcept
        {
            logger::warn(
                "PreloadDiag event=sealed-batch-overflow seq={} tMs={} "
                "firstLoad={} lastLoad={} droppedBatches={} worldCalls={} "
                "candidateCalls={} preloadInteriorCalls={} "
                "interiorBufferAddCalls={} interiorBufferRemoveCalls={} "
                "menuGridCalls={} queueCapacity={} "
                "countingRule=exact-overflow-totals",
                NextSequence(),
                NowMs(),
                a_dropped.firstLoadNo,
                a_dropped.lastLoadNo,
                a_dropped.batches,
                a_dropped.worldCalls,
                a_dropped.candidateCalls,
                a_dropped.preloadInteriorCalls,
                a_dropped.interiorBufferAddCalls,
                a_dropped.interiorBufferRemoveCalls,
                a_dropped.menuGridCalls,
                kSealedBatchCapacity);
        }

        enum class SummaryEmissionResult : std::uint8_t
        {
            kEmpty,
            kDeferred,
            kEmitted
        };

        // Emits at most one log record. Load start publishes its intent before
        // waiting for s_summaryEmissionMutex, so it waits for no more than the
        // one line already in progress. Once s_loading=true is published under
        // this mutex, no aggregate line can begin or continue.
        [[nodiscard]] SummaryEmissionResult EmitOneQueuedSummary(
            bool a_allowWhenUnconfigured) noexcept
        {
            std::unique_lock emissionLock(s_summaryEmissionMutex);
            if (s_loadStartRequested.load(std::memory_order_acquire) ||
                s_loading.load(std::memory_order_acquire) ||
                (!a_allowWhenUnconfigured &&
                    !s_configured.load(std::memory_order_acquire))) {
                return SummaryEmissionResult::kDeferred;
            }

            enum class ItemKind : std::uint8_t
            {
                kNone,
                kDropped,
                kWorld,
                kCandidate,
                kWindow
            };

            ItemKind kind = ItemKind::kNone;
            DroppedBatchTotals dropped{};
            WorldAggregateSummary world{};
            CandidateAggregateSummary candidate{};
            AggregateWindowCounters window{};
            AggregateSealReason reason = AggregateSealReason::kPeriodic;
            std::uint64_t windowEndMs = 0;
            std::uint64_t windowLoadNo = 0;
            bool windowLoading = false;

            {
                std::lock_guard batchLock(s_sealedBatchMutex);
                if (s_droppedBatchTotals.batches != 0) {
                    dropped = s_droppedBatchTotals;
                    s_droppedBatchTotals = {};
                    s_droppedBatchPending.store(
                        false, std::memory_order_release);
                    kind = ItemKind::kDropped;
                } else if (s_sealedBatchCount != 0) {
                    auto& batch = s_sealedBatches[s_sealedBatchHead];
                    reason = batch.reason;
                    if (batch.nextWorld < batch.worldCount) {
                        world = batch.world[batch.nextWorld++];
                        kind = ItemKind::kWorld;
                    } else if (
                        batch.nextCandidate < batch.candidateCount) {
                        candidate =
                            batch.candidate[batch.nextCandidate++];
                        kind = ItemKind::kCandidate;
                    } else if (!batch.windowEmitted) {
                        window = batch.window;
                        windowEndMs = batch.endMs;
                        windowLoadNo = batch.loadNo;
                        windowLoading = batch.loading;
                        batch.windowEmitted = true;
                        kind = ItemKind::kWindow;

                        batch = {};
                        s_sealedBatchHead =
                            (s_sealedBatchHead + 1) %
                            s_sealedBatches.size();
                        --s_sealedBatchCount;
                        s_queuedBatchCount.store(
                            s_sealedBatchCount,
                            std::memory_order_release);
                    } else {
                        batch = {};
                        s_sealedBatchHead =
                            (s_sealedBatchHead + 1) %
                            s_sealedBatches.size();
                        --s_sealedBatchCount;
                        s_queuedBatchCount.store(
                            s_sealedBatchCount,
                            std::memory_order_release);
                    }
                }
            }

            const auto* reasonName = AggregateSealReasonName(reason);
            switch (kind) {
            case ItemKind::kDropped:
                LogDroppedBatchTotals(dropped);
                break;
            case ItemKind::kWorld:
                LogWorldAggregateSummary(world, reasonName);
                break;
            case ItemKind::kCandidate:
                LogCandidateAggregateSummary(candidate, reasonName);
                break;
            case ItemKind::kWindow:
                LogWindowSummary(
                    window,
                    windowEndMs,
                    reasonName,
                    windowLoadNo,
                    windowLoading);
                break;
            default:
                return SummaryEmissionResult::kEmpty;
            }
            return SummaryEmissionResult::kEmitted;
        }

        void RequestAggregateSummaryFlush() noexcept
        {
            s_summaryFlushPending.store(
                true, std::memory_order_release);
            s_summaryWakeCv.notify_one();
        }

        void SealPeriodicWindowIfRequested() noexcept
        {
            std::unique_lock emissionLock(s_summaryEmissionMutex);
            if (s_loadStartRequested.load(std::memory_order_acquire) ||
                s_loading.load(std::memory_order_acquire) ||
                !s_configured.load(std::memory_order_acquire) ||
                !s_summaryFlushPending.exchange(
                    false, std::memory_order_acq_rel)) {
                return;
            }

            SealedAggregateBatch batch{};
            const auto endMs = NowMs();
            {
                std::lock_guard aggregateLock(s_aggregateMutex);
                (void)SealAggregateWindowLocked(
                    batch,
                    AggregateSealReason::kPeriodic,
                    false,
                    s_windowLoadNo,
                    s_windowLoading,
                    endMs);
            }
            (void)EnqueueSealedBatch(batch);
        }

        void AggregateSummaryWorker(std::stop_token a_stopToken) noexcept
        {
            (void)SetThreadPriority(
                GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!a_stopToken.stop_requested()) {
                {
                    std::unique_lock wakeLock(s_summaryWakeMutex);
                    s_summaryWakeCv.wait_for(
                        wakeLock,
                        std::chrono::milliseconds(250),
                        [&a_stopToken]() {
                            return a_stopToken.stop_requested() ||
                                (!s_loading.load(
                                     std::memory_order_acquire) &&
                                 !s_loadStartRequested.load(
                                     std::memory_order_acquire) &&
                                 (s_summaryFlushPending.load(
                                      std::memory_order_acquire) ||
                                  s_queuedBatchCount.load(
                                      std::memory_order_acquire) != 0 ||
                                  s_droppedBatchPending.load(
                                      std::memory_order_acquire)));
                        });
                }
                if (a_stopToken.stop_requested()) {
                    break;
                }
                if (!s_configured.load(std::memory_order_acquire)) {
                    continue;
                }
                SealPeriodicWindowIfRequested();
                while (!a_stopToken.stop_requested()) {
                    const auto result = EmitOneQueuedSummary(false);
                    if (result != SummaryEmissionResult::kEmitted) {
                        break;
                    }
                }
            }
        }

        [[nodiscard]] bool MarkFlushDueLocked(
            std::uint64_t a_nowMs) noexcept
        {
            const bool due = a_nowMs >= s_nextAggregateFlushMs;
            if (due) {
                s_nextAggregateFlushMs =
                    a_nowMs + kAggregateIntervalMs;
            }
            return due;
        }

        AggregateRecordResult RecordWorldAggregate(
            WorldAggregateKey a_key,
            std::uint64_t a_ordinal,
            std::uint64_t a_nowMs) noexcept
        {
            AggregateRecordResult result{};
            std::lock_guard lock(s_aggregateMutex);
            a_key.loadNo = s_windowLoadNo;
            result.loadNo = s_windowLoadNo;
            result.loading = s_windowLoading;
            ++s_windowCounters.world.calls;

            const auto beginWindowKey = [&](WorldAggregateEntry& a_entry) {
                ++s_windowCounters.worldFirstKeyCalls;
                if (s_windowCounters.world.detailsEmitted <
                    kWorldDetailBudget) {
                    ++s_windowCounters.world.detailsEmitted;
                    result.logDetail = true;
                } else {
                    ++s_windowCounters.world.detailOverflow;
                }
                a_entry.windowCallCount = 1;
                a_entry.firstOrdinal = a_ordinal;
                a_entry.lastOrdinal = a_ordinal;
                a_entry.lastMs = a_nowMs;
                a_entry.detailEmitted = result.logDetail;
            };

            WorldAggregateEntry* freeEntry = nullptr;
            WorldAggregateEntry* oldest = nullptr;
            for (auto& entry : s_worldAggregates) {
                if (entry.valid && entry.key == a_key) {
                    if (entry.windowCallCount == 0) {
                        beginWindowKey(entry);
                    } else {
                        ++entry.windowCallCount;
                        ++s_windowCounters.worldRepeatedCalls;
                        entry.lastOrdinal = a_ordinal;
                        entry.lastMs = a_nowMs;
                    }
                    result.flushDue = MarkFlushDueLocked(a_nowMs);
                    return result;
                }
                if (!entry.valid && !freeEntry) {
                    freeEntry = &entry;
                } else if (entry.valid &&
                           (!oldest || entry.lastMs < oldest->lastMs)) {
                    oldest = &entry;
                }
            }

            auto* entry = freeEntry ? freeEntry : oldest;
            if (entry) {
                if (entry->valid) {
                    ++s_windowCounters.worldCapacityEvictions;
                    s_windowCounters.worldEvictedCalls +=
                        entry->windowCallCount;
                }
                *entry = {};
                entry->valid = true;
                entry->key = a_key;
                beginWindowKey(*entry);
            }
            result.flushDue = MarkFlushDueLocked(a_nowMs);
            return result;
        }

        AggregateRecordResult RecordCandidateAggregate(
            CandidateAggregateKey a_key,
            std::uint64_t a_ordinal,
            std::uint64_t a_nowMs,
            float a_distance) noexcept
        {
            AggregateRecordResult result{};
            std::lock_guard lock(s_aggregateMutex);
            a_key.loadNo = s_windowLoadNo;
            result.loadNo = s_windowLoadNo;
            result.loading = s_windowLoading;
            ++s_windowCounters.candidate.calls;

            const auto beginWindowKey = [&](CandidateAggregateEntry& a_entry) {
                ++s_windowCounters.candidateFirstKeyCalls;
                if (s_windowCounters.candidate.detailsEmitted <
                    kCandidateDetailBudget) {
                    ++s_windowCounters.candidate.detailsEmitted;
                    result.logDetail = true;
                } else {
                    ++s_windowCounters.candidate.detailOverflow;
                }
                a_entry.windowCallCount = 1;
                a_entry.firstOrdinal = a_ordinal;
                a_entry.lastOrdinal = a_ordinal;
                a_entry.lastMs = a_nowMs;
                a_entry.minimumDistance = a_distance;
                a_entry.maximumDistance = a_distance;
                a_entry.detailEmitted = result.logDetail;
            };

            CandidateAggregateEntry* freeEntry = nullptr;
            CandidateAggregateEntry* oldest = nullptr;
            for (auto& entry : s_candidateAggregates) {
                if (entry.valid && entry.key == a_key) {
                    if (entry.windowCallCount == 0) {
                        beginWindowKey(entry);
                    } else {
                        ++entry.windowCallCount;
                        ++s_windowCounters.candidateRepeatedCalls;
                        if (std::isfinite(a_distance)) {
                            entry.minimumDistance =
                                std::min(entry.minimumDistance, a_distance);
                            entry.maximumDistance =
                                std::max(entry.maximumDistance, a_distance);
                        }
                        entry.lastOrdinal = a_ordinal;
                        entry.lastMs = a_nowMs;
                    }
                    result.flushDue = MarkFlushDueLocked(a_nowMs);
                    return result;
                }
                if (!entry.valid && !freeEntry) {
                    freeEntry = &entry;
                } else if (entry.valid &&
                           (!oldest || entry.lastMs < oldest->lastMs)) {
                    oldest = &entry;
                }
            }

            auto* entry = freeEntry ? freeEntry : oldest;
            if (entry) {
                if (entry->valid) {
                    ++s_windowCounters.candidateCapacityEvictions;
                    s_windowCounters.candidateEvictedCalls +=
                        entry->windowCallCount;
                }
                *entry = {};
                entry->valid = true;
                entry->key = a_key;
                beginWindowKey(*entry);
            }
            result.flushDue = MarkFlushDueLocked(a_nowMs);
            return result;
        }

        DetailRecordResult RecordDetailEvent(
            DetailStream a_stream,
            std::uint64_t a_nowMs) noexcept
        {
            DetailRecordResult result{};
            std::lock_guard lock(s_aggregateMutex);
            result.loadNo = s_windowLoadNo;
            result.loading = s_windowLoading;

            DetailWindowCounter* counter = nullptr;
            std::uint64_t budget = 0;
            switch (a_stream) {
            case DetailStream::kPreloadInterior:
                counter = &s_windowCounters.preloadInterior;
                budget = kPreloadInteriorDetailBudget;
                break;
            case DetailStream::kInteriorBufferAdd:
                counter = &s_windowCounters.interiorBufferAdd;
                budget = kInteriorBufferDetailBudget;
                break;
            case DetailStream::kInteriorBufferRemove:
                counter = &s_windowCounters.interiorBufferRemove;
                budget = kInteriorBufferDetailBudget;
                break;
            case DetailStream::kMenuGrid:
                counter = &s_windowCounters.menuGrid;
                budget = kMenuGridDetailBudget;
                break;
            }

            if (!counter) {
                return result;
            }
            ++counter->calls;

            std::uint64_t emitted = counter->detailsEmitted;
            if (a_stream == DetailStream::kInteriorBufferAdd ||
                a_stream == DetailStream::kInteriorBufferRemove) {
                emitted =
                    s_windowCounters.interiorBufferAdd.detailsEmitted +
                    s_windowCounters.interiorBufferRemove.detailsEmitted;
            }
            if (emitted < budget) {
                ++counter->detailsEmitted;
                result.logDetail = true;
            } else {
                ++counter->detailOverflow;
            }
            result.flushDue = MarkFlushDueLocked(a_nowMs);
            return result;
        }

        void LogCellEvent(
            const char* a_event,
            const char* a_source,
            RE::TESObjectCELL* a_cell,
            std::uint64_t a_nowMs,
            std::uint64_t a_loadNo,
            bool a_loading) noexcept
        {
            const auto cell = SnapshotCell(a_cell);
            const bool nativeObservation =
                s_nativeLinkedDepth != 0;
            const auto attemptID = nativeObservation
                ? s_nativeAttemptSequence.fetch_add(
                      1, std::memory_order_relaxed) + 1
                : 0;
            const CellSnapshot origin = nativeObservation
                ? SnapshotCell(ObservePlayerCellPointer())
                : CellSnapshot{};
            logger::info(
                "PreloadDiag event={} seq={} tMs={} load={} loading={} source={} "
                "cellPtr={:#x} cellValid={} cell={:08X} editor={} interior={} "
                "state={}({}) loadedData={:#x} worldPtr={:#x} world={:08X} "
                "worldEditor={} grid=({}, {}) nativeAttemptId={} "
                "originObservation={} originCellPtr={:#x} originCellValid={} "
                "originCell={:08X} originEditor={} originInterior={} "
                "originWorld={:08X} originWorldEditor={} direction={}",
                a_event,
                NextSequence(),
                a_nowMs,
                a_loadNo,
                a_loading,
                a_source ? a_source : "<null>",
                cell.address,
                cell.valid,
                cell.formID,
                cell.editorID,
                cell.interior,
                cell.state,
                CellStateName(cell.state),
                cell.loadedData,
                cell.worldAddress,
                cell.worldFormID,
                cell.worldEditorID,
                cell.gridX,
                cell.gridY,
                attemptID,
                nativeObservation,
                origin.address,
                origin.valid,
                origin.formID,
                origin.editorID ? origin.editorID : "<null>",
                origin.interior,
                origin.worldFormID,
                origin.worldEditorID ? origin.worldEditorID : "<null>",
                nativeObservation ? DirectionName(origin, cell) : "not-native-linked");
        }

        [[nodiscard]] bool TryGetBinary(const char* a_name, bool& a_value) noexcept
        {
            bool ok = false;
            __try {
                if (auto* setting = RE::GetINISetting(a_name)) {
                    a_value = setting->GetBinary();
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool TryGetFloat(const char* a_name, float& a_value) noexcept
        {
            bool ok = false;
            __try {
                if (auto* setting = RE::GetINISetting(a_name)) {
                    a_value = setting->GetFloat();
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool TryGetUInt(const char* a_name, std::uint32_t& a_value) noexcept
        {
            bool ok = false;
            __try {
                if (auto* setting = RE::GetINISetting(a_name)) {
                    a_value = setting->GetUInt();
                    ok = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        void LogNativeSettings(const char* a_when) noexcept
        {
            bool binary = false;
            float floating = 0.0F;
            std::uint32_t unsignedValue = 0;

            if (TryGetBinary("bUseMenuLoadCellPreload:General", binary)) {
                logger::info(
                    "PreloadDiag native-setting when={} bUseMenuLoadCellPreload={}",
                    a_when, binary);
            } else {
                logger::warn(
                    "PreloadDiag native-setting when={} "
                    "bUseMenuLoadCellPreload=<unavailable>",
                    a_when);
            }

            if (TryGetBinary("bPreloadLinkedAreas:General", binary)) {
                logger::info(
                    "PreloadDiag native-setting when={} bPreloadLinkedAreas={}",
                    a_when, binary);
            } else {
                logger::warn(
                    "PreloadDiag native-setting when={} bPreloadLinkedAreas=<unavailable>",
                    a_when);
            }

            if (TryGetFloat("fTeleportPreloadDistance:General", floating)) {
                logger::info(
                    "PreloadDiag native-setting when={} "
                    "fTeleportPreloadDistance={:.3f}",
                    a_when, floating);
            } else {
                logger::warn(
                    "PreloadDiag native-setting when={} "
                    "fTeleportPreloadDistance=<unavailable>",
                    a_when);
            }

            if (TryGetUInt("uInterior Cell Buffer:General", unsignedValue)) {
                logger::info(
                    "PreloadDiag native-setting when={} uInteriorCellBuffer={}",
                    a_when, unsignedValue);
                if (unsignedValue >= 1 && unsignedValue <= 16) {
                    s_uInteriorCellBuffer.store(
                        unsignedValue, std::memory_order_relaxed);
                }
            } else {
                logger::warn(
                    "PreloadDiag native-setting when={} uInteriorCellBuffer=<unavailable>",
                    a_when);
            }

            if (TryGetUInt("uGridsToLoad:General", unsignedValue)) {
                logger::info(
                    "PreloadDiag native-setting when={} uGridsToLoad={}",
                    a_when, unsignedValue);
                if (unsignedValue >= 1 && unsignedValue <= 25) {
                    s_uGridsToLoad.store(unsignedValue, std::memory_order_relaxed);
                }
            } else {
                logger::warn(
                    "PreloadDiag native-setting when={} uGridsToLoad=<unavailable>",
                    a_when);
            }
        }

        [[nodiscard]] bool IsExecutableRegion(
            const void* a_address,
            std::size_t a_size) noexcept
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!a_address ||
                VirtualQuery(a_address, std::addressof(mbi), sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }
            const auto protection = mbi.Protect & 0xFF;
            if (protection != PAGE_EXECUTE &&
                protection != PAGE_EXECUTE_READ &&
                protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_EXECUTE_WRITECOPY) {
                return false;
            }
            const auto address = reinterpret_cast<std::uintptr_t>(a_address);
            const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            if (address < region) {
                return false;
            }
            const auto offset = address - region;
            return offset <= mbi.RegionSize && a_size <= mbi.RegionSize - offset;
        }

        [[nodiscard]] bool Matches(
            const void* a_address,
            const MaskedSignature& a_signature) noexcept
        {
            if (!IsExecutableRegion(a_address, a_signature.size)) {
                return false;
            }
            const auto* actual = static_cast<const std::uint8_t*>(a_address);
            for (std::size_t i = 0; i < a_signature.size; ++i) {
                if (a_signature.mask[i] == 'x' &&
                    actual[i] != a_signature.bytes[i]) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ReadNativeSelectionCounts(
            void* a_context,
            std::uint32_t& a_selected,
            std::uint32_t& a_limit) noexcept
        {
            bool ok = false;
            __try {
                const auto* context = static_cast<const std::uintptr_t*>(a_context);
                if (context && context[1] && context[2]) {
                    a_selected =
                        *reinterpret_cast<const std::uint32_t*>(context[1]);
                    a_limit =
                        *reinterpret_cast<const std::uint32_t*>(context[2]);
                    ok = a_selected <= a_limit && a_limit <= 1024;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            return ok;
        }

        void HookedPreloadInterior(void* a_self, RE::TESObjectCELL* a_cell)
        {
            HookInvocation invocation;
            const auto original = s_originalPreloadInterior;
            if (!original) {
                return;
            }

            // Fallout receives the request before diagnostics takes aggregate
            // locks, snapshots forms, formats strings, or logs.
            original(a_self, a_cell);
            if (ObserverLoggingActive()) {
                const auto nowMs = NowMs();
                const auto detail = RecordDetailEvent(
                    DetailStream::kPreloadInterior, nowMs);
                if (detail.logDetail) {
                    LogCellEvent(
                        "preload-interior",
                        CurrentSource(),
                        a_cell,
                        nowMs,
                        detail.loadNo,
                        detail.loading);
                }
                if (detail.flushDue) {
                    RequestAggregateSummaryFlush();
                }
            }
        }

        void HookedPreloadWorld(
            void* a_self,
            RE::TESWorldSpace* a_world,
            int a_gridX,
            int a_gridY,
            bool a_queueOnly)
        {
            HookInvocation invocation;
            const auto original = s_originalPreloadWorld;
            if (!original) {
                return;
            }

            // Preserve Fallout's preload initiation latency. Observation and all
            // formatting happen only after the native call returns.
            original(a_self, a_world, a_gridX, a_gridY, a_queueOnly);
            if (ObserverLoggingActive()) {
                const auto source = CurrentObservationSource();
                const bool nativeObservation =
                    source == ObservationSource::kNativeLinked;
                auto* originCell = nativeObservation
                    ? ObservePlayerCellPointer()
                    : nullptr;
                const auto callOrdinal =
                    s_worldCallSequence.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                const auto nowMs = NowMs();
                const WorldAggregateKey key{
                    reinterpret_cast<std::uintptr_t>(a_world),
                    reinterpret_cast<std::uintptr_t>(originCell),
                    0,
                    a_gridX,
                    a_gridY,
                    source,
                    a_queueOnly
                };
                const auto aggregate =
                    RecordWorldAggregate(key, callOrdinal, nowMs);

                if (aggregate.logDetail) {
                    const auto world = SnapshotForm(a_world);
                    const auto origin = nativeObservation
                        ? SnapshotCell(originCell)
                        : CellSnapshot{};
                    const auto attemptID = nativeObservation
                        ? s_nativeAttemptSequence.fetch_add(
                              1, std::memory_order_relaxed) + 1
                        : 0;
                    const auto side =
                        s_uGridsToLoad.load(std::memory_order_relaxed);
                    const bool fullGrid = !a_queueOnly;
                    logger::info(
                        "PreloadDiag event=preload-world seq={} tMs={} load={} "
                        "loading={} source={} actualCallOrdinal={} "
                        "aggregateDetail=true worldPtr={:#x} worldValid={} "
                        "world={:08X} worldEditor={} center=({}, {}) "
                        "boolFlag={} branch={} fullGrid={} gridSide={} "
                        "gridCells={} nativeAttemptId={} originObservation={} "
                        "originCellPtr={:#x} originCellValid={} "
                        "originCell={:08X} originEditor={} originInterior={} "
                        "originWorld={:08X} originWorldEditor={} direction={} "
                        "countingRule=exact-window-summary",
                        NextSequence(),
                        nowMs,
                        aggregate.loadNo,
                        aggregate.loading,
                        ObservationSourceName(source),
                        callOrdinal,
                        world.address,
                        world.valid,
                        world.formID,
                        world.editorID,
                        a_gridX,
                        a_gridY,
                        a_queueOnly,
                        a_queueOnly
                            ? "queue-single-cell"
                            : "full-arrival-grid",
                        fullGrid,
                        fullGrid ? side : 1U,
                        fullGrid ? side * side : 1U,
                        attemptID,
                        nativeObservation,
                        origin.address,
                        origin.valid,
                        origin.formID,
                        origin.editorID ? origin.editorID : "<null>",
                        origin.interior,
                        origin.worldFormID,
                        origin.worldEditorID
                            ? origin.worldEditorID
                            : "<null>",
                        nativeObservation
                            ? WorldDirectionName(origin)
                            : "not-native-linked");
                }

                if (aggregate.flushDue) {
                    RequestAggregateSummaryFlush();
                }
            }
        }

        void HookedNativeLinkedHelper(void* a_context, RE::TESObjectCELL* a_cell)
        {
            HookInvocation invocation;
            const auto original = s_originalNativeLinkedHelper;
            if (!original) {
                return;
            }

            if (ObserverLoggingActive() && kVerboseNativeVisitorLogging) {
                const auto attemptID =
                    s_nativeAttemptSequence.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                const auto origin =
                    SnapshotCell(ObservePlayerCellPointer());
                const auto destination = SnapshotCell(a_cell);
                std::uint32_t selected = 0;
                std::uint32_t limit = 0;
                const bool countsValid =
                    ReadNativeSelectionCounts(a_context, selected, limit);
                logger::info(
                    "PreloadDiag event=native-linked-candidate seq={} tMs={} load={} "
                    "loading={} source=native-linked nativeAttemptId={} "
                    "originObservation=true originCellPtr={:#x} originCellValid={} "
                    "originCell={:08X} originEditor={} originInterior={} "
                    "originWorld={:08X} originWorldEditor={} direction={} "
                    "candidatePtr={:#x} "
                    "candidateValid={} dest={:08X} editor={} interior={} "
                    "state={}({}) loadedData={:#x} world={:08X} grid=({}, {}) "
                    "selectionCountsValid={} selectedBefore={} selectionLimit={}",
                    NextSequence(),
                    NowMs(),
                    s_loadNo.load(std::memory_order_relaxed),
                    s_loading.load(std::memory_order_relaxed),
                    attemptID,
                    origin.address,
                    origin.valid,
                    origin.formID,
                    origin.editorID ? origin.editorID : "<null>",
                    origin.interior,
                    origin.worldFormID,
                    origin.worldEditorID ? origin.worldEditorID : "<null>",
                    DirectionName(origin, destination),
                    destination.address,
                    destination.valid,
                    destination.formID,
                    destination.editorID,
                    destination.interior,
                    destination.state,
                    CellStateName(destination.state),
                    destination.loadedData,
                    destination.worldFormID,
                    destination.gridX,
                    destination.gridY,
                    countsValid,
                    selected,
                    limit);
            }

            ++s_nativeLinkedDepth;
            original(a_context, a_cell);
            --s_nativeLinkedDepth;
        }

        std::uint64_t HookedNativeLinkedOuter(void* a_context, void* a_candidate)
        {
            HookInvocation invocation;
            const auto original = s_originalNativeLinkedOuter;
            if (!original) {
                return 0;
            }

            // Fallout's exterior linked-area branch calls PreloadWorld from this
            // outer candidate visitor. Do not snapshot the player or allocate an
            // attempt ID here: most per-frame visitors never issue a preload. The
            // nested preload hook captures that context lazily only for real calls.
            if (ObserverLoggingActive() && kVerboseNativeVisitorLogging) {
                const auto attemptID =
                    s_nativeAttemptSequence.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                const auto origin =
                    SnapshotCell(ObservePlayerCellPointer());
                logger::info(
                    "PreloadDiag event=native-linked-world-candidate seq={} tMs={} "
                    "load={} loading={} source=native-linked nativeAttemptId={} "
                    "stage=before-call observationOnly=true candidatePtr={:#x} "
                    "candidateDecoded=false originObservation=true "
                    "originCellPtr={:#x} originCellValid={} originCell={:08X} "
                    "originEditor={} originInterior={} originWorld={:08X} "
                    "originWorldEditor={} direction={}",
                    NextSequence(),
                    NowMs(),
                    s_loadNo.load(std::memory_order_relaxed),
                    s_loading.load(std::memory_order_relaxed),
                    attemptID,
                    reinterpret_cast<std::uintptr_t>(a_candidate),
                    origin.address,
                    origin.valid,
                    origin.formID,
                    origin.editorID ? origin.editorID : "<null>",
                    origin.interior,
                    origin.worldFormID,
                    origin.worldEditorID ? origin.worldEditorID : "<null>",
                    WorldDirectionName(origin));
            }

            ++s_nativeLinkedDepth;
            const auto result = original(a_context, a_candidate);
            --s_nativeLinkedDepth;
            return result;
        }

        void HookedMenuPreloadGrid(
            std::uint32_t a_worldFormID,
            int a_lowerX,
            int a_lowerY,
            RE::TESWorldSpace* a_worldSpace,
            int a_taskValue)
        {
            HookInvocation invocation;
            const auto original = s_originalMenuPreloadGrid;
            if (!original) {
                return;
            }

            original(
                a_worldFormID,
                a_lowerX,
                a_lowerY,
                a_worldSpace,
                a_taskValue);
            if (ObserverLoggingActive()) {
                const auto nowMs = NowMs();
                const auto detail =
                    RecordDetailEvent(DetailStream::kMenuGrid, nowMs);
                if (!detail.logDetail) {
                    if (detail.flushDue) {
                        RequestAggregateSummaryFlush();
                    }
                    return;
                }
                const unsigned side =
                    s_uGridsToLoad.load(std::memory_order_relaxed);
                const auto world = SnapshotForm(a_worldSpace);
                const auto upperX =
                    static_cast<long long>(a_lowerX) + side - 1;
                const auto upperY =
                    static_cast<long long>(a_lowerY) + side - 1;
                logger::info(
                    "PreloadDiag event=menu-geometry-grid seq={} tMs={} load={} "
                    "loading={} source=native-menu geometryOnly=true "
                    "world={:08X} lower=({}, {}) upper=({}, {}) side={} cells={} "
                    "worldArgPtr={:#x} worldArgValid={} worldArg={:08X} "
                    "worldEditor={} worldIdMatch={} taskValue={}",
                    NextSequence(),
                    nowMs,
                    detail.loadNo,
                    detail.loading,
                    a_worldFormID,
                    a_lowerX,
                    a_lowerY,
                    upperX,
                    upperY,
                    side,
                    side * side,
                    world.address,
                    world.valid,
                    world.formID,
                    world.editorID,
                    world.valid && world.formID == a_worldFormID,
                    a_taskValue);
                if (detail.flushDue) {
                    RequestAggregateSummaryFlush();
                }
            }
        }

        [[nodiscard]] bool ReadInteriorRing(
            void* a_tes,
            RE::TESObjectCELL** a_cells,
            unsigned a_slots) noexcept
        {
            bool readable = false;
            __try {
                auto** ring = a_tes
                    ? *reinterpret_cast<RE::TESObjectCELL***>(
                        reinterpret_cast<std::uintptr_t>(a_tes) + 0x60)
                    : nullptr;
                if (ring && a_cells) {
                    for (unsigned i = 0; i < a_slots; ++i) {
                        a_cells[i] = ring[i];
                    }
                    readable = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readable = false;
            }
            return readable;
        }

        void LogInteriorBuffer(
            const char* a_event,
            void* a_tes,
            const char* a_observation,
            const CellSnapshot& a_before,
            const CellSnapshot& a_after,
            std::uint64_t a_nowMs,
            std::uint64_t a_loadNo,
            bool a_loading) noexcept
        {
            constexpr unsigned kMaxSlots = 16;
            std::array<RE::TESObjectCELL*, kMaxSlots> cells{};
            const auto slots = std::min(
                s_uInteriorCellBuffer.load(std::memory_order_relaxed),
                kMaxSlots);
            const bool readable =
                ReadInteriorRing(a_tes, cells.data(), slots);

            std::string ringText;
            if (readable) {
                for (unsigned i = 0; i < slots; ++i) {
                    const auto cell = SnapshotCell(cells[i]);
                    fmt::format_to(
                        std::back_inserter(ringText),
                        "[{}]={:08X}:{}:{}{} ",
                        i,
                        cell.formID,
                        cell.editorID,
                        CellStateName(cell.state),
                        cell.hasLoadedData ? ":ld" : "");
                }
            } else {
                ringText = "<unavailable>";
            }

            logger::info(
                "PreloadDiag event={} seq={} tMs={} load={} loading={} "
                "source=engine-interior-buffer observation={} completionClaim=false "
                "tesPtr={:#x} affected={:08X} "
                "affectedEditor={} affectedState={}({}) affectedLoadedData={:#x} "
                "affectedBeforeState={}({}) affectedBeforeLoadedData={:#x} "
                "affectedAfterState={}({}) affectedAfterLoadedData={:#x} "
                "slots={} ring={}",
                a_event,
                NextSequence(),
                a_nowMs,
                a_loadNo,
                a_loading,
                a_observation ? a_observation : "buffer-call-return",
                reinterpret_cast<std::uintptr_t>(a_tes),
                a_after.formID,
                a_after.editorID,
                a_after.state,
                CellStateName(a_after.state),
                a_after.loadedData,
                a_before.state,
                CellStateName(a_before.state),
                a_before.loadedData,
                a_after.state,
                CellStateName(a_after.state),
                a_after.loadedData,
                slots,
                ringText);
        }

        void HookedAddInteriorBuffer(void* a_tes, RE::TESObjectCELL* a_cell)
        {
            HookInvocation invocation;
            const auto original = s_originalAddInteriorBuffer;
            if (!original) {
                return;
            }
            if (!ObserverLoggingActive()) {
                original(a_tes, a_cell);
                return;
            }
            const auto nowMs = NowMs();
            const auto detail =
                RecordDetailEvent(DetailStream::kInteriorBufferAdd, nowMs);
            if (!detail.logDetail) {
                original(a_tes, a_cell);
                if (detail.flushDue) {
                    RequestAggregateSummaryFlush();
                }
                return;
            }
            const auto before = SnapshotCell(a_cell);
            original(a_tes, a_cell);
            const auto after = SnapshotCell(a_cell);
            LogInteriorBuffer(
                "interior-buffer-add",
                a_tes,
                "buffer-insertion-return",
                before,
                after,
                nowMs,
                detail.loadNo,
                detail.loading);
            if (detail.flushDue) {
                RequestAggregateSummaryFlush();
            }
        }

        void HookedRemoveInteriorBuffer(void* a_tes, RE::TESObjectCELL* a_cell)
        {
            HookInvocation invocation;
            const auto original = s_originalRemoveInteriorBuffer;
            if (!original) {
                return;
            }
            if (!ObserverLoggingActive()) {
                original(a_tes, a_cell);
                return;
            }
            const auto nowMs = NowMs();
            const auto detail =
                RecordDetailEvent(DetailStream::kInteriorBufferRemove, nowMs);
            if (!detail.logDetail) {
                original(a_tes, a_cell);
                if (detail.flushDue) {
                    RequestAggregateSummaryFlush();
                }
                return;
            }
            const auto before = SnapshotCell(a_cell);
            original(a_tes, a_cell);
            const auto after = SnapshotCell(a_cell);
            LogInteriorBuffer(
                "interior-buffer-remove",
                a_tes,
                "buffer-removal-return",
                before,
                after,
                nowMs,
                detail.loadNo,
                detail.loading);
            if (detail.flushDue) {
                RequestAggregateSummaryFlush();
            }
        }

        void ResetTargetState() noexcept
        {
            s_hooksRetainedForLifetime = false;
            for (auto& target : s_targets) {
                target.created = false;
                target.enabled = false;
            }
            s_originalPreloadInterior = nullptr;
            s_originalPreloadWorld = nullptr;
            s_originalNativeLinkedHelper = nullptr;
            s_originalNativeLinkedOuter = nullptr;
            s_originalMenuPreloadGrid = nullptr;
            s_originalAddInteriorBuffer = nullptr;
            s_originalRemoveInteriorBuffer = nullptr;
        }

        [[nodiscard]] bool HasHookState() noexcept
        {
            return std::any_of(
                s_targets.begin(), s_targets.end(),
                [](const HookTarget& a_target) {
                    return a_target.created || a_target.enabled;
                });
        }

        [[nodiscard]] bool RollBackHooks() noexcept
        {
            if (s_hooksRetainedForLifetime) {
                s_installed.store(false, std::memory_order_release);
                std::unique_lock lock(s_hookIdleMutex);
                s_hookIdleCv.wait(lock, [] {
                    return s_activeHookCalls.load(
                               std::memory_order_acquire) == 0;
                });
                return true;
            }
            // Stop observer work before restoring entries. Detours which already
            // crossed an entry remain safe pass-through calls because every
            // original pointer stays alive until all enabled hooks are disabled
            // and all active invocations have drained.
            bool hadCallable =
                s_installed.exchange(false, std::memory_order_acq_rel);
            bool allDisabled = true;
            for (auto it = s_targets.rbegin(); it != s_targets.rend(); ++it) {
                if (it->enabled) {
                    hadCallable = true;
                    const auto status = MH_DisableHook(it->address);
                    if (status == MH_OK || status == MH_ERROR_DISABLED ||
                        status == MH_ERROR_NOT_CREATED) {
                        it->enabled = false;
                    } else {
                        allDisabled = false;
                        logger::error(
                            "PreloadDiag MH_DisableHook({}) failed: {}; "
                            "retaining trampoline/original",
                            it->name,
                            MH_StatusToString(status));
                    }
                }
            }
            if (!allDisabled) {
                return false;
            }

            {
                std::unique_lock lock(s_hookIdleMutex);
                s_hookIdleCv.wait(lock, [] {
                    return s_activeHookCalls.load(
                               std::memory_order_acquire) == 0;
                });
            }

            if (hadCallable) {
                // Retain disabled MinHook objects and original trampolines for
                // process lifetime. A thread can cross an entry immediately
                // before DisableHook and reach HookInvocation only after the
                // active-count drain; removing the trampoline would race it.
                s_hooksRetainedForLifetime = true;
                return true;
            }

            bool allRemoved = true;
            for (auto it = s_targets.rbegin(); it != s_targets.rend(); ++it) {
                if (!it->created) {
                    continue;
                }
                const auto status = MH_RemoveHook(it->address);
                if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
                    it->created = false;
                } else {
                    allRemoved = false;
                    logger::error(
                        "PreloadDiag MH_RemoveHook({}) failed: {}; "
                        "hook is disabled and state is retained",
                        it->name,
                        MH_StatusToString(status));
                }
            }
            if (!allRemoved) {
                return false;
            }

            s_originalPreloadInterior = nullptr;
            s_originalPreloadWorld = nullptr;
            s_originalNativeLinkedHelper = nullptr;
            s_originalNativeLinkedOuter = nullptr;
            s_originalMenuPreloadGrid = nullptr;
            s_originalAddInteriorBuffer = nullptr;
            s_originalRemoveInteriorBuffer = nullptr;
            return true;
        }
    }

    void Configure(bool a_enabled) noexcept
    {
        s_configured.store(a_enabled, std::memory_order_release);
        logger::info("PreloadDiag configured={}", a_enabled);
    }

    bool Install() noexcept
    {
        std::lock_guard lock(s_installMutex);
        if (s_installed.load(std::memory_order_acquire)) {
            return true;
        }
        if (HasHookState()) {
            logger::error(
                "PreloadDiag cannot reinstall while disabled hook/trampoline "
                "state is retained for process lifetime");
            return false;
        }

        s_epochMs = static_cast<std::uint64_t>(GetTickCount64());
        {
            std::lock_guard aggregateLock(s_aggregateMutex);
            s_worldAggregates = {};
            s_candidateAggregates = {};
            s_windowCounters = {};
            s_windowCounters.startMs = NowMs();
            s_windowLoadNo =
                s_loadNo.load(std::memory_order_relaxed);
            s_windowLoading =
                s_loading.load(std::memory_order_relaxed);
            s_nextAggregateFlushMs =
                s_windowCounters.startMs + kAggregateIntervalMs;
        }
        {
            std::lock_guard batchLock(s_sealedBatchMutex);
            s_sealedBatches = {};
            s_sealedBatchHead = 0;
            s_sealedBatchCount = 0;
            s_droppedBatchTotals = {};
        }
        s_worldCallSequence.store(0, std::memory_order_relaxed);
        s_candidateSequence.store(0, std::memory_order_relaxed);
        s_nativeAttemptSequence.store(0, std::memory_order_relaxed);
        s_summaryFlushPending.store(false, std::memory_order_relaxed);
        s_loadStartRequested.store(false, std::memory_order_relaxed);
        s_queuedBatchCount.store(0, std::memory_order_relaxed);
        s_droppedBatchPending.store(false, std::memory_order_relaxed);
        LogNativeSettings("install");

        if (!s_configured.load(std::memory_order_acquire)) {
            logger::info(
                "PreloadDiag hooks not installed (diagnostics disabled); "
                "native settings above were read only");
            return false;
        }

        const auto version = REL::Module::get().version();
        const bool isVR = REL::Module::IsVR();
        const bool isNG = !isVR && REL::Module::IsNG();
        const bool exactOG =
            !isVR && !isNG &&
            version[0] == 1 && version[1] == 10 &&
            version[2] == 163 && version[3] == 0;
        const bool exactVR =
            isVR && version[0] == 1 && version[1] == 2 &&
            version[2] == 72 && version[3] == 0;
        if (!exactOG && !exactVR) {
            logger::warn(
                "PreloadDiag unavailable on runtime {}.{}.{}.{} (VR={}, NG={}); "
                "no hooks placed",
                version[0], version[1], version[2], version[3], isVR, isNG);
            s_supported.store(false, std::memory_order_release);
            return false;
        }
        s_supported.store(true, std::memory_order_release);

        const auto base = REL::Module::get().base();
        ResetTargetState();
        s_targets = {
            HookTarget{
                "PreloadInterior",
                reinterpret_cast<void*>(
                    base + (isVR ? kPreloadInteriorVR : kPreloadInteriorOG)),
                reinterpret_cast<void*>(&HookedPreloadInterior),
                reinterpret_cast<void**>(&s_originalPreloadInterior),
                { kPreloadInteriorBytes.data(), kPreloadInteriorMask,
                    kPreloadInteriorBytes.size() },
                false,
                false },
            HookTarget{
                "PreloadWorld",
                reinterpret_cast<void*>(
                    base + (isVR ? kPreloadWorldVR : kPreloadWorldOG)),
                reinterpret_cast<void*>(&HookedPreloadWorld),
                reinterpret_cast<void**>(&s_originalPreloadWorld),
                { kPreloadWorldBytes.data(), kPreloadWorldMask,
                    kPreloadWorldBytes.size() },
                false,
                false },
            HookTarget{
                "NativeLinkedInteriorHelper",
                reinterpret_cast<void*>(
                    base + (isVR ? kNativeLinkedHelperVR : kNativeLinkedHelperOG)),
                reinterpret_cast<void*>(&HookedNativeLinkedHelper),
                reinterpret_cast<void**>(&s_originalNativeLinkedHelper),
                { kNativeLinkedBytes.data(), kNativeLinkedMask,
                    kNativeLinkedBytes.size() },
                false,
                false },
            HookTarget{
                "NativeLinkedOuterVisitor",
                reinterpret_cast<void*>(
                    base + (isVR ? kNativeLinkedOuterVR : kNativeLinkedOuterOG)),
                reinterpret_cast<void*>(&HookedNativeLinkedOuter),
                reinterpret_cast<void**>(&s_originalNativeLinkedOuter),
                { kNativeLinkedOuterBytes.data(), kNativeLinkedOuterMask,
                    kNativeLinkedOuterBytes.size() },
                false,
                false },
            HookTarget{
                "MenuCombinedGeometryPreloadGrid",
                reinterpret_cast<void*>(
                    base + (isVR ? kMenuPreloadGridVR : kMenuPreloadGridOG)),
                reinterpret_cast<void*>(&HookedMenuPreloadGrid),
                reinterpret_cast<void**>(&s_originalMenuPreloadGrid),
                { kMenuPreloadGridBytes.data(), kMenuPreloadGridMask,
                    kMenuPreloadGridBytes.size() },
                false,
                false },
            HookTarget{
                "AddInteriorBuffer",
                reinterpret_cast<void*>(
                    base + (isVR ? kAddInteriorBufferVR : kAddInteriorBufferOG)),
                reinterpret_cast<void*>(&HookedAddInteriorBuffer),
                reinterpret_cast<void**>(&s_originalAddInteriorBuffer),
                { kAddInteriorBufferBytes.data(), kAddInteriorBufferMask,
                    kAddInteriorBufferBytes.size() },
                false,
                false },
            HookTarget{
                "RemoveInteriorBuffer",
                reinterpret_cast<void*>(
                    base + (isVR
                        ? kRemoveInteriorBufferVR
                        : kRemoveInteriorBufferOG)),
                reinterpret_cast<void*>(&HookedRemoveInteriorBuffer),
                reinterpret_cast<void**>(&s_originalRemoveInteriorBuffer),
                { kRemoveInteriorBufferBytes.data(), kRemoveInteriorBufferMask,
                    kRemoveInteriorBufferBytes.size() },
                false,
                false }
        };

        for (const auto& target : s_targets) {
            if (!Matches(target.address, target.signature)) {
                logger::error(
                    "PreloadDiag {} prologue validation failed at {:#x}; "
                    "no hooks placed",
                    target.name,
                    reinterpret_cast<std::uintptr_t>(target.address));
                s_supported.store(false, std::memory_order_release);
                return false;
            }
        }

        const auto initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error(
                "PreloadDiag MH_Initialize failed: {}",
                MH_StatusToString(initStatus));
            s_supported.store(false, std::memory_order_release);
            return false;
        }

        for (auto& target : s_targets) {
            const auto status = MH_CreateHook(
                target.address, target.detour, target.original);
            if (status == MH_OK) {
                target.created = true;
            }
            if (status != MH_OK || !*target.original) {
                logger::error(
                    "PreloadDiag MH_CreateHook({}) failed: {}; rolling back",
                    target.name,
                    MH_StatusToString(status));
                if (!RollBackHooks()) {
                    logger::error(
                        "PreloadDiag rollback incomplete; surviving detours "
                        "remain pass-through with originals retained");
                }
                s_supported.store(false, std::memory_order_release);
                return false;
            }
        }

        for (auto& target : s_targets) {
            const auto status = MH_EnableHook(target.address);
            if (status != MH_OK) {
                logger::error(
                    "PreloadDiag MH_EnableHook({}) failed: {}; rolling back",
                    target.name,
                    MH_StatusToString(status));
                if (!RollBackHooks()) {
                    logger::error(
                        "PreloadDiag rollback incomplete; surviving detours "
                        "remain pass-through with originals retained");
                }
                s_supported.store(false, std::memory_order_release);
                return false;
            }
            target.enabled = true;
        }

        s_installed.store(true, std::memory_order_release);
        if (!s_summaryWorker.joinable()) {
            try {
                s_summaryWorker =
                    std::jthread(AggregateSummaryWorker);
            } catch (const std::system_error& e) {
                logger::warn(
                    "PreloadDiag summary worker unavailable: {}; "
                    "summaries will drain at shutdown",
                    e.what());
            }
        }
        logger::info(
            "PreloadDiag installed seven mutation-free observers for {} "
            "(menu hook is combined geometry only; per-frame native visitor "
            "snapshots are lazy; repeated confirmed calls/candidates use exact "
            "bounded aggregate counts)",
            isVR ? "Fallout4VR 1.2.72" : "Fallout4 1.10.163");
        return true;
    }

    void SetLoading(bool a_loading, std::uint64_t a_loadNo) noexcept
    {
        HookInvocation invocation;
        const bool configured =
            s_configured.load(std::memory_order_acquire);
        if (a_loading) {
            // Publish intent before waiting for a possible single summary line.
            // The worker will not begin another line after observing this flag.
            s_loadStartRequested.store(true, std::memory_order_release);
        }

        SealedAggregateBatch batch{};
        {
            std::unique_lock emissionLock(s_summaryEmissionMutex);
            const bool wasLoading =
                s_loading.load(std::memory_order_acquire);
            if (configured) {
                const auto endMs = NowMs();
                std::lock_guard aggregateLock(s_aggregateMutex);
                const auto reason = a_loading
                    ? (wasLoading
                        ? AggregateSealReason::kLoadSwitch
                        : AggregateSealReason::kLoadStart)
                    : AggregateSealReason::kLoadEnd;
                (void)SealAggregateWindowLocked(
                    batch,
                    reason,
                    true,
                    a_loadNo,
                    a_loading,
                    endMs);
                // Aggregate recorders select their context while holding the
                // same mutex, so no record can straddle this publication.
                s_loadNo.store(a_loadNo, std::memory_order_relaxed);
                s_loading.store(a_loading, std::memory_order_release);
            } else {
                s_loadNo.store(a_loadNo, std::memory_order_relaxed);
                s_loading.store(a_loading, std::memory_order_release);
            }
            (void)EnqueueSealedBatch(batch);
            if (a_loading) {
                s_loadStartRequested.store(
                    false, std::memory_order_release);
            }
        }

        if (!configured) {
            return;
        }
        logger::info(
            "PreloadDiag event=load-phase seq={} tMs={} load={} loading={}",
            NextSequence(), NowMs(), a_loadNo, a_loading);
        if (!a_loading || batch.valid) {
            // Drain exact summaries on the below-normal diagnostic worker only
            // after the measured native load has closed.
            RequestAggregateSummaryFlush();
        }
    }

    void SetPluginCrosshairContext(bool a_active) noexcept
    {
        if (a_active) {
            ++s_pluginCrosshairDepth;
        } else if (s_pluginCrosshairDepth != 0) {
            --s_pluginCrosshairDepth;
        }
    }

    PluginCrosshairScope::PluginCrosshairScope() noexcept
    {
        SetPluginCrosshairContext(true);
    }

    PluginCrosshairScope::~PluginCrosshairScope() noexcept
    {
        SetPluginCrosshairContext(false);
    }

    void NoteCrosshairCandidate(
        RE::TESObjectREFR* a_door,
        RE::TESObjectCELL* a_destination,
        const char* a_trigger,
        float a_distance,
        int a_result,
        const char* a_resultName) noexcept
    {
        HookInvocation invocation;
        if (!s_configured.load(std::memory_order_relaxed)) {
            return;
        }
        CandidateAggregateKey key{};
        key.door = reinterpret_cast<std::uintptr_t>(a_door);
        key.destination = reinterpret_cast<std::uintptr_t>(a_destination);
        key.loadNo = 0;
        key.result = a_result;
        CopyTrigger(key.trigger, a_trigger);
        const auto ordinal =
            s_candidateSequence.fetch_add(
                1, std::memory_order_relaxed) + 1;
        const auto nowMs = NowMs();
        const auto aggregate =
            RecordCandidateAggregate(key, ordinal, nowMs, a_distance);

        if (aggregate.logDetail) {
            const auto door = SnapshotForm(a_door);
            const auto destination = SnapshotCell(a_destination);
            logger::info(
                "PreloadDiag event=crosshair-candidate seq={} tMs={} load={} "
                "loading={} source=plugin-door-prefetch ordinal={} "
                "aggregateDetail=true trigger={} distance={:.3f} "
                "result={} resultName={} "
                "doorPtr={:#x} doorValid={} door={:08X} doorEditor={} "
                "destPtr={:#x} destValid={} dest={:08X} destEditor={} "
                "interior={} state={}({}) loadedData={:#x} world={:08X} "
                "worldEditor={} grid=({}, {}) "
                "countingRule=exact-window-summary",
                NextSequence(),
                nowMs,
                aggregate.loadNo,
                aggregate.loading,
                ordinal,
                key.trigger.data(),
                a_distance,
                a_result,
                a_resultName ? a_resultName : "<null>",
                door.address,
                door.valid,
                door.formID,
                door.editorID,
                destination.address,
                destination.valid,
                destination.formID,
                destination.editorID,
                destination.interior,
                destination.state,
                CellStateName(destination.state),
                destination.loadedData,
                destination.worldFormID,
                destination.worldEditorID,
                destination.gridX,
                destination.gridY);
        }
        if (aggregate.flushDue) {
            RequestAggregateSummaryFlush();
        }
    }

    bool IsConfigured() noexcept
    {
        return s_configured.load(std::memory_order_acquire);
    }

    bool IsInstalled() noexcept
    {
        return s_installed.load(std::memory_order_acquire);
    }

    void ObservePlayerCell(const char* a_reason) noexcept
    {
        if (!s_configured.load(std::memory_order_relaxed) ||
            !s_supported.load(std::memory_order_acquire)) {
            return;
        }

        RE::PlayerCharacter* player = nullptr;
        RE::TESObjectCELL* cell = nullptr;
        __try {
            player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                cell = player->GetParentCell();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            player = nullptr;
            cell = nullptr;
        }

        const auto snapshot = SnapshotCell(cell);
        logger::info(
            "PreloadDiag event=player-cell seq={} tMs={} load={} loading={} "
            "reason={} playerPtr={:#x} cellPtr={:#x} cellValid={} cell={:08X} "
            "editor={} interior={} state={}({}) loadedData={:#x} world={:08X} "
            "worldEditor={} grid=({}, {})",
            NextSequence(),
            NowMs(),
            s_loadNo.load(std::memory_order_relaxed),
            s_loading.load(std::memory_order_relaxed),
            a_reason ? a_reason : "<null>",
            reinterpret_cast<std::uintptr_t>(player),
            snapshot.address,
            snapshot.valid,
            snapshot.formID,
            snapshot.editorID,
            snapshot.interior,
            snapshot.state,
            CellStateName(snapshot.state),
            snapshot.loadedData,
            snapshot.worldFormID,
            snapshot.worldEditorID,
            snapshot.gridX,
            snapshot.gridY);
    }

    void Shutdown() noexcept
    {
        std::lock_guard lock(s_installMutex);
        const bool configured =
            s_configured.load(std::memory_order_relaxed);
        // Close public diagnostic producers before disabling observers. A
        // candidate which already crossed its fast check is covered by the same
        // active-invocation drain as the hooks.
        s_configured.store(false, std::memory_order_release);
        if (s_summaryWorker.joinable()) {
            s_summaryWorker.request_stop();
            s_summaryWakeCv.notify_all();
            s_summaryWorker.join();
        }
        s_summaryFlushPending.store(false, std::memory_order_release);
        const bool hadHookState =
            s_installed.load(std::memory_order_acquire) ||
            HasHookState();
        // RollBackHooks also drains public diagnostic producers counted by
        // HookInvocation, even when no native target was ever installed.
        const bool safelyDisabled = RollBackHooks();

        bool skippedForActiveLoad = false;
        if (configured && safelyDisabled) {
            SealedAggregateBatch batch{};
            {
                std::unique_lock emissionLock(s_summaryEmissionMutex);
                if (s_loading.load(std::memory_order_acquire) ||
                    s_loadStartRequested.load(
                        std::memory_order_acquire)) {
                    skippedForActiveLoad = true;
                } else {
                    const auto endMs = NowMs();
                    std::lock_guard aggregateLock(s_aggregateMutex);
                    (void)SealAggregateWindowLocked(
                        batch,
                        AggregateSealReason::kShutdown,
                        true,
                        s_windowLoadNo,
                        s_windowLoading,
                        endMs);
                    (void)EnqueueSealedBatch(batch);
                }
            }

            if (!skippedForActiveLoad) {
                while (EmitOneQueuedSummary(true) ==
                       SummaryEmissionResult::kEmitted) {
                }
            }
        }

        if (skippedForActiveLoad) {
            logger::warn(
                "PreloadDiag shutdown aggregate drain skipped because an "
                "active load or load-start publication was in progress");
        }
        if (safelyDisabled && hadHookState) {
            logger::info(
                "PreloadDiag observers disabled; hook/trampoline state {}",
                s_hooksRetainedForLifetime
                    ? "retained for process lifetime"
                    : "removed before becoming callable");
        } else if (!safelyDisabled) {
            logger::error(
                "PreloadDiag shutdown incomplete; aggregate flush skipped "
                "because a callable hook could not be disabled");
        }
    }

    namespace
    {
        struct DiagnosticsLifetimeGuard
        {
            ~DiagnosticsLifetimeGuard()
            {
                Shutdown();
            }
        } s_diagnosticsLifetimeGuard;
    }
}
