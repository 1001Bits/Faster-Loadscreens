# Fallout 4 preload diagnostic run — 2026-07-29

## Run identity

- Runtime: Fallout 4 flat 1.10.163
- Plugin: full mode (`iBenchmarkMode=1`), diagnostics enabled
- High FPS Physics Fix: not detected
- Loading-screen presentation: black
- Native menu-load preload: on
- Native linked-area preload: on
- Native teleport radius: 4096 units (one vanilla cell in this run)
- Interior cell buffer: 3
- Exterior arrival grid: `uGridsToLoad=5` (5x5)
- Crosshair preload: off
- Exterior-gate proximity: on, 4096 units, single arrival cell
- Extended Havok ray: off
- Fades: Bethesda defaults

The raw log is:

`C:\Users\Noud\Documents\My Games\Fallout4\F4SE\LoadingScreens.log`

It is 57,110,083 bytes because the original native visitor diagnostics emitted
more than 104,000 per-frame records. That verbose candidate logging is now
disabled; confirmed preload calls remain logged.

## Route and LoadingMenu timings

| Load | Observed destination | Time |
|---:|---|---:|
| 1 | Save load -> DiamondCityExt, Commonwealth (-4,-8) | 29.45 s |
| 2 | DiamondCityEntrance, Diamond City (-1,-1) | 5.46 s |
| 3 | Chapel | 0.74 s |
| 4 | Upper Stands, Diamond City (0,-1) | 1.23 s |
| 5 | Publick Occurrences | 0.82 s |
| 6 | Diamond City origin area (0,0) | 1.28 s |
| 7 | DiamondCityExt, Commonwealth (-4,-8) | 7.81 s |

Total LoadingMenu time was 46.79 seconds. Because diagnostics produced a
57 MB synchronous log, this is discovery evidence rather than a clean
performance result.

For load 1, F4SE `kPreLoadGame` arrived at 05:19:10.805 and LoadingMenu opened
at 05:19:10.816. `kPostLoadGame` arrived at 05:19:36.302, so the engine's
synchronous save-load phase alone took about 25.49 seconds. The menu closed at
05:19:40.270 for the 29.45-second total. The later native linked-area calls did
not cause this long save load.

## What the native engine preloaded

The interior path made 15 calls associated with six unique named linked
interiors and completed 13 buffer insertions:

| Interior | Calls / completed insertions | Visited next? |
|---|---:|---|
| Kellogg's House | 2 | No |
| Hawthorne residence | 2 | No |
| Chapel | 3 / 2 | Yes |
| Publick Occurrences | 3 / 2 | Yes |
| Choice Chops | 3 | No |
| John's House | 2 | No |

There were also three engine-other calls for the Publick elevator transition.
Only Chapel and Publick matched the route. At the vanilla 4096-unit search
radius, 31 unique nearby interior destinations were observed, explaining the
unhelpful churn in dense Diamond City. Fallout applies its one distance filter
before resolving destination type, so there is no stock direction-specific
radius.

Native menu-load geometry requested:

- Load 1: Commonwealth 5x5 twice, centered on (-4,-8)
- Load 2: Diamond City 5x5 containing (-1,-1)
- Load 7: Commonwealth 5x5 centered on (-4,-8)

## Why there were 253 `PreloadWorld` calls

`PlayerCharacter::Update` invokes the native linked-area preloader once per
player update with no cooldown. Two episodes contained exactly 253 calls:

- Chapel: 05:20:04.987-05:20:10.280 (5.293 s, about 47.61 calls/s), targeting
  Diamond City (0,-1), which matched the later Upper Stands visit.
- Publick: 05:20:19.964-05:20:25.244 (5.280 s, about 47.73 calls/s), targeting
  Diamond City (0,0), which matched the later origin-area visit.

Every logged request used `queueOnly=false`, so its nominal request was the
full 5x5 grid. This does **not** mean 253 x 25 unique cell-I/O jobs:
`TES::PreloadWorld` checks whether the same grid is already resident, and the
cell loader checks loaded/buffered state and deduplicates pending work under its
lock. Most repeats were checks. The first 25-cell burst can still hitch; the
subsequent identical calls are largely deduplicated.

## Why the Diamond City exit gate did not preload

The relevant pair was:

- Outside door: `0012F822`
- Inside door: `000A8F9C`

Thirteen gate-enumerator scans received exactly one callback each and accepted
zero gates. Two source blockers caused this:

1. The plugin interpreted the enumerator visitor result backwards. In Fallout,
   `false` means continue and `true` means stop, so the scan stopped after its
   first reference.
2. Persistent linked exterior doors can have no live parent cell. The plugin
   used only `GetParentCell()` and missed Fallout's `GetSaveParentCell()`
   persistent-cell fallback.

Both blockers are fixed in the current source. Gate coordinates now come from
the door's XTEL destination and loading still goes through the engine-owned
`TES::PreloadWorld` path.

## Current test policy after this run

- Keep `uInterior Cell Buffer=3`.
- Keep native linked-area preload enabled.
- Reduce the shared teleport radius to 2048 units (half a cell) to cut dense
  exterior-to-interior churn.
- On OG 1.10.163 and VR 1.2.72, keep Fallout's native linked-door selection but
  switch its exterior destination request to the engine's single-arrival-cell
  mode. Exact AE 1.11.221 now does the same with an exact-return-filtered
  `TES::PreloadWorld` hook; unrelated callers retain their original mode.
- Keep exterior-gate proximity on at one cell and single-arrival-cell mode.
- Keep crosshair preload and the extended Havok ray off pending physics tests.
- Leave native menu-load preload at the engine/user value (Bethesda default is
  on).
- Keep High FPS Physics Fix and per-load single-core affinity off.
- Keep Bethesda fade values.

These source changes compile and pass release-input validation, but their
runtime behavior has not yet been re-measured because the game was deliberately
not relaunched.

## Next run: two-way Diamond City gate proof

Use the diagnostic gate profile and do not aim at either gate:

1. Outside Diamond City, approach Commonwealth door `0012F822`. It must resolve
   linked door `000A8F9C` and request Diamond City world `00000F94`, grid
   `(-1,-1)`.
2. Enter Diamond City and wait for more than five seconds of post-transition
   quiet.
3. Approach inside door `000A8F9C`. It must resolve linked door `0012F822` and
   request Commonwealth world `0000003C`, grid `(-4,-8)`.

Each direction should first log
`event=exterior-preload-decision decision=engine-call-returned` with a matching
result 16 (`fired-exterior-engine`). The first repeated nearby observation
emits one `decision=recent-plugin-request`; its crosshair-candidate outcome is
result 9 (`cooldown`). Crosshair-candidate records may repeat at the poll
cadence, but the dedicated duplicate-decision record is emitted once per
cooldown.

The eight-second cooldown is keyed by exact destination world + grid + engine
mode, so the entry and exit targets are distinct. Fallout's own
`TES::PreloadWorld` still owns resident/buffered/pending deduplication. A call
therefore safely asks the engine to ensure the arrival target is available; it
does not prove that new I/O was scheduled.

Gate enumeration and decision diagnostics are available on exact AE 1.11.221 or 1.11.240
for this test. The seven native observer hooks remain OG/VR-only, so their
absence in an AE log must not be mistaken for a failure of the gate path. The
exact AE runtime also uses the exact-return-filtered single-arrival-cell policy
for the native linked exterior call.
