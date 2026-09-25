# Issue #2: cross-worldspace door preload and missing multibound maps

Investigated 2026-09-26 against the surviving local source, the supplied release
archive, Fallout 4 OG 1.10.163 machine code, and the existing **Combined** Ghidra
project. [Upstream report](https://github.com/1001Bits/Faster-Loadscreens/issues/2).

The report identifies a real gap in the plugin's preload checks. The reported
fault is consistent with a null **destination worldspace multibound reference
map**, rather than a missing bound inside the BSMultiBoundNode. Our speculative
single-cell request bypasses the normal worldspace setup that creates this map.
The exact fault and initialization chain are verified statically; the reporter's
game and complete mod list have not been reproduced locally.

## Release provenance

The downloaded archive named `Faster Loadscreens 102233 1.6.3
2026-09-02T18-16Z 3TfnOrClT.7z` is byte-identical to the local developer checkout's
`dist/Faster Loadscreens 2.1.7z`:

```text
Archive SHA-256:
903208c86a4dc16e18959a74336bf39c9d6c9c59c17fb61c48afb28bb1b268bd
LoadingScreens.dll SHA-256:
82a35b64e934a9b966ffee1e2a27ed010d5f0575db1c4edb7871ba1824767f6c
DLL size: 1,570,816 bytes
DLL PE timestamp: 2026-09-02 17:54:52 UTC
```

At the start of the investigation the same DLL was in `build/Release`,
`package/Data/F4SE/Plugins`, `dist`, and `.tmp/inspect_faster_163`.
The build/package outputs now contain the 2.1.1 fix; the original archive and
inspection copy remain preserved locally. Its embedded source paths point to that checkout.
The local workspace also contains older June archives called 1.6.3/1.6.4; their DLLs
are different. Archive labels alone are therefore insufficient to identify the
code under investigation. At the start of the investigation, the local source
had substantial uncommitted changes; the March Git HEAD was not a snapshot of
the September release. This source update publishes the current Fallout tree
under the repository's existing `fallout/` layout.

## Released 2.1 plugin path and configuration

- `src/main.cpp:161` reads `bPreloadExteriorGates`, default **true**.
  `:164-167` maps distance selector 0 to one cell; `:2563` converts this to
  **4096 units**, so the reported door at approximately 3173 units qualifies.
- `src/DoorPrefetch.cpp:1168` rejects same-worldspace destinations. Additional
  same-worldspace filters exist in both gate enumeration paths. Cross-worldspace
  destinations are the purpose of the current feature, not a missed equality
  check at this one callsite.
- `src/DoorPrefetch.cpp:1291` submits
  `s_preloadWorld(tes, destWS, centerX, centerY, true)`.
  The preceding checks cover session/transition state, player/source cell,
  destination position, and deduplication. They do not establish that the target
  world's portal graph and multibound maps exist or remain valid for the queued
  work.
- Result code 16 means the void engine call returned. It is not proof of loading
  completion. `presentationEvidenceArmed` concerns loading-screen presentation;
  it does not validate worldspace loading state.
- `CellWorldspaceGuard` is a VR-only guard for a different null-worldspace
  unregister crash. It does not protect this OG load-time failure.
- `package/Data/MCM/Config/FasterLoadscreens/settings.ini` ships the feature on.
  The MCM toggle was removed. `cmake/ValidateReleaseInputs.cmake:203` rejects that
  control, while `:297` and `:477` require the enabled default and parser fallback.

## Exact engine failure

Analysis source: `C:\Ghidraprojects\Combined`, program
`/Fallout4/Fallout4_OG_1_10_163.exe`. The critical instructions were independently
checked against `C:\Games\Fallout.4 1.10.163\Fallout4.exe` using Capstone.
Addresses below are RVAs for **OG 1.10.163 only**.

| RVA | Function or instruction | Relevance |
| --- | --- | --- |
| `0xFC0D0` | `TES::PreloadWorld` | `queueOnly=true` directly calls the cell loader and returns. |
| `0x391DB0` | `ExteriorCellLoader::QueueCellLoad` | Receives the destination world and grid without the normal worldspace switch. |
| `0xF7A90` | `TES::SetWorldSpace` | Calls `TESWorldSpace::CreatePortalGraph` when changing worlds. |
| `0x4949D0` | `TESWorldSpace::CreatePortalGraph` | Creates the portal graph and allocates both multibound maps when absent. |
| `0x494D50` | `TESWorldSpace::ClearPortalGraph` | Confirms that graph/node/map state participates in the worldspace lifecycle. |
| `0x3C2C10` | `TESObjectCELL::GetMultiboundRefMap` | For exterior cells, returns `cell->worldSpace->multiboundRefMap` at world offset `0x168`. |
| `0x3C2C40` | `TESObjectCELL::GetRefMultiboundMap` | Returns the exterior world's reverse map at offset `0x170`. |
| `0x3E26B0` | `TESObjectCELL::AddMultiBoundRef` | Registers the node in those maps after node/bound construction. |

The faulting sequence is:

```asm
; TESObjectCELL::AddMultiBoundRef
0x3E2AAF  call 0x3C2C10          ; GetMultiboundRefMap(this), result in RAX
0x3E2ABE  lea  r14, [rax+8]      ; embedded hash-table kernel
0x3E2AC2  call 0x1B107A0         ; CRC for the reference handle
0x3E2AC7  mov  r13d, [rbp-0x59]
0x3E2ACB  mov  rdx, [r14+0x20]  ; reported fault
```

Given the reported `R14=8`, the getter returned null. The attempted access is
`0 + 8 + 0x20 = 0x28`, exactly matching the issue. RAX at the fault is no longer
the getter result because the intervening CRC call may overwrite it. This also
explains why several reported registers are repeatable integer values rather
than meaningful object pointers.

For an exterior cell, the getter executes:

```asm
0x3C2C22  mov rax, [rcx+0xC8]   ; owning TESWorldSpace
0x3C2C29  mov rax, [rax+0x168]   ; multiboundRefMap pointer
0x3C2C30  ret
```

The crash occurred later, in AddMultiBoundRef, so the observed failure is a null
map returned from the worldspace, rather than a null worldspace at the getter.
CommonLib's `TESWorldSpace.h` independently identifies `+0x168` and `+0x170` as
these two map pointers.

`PreloadWorld(..., true)` does not run `SetWorldSpace` or `CreatePortalGraph`.
It can therefore queue an arrival cell in an uninitialized destination world.
When that cell contains a multibound marker, asynchronous Load3D can reach this
unchecked map insertion. This is a concrete mechanism for the reported
Commonwealth-to-Far-Harbor crash; confirming the full runtime history would still
require the reporter's save/mod setup and complete crash evidence.

The old changelog's "queueOnly byte flip" describes a retired native-callsite
policy. `NativePreloadPolicy.cpp` is not in the current CMake source list. The
current failure does not require an active patch to PreloadWorld's prologue or
an invented calling convention: DoorPrefetch directly selects its existing
queue-only branch with the fifth argument set to true.

## Mitigation and fix direction

For the existing release, add or update the following in the effective
`Data/MCM/Settings/FasterLoadscreens.ini`, then restart the game:

```ini
[DoorPrefetch]
bPreloadExteriorGates=0
```

The parser loads this user file after the packaged defaults. In Mod Organizer,
edit the winning virtual file, including an overwrite copy if present. This
disables speculative exterior preloading while retaining the loadscreen
presentation and other speed patches. A restart avoids already-queued engine
work; changing a setting cannot cancel cell-loader work that has already been
accepted. The reporter says this switch prevents their crash.

This is a workaround for the old DLL. The requested 2.1.1 fix keeps
`bPreloadExteriorGates=1` supported and enabled by default, and implements the
destination initialization and lifetime requirements described below.

The proposed `destinationWorld == sourceWorld` restriction would block this case,
but the current feature already rejects same-worldspace requests. Adding it
without redesigning the existing filters disables all gate submissions.
Enabling same-worldspace speculative work instead would be new behavior needing
separate validation.

Pointer presence at submission alone does not establish lifetime. Calling
CreatePortalGraph without controlling scene attachment also changes the active
world's scene nodes. Switching to `queueOnly=false` invokes live-world/grid
mutation, a path already retired after earlier crashes. Skipping the faulting
map insertion leaves registration incomplete.

Validation for a code fix should cover the reporter's cold Far Harbor
destination, previously visited destinations, both Diamond City directions,
save/new-game replacement, and a transition starting while the speculative
loader is active. Runtime support beyond OG requires independent layout and
lifetime verification.

## Evidence and limits

Raw Combined decompilations are saved under `.tmp/issue-2-20260926/`:

- `engine-query.txt`: PreloadWorld, QueueCellLoad, AddMultiBoundRef.
- `multibound-map-getters.txt`: both cell-to-map accessors.
- `worldspace-lifecycle.txt`: SetWorldSpace, ClearPortalGraph, GetSkyCell.
- `create-portal-graph.txt`: allocation and scene attachment.

Ghidra has some incorrect inferred union/type aliases in larger functions;
the critical exterior getter, null-to-0x28 arithmetic, and preload branch were
checked in machine code rather than inferred from those aliases.

## Implemented in 2.1.1

`WorldspacePreload::Submit` is called inside DoorPrefetch's final submission barrier,
after refreshing the door's world and arrival grid. A changed or unresolved
destination is rejected. The native initializer creates the graph, both maps,
and both nodes before any async cell request can reach AddMultiBoundRef.

Only the initializer's two `NiNode::SetAt` callsites are suppressed within the
preparation thread's scoped flag. The engine still allocates and constructs all
objects, including its own hash-table sentinels. The active scene is untouched.
Preparation and the native queue-only call share the teardown mutex, so cleanup
cannot run between those two operations.
The exact `TES::SetWorldSpace -> CreatePortalGraph` callsite reattaches prepared
nodes during a real world selection. A cell-loader thread's later initializer
call finds the already-existing graph and takes the native early return.

Every prepared world receives one extra native graph reference. This prevents
`ClearPortalGraph`'s `refCount == 1` cleanup from destroying nodes while an async
request or a resident destination cell can still use them. The lease lasts for
the worldspace form's lifetime, including menu transitions and save replacement
that retain the forms. It is not released by a plugin queue flush, timeout,
configuration change, or a void PreloadWorld return. Map pointers are never
cached outside their owning world, replaced, or freed by the plugin.

`TESWorldSpace::ClearData` releases that additional reference before normal
engine-owned teardown. AE also inlines graph/map cleanup in its destructor, so
that destructor has its own release hook. OG and VR destructors call ClearData.
Repeated cleanup is idempotent; an address reused for a new form has no stale
lease. The fixed registry bounds retained worlds to 128; exhaustion rejects a
new speculative request before allocation. Registry destruction itself never
calls into the engine during DLL shutdown.

The engine's native form-teardown boundary remains responsible for draining its
loaders. A hook at Main::PerformGameReset publishes a reset barrier, drains any
plugin submission already in progress, and parks prediction **before** native
loader draining begins. This covers reset paths whose loading-menu notification
arrives too late. Submission rechecks that barrier under the teardown mutex.
Save loading also retains the existing synchronous F4SE pre-load barrier.
In OG, Main::PerformGameReset processes priority-5 work for full reset,
cancels master-file loads, clears cells, purges buffers and flushes task queues
before TESDataHandler::ClearData destroys worldspace forms. Plugin transition
barriers intentionally do not pretend to drain that global engine queue.

The entire initializer, SetWorldSpace, ClearData and PerformGameReset function bodies must match
their per-executable FNV-1a machine-code contracts before installation. AE's
inlined destructor is also checked. A failed contract or partial installation
leaves speculative submissions unavailable, with an explicit log entry.
Partial callsite edits are rolled back. Native allocation failure/incomplete
state never authorizes a plugin submission.

| Runtime | CreatePortalGraph | SetWorldSpace call | ClearData | Inlined destructor | PerformGameReset |
| --- | --- | --- | --- | --- | --- |
| OG 1.10.163 | `4949D0` | `F7ADA` | `490910` | calls ClearData | `D3B800` |
| VR 1.2.72 | `47DB10` | `F7B7A` | `479A50` | calls ClearData | `D86670` |
| AE 1.11.221 | `575580` | `2D015A` | `571230` | `570AB0` | `C36850` |
| AE 1.11.240 | `5758A0` | `2D047A` | `571550` | `570DD0` | `C36BE0` |

All addresses are executable-relative RVAs. All four implementations use world
offsets 160/168/170/178/180 and graph-root offset 70. The initializer's actual
three-argument SetAt slot is 208 on flat and 220 on VR; the CommonLib overload
labels differ, so the verified slot is invoked directly.

The working source before the fix is preserved in
`.tmp/issue-2-20260926/before-fix.zip`. Complete cross-runtime instruction listings
and decompilations are in that directory's `*.worldspace.txt` and JSON files.
The private runtime probe uses the exact release DLL entry from its local linker
map and is excluded from the release archive. The reporter's full 1,795-plugin
environment has not been reproduced. No issue comment or binary release was
published as part of this investigation.

## Runtime validation and limits

The tested production DLL is version **2.1.1.0**, SHA-256
`477d056983eb3a4a7ffed59bac82810579d6a7119099aebd685544d5c1a55be5`.
All four CTest checks passed, including the worldspace ownership tests, release
input checks, and archive validation. The distribution contains 83 files: the
production DLL, two MCM files, and 80 DDS backgrounds. Probe and cursor-helper
DLLs are not distributed.

OG 1.10.163 was exercised on a separate Windows desktop without switching the
input desktop. The host checked that its game/child windows never appeared on
the input desktop. The probe also checked its own thread desktop before issuing
commands. Existing saves were loaded; the probe issued no save commands.

`private-04` used the installed native plugin set and the older official-DLC-only
save `Save76_00000000__Commonwealth_000018_20260428140007_2_2`:

| Check | Observed result |
| --- | --- |
| Native gate prediction enabled | `bPreloadExteriorGates=1`; nearby Diamond City doors `000C447C` and `0012F822` queued through the normal gate path. |
| Cold Far Harbor `(14,2)` | Before submission, graph and both maps were null and marker `030180DE` had no loaded parent cell. The exact release `Submit` created native state and queued the cell. |
| Reported marker registration | Cell `03000BD8` reached state 4; marker `030180DE` was present in both native multibound maps. The player remained in Commonwealth. |
| Real world transition | Traveling to Far Harbor called the scene handoff hook. The player reached world `03000B0F`, cell `03000BD8`, with cell state 8 and both registrations intact. |
| Return travel and repeat | Returning to Commonwealth succeeded. A repeat request reused the same graph/maps and registered the marker again. The graph held two references, without accumulating additional plugin references. |
| Save replacement | Reloading the original save succeeded; a subsequent Far Harbor preload again completed both map registrations. |

`private-10` repeated cold preloading with the installed native plugin set,
returned to the main menu through `Main::PerformGameReset` with `fullReset=false`,
and loaded the save again. The reset barrier completed, the graph/maps retained
their identities and two graph references, and a new Far Harbor request again
registered the marker in both maps. All menu-reset sequence checks passed and
the probe issued `qqq` to finish the private game.

Two early automation attempts overwrote an unacknowledged command or allowed too
little time for a loading screen. The corrected driver waited for command
acknowledgement and the completed world transition; these driver timeouts are
not reported as engine failures or successful tests.

Native form teardown was observed separately in `private-07` and `private-08`:
the reset barrier ran before loader draining, and native `ClearData` retired both
the Diamond City and Far Harbor leases. However, **a complete forced full reset
is not a passing test**. The test requested `Main::fullReset/resetGame` directly;
the game later crashed during/after world cleanup. The minimal-plugin control
`private-09` also crashed after a forced full reset with **LoadingScreens.dll
entirely absent**, confirmed by its F4SE plugin log. This control establishes a
limitation of this reset test/environment; it does not prove full-reset stability
or identify every cause of those crashes.

The installed F4SE Menu Framework introduced another reset failure before
worldspace teardown. `private-06/debugger.log` captured a stack overflow in
`Hooks::DevicePollHook::keyboardThunk`, whose `originalKeyboardPoll` pointed
back to `keyboardThunk` after a repeated game-data notification. It was
temporarily omitted for the teardown/control runs. No changes to that plugin
are part of this fix.

Runtime logs and control-run records are retained locally under
`test-evidence/issue-2-20260926/`; machine-local logs, dumps, save metadata, and
raw decompilations are not included in this source repository.
VR and both AE builds have independent static
layout, callsite, destructor, and complete function-byte verification from
Combined, but were **not runtime tested** in this session.


The local game's base MCM directory was absent during the initial runs, so those
runs used the DLL's compiled full-mode defaults (including exterior preloading).
The two packaged MCM files were then added without modifying the existing user
INI. `private-11` confirmed successful base/user configuration loading,
`exteriorGate=true`, and another cold Far Harbor preload with both marker-map
registrations. The installed DLL remained byte-identical to the tested release.

After the final private game exited, both task-created helper DLLs were removed,
all temporarily omitted plugins were restored with matching hashes, and the
original Fallout4Custom.ini was restored byte for byte. The original disabled
2.1 DLL remains intact. `final-audit.json` records these checks and package hashes.
