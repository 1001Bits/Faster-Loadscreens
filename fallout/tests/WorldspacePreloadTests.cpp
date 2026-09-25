#include "WorldspacePreloadState.h"

#include <cstdlib>
#include <iostream>

namespace
{
    struct Graph { int references{ 1 }; };
    struct World
    {
        Graph storage;
        Graph* graph{};
        bool forwardMap{}, reverseMap{}, nodes{};
    };
    struct Engine
    {
        World* scene{};
        int initializations{}, retains{}, releases{};
        bool allocationFailure{};
        bool Ready(World* world) const
        {
            return world->graph && world->forwardMap &&
                world->reverseMap && world->nodes;
        }
        Graph* GraphOf(World* world) const { return world->graph; }
        void InitializeDetached(World* world)
        {
            ++initializations;
            if (!allocationFailure) {
                world->graph = &world->storage;
                world->forwardMap = world->reverseMap = world->nodes = true;
            }
        }
        void Retain(Graph* graph) { ++graph->references; ++retains; }
        void Release(Graph* graph) { --graph->references; ++releases; }
        void AttachToScene(World* world) { scene = world; }
    };
    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }
}

int main()
{
    using namespace VRLoadingScreens::Policy;
    PreparedWorldspaces<World, Graph, 2> state;
    Engine engine;
    World commonwealth, farHarbor, diamondCity, third;
    engine.scene = &commonwealth;
    Check(state.Prepare(&farHarbor, engine) == WorldspacePreparation::kInitialized,
        "Cold destination must initialize before submission");
    Check(engine.Ready(&farHarbor) && engine.scene == &commonwealth,
        "Worker prerequisites must exist without changing the current scene");
    Check(farHarbor.storage.references == 2,
        "Native ClearPortalGraph's refcount==1 path must be held off");
    Check(state.Prepare(&farHarbor, engine) == WorldspacePreparation::kReady &&
        engine.initializations == 1 && engine.retains == 1,
        "Repeated requests must reuse the graph and its single lease");
    state.OnNativeWorldSelection(&farHarbor, engine);
    Check(engine.scene == &farHarbor && farHarbor.storage.references == 2,
        "Real arrival must bind prepared nodes while preserving worker lifetime");
    state.OnNativeWorldSelection(&commonwealth, engine);
    Check(engine.scene == &farHarbor,
        "Unprepared world selection belongs entirely to the original engine call");
    engine.InitializeDetached(&diamondCity);
    Check(state.Prepare(&diamondCity, engine) == WorldspacePreparation::kReady &&
        engine.initializations == 2 && engine.retains == 2,
        "Previously visited destinations also need a lease, without reinitialization");
    Check(state.Prepare(&third, engine) == WorldspacePreparation::kCapacityReached &&
        !third.graph && engine.initializations == 2,
        "Registry exhaustion must reject work before native allocation");
    Check(state.BeforeNativeClearData(&farHarbor, engine) &&
        farHarbor.storage.references == 1 && engine.Ready(&farHarbor),
        "Native teardown must receive ownership with maps and nodes intact");
    Check(!state.BeforeNativeClearData(&farHarbor, engine) && engine.releases == 1,
        "Duplicate teardown notifications cannot release a graph twice");
    // Simulate native form replacement reusing the same world address.
    farHarbor = {};
    Check(state.Prepare(&farHarbor, engine) == WorldspacePreparation::kInitialized &&
        farHarbor.storage.references == 2,
        "Reused form addresses must not inherit stale preparation state");
    Graph unexpected;
    farHarbor.graph = &unexpected;
    Check(state.Prepare(&farHarbor, engine) == WorldspacePreparation::kLifetimeMismatch &&
        unexpected.references == 1,
        "Unexpected graph replacement must fail closed without adopting a stale lease");
    farHarbor.graph = &farHarbor.storage;
    state.BeforeNativeClearData(&farHarbor, engine);
    engine.allocationFailure = true;
    Check(state.Prepare(&third, engine) == WorldspacePreparation::kIncomplete &&
        state.Size() == 1 && !third.graph,
        "Incomplete native initialization must never authorize a preload");
    state.BeforeNativeClearData(&diamondCity, engine);
    Check(state.Size() == 0 && engine.retains == engine.releases,
        "Native form teardown must balance every additional graph reference");
    std::cout << "Worldspace preload ownership and scene handoff checks passed\n";
}
