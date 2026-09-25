#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace VRLoadingScreens::Policy
{
    enum class WorldspacePreparation
    {
        kReady,
        kInitialized,
        kIncomplete,
        kCapacityReached,
        kLifetimeMismatch
    };

    // These are form-lifetime leases, not submission-lifetime leases. A void
    // PreloadWorld return, loading-menu close, or plugin queue flush does not
    // mean the engine's cell-loader tasks have finished. Only native ClearData
    // may retire a lease. Maps and scene objects always remain engine-owned.
    // Synchronization and native calls belong to the caller/backend.
    template <class World, class Graph, std::size_t Capacity = 128>
    class PreparedWorldspaces
    {
    public:
        template <class Backend>
        [[nodiscard]] WorldspacePreparation Prepare(World* a_world, Backend& a_backend)
        {
            if (!a_world) {
                return WorldspacePreparation::kIncomplete;
            }
            if (const auto* lease = Find(a_world)) {
                return a_backend.Ready(a_world) &&
                        a_backend.GraphOf(a_world) == lease->graph ?
                    WorldspacePreparation::kReady :
                    WorldspacePreparation::kLifetimeMismatch;
            }
            Lease* available = nullptr;
            for (auto& lease : leases) {
                if (!lease.world) {
                    available = &lease;
                    break;
                }
            }
            // Reject before allocating any native resources when our bounded
            // registry cannot guarantee their lifetime.
            if (!available) {
                return WorldspacePreparation::kCapacityReached;
            }
            const bool initialized = !a_backend.Ready(a_world);
            if (initialized) {
                a_backend.InitializeDetached(a_world);
            }
            if (!a_backend.Ready(a_world)) {
                return WorldspacePreparation::kIncomplete;
            }
            auto* graph = a_backend.GraphOf(a_world);
            a_backend.Retain(graph);
            *available = { a_world, graph };
            return initialized ? WorldspacePreparation::kInitialized :
                WorldspacePreparation::kReady;
        }

        template <class Backend>
        void OnNativeWorldSelection(World* a_world, Backend& a_backend) const
        {
            if (const auto* lease = Find(a_world);
                lease && a_backend.Ready(a_world) &&
                a_backend.GraphOf(a_world) == lease->graph) {
                a_backend.AttachToScene(a_world);
            }
        }

        template <class Backend>
        bool BeforeNativeClearData(World* a_world, Backend& a_backend)
        {
            for (auto& lease : leases) {
                if (lease.world == a_world && a_world) {
                    const auto retired = std::exchange(lease, {});
                    // Drop only our additional graph reference. ClearData then
                    // performs the native graph/node/map teardown itself.
                    a_backend.Release(retired.graph);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            std::size_t count = 0;
            for (const auto& lease : leases) {
                count += lease.world != nullptr;
            }
            return count;
        }

    private:
        struct Lease { World* world{}; Graph* graph{}; };
        [[nodiscard]] const Lease* Find(World* a_world) const noexcept
        {
            for (const auto& lease : leases) {
                if (lease.world == a_world && a_world) {
                    return &lease;
                }
            }
            return nullptr;
        }
        // Trivial pointer storage deliberately performs no engine calls during
        // DLL/static destruction, when Fallout's allocators may already be gone.
        std::array<Lease, Capacity> leases{};
    };
}
