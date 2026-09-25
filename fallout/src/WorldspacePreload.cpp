#include "PCH.h"
#include "WorldspacePreload.h"
#include "WorldspacePreloadState.h"
#include "DoorPrefetch.h"

#include <MinHook.h>
#include <array>
#include <cstring>
#include <mutex>

namespace VRLoadingScreens::WorldspacePreload
{
    namespace
    {
        using World = RE::TESWorldSpace;
        using Graph = RE::NiRefObject;
        using CreateGraph = Graph* (*)(World*);
        using ClearWorld = void (*)(World*);
        // AE passes an additional reset context in RDX. OG/VR ignore RDX.
        using ResetGame = void (*)(void*, void*);
        using SetSceneChild = void (*)(RE::NiNode*, std::uint32_t, RE::NiNode*);

        struct FunctionContract
        {
            std::uintptr_t rva;
            std::size_t size;
            std::uint64_t hash;
        };
        struct RuntimeContract
        {
            REL::Version version;
            bool vr;
            FunctionContract create, select, clear, destroy, reset;
            std::uintptr_t selectionCall, multiboundCall, sharedCall, sceneRoot;
            std::size_t setAtSlot;
        };

        // Combined decompilation + complete, contiguous machine-code contracts.
        // See docs/ISSUE_2_INVESTIGATION_2026-09-26.md. Only the two scene SetAt
        // calls are suppressed during preparation; all constructors, map
        // sentinels, allocator choices and ownership transfers remain native.
        const std::array kContracts{
            RuntimeContract{
                { 1, 10, 163, 0 }, false,
                { 0x4949D0, 875, 0xD4D998F6D8DACBFFULL },
                { 0xF7A90, 683, 0x94B9ADE868DCEF96ULL },
                { 0x490910, 367, 0x1708135A9B851480ULL }, {},
                { 0xD3B800, 1756, 0x8E40DEFB72526097ULL },
                0xF7ADA, 0x494C20, 0x494CDE, 0x6721B70, 0x208 / 8 },
            RuntimeContract{
                { 1, 2, 72, 0 }, true,
                { 0x47DB10, 875, 0x0BFC591F500CDF96ULL },
                { 0xF7B30, 671, 0x9B117BC8D9C0D774ULL },
                { 0x479A50, 367, 0x3A12862DF55F37C6ULL }, {},
                { 0xD86670, 1797, 0xD765A25222B51B44ULL },
                0xF7B7A, 0x47DD60, 0x47DE1E, 0x6879520, 0x220 / 8 },
            RuntimeContract{
                { 1, 11, 221, 0 }, false,
                { 0x575580, 908, 0x044BA10680E7BFA5ULL },
                { 0x2D0110, 683, 0xF58D3EC83E67B28DULL },
                { 0x571230, 310, 0x1FF16CFAAB48EF30ULL },
                { 0x570AB0, 1525, 0xEECBD68A7867C32DULL },
                { 0xC36850, 827, 0xE71FF0F16548804EULL },
                0x2D015A, 0x5757E4, 0x5758A9, 0x3E47620, 0x208 / 8 },
            RuntimeContract{
                { 1, 11, 240, 0 }, false,
                { 0x5758A0, 908, 0xDB1CE5C909B2C121ULL },
                { 0x2D0430, 683, 0xCA5804950FF7CC06ULL },
                { 0x571550, 310, 0x4C8E8931FCC1336CULL },
                { 0x570DD0, 1525, 0x6342CC239ABF9931ULL },
                { 0xC36BE0, 827, 0xF1798F62B90BB4FAULL },
                0x2D047A, 0x575B04, 0x575BC9, 0x3E5DF60, 0x208 / 8 }
        };

        static_assert(offsetof(World, portalGraph) == 0x160);
        static_assert(offsetof(World, multiboundRefMap) == 0x168);
        static_assert(offsetof(World, refMultiboundMap) == 0x170);
        static_assert(offsetof(World, multiboundNode) == 0x178);
        static_assert(offsetof(World, portalSharedNode) == 0x180);

        std::recursive_mutex s_mutex;
        Policy::PreparedWorldspaces<World, Graph> s_worlds;
        std::atomic<bool> s_installed{};
        CreateGraph s_create{};
        ClearWorld s_clear{}, s_destroy{};
        ResetGame s_reset{};
        std::atomic<bool> s_resetting{};
        RE::NiNode** s_sceneRoot{};
        std::size_t s_setAtSlot{};
        thread_local bool s_detachedInitialization{};
        // Keep installed executable thunks alive for the entire process.
        F4SE::Trampoline* s_trampoline{};

        bool MatchFunction(std::uintptr_t a_base, const FunctionContract& a_contract) noexcept
        {
            if (!a_contract.rva || !a_contract.size) {
                return false;
            }
            __try {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_base + a_contract.rva);
                std::uint64_t hash = 14695981039346656037ULL;
                for (std::size_t i = 0; i < a_contract.size; ++i) {
                    hash = (hash ^ bytes[i]) * 1099511628211ULL;
                }
                return hash == a_contract.hash;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void NativeSetAt(RE::NiNode* a_root, std::uint32_t a_index, RE::NiNode* a_child)
        {
            // Use the verified three-argument vtable slot. CommonLib's flat
            // SetAt overload labels do not agree with this native callsite.
            const auto vtable = *reinterpret_cast<SetSceneChild**>(a_root);
            vtable[s_setAtSlot](a_root, a_index, a_child);
        }

        void SceneChildHook(RE::NiNode* a_root, std::uint32_t a_index, RE::NiNode* a_child)
        {
            if (!s_detachedInitialization) {
                NativeSetAt(a_root, a_index, a_child);
            }
        }

        struct Backend
        {
            Graph* GraphOf(World* a_world) const
            {
                return reinterpret_cast<Graph*>(a_world->portalGraph.get());
            }
            bool Ready(World* a_world) const
            {
                auto* graph = GraphOf(a_world);
                return graph && a_world->multiboundRefMap && a_world->refMultiboundMap &&
                    a_world->multiboundNode && a_world->portalSharedNode &&
                    *reinterpret_cast<RE::NiNode**>(reinterpret_cast<std::byte*>(graph) + 0x70) ==
                        a_world->portalSharedNode.get();
            }
            void InitializeDetached(World* a_world) const
            {
                struct Scope
                {
                    bool previous{ std::exchange(s_detachedInitialization, true) };
                    ~Scope() { s_detachedInitialization = previous; }
                } scope;
                s_create(a_world);
            }
            void Retain(Graph* a_graph) const { a_graph->IncRefCount(); }
            void Release(Graph* a_graph) const { a_graph->DecRefCount(); }
            void AttachToScene(World* a_world) const
            {
                auto* root = s_sceneRoot ? *s_sceneRoot : nullptr;
                if (root) {
                    NativeSetAt(root, 8, a_world->multiboundNode.get());
                    NativeSetAt(root, 9, a_world->portalSharedNode.get());
                    logger::info("WorldspacePreload: native world selection attached world={:08X}",
                        a_world->GetFormID());
                }
            }
        };

        // This replaces only TES::SetWorldSpace's direct CreatePortalGraph call.
        // The late CreatePortalGraph call on a cell-loader thread cannot bind a
        // prepared foreign world to the current scene: its graph already exists.
        Graph* WorldSelectionHook(World* a_world)
        {
            std::lock_guard lock(s_mutex);
            auto* graph = s_create(a_world);
            Backend backend;
            s_worlds.OnNativeWorldSelection(a_world, backend);
            return graph;
        }

        void ClearDataHook(World* a_world)
        {
            std::lock_guard lock(s_mutex);
            Backend backend;
            if (s_worlds.BeforeNativeClearData(a_world, backend)) {
                logger::info("WorldspacePreload: lease retired by native ClearData world={:08X}",
                    a_world->GetFormID());
            }
            s_clear(a_world);
        }

        void DestroyHook(World* a_world)
        {
            std::lock_guard lock(s_mutex);
            Backend backend;
            // AE inlines map/node teardown in the destructor. OG and VR call
            // ClearData instead, so their contract needs only ClearDataHook.
            if (s_worlds.BeforeNativeClearData(a_world, backend)) {
                logger::info("WorldspacePreload: lease retired by native destructor world={:08X}",
                    a_world->GetFormID());
            }
            s_destroy(a_world);
        }

        void ResetHook(void* a_main, void* a_context)
        {
            // Publish before the native reset starts draining priority-5 work.
            // Menu notifications happen later and cannot protect this boundary.
            s_resetting.store(true, std::memory_order_release);
            DoorPrefetch::FlushQueuedLoads(true);
            DoorPrefetch::SetGameSessionActive(false);
            {
                std::lock_guard drain(s_mutex);
            }
            logger::info("WorldspacePreload: native reset barrier entered before loader drain");
            s_reset(a_main, a_context);
            s_resetting.store(false, std::memory_order_release);
            logger::info("WorldspacePreload: native reset completed; prediction awaits a loaded game session");
        }
    }

    bool Install()
    {
        std::lock_guard lock(s_mutex);
        if (s_installed.load(std::memory_order_acquire)) {
            return true;
        }
        const auto& module = REL::Module::get();
        const auto base = module.base();
        const RuntimeContract* contract = nullptr;
        for (const auto& candidate : kContracts) {
            if (candidate.version == module.version() && candidate.vr == REL::Module::IsVR()) {
                contract = &candidate;
                break;
            }
        }
        if (!contract || !MatchFunction(base, contract->create) ||
            !MatchFunction(base, contract->select) || !MatchFunction(base, contract->clear) ||
            !MatchFunction(base, contract->reset) ||
            (contract->destroy.rva && !MatchFunction(base, contract->destroy))) {
            logger::error("WorldspacePreload: native initialization/lifetime contract mismatch; exterior submissions unavailable");
            return false;
        }

        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            return false;
        }
        auto* clearTarget = reinterpret_cast<void*>(base + contract->clear.rva);
        auto* destroyTarget = contract->destroy.rva ?
            reinterpret_cast<void*>(base + contract->destroy.rva) : nullptr;
        auto* resetTarget = reinterpret_cast<void*>(base + contract->reset.rva);
        if (MH_CreateHook(clearTarget, reinterpret_cast<void*>(&ClearDataHook),
                reinterpret_cast<void**>(&s_clear)) != MH_OK) {
            logger::error("WorldspacePreload: could not install native ClearData lifetime hook");
            return false;
        }
        if (destroyTarget && MH_CreateHook(destroyTarget, reinterpret_cast<void*>(&DestroyHook),
                reinterpret_cast<void**>(&s_destroy)) != MH_OK) {
            MH_RemoveHook(clearTarget);
            logger::error("WorldspacePreload: could not install native destructor lifetime hook");
            return false;
        }

        if (MH_CreateHook(resetTarget, reinterpret_cast<void*>(&ResetHook),
                reinterpret_cast<void**>(&s_reset)) != MH_OK) {
            MH_RemoveHook(clearTarget);
            if (destroyTarget) MH_RemoveHook(destroyTarget);
            logger::error("WorldspacePreload: could not install native reset barrier");
            return false;
        }

        const std::array sites{
            base + contract->selectionCall, base + contract->multiboundCall, base + contract->sharedCall
        };
        const std::array<std::size_t, 3> sizes{ 5, 6, 6 };
        std::array<std::array<std::uint8_t, 6>, 3> original{};
        for (std::size_t i = 0; i < sites.size(); ++i) {
            std::memcpy(original[i].data(), reinterpret_cast<const void*>(sites[i]), sizes[i]);
        }
        s_create = reinterpret_cast<CreateGraph>(base + contract->create.rva);
        s_sceneRoot = reinterpret_cast<RE::NiNode**>(base + contract->sceneRoot);
        s_setAtSlot = contract->setAtSlot;
        bool patched = false;
        try {
            if (!s_trampoline) {
                s_trampoline = new F4SE::Trampoline("WorldspacePreload");
                s_trampoline->create(128);
            }
            patched = true;
            s_trampoline->write_call<5>(sites[0], &WorldSelectionHook);
            s_trampoline->write_call<6>(sites[1], &SceneChildHook);
            s_trampoline->write_call<6>(sites[2], &SceneChildHook);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            if (MH_EnableHook(clearTarget) == MH_OK &&
                (!destroyTarget || MH_EnableHook(destroyTarget) == MH_OK) &&
                MH_EnableHook(resetTarget) == MH_OK) {
                s_installed.store(true, std::memory_order_release);
                logger::info("WorldspacePreload: native detached initialization + world-selection handoff + form-lifetime leases installed (runtime={}, maxWorlds=128)",
                    module.version().string());
                return true;
            }
        } catch (const std::exception& error) {
            logger::error("WorldspacePreload: installation failed: {}", error.what());
        }

        // A partial installation never authorizes a submission. Restore exactly
        // the three owned callsites and leave other plugins' hooks untouched.
        if (patched) {
            for (std::size_t i = 0; i < sites.size(); ++i) {
                REL::safe_write(sites[i], original[i].data(), sizes[i]);
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        }
        MH_DisableHook(clearTarget);
        MH_RemoveHook(clearTarget);
        if (destroyTarget) {
            MH_DisableHook(destroyTarget);
            MH_RemoveHook(destroyTarget);
        }
        MH_DisableHook(resetTarget);
        MH_RemoveHook(resetTarget);
        logger::error("WorldspacePreload: rolled back incomplete installation; exterior submissions unavailable");
        return false;
    }

    bool Prepare(World* a_world)
    {
        if (!s_installed.load(std::memory_order_acquire) || !a_world) {
            return false;
        }
        std::lock_guard lock(s_mutex);
        Backend backend;
        const auto result = s_worlds.Prepare(a_world, backend);
        const bool ready = result == Policy::WorldspacePreparation::kReady ||
            result == Policy::WorldspacePreparation::kInitialized;
        if (result != Policy::WorldspacePreparation::kReady) {
            logger::info("WorldspacePreload: prepare world={:08X} ready={} result={} graph={:p} maps=({:p},{:p}) retainedWorlds={} ownership=engine/form-lifetime",
                a_world->GetFormID(), ready, static_cast<int>(result),
                static_cast<void*>(backend.GraphOf(a_world)),
                static_cast<void*>(a_world->multiboundRefMap),
                static_cast<void*>(a_world->refMultiboundMap), s_worlds.Size());
        }
        return ready;
    }

    // Addressable in the linker map for probes of the exact release boundary.
    __declspec(noinline) bool Submit(void* a_tes, World* a_world,
        int a_x, int a_y, PreloadWorldFn a_preload)
    {
        if (!a_tes || !a_preload || s_resetting.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(s_mutex);
        if (s_resetting.load(std::memory_order_acquire) || !Prepare(a_world)) return false;
        a_preload(a_tes, a_world, a_x, a_y, true);
        return true;
    }
}
