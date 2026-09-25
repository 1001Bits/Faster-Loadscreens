# Fallout 4 preload benchmark

This test is for Fallout 4 OG 1.10.163. Every measured variant must start in a
new game process; speculative cells and the OS file cache otherwise carry state
between variants.

## What carries over from the Skyrim investigation

- Reuse the event vocabulary and destination/ring-style logging, not Skyrim's
  raw offsets. Fallout tracks teleport references in
  `BGSLoadedReferenceCollection` and has different manager/TES layouts.
- Skyrim's exterior one-cell proximity experiment was custom plugin behavior;
  Skyrim's native switch covers linked interiors. Fallout 4 already has a
  native linked-area system with both an interior and an exterior branch.
- Skyrim's crosshair path required two 100 ms observations before firing.
  Fallout's current close-range pick fires immediately and then applies an
  eight-second destination cooldown. The diagnostic therefore records the
  trigger source so we can see whether quick aim sweeps create waste before
  deciding whether Fallout also needs a dwell gate.
- The Skyrim probe was reached only after its benchmark-mode early return.
  Fallout's observer is deliberately independent of `iBenchmarkMode`, so a
  timing-only baseline can still be used for a separate diagnostic discovery
  run.

## What the native settings actually do

`bUseMenuLoadCellPreload`

- Default: on.
- Read by `TES::GridArrayLoad` only while the loading menu is open.
- Gates `BGSCombinedCellGeometryDB::PreloadGrid` for the complete
  `uGridsToLoad` square (5x5 at the vanilla value).
- This is an arrival-grid geometry warm-up, not door prediction. It operates
  after Fallout already knows the cell/worldspace being loaded; it does not
  choose or preload a likely future door destination.
- Does **not** gate normal cell loading. Turning it off merely defers/skips the
  combined-geometry warm-up, so menu-close time alone is not enough: also check
  post-load hitches and visible geometry pop-in.
- The March 5-era 1.0 package neither configures this setting nor contains its
  setting name in the DLL. Its faster first save load therefore did not come
  from explicitly disabling this native geometry warm-up.

`bPreloadLinkedAreas`

- Default: off. Vanilla `fTeleportPreloadDistance` is 4096 units.
- Each player update enumerates already-loaded references with `ExtraTeleport`,
  rejects disabled doors and destinations beyond the radius, then sorts the
  remaining doors nearest first.
- Static analysis predicts an interior working set of at most
  `uInterior Cell Buffer` cells (vanilla 3), with nearer candidates selected
  first and more distant selections cancelled/evicted. Verify this from
  `selectionLimit` and ring diagnostics rather than treating it as a timing
  result.
- From an exterior, the native linked scan can select nearby load doors leading
  to interiors. From an actual interior, its exterior branch can select only
  the nearest qualifying exterior destination and request the full
  `uGridsToLoad` arrival grid (normally 25 cells).
- It does not perform exterior-to-exterior cross-worldspace prediction. Diamond
  City is an exterior worldspace, so its gate to the Commonwealth is not covered
  by that native interior-to-exterior branch.

The native buffer sub-matrix tests two specific hypotheses:

- Outside, buffer 1 should retain only the nearest qualifying interior;
  buffer 2 should permit two, while buffer 3 is the vanilla control.
- Indoors, the current interior should consume the sole interior slot at
  buffer 1. The native exterior branch is separate, so it should still be able
  to request its nearest qualifying exterior destination.

These are hypotheses to confirm through candidate, request, buffer-ring, and
later-player-cell records. Neither buffer 1 nor buffer 2 is presumed to be a
timing winner.

The plugin has three separately configured door-detection paths:

- Crosshair/wand pick: event-driven and aim-dependent. It can request the
  targeted interior. A picked exterior destination is permitted to reach the
  engine `TES::PreloadWorld` path only when the separate experimental
  `bPreloadExteriorGates` policy is also on; otherwise diagnostics report
  `exterior-disabled`.
- Exterior-gate proximity: on in the packaged combined profile, and explicitly
  disabled by isolation profiles which are not testing it. It runs only when
  the player's origin cell is exterior, scans only already-loaded door
  references near the player, selects exterior destinations in a different
  worldspace, and needs no Havok ray. This is the path intended to cover both
  directions across the Diamond City/Commonwealth exterior-worldspace boundary.
- Extended range: optional camera/movement Havok rays. It must remain at
  `iExtendedRange=0` throughout this matrix. Exterior destinations found by a
  ray are subject to the same exterior-policy switch.

Test crosshair and gate proximity with native linked-area preload off, and test
them in separate profiles. `iExteriorGateDistanceCells` is a zero-based
stepper: 0 means one cell (4096 units), through 3 meaning four cells.
The legacy-named `iExteriorGridRadius` now selects one of two engine behaviors:
0 calls `TES::PreloadWorld` with `queueOnly=true` for the destination center
cell; 1 calls it with `queueOnly=false`, allowing Fallout's native
`uGridsToLoad` arrival square (normally 5x5). There is no plugin-built 3x3 or
5x5 loop and no direct `TESWorldSpace::cellMap`/`QueueCellLoad` access.

Interior-to-exterior exits are deliberately **not** gate-proximity cases. Test
those with `bPreloadLinkedAreas=1`; they are the native linked-area exterior
branch. The two `gate-*` profiles isolate exterior-to-exterior behavior,
especially the Diamond City/Commonwealth boundary.

## Fixed route

Use `Exitsave0_..._Vault111Cryo...fos`, then perform the same actions and aim
behavior each time:

| Load | Action |
|---:|---|
| 1 | Load the save from the main menu |
| 2 | Take the Vault 111 elevator to the surface |
| 3 | Elevator's chained/reopened load, if emitted |
| 4 | Fast travel to The Fens |
| 5 | Enter Diamond City |
| 6 | Enter All Faiths Church |
| 7 | Exit All Faiths Church |
| 8 | Enter Publick Occurrences |
| 9 | Exit Publick Occurrences |
| 10 | Exit Diamond City |

For crosshair runs, aim at each interior door for the same dwell time. For
native-only and gate-only runs, do not deliberately aim at doors. Walk the same
line and pause in the same places so gate-distance lead time is comparable.

### Focused two-way Diamond City gate check

Use `diagnostic-gate-buffer3` and do not aim at either door. This pass checks
both exterior-to-exterior directions:

1. Start outside in the Commonwealth and approach door `0012F822`. It must
   resolve linked door `000A8F9C` and request Diamond City world `00000F94`,
   arrival grid `(-1,-1)`.
2. Enter Diamond City, then wait until the post-transition quiet period has
   been clear for more than five seconds.
3. Approach the inside door `000A8F9C`. It must resolve linked door `0012F822`
   and request Commonwealth world `0000003C`, arrival grid `(-4,-8)`.

For each direction, expect
`event=exterior-preload-decision decision=engine-call-returned` with a matching
result 16 (`fired-exterior-engine`). The first repeated nearby observation emits
one `decision=recent-plugin-request`; its crosshair-candidate outcome is result
9 (`cooldown`). Crosshair-candidate records may repeat at the poll cadence, but
the dedicated duplicate-decision record is emitted only once per cooldown.

The cooldown key is the exact destination world + grid + engine mode and lasts
eight seconds. Entry and exit therefore have distinct keys. This prevents a
duplicate request for the same arrival target without confusing the reverse
route with it. Fallout's `TES::PreloadWorld` remains responsible for checking
already resident, buffered, and pending cells; an invocation is a safe
engine-owned check, not proof that new cell I/O occurred.

## Diagnostic pass, then timing pass

First run each row once with `bPreloadDiagnostics=1`. These are discovery runs:
use them to identify which doors/cells were requested, which engine-call
paths fired, whether the destination was later visited, and whether any physics
anomaly appeared. The log distinguishes `native-menu`, `native-linked`,
and `plugin-door-prefetch` (including the engine interior-call context, door
candidates, and exterior `PreloadWorld` calls). Candidate records also identify
their exact trigger, such as `crosshair-pick`, `vr-pick`, or `gate-proximity`.
On exact AE 1.11.221 or 1.11.240, gate enumeration and gate-decision diagnostics are
available for this focused check even though the seven native observer hooks
remain OG/VR-only.

Then repeat the same matrix with `bPreloadDiagnostics=0` for the actual timing
comparison. The normal `LoadingMenu CLOSE` / `LOADTIME` timers remain active,
but the seven mutation-free diagnostic observers and per-door diagnostic
formatting are absent. Use black display mode for plugin rows so presentation
cost does not obscure the preload comparison.

| Variant | Benchmark | Menu geometry | Native linked | Interior buffer | Crosshair | Exterior gate | Gate distance | Exterior engine mode | Extended ray |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `baseline` | timing only | on/default | off | 3 | off | off | 1 cell | native grid (dormant) | off |
| `baseline-buffout` | timing only + crash capture | on/default | off | 3 | off | off | 1 cell | native grid (dormant) | off |
| `menu-off` | timing only | off | off | 3 | off | off | 1 cell | native grid (dormant) | off |
| `native-1cell-buffer1` | timing only | on | on, 2048 | 1 | off | off | 1 cell | native grid (dormant) | off |
| `native-1cell-buffer2` | timing only | on | on, 2048 | 2 | off | off | 1 cell | native grid (dormant) | off |
| `native-1cell` | timing only | on | on, 2048 | 3 | off | off | 1 cell | native grid (dormant) | off |
| `core` | full | on | off | 3 | off | off | 1 cell | native grid (dormant) | off |
| `crosshair` | full | on | off | 3 | on | off | 1 cell | native grid (dormant) | off |
| `plugin-native-1cell` | full | on | on, 2048 | 3 | off | off | 1 cell | native grid (dormant) | off |
| `gate-1cell` | full | on | off | 3 | off | on | 1 cell | engine single cell | off |
| `gate-1cell-buffout` | full + crash capture | on | off | 3 | off | on | 1 cell | engine single cell | off |
| `gate-native-grid` | full | on | off | 3 | off | on | 1 cell | engine native grid | off |
| `candidate-minimal` | full | on | on, 2048 | 1 | off | on | 1 cell | engine single cell | off |
| `b-all-preload-buffer3` | full | on | on, 2048 | 3 | on | on | 1 cell | engine native grid | off |
| `diagnostic-gate-buffer3` | full + diagnostics | on | on, 2048 | 3 | off | on | 1 cell | engine single cell | off |
| `combined-speed-buffer3` | full | on | on, 2048 | 3 | off | on | 1 cell | engine single cell | off |
| `hfpf-all-speed` | timing only + HFPF | on | off | 3 | off | off | 1 cell | native grid (dormant) | off |

The gate distance is held at one cell so the two gate profiles isolate only
the engine's single-cell versus native-grid choice. Although the table shows
dormant distance/mode values for rows with gate preload disabled, each such
isolation profile explicitly writes `bPreloadExteriorGates=0`; every profile
writes `iExtendedRange=0`.

Run `candidate-minimal` only after the native-buffer and gate-mode rows have
been evaluated independently. It combines native one-cell/buffer-1 selection
with the gate single-cell experiment, so its diagnostic attribution is
intentionally mixed; it is a post-isolation confirmation candidate, not
evidence about which component caused a timing change.

`b-all-preload-buffer3` is the explicit integrated B comparison against
`baseline`: full loading-screen/3D-model speed patches, native menu and linked
preloading, buffer 3, crosshair interior preloading, and cross-worldspace gate
preloading through the engine's native grid mode. The extended Havok ray stays
off because it only increases detection range; it is not another engine preload
primitive and would add a separate experimental variable.

`diagnostic-gate-buffer3` is the focused evidence run for the combined DLL:
loading-screen speed patches, native menu and linked preload, buffer 3, and
cross-worldspace gates through the engine's single-arrival-cell path are on.
The Combined Ghidra project shows that `TES::PreloadWorld(queueOnly=false)`
only enters its full-grid branch when the engine has an interior-cell context;
Diamond City to Commonwealth is exterior-to-exterior, while
`queueOnly=true` directly queues the XTEL arrival cell. Crosshair and
extended-ray detection are off, HFPF is removed, and the harness rejects this
profile if `-Diagnostics:$false` is supplied. All profiles enforce the default
fade values and `iPresentInterval=1` in both Fallout INIs.

`combined-speed-buffer3` is the diagnostics-off timing twin of that evidence
profile. It keeps the same engine-native menu, linked-area, buffer-3, and
single-arrival-cell gate policy, but removes diagnostic hook/logging overhead.
Use it for the combined-DLL comparison against the 20.04-second HFPF route.

`hfpf-all-speed` leaves Faster Loadscreens in timing-only mode and activates
only the hash-backed High FPS Physics Fix DLL alongside it. HFPF's loading
VSync, black-screen, animation, post-loading-menu, and CPU-thread speed options
are enabled. Its new-game-only one-thread workaround and HFPF OSD are disabled
because neither is a loading speedup and both would contaminate measurement.

Run at least three repeats per variant in balanced order (for example ABBA,
then BAAB). Do not compare a first cold run only against a later warm run.

`PreloadDiag event=menu-geometry-grid` proves the native menu warm-up path was
invoked with the logged grid.
`event=native-linked-candidate` shows the native interior selection helper;
the following `event=preload-interior source=native-linked` proves a preload
function call and its arguments, not completion, only when both records share
the same `nativeAttemptId` **and** the call's cell FormID equals the candidate
destination FormID. The analyzer leaves same-attempt/different-destination
pairs unconfirmed and reports `destination-mismatch`. The outer
`event=native-linked-world-candidate` is observation-only and its candidate
payload is deliberately undecoded: treat it as provisional unless a nested
`event=preload-world source=native-linked` has the same `nativeAttemptId`.
Attempt ID alone is used for this world correlation precisely because there is
no decoded outer destination to compare.
Origin and `direction` fields then distinguish exterior-to-interior from
interior-to-exterior behavior without borrowing a destination from an unrelated
candidate. The matching `event=preload-world source=native-linked` proves only
the world-preload function invocation and arguments. The analyzer also reports
same-world, cross-world, and unknown relations by comparing the call's
destination world to `originWorld`. Treat that relation as empirical context,
not authoritative door/source decoding: `originWorld` is a current-player
snapshot taken when the outer visitor ran.

For each confirmed world call, the analyzer separately tests whether a later
`player-cell` sample has the same worldspace and lies within the requested grid.
`fullGrid=true` uses the logged `gridSide`: on each axis the lower bound is
`center - floor(gridSide/2)` and the inclusive upper bound is
`lower + gridSide - 1`. `fullGrid=false`/`queue-single-cell` matches only the
exact center coordinate. Calls are reported separately as evaluable hits,
waste within the recorded window, or indeterminate when request geometry or a
later comparable world/grid sample is missing. This is visit correlation only;
it neither proves preload completion nor rules out visiting the grid after the
recording stops.

A post-call
`event=interior-buffer-add`
proves the add function returned and records the post-call ring snapshot. It
does not by itself prove that the requested cell was inserted, nor does its
state/`loadedData` context prove full 3D/Havok attachment.
`event=interior-buffer-remove` exposes possible eviction/churn.
The requested-cell buffer summary counts add/remove records only when they occur
after that FormID's first native candidate; earlier ring activity is not
attributed to the request. `event=player-cell` is only evidence that the player
visited that cell later.
`event=crosshair-candidate` reports the plugin result code (3 means an interior
request call fired; current exterior engine calls report
`resultName=fired-exterior-engine`; old logs may use
`exterior-submitted` or numeric result 10 `fired-exterior`), its stable
`resultName`, its
`source=plugin-door-prefetch`, and its trigger.
Safety outcomes such as `transition-active`, `player-context-unavailable`,
`interior-origin-unsupported` (use the native linked test),
`source-worldspace-missing`, and `post-transition-quiet`
explain why the plugin declined an exterior attempt; `exterior-disabled`
confirms that the experimental exterior policy was off. Old raw-grid logs may
also contain `exterior-cell-layout-unavailable` or `no-cold-work`; those
outcomes are unreachable on the current engine-only exterior path.

An exterior attempt invokes the same engine function observed for the native
exterior branch and emits
`event=preload-world source=plugin-door-prefetch`. The record contains the
destination worldspace, arrival center, `boolFlag`, branch, `fullGrid`,
`gridSide`, and `gridCells`. `queue-single-cell`/`boolFlag=true` is the engine's
center-cell mode; `full-arrival-grid`/`boolFlag=false` delegates the complete
arrival square to Fallout. The event proves that `TES::PreloadWorld` was
invoked with those arguments. The later `fired-exterior-engine` candidate
outcome is emitted after that call returns. Neither record proves that the
engine accepted, loaded, attached, or completed a cell.

The analyzer correlates every plugin `preload-world` invocation with later
`player-cell` world/grid samples using the same square calculation as the
native report. This tells us whether the requested destination region was
later visited, not what finished streaming. Older logs containing
`plugin-exterior-grid`, `queued`, or `residentSkipped` remain readable in a
separate legacy section, but those raw `QueueCellLoad` attempts are not current
behavior and are never mixed into the engine-call count.

## Controls

- Disable every other F4SE DLL for both sides, especially Fast Saving Fallout.
  HFPF is also disabled except in the explicit `hfpf-all-speed` comparison.
  `gate-1cell-buffout` is the crash-capture exception: it permits only the
  hash-backed `Buffout4.dll` alongside `LoadingScreens.dll` and records both.
  The harness writes a reversible crash-capture-only Buffout TOML with its
  configurable fixes, allocator/performance patches, and positive-value
  Papyrus detour disabled.
- Disable non-vanilla ESP/ESL plugins and loose-file overrides for both sides.
- Keep `uGridsToLoad=5`, graphics settings, save, route, door dwell, and
  autosave policy identical.
- Fully exit Fallout 4 between repeats.
- Archive `LoadingScreens.log` immediately; only one previous session is
  retained automatically.

Loading-menu open-to-close time can improve merely because work moved into
gameplay before the menu opened. A winning formula must therefore satisfy all
three:

1. Lower or unchanged route load total.
2. No increase in post-load hitches or physics anomalies.
3. A high ratio of speculatively requested destinations that were actually
   entered.

## Harness and report commands

Run these from the plugin workspace. `Stage` is reversible: the first call
hash-backs every affected file, later variants reuse that snapshot, and
`Restore` puts the exact originals back.

```powershell
.\tools\Fallout4PreloadBenchmark.ps1 SelfTest

.\tools\Fallout4PreloadBenchmark.ps1 Stage gate-1cell-buffout -Diagnostics:$false -LoadingScreenMode 3

.\tools\Fallout4PreloadBenchmark.ps1 Stage baseline-buffout -Diagnostics:$false -LoadingScreenMode 0

.\tools\Fallout4PreloadBenchmark.ps1 Stage baseline -Diagnostics:$false
# Run the fixed route, fully exit Fallout 4, then:
.\tools\Fallout4PreloadBenchmark.ps1 Archive baseline-run1

# Discovery example:
.\tools\Fallout4PreloadBenchmark.ps1 Stage native-1cell-buffer1 -Diagnostics:$true

# Isolated cross-worldspace gate/native-grid discovery (no crosshair or ray):
.\tools\Fallout4PreloadBenchmark.ps1 Stage gate-native-grid -Diagnostics:$true

# Combined diagnostic evidence run requested after the HFPF comparison:
.\tools\Fallout4PreloadBenchmark.ps1 Stage diagnostic-gate-buffer3 -Diagnostics:$true

# Only after both isolation matrices:
.\tools\Fallout4PreloadBenchmark.ps1 Stage candidate-minimal -Diagnostics:$true

# When the matrix is finished:
.\tools\Fallout4PreloadBenchmark.ps1 Restore
```

Summarize one log or a complete archived run directory:

```powershell
python .\tools\analyze_preload_log.py <path-to-log-or-run-directory>
```
