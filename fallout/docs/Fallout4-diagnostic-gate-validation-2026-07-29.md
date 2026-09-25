# Fallout 4 Diamond City two-way gate validation — 2026-07-29

## Run identity

- Runtime: Fallout 4 flat 1.10.163
- Variant: `diagnostic-gate-buffer3`
- Combined plugin: full mode, diagnostics enabled
- Loading-screen mode: black
- Present interval: 1
- Display mode: borderless windowed
- High FPS Physics Fix: disabled
- Native menu preload: enabled
- Native linked-area preload: enabled at 2048 units
- Interior cell buffer: 3
- Native exterior arrival mode: engine single cell
- Exterior gate proximity: enabled at 4096 units
- Crosshair preload: disabled
- Extended Havok ray: disabled
- Fades: Bethesda defaults

The immutable archive for this run is:

`benchmark-state\20260728-210930-304\runs\20260729-024202-619-diagnostic-diamond-city-bidirectional-run1`

Its staged DLL SHA-256 is
`1C44E94CFB5CE6E8ABC7704F0F7418A201AA98557F495E5B776BE32721A50A0B`.

## Route and timings

| Load | Destination | Time |
|---:|---|---:|
| 1 | Save -> DiamondCityExt, Commonwealth `(-4,-8)` | 13.70 s |
| 2 | DiamondCityEntrance, Diamond City `(-1,-1)` | 2.88 s |
| 3 | All Faiths Chapel | 0.72 s |
| 4 | Diamond City Upper Stands `(0,-1)` | 1.44 s |
| 5 | Publick Occurrences | 0.59 s |
| 6 | Diamond City origin area `(0,0)` | 1.16 s |
| 7 | DiamondCityExt, Commonwealth `(-4,-8)` | 4.88 s |

The manager-duration total was 25.37 seconds. The six door transitions after
the save load totalled 11.67 seconds.

The save's synchronous `kPreLoadGame` to `kPostLoadGame` phase was 11.942
seconds. The loading menu then remained open for another 1.768 seconds.

This is a diagnostic result, not a clean timing winner. It ran 7 native
observer hooks, formatted more than 1,000 diagnostic events synchronously, and
used an already-warmed save/OS cache.

## Diamond City entry: confirmed

At 09:38:55.231 the exterior-gate path called Fallout's
`TES::PreloadWorld(queueOnly=true)` for:

- source door `0012F822`
- linked door `000A8F9C`
- destination world `00000F94` (`DiamondCity`)
- arrival grid `(-1,-1)`

The destination was initially unresolved/not loaded. At 09:38:55.364, 133 ms
later, diagnostics resolved `00000FC9` (`DiamondCityEntrance`) in the same grid
as state 4 with non-null loaded data. The player used that gate about 6.86
seconds after the request and arrived in that exact cell/grid.

This proves the main entry-gate prediction selected the route actually taken.

The 4096-unit gate radius also found the Diamond City elevator pair
`000C447C -> 000C4486` and requested grid `(0,-1)`. That second grid was not the
entry route taken, so a nominal one-cell proximity radius can still issue more
than one one-cell request when multiple cross-world gates are nearby.

## Diamond City exit: target confirmed, cold benefit not proven

At 09:39:10.110 the inside gate path called
`TES::PreloadWorld(queueOnly=true)` for:

- source door `000A8F9C`
- linked door `0012F822`
- destination world `0000003C` (`Commonwealth`)
- arrival grid `(-4,-8)`

The player later used this exact gate and arrived in `0000E50C`
(`DiamondCityExt`) at the requested grid.

The same request fired again at 09:39:31.775 and 09:39:46.532 because the
eight-second plugin cooldown expired. The final request was about 7.94 seconds
before the exit transition.

This run began in `DiamondCityExt`, and the real destination cell remained
resident. The persistent linked door nevertheless resolved through save-parent
cell `00018AA2` (`Wilderness`), whose state was not loaded. The plugin therefore
could not recognize the already-resident arrival grid and issued three
engine-owned checks. Fallout safely deduplicates loaded/buffered/pending work,
but the plugin's durable resident/previous-request suppression is incomplete.
The 4.88-second exit therefore cannot be attributed solely to speculative
preloading.

## Native linked-area behavior

The 2048-unit native radius requested five unique interiors:

| Destination | Calls | Visited? |
|---|---:|---|
| Publick Occurrences | 31 | Yes |
| Kellogg's House | 2 | No |
| John's House | 2 | No |
| All Faiths Chapel | 2 | Yes |
| Choice Chops | 3 | No |

Only two of the five unique interiors were used. Publick Occurrences received
30 repeated calls in one 0.598-second burst. The three-cell engine buffer
recorded 16 add returns and 12 remove returns, confirming real buffer churn even
at the reduced half-cell radius.

The native interior-to-exterior branch selected the correct return grids:

- Chapel -> Diamond City `(0,-1)`, later visited as Upper Stands
- Publick Occurrences -> Diamond City `(0,0)`, later visited as the origin area

However, it called `TES::PreloadWorld` once per player update:

- Chapel: 281 calls over 5.890 seconds, about 47.54 calls/s
- Publick Occurrences: 269 calls over 5.603 seconds, about 47.83 calls/s

All 550 requests used Fallout's one-arrival-cell mode, so the former 25-cell
first burst is fixed. The engine still performs resident/pending checks on
every repeated call. A native-callsite destination cooldown is required before
this can be considered stutter-safe.

## Other findings

- Native menu geometry preload remained enabled. It emitted four 5x5 geometry
  requests: two during the save load, one entering Diamond City, and one
  returning to the Commonwealth.
- Gate proximity emitted 412 candidate records: 5 actual engine calls and 407
  cooldown outcomes. Crosshair preload was off; `crosshair-candidate` is only
  the legacy shared diagnostic event name.
- Save-popup cache warming did not run. The selected save was armed with
  `popupOpen=false` only 15 ms before `kPreLoadGame`, with no popup-open or
  completed cache-warm event.
- Native `System32\dxgi.dll` was identified correctly. Startup still selected
  the inline fallback because orphaned ENB marker files were present despite
  no game-local `d3d11.dll`; ENB detection needs to require a real loaded/local
  proxy.
- No HFPF DLL, plugin error, explicit timeout, or diagnostic proof of a
  post-load stutter was present.

## Verdict

The two-way Diamond City gate resolver now works and selected the exact
world/grid used in both directions. It is not ready as the final formula yet:

1. throttle/deduplicate the native linked exterior call at its exact engine
   callsite;
2. retain or invalidate exterior world/grid request state based on transitions
   instead of expiring every eight seconds;
3. avoid the extra nearby elevator request when the main gate is the nearest
   candidate;
4. fix the save confirmation-popup observation so warming starts before
   `kPreLoadGame`;
5. require an actual ENB/proxy module before selecting the inline Present path.

After those fixes, rerun the same route first with diagnostics, then at least
three clean timing repetitions with diagnostics off.

## Post-run source fixes

The following fixes were implemented after archiving this run. They have passed
the combined Release build and automated tests, but have not yet been staged or
game-tested:

1. The exact OG/VR native-linked `TES::PreloadWorld` callsite now has a bounded
   32-entry destination throttle. Identical single-cell requests are forwarded
   at most once per second. Fallout still owns every forwarded request and its
   resident/pending checks. AE 1.11.221 applies the same policy only at its
   verified native-linked return address. Baseline mode installs neither the
   callsite trampoline nor the single-cell byte patch.
2. Exterior-gate request state now accepts live linked-cell residency from any
   nearby door sharing the exact destination world/grid. A void-return request
   can receive at most one retry, after at least two seconds and only when the
   player is within 768 units or is materially closer. State expires after 120
   seconds. This replaces the unconditional eight-second resubmission cycle.
3. Active ENB detection now requires a loaded non-System32 `d3d11.dll`.
   Orphaned INI/compiler markers alone no longer force the shared inline Present
   hook.
4. The 15 ms save path in this run was confirmed to be a direct load with no
   confirmation popup, not a missed warming opportunity. MessageBox open/close
   and pre-load drain instrumentation was added so the next deliberate popup and
   cancel test can prove the cache warmer's complete state sequence.

The remaining runtime validation is therefore:

- deliberately open and cancel a save confirmation popup;
- repeat the two-way Diamond City route with diagnostics to verify the call
  reductions and resident suppression;
- then run at least three clean timing repetitions with diagnostics disabled.

Final source validation:

- combined Release DLL built successfully;
- release/package tests passed 2/2;
- preload-log analyzer self-test passed;
- diagnostic benchmark-harness self-test passed;
- DLL SHA-256:
  `0E16AD717713335728D4F386CED5DEA9B7EEE40458D35D95C955F9B44442EEA9`.

The DLL was not staged into the game and Fallout was not relaunched.
