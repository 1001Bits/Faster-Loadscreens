# Changelog

## 2.1.1

Fixed missing destination-world initialization in exterior gate preloading
(GitHub issue #2). The engine now creates the destination's portal graph,
multibound maps, and nodes before queueing its arrival cell. Scene attachment
occurs at the real world transition. An additional graph reference keeps this
state alive until native worldspace cleanup, including AE's inlined destructor
path. No plugin transition or settings change frees pending engine work.
A native reset barrier stops new submissions before the engine drains its loaders.
`bPreloadExteriorGates=1` remains supported and enabled by default. Unknown or
modified initialization/lifetime contracts reject speculative submissions.

## 2.1

Removed the entire exterior cell/gate preload group from MCM. Gate prediction
remains internally enabled at the safe one-cell default; the menu no longer
exposes either the enable switch or distance selector. Existing user settings
remain compatible and are never deleted during deployment.

## 2.3.25

Restored exterior-gate preloading in the two tip-preserving modes on Fallout 4
1.10.163 and Fallout 4 VR 1.2.72. Each executable now has its own exact,
version-gated PositionPlayerJob contract: the worker prologue, copied exterior
target, ShowLoadingMenu setup, direct call target, return address, menu-shown
flag, and Show/Hide flag writes must all match before the optimization is
enabled. The plugin replaces only that worker-owned Show call and changes its
minimal flag from true to false only for the first non-save, non-deferred
exterior arrival whose exact world/grid has live proof from this plugin's own
PreloadWorld submission. Bethesda still selects and displays the tip and level.

Presentation provenance is now armed from that verified runtime capability
instead of an NG-only platform check, fixing the path which previously could
log an OG/VR submission without recording any usable proof. The callsite hook
uses a coherent lock-free mode snapshot and performs no destination scan;
unsupported executable layouts continue to disable gate preloading only in
tip-preserving modes and fail closed.

## 2.3.24

Restored exterior-gate prediction after the 2.3.20 residency guard incorrectly
treated Fallout's context-sensitive `AllCellsInGridLoaded` helper as an arbitrary
destination-world query. Its fast path reads the current `g_TES` GridArray, so a
fully loaded Diamond City grid could falsely classify Commonwealth `(-4,-8)` as
already resident and skip the only gate preload request. The ambient poller no
longer resolves or calls that helper: it skips only for an exact, non-save-parent
live linked destination cell and otherwise delegates loaded/pending dedup to the
engine-owned single-cell `PreloadWorld(..., queueOnly=true)` path.

The final pre-call residency check now refreshes the door destination instead of
reusing the poll snapshot. Every actual NG submission records the small exact-key
presentation proof independently of tip capture, then immediately re-resolves the
linked destination after return to catch synchronous materialization; the existing
120 ms poll remains the asynchronous fallback. Tip/level correction still requires
that later exact live world/grid proof plus all PositionPlayerJob guards, so UI
presentation neither decides nor adds a destination-world scan to gate preloading.

## 2.3.23

Fixed flat mode 3's verified native-minimal path remaining live for the whole
load. LoadingMenu OPEN disabled the unnecessary tip extractor through a generic
cleanup path that also erased the exact native selection owner, leaving the
background-only publication unable to certify its already-composited black
frame. Minimal OPEN now retires only the capture resources while preserving the
owner/content/serial; a successful opaque-black Present and the configured frame
floor still remain mandatory before AdvanceMovie and the NG animation patch can
freeze. Close, fallback, VR, tip-bearing, and superseded publications retain
their full invalidation behavior.

## 2.3.22

MCM changes now apply outside active loading through a debounced settings-file
watcher and a coalesced game-thread refresh. A dirty-generation check at native
load entry closes the edit-to-load race without waiting at the visual boundary,
so the next load consumes the newly published mode, gate, save-prefetch, and
fade settings while ordinary loads perform only two metadata checks.

The extended Havok-ray and menu-load cell-preload controls are retired from the
schema, defaults, parser, and benchmark-generated user settings. Stale user INI
keys are ignored, the exterior ray is forced off, and Fallout's own
`bUseMenuLoadCellPreload` value is left untouched. Exterior gate preloading
remains available and defaults on. Every MCM help description is now limited to
one sentence, with release validation preventing longer text from returning.

## 2.3.21

Native minimal loading screens now remain visually minimal. In mode 3, an
exact `SendLoadingText` background-only selection renders opaque black without
acquiring or drawing the custom DDS; full tip-bearing selections continue to
show custom art with the captured vanilla tip and level. The same rule uses
flat's swapchain clear and VR's existing blocker-only black presenter, and the
animation loop freezes only after that owned black frame is presented. Explicit
black, native, and background-only user modes keep their configured behavior.

The live 1.10.163 Diamond City test also proved why the OG exit lost its tip and
level: the plugin submitted `TES::PreloadWorld` for Commonwealth grid `(-4,-8)`
at the gate, and the later exit was consequently classified by Bethesda as
minimal (`interior=true`, no art screen, no candidates). OG and VR lack NG's
exact pre-Show residency/provenance/correction contract, so plugin-issued
exterior speculation is now disabled there only in the two tip-preserving modes
(native and background+tips). It remains available in presentation-independent
black/background-only modes and remains enabled on verified NG, where the exact
counterfactual-preservation path exists.

## 2.3.20

Fixed the remaining warm Diamond City exit case proven by the 2.3.19 field
log. The exact NG residency probe reported Commonwealth grid `(-4,-8)` already
loaded, so presentation evidence correctly remained unarmed, but the plugin
still called `TES::PreloadWorld`. The gate had only a persistent save-parent
before that call; 109 ms afterward Fallout exposed a live arrival cell, and
188 ms afterward PositionPlayerJob requested a minimal background-only menu.
Thus the nominally redundant request could still materialize the linked
reference and change Bethesda's loading-screen classification.

On 1.11.221/1.11.240, a true exact arrival-grid residency result now marks the
destination resident and returns without calling `PreloadWorld`. This preserves
the untouched target-without-live-parent path, where PositionPlayerJob requests
the normal tip-bearing screen, and avoids a needless engine mutation for an
already-loaded grid. Cold destinations retain 2.3.19's exact submission plus
live-arrival evidence correction. OG and VR behavior are unchanged.

## 2.3.19

Fixed the field-confirmed Diamond City exit case left by 2.3.18. The plugin
submitted the exact Commonwealth arrival cell (`0000003C`, grid `-4,-8`), but
the first intervening shop/interior load erased that proof. The destination
remained resident, so PositionPlayerJob opened a minimal background-only menu
on the later city exit and Bethesda's non-minimal LoadExterior Show arrived too
late to change the already-open menu.

Exterior presentation evidence is now separate from per-transition cooldown
state. It survives complete interior round trips, including their LoadExterior
return legs, and is consumed on the first exact PositionPlayerJob world/grid
arrival whether Bethesda requests a minimal or full screen. Remaining records
are retained only when their recorded source world matches the exterior just
entered, capped at 64 oldest-first, and cleared on save/new-game replacement,
session loss, install, and shutdown. An exact-arrival tombstone remains until
the synchronized transition drain so a racing PreloadWorld return cannot
re-arm stale evidence. Save replacement clears evidence atomically with that
barrier/drain, and the critical first-Show lookup uses fixed tombstone storage
without allocating or freeing heap memory.

On 1.11.221/1.11.240, evidence is armed only when
`TES::AllCellsInGridLoaded` says the exact XTEL arrival grid was cold
immediately before the plugin's PreloadWorld call. It does not become eligible
until Fallout later exposes a non-persistent, resident linked cell at that
exact world/grid. This mirrors PositionPlayerJob's two real exterior paths:
it queries the live target-parent grid when one exists, falls back to the
copied world/position only when the target reference is absent, and requests a
normal screen without a residency query when a target exists but has no live
parent. Persistent save-parent cells such as Diamond City's `(0,0)` container
are never treated as live arrival proof. A no-op request against a naturally
resident target therefore cannot authorize a full screen. The payload
correction is also limited to the two tip-bearing user modes. Real interiors,
naturally resident destinations, black/background-only modes, OG, and VR
presentation behavior remain unchanged.

## 2.3.18

Fixed the remaining Fallout 4 AE exterior tip/level failure using the exact
first-Show path observed on 1.11.240. PositionPlayerJob asks whether the
destination is already resident and passes that answer as LoadingMenu's
minimal/interior flag. Exterior look-ahead made the answer true, so Bethesda
created a background-only menu before `TES::LoadExterior`. Its later
non-minimal Show arrived 32-61 ms afterward but was inert because LoadingMenu's
`loadScreenShown` latch was already set. That is why the 2.3.17 LoadExterior
predicate could never correct the failing path.

The plugin now corrects only that first PositionPlayerJob payload, before the
native menu exists. Both supported AE executables must match the exact worker
entry, copied target layout, Show call sequence, decoded Show target, and return
address. The caller stack must then prove an exterior target (`world != null`,
`interior == null`), and DoorPrefetch's current cooldown generation must prove
that this plugin actually submitted the same world/grid destination. Only then
is the minimal flag changed from true to false; Bethesda performs its normal
candidate selection and `SendLoadingText` call. The plugin never invents or
proactively consumes a loading tip.

Resetting the already-visible LoadingMenu was deliberately rejected: it would
deallocate and rebuild the live native candidate array and reuse active
Scaleform state. Save loads, naturally resident minimal screens, real
interiors, deferred calls, passive measurement mode, OG, and VR remain
unchanged.

## 2.3.17

Fixed the two independently verified causes of missing flat tip/level UI. The
OG/AE visible-delta shader had used the small frame-to-frame color change as
both an ownership gate and replay opacity. Its occlusion query could therefore
accept thousands of pixels whose retained texture was effectively transparent.
Change is now only the ownership gate; every accepted replay sample has at
least half opacity under the existing luminance key. The VR shader and March
presentation path are unchanged.

The AE exterior correction in this version was superseded by 2.3.18. Field
logging proved that the real non-minimal exterior Show was not skipped: it
arrived while the early minimal menu was already visible, where Bethesda's
shown latch ignored it. The fresh-Show predicate in 2.3.17 therefore could not
match the failing path.

The invalid NG "level-only" state introduced in 2.3.16 was removed. Bethesda's
movie starts in minimal mode and `SetLevel` cannot make its widgets visible;
only `SetLoadingText` leaves minimal mode. This also removes the futile repeated
GPU proof attempts seen on affected transitions.

## 2.3.16

Superseded by 2.3.17. This version attempted to model minimal NG publications
as a separate "level-only" state. Later SWF and runtime evidence proved that
assumption false: `SetLevel` remains hidden while `LoadingMenu` is in minimal
mode, so those publications cannot yield visible level or retained-tip UI.

## 2.3.15

Fixed the Fallout 4 AE/NG spinner-only regression introduced by 2.3.14. Every
verified native LoadingMenu Show now retains the configured custom
presentation, including Bethesda's no-model/no-tip transitions. The exact
native selection controls only whether mode 3 captures tip/level or publishes
the configured background alone; it no longer decides whether that background
is eligible to appear. If a chained mode-3 selection cannot be restored, the
safe fallback is background-only custom presentation instead of native
spinner-only presentation.

The working Fallout 4 OG and VR presentation paths are unchanged.

## 2.3.14

Fixed the remaining per-screen presentation failures without changing the
working Fallout 4 OG path or the March VR `player.moveto player` handoff.

Fallout 4 VR now reads the runtime's exact `LoadingMenu::SendLoadingText`
selection before Bethesda mutates its candidate list. A verified no-tip screen
still freezes at March's 500/100 ms floor, while a verified tip-bearing screen
keeps Scaleform live for the bounded capture window instead of incorrectly
freezing a background-only frame at 100 ms. Capture remains capped at 600 ms,
with a final 100 ms grace only for an already-pending GPU proof. Every VR delta
is tied to its LoadingMenu owner, native selection serial, and load epoch, so a
late query from an older selection cannot publish stale tip pixels.

Fallout 4 AE/NG now keeps the native 3D-model decision separate from the native
tip/level decision. Mode 3 uses the custom presentation whenever Bethesda
selected either a model or tip text, including text-only `validScreens` loads;
verified minimal no-model/no-tip transitions remain native. The initial model
decision is latched before `AdvanceMovie` clears `artScreen`, and chained Shows
cannot inherit the prior menu's selection. Flat mode 3 also keeps its exact
tip capture active when GFx spinner access is unavailable because both its live
and replay shaders mask the complete spinner region.

## 2.3.13

Fixed recurring tip and level capture on Fallout 4 AE/NG 1.11.221 and
1.11.240. Flat mode 3 now uses the same exact native publication, LoadingMenu
owner, visible pixel-delta, successful-Present, and persistent replay proof as
1.10.163. The previous NG-only one-second timer freeze was removed because it
could lock the movie before a later loading screen had drawn its tip and level.

All three added NG hooks are restricted to the two verified runtimes and must
match both their Address Library ID/RVA and runtime-specific entry signature.
If any required hook or draw proof is unavailable, tip-bearing loads keep the
native movie live instead of freezing an unproven background-only frame.

## 2.3.12

Added exact Fallout 4 AE/NG 1.11.240.0 support. The plugin now accepts the
matching F4SE 0.7.9 host and advertises 1.11.240 in its layout-dependent plugin
metadata. LoadingMenu show/hide, menu-state, door-load, AdvanceMovie,
performance-patch, exterior gate-enumeration, PreloadWorld, player, TES, and
optional Havok targets were mapped against the exact 1.11.240 executable and
Address Library. Every NG target must match its build-specific RVA plus its
existing code/data validation before the related capability is enabled.

NG renderer discovery no longer calls the pinned CommonLib RendererData
singleton relocation, whose ID is absent from both verified NG Address Library
files; it uses the validated DXGI factory/Present fallback instead. Exterior
gate enumeration likewise no longer calls CommonLib's unavailable global form
map lookup. It retains candidates under Fallout's collection lock, stores safe
object handles, and resolves those handles for later proximity polls. The two
door-load hooks are now installed and rolled back as one transaction so a
partial pair can never remain active.

## 2.3.11

Fixed the remaining flat mode-3 first-screen-only tip/level failure. The OG
`AdvanceMovie` relocation was wrong: Address Library ID 314582 resolves to the
real `LoadingMenu` destructor at RVA `0x1296D40`, not `AdvanceMovie`. The plugin
could therefore write its temporary RET to lifecycle code while leaving the
movie update live. Fallout 4 1.10.163 now uses verified ID 618896/RVA
`0x1297120`; both OG and AE targets must also match LoadingMenu's primary-vtable
slot 4 and a full runtime-specific function prefix before any write is allowed.

The one-second v2.3.10 timer was removed from OG as a second part of the fix: a
timer cannot prove that the current tip and level reached the movie. A validated
hook now observes `LoadingMenu::SendLoadingText` only after its native
`SetLoadingText` and `SetLevel` calls return. Each load must then complete a
post-publication composite and Present before the real `AdvanceMovie` and the
animation loop may freeze. If Fallout never publishes text for a load, the fast
freeze remains off for that load instead of deliberately locking in a
background-only frame. No native loading-screen selection function is called or
replayed by the plugin.

## 2.3.10

Fixed flat mode 3 showing tip/level content only on the first loading screen.
The old freeze gate treated 60 Present calls as roughly one second, but the
load-time 350-FPS policy reached it in only 218-238 ms on later loads. The
LoadingMenu movie now receives a monotonic one-second warm-up, in addition to
the existing 60-frame floor, before `AdvanceMovie` and the animation loop are
frozen. Exterior gate preloading is unchanged.

## 2.3.9

Retired both speculative interior preload routes after the native-only Skyrim
linked-interior test reproduced the same structural freeze. Full mode now
forces Fallout's `bPreloadLinkedAreas` off, ignores stale native/crosshair MCM
overrides, no longer changes the teleport radius or interior cell buffer, and
does not build the native linked-callsite hook. Door candidates resolving to an
interior return `interior-preload-retired` without an engine call.

The retained B path is exterior gate proximity only. It accepts only stable
exterior origins and cross-worldspace exterior destinations, and submits only
Fallout's single-arrival-cell `PreloadWorld(..., queueOnly=true)` request. The
retired full-grid selector and unsafe interior controls were removed from MCM.

## 2.3.8

Aligned the remaining VR load-time policy with the March implementation while
preserving the current exact Scaleform tip/level capture and composite.

Fallout VR's native background-load drain budgets now remain at their existing
20/5 ms values instead of being replaced by the later unvalidated 500/500 ms
experiment. `UntieSpeedFromFPS`, the VR timer byte, and `iFPSClamp` are again
deferred until the first successful save reaches native CLOSE, matching March's
first-load state. The normal two-second VR heartbeat worker is disabled; it is
available only when the explicit VR presentation probe is enabled.

## 2.3.7

Made `iBenchmarkMode=0` a genuinely passive measurement observer. It now
returns before allocating trampoline storage or initializing the manager,
heartbeat, assets, renderer/OpenVR hooks, executable patches, prefetch systems,
crash guard, fades, spinner, tasks, overlays, or self-MoveTo. It records only
F4SE save boundaries and matched native LoadingMenu OPEN/CLOSE timestamps.

Each native CLOSE emits one parseable `PASSIVE_MEASURE` sample with the real
save-attempt number, menu ordinal, engine time, native-menu time, cumulative
request-to-close time, native-chain time, and previous-close gap. This keeps a
March self-MoveTo menu as a subphase of the same save attempt rather than a
second save load.

If the distinct March `VRLoadingScreens.dll` is present while full mode was
configured, the current DLL now forces itself passive before any hook can be
installed. This prevents two plugins from competing for the Submit hook,
animation-loop patch, overlay keys, or self-MoveTo lifecycle.

## 2.3.6

Removed the exposed native VR LoadingMenu at the beginning of a load. The
blocker and preloaded custom background now take ownership synchronously at
native OPEN instead of waiting for March's 500/100/0 ms animation-loop timer.
That timer still governs only the speed NOP, so first-load menu-fade safety and
March's loading-speed behavior are unchanged.

Mode 3 continues capturing Bethesda's tip and level UI underneath the opaque
custom presentation. Once ready, its 2048x1152 composite replaces the visible
plain background through an in-place OpenVR texture/bounds update that preserves
the already-accepted pose, width, alpha and visibility.

## 2.3.5

Replaced the VR load lifecycle with the known-good March 5 v1.0 sequence. The
500 ms first-title delay and 100/0 ms in-session delays are now captured at
native LoadingMenu OPEN. At that fixed boundary the plugin requests the exact
ten-byte animation-loop NOP, publishes the custom presentation, and applies the
NOP after the next right-eye Submit without waiting for an accepted eye-pair,
tip-readiness, or post-load world-state proof.

Native CLOSE restores the animation loop immediately. A successful save load
retains the complete presentation while `player.moveto player` runs from the
same direct Fallout main-loop callsite used in March; its synthetic reload
reuses the existing art and pose. The final CLOSE removes background, blocker,
tip content, and scene black together after 200 ms.

The sole visual substitution is the current tip/level pipeline: Bethesda's
LoadingMenu is captured as an exact Scaleform before/after delta and composited
into the 2048x1152 custom background. Capture is best-effort and can never hold
back March's loading-speed deadline.

## 2.3.4

Restored the known-good March VR presentation contract instead of adding
another inferred readiness delay. Fallout's native LoadingMenu and world
renderer now continue drawing underneath the opaque custom artwork; the plugin
no longer suppresses those draws or clears the submitted eye textures.

The first successful title-screen save retains the complete custom stack and
queues the original one-shot `player.moveto player` transition on Fallout's
game thread. Its resulting LoadingMenu reuses the visible artwork. After that
second CLOSE—or 200 ms after an ordinary transition—the background, tips and
blocker are removed together on the game thread and scene black is cleared
instantly. Accepted eye pairs, world-state flags, the post-close fade latch and
the 0.35-second reveal no longer authorize production presentation release.

The first title load again observes March's 500 ms pre-NOP menu-fade floor.
Async DDS preparation, generation/device-loss protection, exact NOP ownership,
accepted-pair validation for applying the NOP, and the independent VR playspace
repair remain intact.

## 2.3.3

The v2.3.2 field run isolated why the title-screen flash survived its new
world-readiness gate. The gate behaved exactly as designed: transition state
became healthy 389 ms after CLOSE and release was authorized at 614 ms. But the
opaque `FadeToColor` request and its 0.35-second clear were both issued at the
same timestamp, in the same post-right-eye callback. OpenVR therefore had no
compositor interval in which opaque scene black was guaranteed to exist; the
clear target could replace the opaque target before it was ever presented.

The normal VR save handoff now latches that fade across callbacks. Once world
readiness authorizes release, the plugin keeps the custom artwork visible,
reasserts opaque scene black underneath it, and waits for generation-tagged,
accepted left-to-right eye pairs that began after the fade was armed. Only a
later callback retires the overlays and starts the 0.35-second reveal. A short
32 ms floor ensures the commands cannot collapse into one callback, while a
250 ms callback-time fail-open backstop prevents rejected submissions from
stranding the artwork once right-eye callbacks resume. If submissions stop
entirely, the artwork intentionally remains because there is no new compositor
work to reveal. In-flight submissions from an old load cannot be credited to a
new latch.

## 2.3.2

The v2.3.1 field run proved that a valid 0.35-second scene fade was still
starting too early. On the first reported load the custom presentation released
371 ms after native CLOSE, while Fallout's transition state did not become
healthy until 1,096 ms; the ramp was fully transparent roughly 375 ms before
the game handoff completed. A second load resumed submitted frames only after
1,176 ms and exposed another bad assumption: the playspace repair's nominal
"60-frame" dwell took 8.8 seconds because this callback was running near 8 Hz,
not 90 Hz.

Successful VR save loads now retain the existing custom artwork until readable
world and playspace transition state is stable across fresh post-close frames.
The release remains bounded by a 2.5-second hard fallback; door and cell
transitions keep their original 200 ms path. The
native LoadingMenu/MainMenu suppression window now covers the complete save
handoff.

Immediately before the overlays are retired, the plugin reasserts opaque scene
black and then clears it over 0.35 seconds. This guarantees that Bethesda's own
fade calls during a long load cannot turn the intended ramp into an alpha-zero
no-op. Both the visual teardown and queued playspace repair are generation
tagged so work from an old load cannot affect a newer one. Playspace repair now
uses a 1.5-second steady-clock dwell with a minimum sample count and refuses
out-of-range state instead of assuming an HMD-rate callback.

## 2.3.1

Fixed the final ordering bug in the v2.3.0 scene-fade handoff. The normal
post-close path called `EndLoadingPresentationNow()`, which cleared the opaque
scene fade instantly, and only then requested the documented 0.35-second reveal.
At that point the fade was already transparent, so the second call was a no-op
and the retained title/mono frame was still binary-revealed. The field log made
the defect explicit: `scene fade APPLIED` was followed by `CLEARED over 0.00s`.

The reveal duration is now passed into the overlay teardown and the scene fade
is cleared exactly once, after the custom overlays are retired. Normal loads
therefore hand off through a short black-to-game fade; abort, device-loss and
native-fallback paths retain their instant fail-open cleanup.

## 2.3.0

**Restores `IVRCompositor::FadeToColor`, which the original and committed v1.5
builds used but the later 2.x refactor had silently lost.** This is the
mechanism behind the end-of-load 2D/broken-stereo title flash, and the reason
six previous fixes could not work. Before this restoration, the 2.x helper
bound compositor vtable entries 1, 3, 15, 16, 17 and 32, but not entry 12.

Why it matters: **OpenVR overlays and the app's scene layer are different
layers.** Every previous fix moved overlay visibility, ordering, sort order,
suppression windows or hold length — but hiding an overlay is a *binary reveal*
of whatever Fallout last submitted, and no amount of changing *when* you flip it
changes *what is underneath* when you do. `FadeToColor` is the only lever that
masks the scene layer itself, and the only one that can be released gradually.

The original applied it at the exact instant the animation-loop NOP froze the
render loop, and cleared it in the same call that hid the overlays. That is
restored here:

- Applied when the deferred NOP lands (`D3D11Compositor::HookedSubmit`), opaque
  black, instant. From there until the overlays hide, nothing Fallout submits
  can reach the eyes: not the retained pre-load frame, not a stereo-mismatched
  pair left by breaking the render loop between eyes, not the world-less menu
  frames the loading pump produces. Overlays composite above it, so the loading
  art is unaffected — the original held this at alpha 1.0 for entire loads with
  its art on screen.
- Cleared in the same step that removes the overlays, over a 0.35 s ramp rather
  than instantly, so any residual stale frame is multiplied toward zero instead
  of switched on.
- Cleared unconditionally by abort/fallback teardown and by the normal timed
  release, so a leaked blackout can never survive into gameplay.

## 2.2.2

**The eye-buffer scrub was erasing the very frames the post-close hold exists to
produce.** This is the mechanism behind the end-of-load artifact, and it is the
one thing the original VR release did differently that actually mattered.

The original's hold has a stated purpose, in its own comment: keep the overlays
up briefly "so the game renders a clean gameplay frame before we reveal it
(avoids brief flash of stereo-mismatched loading content)". Its close path
un-freezes the render loop first, then holds the overlays for 200 ms while the
game draws real frames underneath, then reveals one. **The original never blanks
an eye buffer anywhere** — its only `ClearRenderTargetView` reference is a hook
installation comment.

This build's Submit hook was blanking every submitted eye texture while
`m_inLoadingScreen || IsPostCloseCoverActive()`, and the second half of that
condition covers the entire hold. So every clean frame the game produced during
the hold was erased as it was produced, and at the reveal there was nothing
clean to show. The hold and the scrub were cancelling each other out — and the
longer the hold, the more good frames were destroyed, which is why every attempt
to fix this by lengthening the hold failed or made it worse.

The scrub now runs during the load only, where it is still correct: the game
submits almost nothing there (heartbeat-measured at zero pairs across an entire
20 s load), and what little it does submit can be the retained pre-load frame.

## 2.2.1

From a line-by-line comparison against the original VR release (924a5f79) for
save-load speed. The honest headline first: **most of the slowness is not this
plugin.** Same machine, same save pattern:

| Build | First main-menu save load |
|---|---|
| Original v1.1.0, March | 5.41 s |
| Original v1.1.0, **same DLL**, April | **21.37 s** |
| Current, `iBenchmarkMode=0` (applies nothing) | 22.74 s |
| Current, fully active | 14.01 s |

The original DLL itself logged 21.37 s once the environment changed, and the
current build with everything disabled is no faster. The mod list, the save and
disk state dominate. What follows is what is genuinely still ours.

- **Native door-linked preloading now defaults OFF** (`bPreloadLinkedAreas`,
  `bNativeExteriorArrivalCellOnly`). The engine ships `bPreloadLinkedAreas`
  off; this plugin forced it on with no snapshot and no restore, so it stayed
  on for the whole session. For a **save** load it is strictly additive with no
  possible upside — a save's destination comes out of the .fos, never from a
  nearby door, so linked-area prediction can never hit; it can only leave
  speculative cell queues and extra resident interiors for the load to tear
  down. The `queueOnly` byte flip is off for the same reason plus one more:
  with the original byte an exterior-origin `TES::PreloadWorld` call is a
  verified no-op, so flipping it converts nothing into real BA2 reads
  (measured at roughly one speculative queue per second near a gate, against
  zero in vanilla). Both remain available in the INI — they do help *door*
  transitions, which is what they were added for.
- **The `BackgroundLoad` budget change now logs what it replaces**
  (`queued N -> 500, general N -> 500`). This is the only engine setting the
  plugin changes that is live *inside* the measured load window, and its sign
  had never been checked: a larger drain budget means fewer pumps (faster), but
  also lets the main thread sit in one drain for up to 500 ms (slower). Every
  other tweak logged `old -> new`; this one did not, so the comparison was
  impossible to make from a log.

Falsified by this comparison, recorded so they are not chased again:

- **"The mode-3 tips capture delays the animation-loop NOP."** The 600 ms
  deadline never fired in any logged load. Measured OPEN→NOP: 39-190 ms. The
  *original* took 515 ms on the same transition because of its unconditional
  0.5 s main-menu floor. The current build freezes the loading pump roughly
  470 ms **sooner** on exactly the case being judged.
- **"A byte patch is silently skipped by the signature check."** All four apply,
  log-confirmed, and the two timer patches now land earlier than the original's.
- **SaveGamePrefetch** never armed in any field session; its 250 ms quiesce
  costs nothing.
- **Per-frame work during a load** is zero after the first ~40-190 ms
  (`totalSubmits` unchanged for the entire remainder).
- **Background DDS loading is better than the original's**, which did a
  synchronous disk read plus software BC decode on the game thread at every
  close — 4.8-5.7 s, landing *after* its own timer stopped. The original's
  logged durations understate what the player actually waited through.

## 2.2.0

**Reverts the post-close release to the original VR design.** The two-stage
release introduced after the initial release was the source of the end-of-load
artifact, and no amount of tuning fixed it because the design itself was wrong.

The original (commit 924a5f79, 2026-03-05) closed a loading screen in four
lines: schedule a hide 200 ms after CLOSE, then hide the background overlay and
clear the skybox in ONE step. The blocker was never involved. Its own comment
states the delay exists "so the game renders a clean gameplay frame before we
reveal it (avoids brief flash of stereo-mismatched loading content)" — the exact
symptom later reported against the design that replaced it.

What replaced it dropped the artwork first and then held a black blocker until
three eye pairs had been submitted. That created an intermediate window the
original never had, and the release gate was incapable of doing its job: the
pairs it counts are frames this plugin's own eye scrub paints black, so it
proves the render thread is alive and nothing about the pixels. The magenta
cover diagnostic confirmed both halves — the cover was seen (so it works), and
the frame it uncovered was measurably stale (`mean=(127,113,97)`, non-black,
unchanged across five frames).

- Single-stage release: tips, background, blocker and skybox all come down
  together, 200 ms after CLOSE. No intermediate state, no content gate.
- Hold restored to the original 200 ms for every transition. The value had
  drifted 200 → 250 → 600 → 1000 across the two-stage era; every move was an
  attempt to make the intermediate window outlast an artifact that window was
  itself creating.
- Native-menu suppression reduced 2000 → 700 ms; it no longer has to outlive a
  separate cover stage, and is still carried past the reveal.

Kept from the two-stage era, because they are independent and proven:
`CellWorldspaceGuard` (the Diamond City CTD guard, field-confirmed intercepting
null-worldspace cells), the tips full-surface rejection (the black square), the
non-convergence bailout, the working load heartbeat, and startup log rotation.

## 2.1.24

Adversarial review of the 2.1.22/2.1.23 fixes themselves (four independent
reviewers plus a judge). Verdict was no-ship, with five real defects in code
written to close the previous audit. All are fixed here; 2.1.23 was never
deployed.

- **The new suppression could be inert exactly when it mattered.** The hook's
  outer guard was `inLoadingScreen || observeWindow`, so suppression depended on
  the *diagnostic* deadline. Those are two separate atomics, and the extend path
  published suppress before observe, creating a real state where suppression was
  live but the guard evaluated false — a native tail frame there is precisely
  the grid flash. The guard now tests `postCloseWindow` explicitly, and both the
  arm and extend paths publish observe first, so the two can only ever disagree
  in the harmless direction.
- **`bVRAnimationLoopNOP` never republished on a config refresh.** It was
  applied once at startup while every neighbouring `[Main]` key hot-applied, so
  editing it mid-session silently did nothing — and its tell is a log line that
  *fails* to print, so a contaminated A/B arm looked identical to a clean one.
  The same omission applied to `bVRPresentationProbe`. Both now republish, and
  the refresh line reports the in-force values.
- **The mode-3 watchdog could not see the hopeless bailout**, so it kept the
  600 ms capture deadline instead of the 100 ms no-capture one, handing back up
  to half a second of the dominant VR speed lever for a capture that had already
  given up. Exposed via `IsTipsCaptureHopeless()`; the timeout warn now reports
  it, so "gave up early" is distinguishable from "timed out".
- **The heartbeat reported a false `0.0 pairs/s` on flat.** `m_eyeSubmitTotal`
  is only incremented by the VR Submit hook, but the gate tested
  `IsInitialized()`, which is also true for both flat init paths — so every flat
  load read as a frozen engine. Now VR-only, else `n/a`.
- **The heartbeat outlived the compositor it dereferences.** This manager's
  singleton is constructed first and therefore destroyed last, and the jthread
  was only stopped by its own destructor, after the body ran. It is now stopped
  and joined first, and its 500 ms sleep is stop-aware so the join is bounded.

Also fixed, from the same review: MainMenu is now identifiable in the
observe-only census (it was reported as "other" with a vtable printed nowhere,
in the very window built to catch it — and the log line that was supposed to
print both vtable pairs lived in `InstallDisplayMenuHook`, which is never called
on VR, so it was absent from the binary entirely); the coverage test now fails
closed on a zero denominator instead of silently skipping; the hopeless bailout
is checked in the eligibility gate so it stops taking the render lock and
inflating `beginRejects`; and the bailout logs elapsed-into-load plus the
insufficient count so a wrongly-early bailout is falsifiable from one session.

Ruled NOT defects by the judge, after verification: the observe deadline *is*
cleared at OPEN (`BeginPostCloseLoadingMenuSuppression(0)` zeroes all three
fields), the CAS loops are sound, there is no lock inversion at the new call
site, and the DisplayMenu refactor is behaviour-identical for in-load and
post-close draws.

## 2.1.23

The two suppression-timing fixes from the audit, held back from 2.1.22 because
they change real visual timing and must be field-tested without other changes
confounding them. They also have to land together: the first alone would strip
an accidental protection the second replaces properly.

- **The observation window was not log-only.** 2.1.20 added a window that
  observes native menu draws for 2.5 s beyond the suppression deadline,
  documented and logged as log-only. Its draws actually fell through to the
  IN-LOAD decision, where modes 0/2 skip unconditionally and mode 3 consults an
  `m_tipsCaptureComplete` left over from the finished load. So suppression
  silently ran to 4.5 s instead of the stated 2 s, mode-3 tails became
  nondeterministic on stale cross-load state, and the census logged
  `suppressed=false` for draws the hook then skipped — corrupting exactly the
  data the window exists to collect. The skip decision is now computed before
  the census, the observe-only state suppresses nothing, and the census reports
  what actually happened (`window=suppress|observe skipped=true|false`).
- **Suppression is now anchored to the cover release, not to wall-clock.** The
  2 s window was armed at CLOSE, but the black cover is released by a tick that
  only runs from the frame callback after a right-eye Submit, and its deadline
  is re-based when the art drops. A post-close submit stall — a state the
  heartbeat has measured directly — therefore pushed the cover release past a
  suppression window that had already expired, leaving native LoadingMenu tail
  frames drawing with nothing in front of them, which is the grid flash.
  `ExtendPostCloseLoadingMenuSuppression` monotonically carries suppression
  500 ms past the actual cover release. It only ever lengthens an armed window
  and never revives one a new load cancelled.

## 2.1.22

Fleet audit of 2.1.14-2.1.21 (19 agents: six subsystem auditors, adversarial
verification of every finding, synthesis). 12 findings survived verification;
this release ships the six that are safe to batch (instrumentation, safety,
hardening - no visual-timing change). The two suppression-timing fixes are held
for 2.1.23 with their own field test, and the terminal keyed tips capture for
2.1.24, so results cannot confound each other.

Fixed here:

- **The 2.1.21 NOP isolation toggle was broken in the mode that matters.** A
  third request site - the mode-3 watchdog deadline - was not gated, so with
  `bVRAnimationLoopNOP=0` the NOP still landed within ~620 ms on effectively
  every mode-3 load. Every long-load A/B conclusion drawn from that toggle
  would have been invalid. Now gated, with an explicit SUPPRESSED log line as
  the contamination tell. Also corrected a comment still claiming the capture
  deadline is 2.5 s (it is 600 ms).
- **The crash guard was absent in the control mode.** CellWorldspaceGuard was
  installed inside the `iBenchmarkMode != 0` block, so the exact control run
  used to investigate long loads removed the Diamond City CTD guard. Hoisted
  out; it now installs unconditionally on VR. Its intercept counter is also
  surfaced at CLOSE on change instead of being write-only.
- **The load heartbeat was blind in the control run.** It keyed on the
  compositor's visual-pipeline flag, which is never raised in timing-only mode,
  VR native mode 1, fallback loads, or ANY flat load. It now keys on the
  manager's own load flag, tags each line with mode/state, and reports the eye
  rate as n/a (not a false 0.0) when the Submit hook is not installed.
- **The 2.1.21 log rotation was a silent no-op on every launch.** It called
  F4SE::log::log_directory() before F4SE::Init, when the game-folder component
  is still empty, so it checked a path that never exists. The path is now built
  by hand from the runtime (provably identical to where the sink writes), and
  the outcome is logged after startup ("Log rotation: rotated / no-previous-log
  / failed").
- **The 40% full-surface tips rejection had a unit mismatch.** The occlusion
  numerator counts dest-resolution samples (crop scaled into a <=2048 target)
  while the denominator was the full source-RTV area, so a full repaint under a
  sub-RTV crop could slip beneath the threshold and re-create the black square.
  The denominator is now the delta draw's own target area, snapshotted at arm
  time in dedicated fields (behaviour-identical in the observed 1024x1024
  geometry).
- **Futile tips captures now bail out.** Each armed attempt costs two 4 MiB
  RTV copies plus a megapixel delta draw; short loads made 8-27 provably
  futile attempts (~70-230 MiB of copies in 600 ms). After 6 consecutive
  full-surface rejects the load is declared hopeless and no further captures
  are armed. Nothing is published (that would resurrect the black square);
  tips on short loads remain OPEN until the 2.1.24 keyed terminal capture.

Audit verdicts on the ledger, for the record: grid-flash fix FRAGILE (the 2000
ms suppression is wall-clock-anchored while the cover release is tick-driven -
2.1.23 anchors suppression to the cover-release event; the observation window
also currently extends suppression instead of only logging, and the census
`suppressed=` field lies - both fixed together in 2.1.23). bPreloadLinkedAreas
is force-enabled against the user's own INI with no restore - a defect in
itself; default flip is deliberately deferred until after the A/B run so it
cannot confound arm A.

## 2.1.17

The 600 ms post-close artwork hold now applies to EVERY transition, not just
save loads.

2.1.14 scoped it to save loads on the theory that door transitions had to stay
under the engine's shortest door fade. The 2026-08-02 field test settled it in
one session: the save load into Diamond City (600 ms hold) had no flash, and
the Diamond City exit minutes later (250 ms hold) did. The artifact tracks the
HOLD LENGTH, not the kind of transition, so the door path needs the same value.

The door-fade concern was measured against a 1000 ms hold, where the artwork
outlived a finished load and could survive into the next transition. 600 ms is
well short of that, and both windows are still cleared by the next native OPEN.

Still open, and the reason this hold is load-bearing rather than cosmetic: the
black blocker overlay does not cover the window after stage 1 drops the art.
Raising its sort order and fully re-asserting it both failed with OpenVR
reporting success on every call. The magenta cover diagnostic
(`bVRPresentationProbe=1`) is the unfinished experiment.

## 2.1.16

- `CellWorldspaceGuard`: guards the reproducible VR worldspace-transition CTD
  (`Fallout4VR.exe+0x35BFA5`). Full mechanism in `src/CellWorldspaceGuard.h`.
  A null `TESWorldSpace` reaches an unguarded hashmap probe during the
  attached-cell purge that `MoveTo` performs; the engine's only guard compares
  the field to itself so `NULL == NULL` passes. Verified arithmetically against
  the binary: `lea rcx,[rbp+0x258]` with rbp=0 gives R15=0x258 and a read of
  0x2B0, exactly the logged values. The hook checks the 10-byte prologue before
  patching and takes the engine's own bail path on null.
- Tips capture rejects full-surface deltas (>40% coverage). On short interior
  loads the before-snapshot could be taken mid-initialisation, making the delta
  the entire 1024x1024 surface, which composited the menu's opaque black
  backdrop over the artwork — the black square seen entering All Faiths Church
  and Publick Occurrences. Only a minimum sample count was ever checked.

## 2.1.14

Fixes the end-of-load title-screen flash, from a bisect against a known-clean
binary rather than another theory.

- `C:\Games\MO2\mods\Faster Loadscreens 2.1` is **v2.1.5** (packed version
  0x02010050 present, 2.1.4/2.1.6/2.1.7 absent; carries the pre-2.1.7
  `VR stereo probe` log strings) and has no flash. 2.1.6 onward does.
- The only behavioural change across that boundary: 2.1.5 still had the dead
  stereo-resume wait. Its signal never fires, so stage 1 ran to its 600 ms
  backstop on EVERY load. 2.1.6 removed the wait and the art hold became
  `kVRPostCloseHoldMs`, 250 ms. That one number is the whole delta.
- Save loads now hold the artwork for 600 ms again
  (`kVRPostCloseHoldSaveLoadMs`), explicitly, rather than by way of a dead
  gate's timeout. Door transitions keep 250 ms: they must stay under
  `fNormalDoorFadeSecs` 0.4, and the flash is only ever reported on the first
  load from the main menu.

What this actually reveals, and why it is not just "hold longer": the overlay
covering that window is the ARTWORK, not the blocker. The blocker is supposed
to own everything after stage 1 drops the art, and it does not — the reported
sequence is background, title screen, world with NO black phase, and the black
phase is precisely the blocker. Raising its sort order from 150 to 10000
(2.1.12) and re-asserting texture, alpha, transform and visibility at stage 1
(2.1.12) both failed, with OpenVR returning success for every call. So the
blocker is still an unexplained defect; 2.1.14 stops the artwork from exposing
it. The magenta cover diagnostic added in 2.1.13 remains the way to finish
that investigation.

## 2.1.13

The 2.1.12 sort-order fix did not work. The log proves the cover is published
and accepted (`blocker cover re-asserted (sort=10000)`, zero OpenVR errors) and
the title screen is still seen in its place, so that theory is dead alongside
the two before it. This release adds no further theory, only the two things
needed to stop guessing.

- **The blocker turns magenta while `bVRPresentationProbe=1`.** Black is
  indistinguishable by eye from "nothing rendered", from a faded compositor and
  from a dark game frame, which is why five rounds of log reading could not
  settle whether the cover was ever on screen. Magenta is not. Magenta seen in
  the stage-1 window means the cover IS on top and the artifact comes from
  somewhere else entirely; the title screen seen instead means the cover is
  provably not on top and the search moves to the OpenVR overlay layer, which
  our Scaleform menu hooks cannot observe at all.
- **The post-close census now logs VISIBLE draws only**, budget 24, each
  stamped with elapsed-since-CLOSE. The previous census spent its entire
  10-entry budget within 20 ms on eight invisible HUD menus and went silent at
  +168 ms, while the artifact window is +270 to +430 ms — blind to exactly the
  interval it was built to watch. That is the third time in this investigation
  a shared or capped counter hid the answer; elapsed stamps are now mandatory
  so an entry can be placed against the release boundaries.

Build note: `COMMONLIBF4_PATH` moved to `D:/Development/...`. Same pinned
revision `2b64114a`, same local modifications, so the dependency is unchanged.

## 2.1.12

Candidate fix for the end-of-load title-screen flash, from the first
measurement that actually localised it.

- The flash sits in the window between stage 1 of the post-close hold (art
  hidden) and stage 2 (cover released), roughly 270 ms to 395 ms after CLOSE.
  User-confirmed sequence: background, then title screen, then world, with NO
  black phase — which is the tell, because that window is supposed to be a
  black cover and nothing else.
- The black blocker overlay was created with sort order 150, against bg 200 and
  tips 210. Our own comment already recorded the hazard it exists for: the
  FO4VR title screen submits its own OpenVR overlays and they peek through a
  low blocker. 150 was never verified to beat them. It only looked correct
  because for most of a load the art (200/210) sits in front of everything; the
  instant stage 1 drops the art, the blocker at 150 becomes the sole cover and
  any game overlay above it draws straight over the black. Blocker, bg and tips
  are now 10000/10010/10020, preserving our internal order while clearing any
  plausible game value.
- The blocker is also re-asserted at art release (texture, alpha, sort order,
  transform, visibility) instead of trusting a "shown" flag set at OPEN, up to a
  whole load earlier. Any OpenVR rejection is logged rather than assumed away.

Measurements that got here, all of which stay in the log:

- `Eye scrub [close]: ok=8 fail=0` on every load, so the scrub never failed and
  is nearly irrelevant: with the render loop frozen the game submits about four
  eye pairs across an entire load.
- Burst-sampled eye buffer at the reveal: five consecutive frames, identical to
  within one quantisation step. The revealed frame is settled world, not a
  one-to-three frame artifact. That ruled out the reveal itself.
- Post-close menu census plus RTTI on the unpacked exe: the unsuppressed
  visible menu at +96 ms is `FaderMenu` (RVA 0x2dc8088). It draws behind the
  cover, so it is not the flash, but it is now named. `LoadingMenu` 0x2dca698
  and `MainMenu` 0x2dcbb38 resolved exactly to our existing constants, which
  validates the whole census.

## 2.1.9

Fixes the flash regression that 2.1.7 introduced. It WAS a regression: 2.1.6 is
the build that was confirmed clean, and 2.1.7 was labelled "documentation and
diagnostics only, no behaviour change" when it was neither.

- What 2.1.7 actually changed: the two post-close probe helpers gained a
  `PlayerCharacter::GetSingleton()` + `Get3D()` call. In VR, `Update()` runs
  from the frame callback inside the Submit hook, so those helpers execute on
  the OpenVR submit thread, and the live-world check ran on EVERY submit after
  CLOSE until it first reported true. `Get3D()` walks `loadedData`, which the
  engine is rebuilding across exactly that window, and unlike the flag bytes it
  carries no SEH guard. 2.1.6 read two plain global bytes on that path and
  nothing else.
- `VRWorldPresentationLive()` is byte-only again, restoring 2.1.6's submit-thread
  profile exactly. The player-3D term survives only where it is safe: the CLOSE
  snapshot, which runs on the game thread.
- The whole per-frame probe is now opt-in behind `[Diagnostics]
  bVRPresentationProbe`, default 0.
- 2.1.8's pre-scrub eye readback is behind the same flag. It performs a blocking
  `Map` on the submit thread at the release boundary, which is the same hazard
  class as the bug above, so it must never be on for play. Enabling the flag
  reproduces the regression by design, which is what makes it a test.
- The scrub ok/fail counters stay unconditional: they are atomic increments,
  reported once at CLOSE on the game thread.

Lesson recorded: "diagnostics only" is a claim about the thread and the work,
not about the intent. Anything added to the submit thread inside the post-close
window is a behaviour change.

## 2.1.8

The end-of-load flash is back. It was never fixed, and 2.1.7 should not have
said it was: that claim rested on a single clean session, while 2.1.6's own
entry correctly recorded the flash as unfixed. The binary that flashed is
byte-identical to the one that did not, so the artifact is intermittent and
nothing regressed — 2.1.6 removed a wait that could never fire, which shortened
the hold but never touched the mechanism that puts a stale frame on screen.

Diagnostics only, no behaviour change, no added hold time.

- The eye-buffer scrub is the only thing between a load and the retained
  pre-load frame, and it is fail-open — yet it shared ONE log gate between its
  success and failure branches, so a scrub that worked on the first submit and
  failed for the rest of the load produced a log identical to a healthy one.
  Successes, failures and the last failing HRESULT are now counted per load and
  reported at CLOSE.
- The post-close cover releases on a submitted-PAIR count, which proves a frame
  arrived and says nothing about its contents; the scrub then makes every frame
  read as black from the outside. The one question that decides this bug — is
  the frame being revealed the world, or the menu retained from before the load
  — was therefore unanswerable from the log. Both release boundaries now sample
  the actual submitted eye texture, read before the scrub blacks it, and log
  mean colour, peak luminance and non-black pixel count.
- This is the counter-split lesson from the tips-capture bug applied to the
  same failure shape: a single undifferentiated signal made the fault
  unfalsifiable. Measure first, then change the release gate.

## 2.1.7

Documentation and diagnostics only — no behaviour change.

**Superseded by 2.1.8: the claim below that the flash was gone was wrong.**

- Identified why the 2.1.1 handoff could never work: the engine byte it waited
  on is the weapon-scope setting, which is zero in all normal play. That code
  and its misleading comments are replaced rather than left as a trap.
- Established that VR has no monoscopic post-load window at all. What the
  "world briefly in 2D" actually was is the engine's freeze-frame micro-load:
  when a newly loaded cell's combined objects are not yet resident it captures
  one still and re-blits it head-locked until the load finishes. The
  title-screen flash was a separate case — with the player's 3D not yet
  created, the renderer draws the menu and no world at all.
- The per-load diagnostics now sample those real signals, so if the artifact
  ever returns its cause is named in the log immediately.

## 2.1.6

- Removed the stereo-resume wait added in 2.1.1. Probe logging proved it could
  never work: the engine flag it depended on reads the same value at the close
  of a load and a full second later while the world is visibly back in 3D, so
  the wait always ran to its timeout and that timeout silently became the hold
  length on every load. The post-close hold is once again the plain bounded
  timer, so the background no longer outstays the load.
- The probes stay in the log. Finding the signal that genuinely marks the
  return to normal VR presentation is now a separate piece of work; the flash
  at the end of a load is unfixed and no longer has a placeholder pretending
  otherwise.

## 2.1.5

- Fixes a regression from 2.1.1: the stereo-resume signal that gates the
  post-close handoff never actually fires, so its 2.5-second safety timeout was
  becoming the hold length on every load — reproducing the "background outstays
  the load" problem. The timeout is now 0.6 s, close to the plain hold, so the
  worst case is bounded while the signal is diagnosed.
- The two engine flags the gate reads are verified correct addresses, so the
  fault is in what they mean, not where they are. Every load now logs the whole
  renderer flag block plus the world flag at close and at four points across the
  post-close window, so the byte that genuinely flips can be identified from one
  session instead of guessed at.

## 2.1.4

Fixes the post-load body/turning disconnect for real, and it was our bug.

- The 2.1.3 telemetry paid for itself immediately: at a save-load close the
  playspace reads `counter=0, committed=1, cachedNode=0, detached=1`. The
  reposition counter that 2.1.0 was built to drain is balanced; the field that
  is actually stuck is the playspace "detached" flag, and the repair now
  clears it (a plain byte write that moves nothing).
- Why it disconnects the body: the engine's view-rotation getter checks that
  flag in its very first instruction and, when set, returns the raw headset
  pose without composing the artificial-turn yaw. The per-frame body code then
  pins the actor's heading to the physical head direction, so stick turning
  moves the view while the body and hands stay put — exactly the reported
  symptom.
- Why it is ours: the flag has exactly three writers — the loading pump's
  prologue sets it, the pump's epilogue clears it, and the save-load path sets
  it again. This mod's loading-animation freeze makes the pump body run once
  and fall through, so the epilogue's clear happens at the START of the load
  instead of the end, and nothing clears the flag the save-load path sets
  afterwards. Vanilla spins that loop for the whole load, so its clear lands
  after. The repair restores the epilogue's effect at its natural moment,
  which is why `player.moveto player` used to mask it.

## 2.1.3

Un-blocks the VR playspace repair (post-load body/turning disconnect).

- Field logs showed the 2.1.0 repair never ran once across eight save loads: it
  was silently blocked by its own preconditions. It required the player's cell
  to report attached 3D — a predicate that never returns true on VR (proven by
  the 2.1.1 logs, where the attach-gated hold ran to its backstop on every
  load) — and it required the playspace's cached player node to be non-null,
  which is itself part of the leaked-hold state it was built to repair. Both
  gates are removed; the repair now relies on the counter-stability dwell
  (lengthened to ~2/3 s) plus the pre-drain staged-yaw neutralization, which
  keeps even a mistimed drain view-neutral.
- Every save-load close now logs the playspace and stereo-gate state it starts
  from, the stereo feed logs when the engine actually resumes stereo, and a
  failed state read warns instead of doing nothing. The two silent failure
  modes this release fixes can never be silent again.

## 2.1.1 / 2.1.2

Root-caused the end-of-load wrong-image flash. It was never a timing problem.

- After a load Fallout renders several MONOSCOPIC frames before it resumes the
  stereo eye pair: `Main::RenderFrame` runs the two-eye pass only once both of
  the engine's own gates are set, and until then it renders a single pass and
  force-submits it. That is exactly what was being seen — the stale main menu
  on the first load from the title screen, and "the whole world briefly shown
  in 2D" on every load after that. Vanilla hides this behind the still-drawing
  loading menu and its fade; suppressing that render exposed it.
- The previous handoff counted submitted eye pairs, which cannot work: a mono
  frame submits both halves of the side-by-side texture exactly like a stereo
  frame, so the count was satisfied by precisely the frames the cover existed
  to hide. Both the artwork and the black cover now release on the engine's own
  stereo-resume state instead, with a bounded backstop.
- Reverted the extended save-load hold and the cell-attach gate from 2.0.11 and
  2.0.12. They were the wrong lever: the attach gate never fired (it always ran
  to its backstop), which is why the artwork lingered over an already-live
  world while the flash remained.
- The tip/level display is 5% wider.

- Silenced the constant spinning/clicking sound during loading. The loading
  model has a looping "rotate" sound, and the engine plays it whenever the time
  since the model was last rotated is under 100 ms. That timestamp is only ever
  written by the rotate function, which does nothing when there is no model —
  so with the model suppressed the timestamp stays at zero and the comparison
  is decided entirely by a timer value the plugin's own FPS patches modify.
  Rather than depend on that, the plugin now stops the model's sound loops for
  as long as it is showing a custom loading screen: the model is gone, so its
  audio goes with it. Native mode 1 keeps Bethesda's model and its sounds.

Not changed, and deliberately so: the loading-screen animation freeze. It was
also suspected of causing the sound, but that is refuted at instruction level —
the freeze does not re-enter the loading pump, so it cannot be re-triggering
anything.

## 2.1.0

Root-caused and fixed the VR post-load body/turning disconnect.

- The bug that `player.moveto player` used to paper over is now understood and
  repaired at its source. Fallout VR keeps a reposition "hold" counter on its
  playspace object. Every transition raises it and stages the destination
  room-yaw; the call that lowers it back to zero commits that yaw and releases
  the engine's own handshake, which is what re-links your body's facing to the
  room you turn with the stick. The save-load path raises the counter in three
  places but only balances it as a side effect of load ordering, so a save load
  can finish with the hold still up: turning keeps rotating the view (that code
  never checks the counter) while the body-follow machinery waits forever on a
  handshake that can no longer start. `player.moveto player` cured it because
  its own path calls the release directly.
- The plugin now drains exactly that leaked hold, once, on the game thread,
  after the loaded cell's 3D attaches, and only after a save load. It is a
  no-op on a healthy load: the release is called only while the counter is
  positive, because calling it at zero would overwrite the live room-yaw with
  the staged one and throw away the turn you had accumulated. The engine's own
  commit veto is honoured, reads are fault-protected, and the whole path is
  gated to Fallout 4 VR 1.2.72. Every load logs the before/after state, so a
  recurrence is diagnosable from the log rather than by feel.
- This replaces the removed `player.moveto player` workaround without its side
  effects (no teleport, no havok re-add, no inventory/quest re-scan).

## 2.0.12

VR save-load title flash: deterministic fix.

- The 2.0.11 log proved timers cannot beat this one: after a save load the
  engine renders the MAIN-MENU BACKDROP SCENE as "the world" until the loaded
  cell's 3D actually attaches — it is world content, not a menu, so menu
  suppression, eye-buffer scrubbing, and fresh-frame counting all pass it
  through. The custom background is now held until the player's parent cell
  reports attached 3D (checked every frame during the hold, 4-second hard
  backstop, cleared by every presentation boundary), so the artwork hands off
  directly to the real world's fade-in. Door transitions are unchanged.

## 2.0.11

VR save-load handoff polish.

- After a save load, the custom background now stays up for 1.5 seconds past
  the loading menu's close (door transitions keep the short quarter-second
  hold). The engine's post-load sequence can show transient content between
  the menu closing and its own fade-in; the world is behind the black settle
  and fade for longer than the extended hold, so the artwork now bridges
  straight into the engine's fade with nothing else visible in between. Menu
  suppression and the eye-buffer scrub are extended to match, and a save load
  cannot reach a door within the window, so the lingering-background problem
  the short hold fixed cannot return.

## 2.0.10

Crash fix: retired the native-uGrids exterior preload mode. Also removes the
remaining VR title-screen flash.

- The last source of the post-load title flash in VR was not stale buffers
  (2.0.9's scrub was active and working, log-proven): after a save load
  started from the title screen, the engine keeps MainMenu on the menu stack
  and re-renders it for about half a second after LoadingMenu closes. The
  post-close suppression window now also suppresses MainMenu renders — but
  only for closes that actually entered a game session (kPostLoadGame seen),
  so a real quit-to-main-menu is never blanked.
- VR mode-3 tips now look native instead of glowing. The capture shader was
  replacing Scaleform's own antialiased glyph coverage with a luminance-keyed
  alpha and full-brightness color, which read as fattened, shiny text. The
  game's premultiplied pixels and coverage alpha now pass through unmodified
  (the before/after delta remains purely the ownership gate, so controller or
  pre-existing pixels still cannot enter the texture), and the composite
  blends premultiplied. Position and size are unchanged.

- `iExteriorGridRadius=1` made the exterior-gate preload call
  `TES::PreloadWorld` with `queueOnly=false`. That flag value is not a preload
  hint — it runs the engine's live grid-switch machinery, which is only legal
  in the engine's own transition contexts. Issued from an exterior origin it
  corrupts live grid state and crashes at the next real transition, reproduced
  at the Diamond City exit gate during A/B benchmarking. Every selector value
  now clamps to the single-arrival-cell mode (`queueOnly=true`), the callsite
  hard-codes `true`, and a warning is logged when an INI still requests the
  retired mode. The key remains accepted so existing INIs load unchanged.

## 2.0.9

VR mode-3 corrections from headset testing of 2.0.8.

- Tip/level text is back at its original size. The 2.0.8 enlargement read as
  far too big in the headset. It now draws at the plain fit scale — the same
  on-screen size as before — inside the high-resolution 2048x1152 composite,
  which is what actually fixed the softness (the text renders near 1:1 with
  the game's own 1024-pixel UI surface, which is the sharpness ceiling; a
  larger composite cannot add detail the game never drew). The measured glyph
  bounds now only slide the block into the lower-left corner.
- Found and fixed the real source of the title-screen flash at the end of a
  save load. The log proved the black cover released correctly on fresh game
  frames — but those frames themselves still contained the title screen:
  Fallout never clears its eye buffers (the world pass normally overwrites
  them), and with the loading model and menu UI suppressed, the last thing
  drawn before the load — the main menu — survives in the buffers through the
  whole load and is re-submitted at load end until the world pass resumes.
  The plugin now clears each submitted eye buffer to black while the loading
  screen or its post-close cover owns presentation, so what the engine shows
  in that gap is black, exactly like its own fade.

## 2.0.8

VR mode-3 presentation polish.

- Removed the title-screen flash at the end of a load. During a load the render
  loop is frozen, so SteamVR keeps re-showing the last frame submitted before
  the load — the main menu, for a load started there. Dropping every overlay a
  quarter second after close exposed that retained frame until Fallout's first
  post-load submissions arrived. The black blocker now stays up until the game
  has submitted fresh eye frames (bounded by a one-second backstop), and only
  the artwork releases on the quarter-second timer.
- The baked tip/level display is sharper, larger, and sits in the lower left
  like the native loading screen. The composite now renders at 2048x1152
  instead of 1024x576, and the plugin measures where the captured glyphs
  actually are so the text block itself is scaled to 70% of the screen width
  and anchored left, instead of shrinking the whole square UI surface into the
  middle of the frame. Placement is bounded (magnification cap, D3D11 viewport
  range check) and falls back to the previous fit if a capture measures
  implausibly small.

## 2.0.7

Fixes the two problems 2.0.6 left behind in VR mode 3.

- Tips and player level now capture correctly. Scaleform rebinds the Direct3D
  viewport during the loading-menu draw and never restores it, so the plugin's
  check that the render target was unchanged before and after that draw rejected
  every single capture. The capture now reads back from the surface it already
  holds a reference to, which is the surface the menu drew into, instead of
  requiring it to still be bound.
- Removed the background image that lingered after a load and could still be on
  screen when the next transition started. The 2.0.6 hold kept the art up for a
  full second after the load ended, anchored where the player had been standing
  when it began; it is now a quarter second, below the engine's shortest door
  fade. Suppressing the loading menu's own tail frames, not the hold, is what
  removes the black flash, and that is unchanged.
- The mode-3 capture window is down from 2.5 seconds to 0.6, so a capture that
  cannot complete no longer delays the loading-speed freeze. The 2.5 second
  value in 2.0.6 rested on a mistaken reading of when the loading menu starts
  rendering; it starts within a frame or two of the screen opening.
- A loading screen that OpenVR refuses to hide no longer has its image released
  while it may still be on screen.

## 2.0.6

VR mode-3 (Background + Tips) presentation timing.

- Tips and player level now actually appear on the custom background. Fallout
  hands LoadingMenu to its screen-space menu renderer only after the engine's
  fade-in, so the previous 100/500 ms animation-loop freeze always ran before
  the tip capture could arm. While that capture is live, mode 3 now waits up to
  2.5 seconds; a failed or disabled capture keeps the original freeze deadlines,
  so the loading-speed floor is unchanged in every other case.
- Removed the black frame at the end of a load. Bethesda's loading-menu close
  event fires while the menu is still fading out, so hiding the custom overlays
  there exposed the native screen's tail frames. The custom art is now held for
  a bounded moment past that boundary and the menu's own post-close renders are
  suppressed, with both windows cleared by the next loading screen.
- The compositor now holds its own reference on every texture it publishes to
  SteamVR, so a background retired by the next-image worker can no longer be
  freed while an overlay is still displaying it.
- Added per-load mode-3 capture diagnostics to the log.

## 2.0.5

VR mode-3 presentation and transition safety correction.

- Removed the legacy post-load `player.moveto player` workaround that opened a
  second, short LoadingMenu after the real load had already closed.
- Replaced submitted-eye tip extraction with an exact before/after delta around
  the vtable-verified native LoadingMenu movie draw. Unchanged world, controller,
  and hand pixels cannot enter the custom loading-screen texture.
- Native `SendLoadingText` completion now gates capture, preserving Bethesda's
  `SetLoadingText` then `SetLevel` order so mode 3 includes the current tip and
  player level.
- Spinner suppression targets only `VaultTecLogo_mc`, preserves its original
  SWF visibility value, and fails closed to background-only mode if that state
  cannot be safely captured.
- Mode-3 hooks are batch-enabled before the first timed load, eliminating three
  separate process-wide hook suspensions observed during VR startup.
- D3D capture now uses owned subresource copies, bounded allocation, complete
  shader-stage restoration, a verified viewport/scissor crop, and synchronized
  COM ownership/device teardown.
- Native CLOSE now seals and hides the custom presentation before GPU-query
  retirement or background-worker release. DDS publication is serialized with
  native OPEN, so neither cleanup nor a completed asynchronous upload can swap
  a second image into an active loading screen.
- Render-thread fallback no longer traverses Scaleform to restore the spinner;
  the original SWF state is restored only from the game-thread menu boundary.

## 2.0.4

Emergency VR startup crash correction.

- Removed the VR `BackgroundScreenModel::InitModels` RET patch. Fallout requires
  the state built by this initializer even when custom overlays own presentation;
  skipping it produced a repeatable null dereference in the startup LoadingMenu
  at VR RVA `0x9E4F3D`.
- Retained the safe, exact-byte `SetForegroundModel` call suppression and the
  v1.0 animation-loop speed patch.
- Required model initialization now completes normally behind the custom
  background/black blocker.

## 2.0.3

VR first-load presentation correction.

- VR DDS backgrounds are normalized off the load path to an OpenVR-compatible
  single-mip RGBA texture, fixing `VROverlayError_InvalidTexture` for the
  packaged BC1/BC3 landscape art.
- Loading-model creation and foreground selection are suppressed during plugin
  load, before Fallout can create the persistent startup model.
- If custom art or tips cannot attach, the validated black blocker remains
  visible until Bethesda's native LoadingMenu close instead of exposing a stale
  native frame.
- Overlay diagnostics now name OpenVR errors and report texture/device details.

## 2.0.2

Full correctness, compatibility, and reproducibility hardening pass.

- Exact runtime/editor checks now protect legacy F4SE Query and modern Load.
- VR OpenVR submission and executable-patch ownership were hardened.
- The unsafe live ten-byte PresentThread rewrite is no longer applied; every
  remaining code patch requires its exact supported-runtime signature.
- Custom VR presentation now ends at Bethesda's native LoadingMenu close.
- VR backgrounds are presented on a 16:9 landscape quad, custom tip/level mode
  suppresses the native spinner, and first-load timer/model hooks are armed
  before the first timed save load.
- Diagnostic worldspace ABI and hot-path logging were corrected and bounded.
- Native and plugin door-preload cooldowns are scoped to the current session.
- Save cache warming was reintroduced as a read-only feature with bounded work
  and a bounded hand-off to the real loader.
- AE timed loads no longer include synchronous MCM/config file reads.
- Papyrus budget updates are parked in menus and loads and defer to HFPF when it
  owns the same setting.
- The plugin never raises the Fallout process priority; background DDS and save
  cache workers run below normal priority.
- The first landscape background is prewarmed after D3D initialization, and
  background work is prevented from starting an upload during an active load.
- Door/gate polling remains parked in the main menu until the first live game
  session. Gate candidates are distance-sorted and deduplicated after Fallout's
  collection lock is released, with at most one new destination submitted per
  polling episode.
- Exterior-gate preloading intentionally ships on at one cell; crosshair and
  extended Havok paths remain off.
- Vanilla fade mode captures the running executable/INI's six fade and
  scene-settle values before any custom write and restores that snapshot.
  Custom values remain visual/timing changes, not cell/save I/O optimizations.
- Release validation now structurally checks MCM controls, rejects duplicate
  control IDs and active INI keys, requires the full-plugin benchmark arm and
  explicit custom-fade/preload defaults, verifies the compiled performance-patch
  policy, and enforces the complete 80-image 2048x1024 landscape art set.

## 2.0.1

Correctness and stability release. Fixes the permanent black-screen reports, recovers VR load speed, and removes a startup crash source. No config changes required; existing settings carry over.

### Fixed - black screen after elevators / scripted transitions (NG/AE)
The most-reported bug. Elevators (ArcJet, Vault 111 exit), the cryo intro, and other menu-less transitions could leave the screen permanently black with audio still playing.

Root cause was in the Next-Gen loading state machine: those transitions engage the custom loading screen through the engine load hooks but never fire the normal close signal, and a fast transition could also have its close signal consumed before the screen was even shown. Either way the compositor stayed on forever.

- The loading screen close is now guaranteed to arm whenever a load ends, including menu-less transitions.
- A fast/cached transition that finishes before the screen appears now cancels cleanly instead of stranding a black frame.
- A 120-second watchdog force-clears the screen if anything ever gets stuck, so a black screen can no longer be permanent.
- Save-into-interior loads no longer get forced to the seamless-black path.

This also resolves the "framerate stays uncapped / conflicts with Frame Generation after a load" reports, which were the same stuck state leaving vsync and the frame cap latched.

### Fixed - slower VR loading vs the old VR-only mod
VR loads were slower than the previous VR-only build. The animation-loop freeze (the main VR speed lever) was tied to the tips-capture pipeline, so on quick reloads or when tips capture was slow the freeze never engaged and the whole load ran at reduced speed.

- The freeze now always engages via a watchdog, even when tips aren't captured, matching the old mod's speed floor while keeping tips when they are available.
- VSync-during-loading is now disabled on VR (it was only ever correct to leave on for flat, not VR).
- Removed a load-close stall (up to ~250 ms) and reduced logging overhead during loads.

### Fixed - crosshair exterior door preload policy
With the extended-range ray off, crosshair-aimed exterior gate preloads could
silently do nothing while still reporting success. The crosshair path no longer
depends on the extended ray, but exterior destinations remain guarded by the
separate experimental exterior-gate policy, which ships off by default.

### Removed - Save prefetch
The save-warming feature was removed. It could crash the game at the main menu for characters or saves with non-Latin names, and its benefit was marginal.

### Hardened
- Loading-screen engine patches on Next-Gen now refuse to apply on unrecognized game versions rather than risk patching the wrong code (protects against future game updates).
- Runtime support is now exact: VR 1.2.72, OG 1.10.163, and AE 1.11.221. Builds 1.10.980, 1.10.984, and 1.11.191 are refused because their full hook sets have not been verified.
- Config values (fade durations, extended-range level, background width) are clamped to safe ranges.
- Various threading and lifecycle fixes found in a full code audit.

## 2.0.0

- Renamed to Faster Loadscreens; one DLL for supported Fallout 4 VR, OG, and AE runtimes.
- Four loading screen modes (Black / Native / Background / Background + Tips), selectable in the MCM.
- Crosshair door cell preload (look-ahead loading) with optional extended-range ray.
- MCM controls for loading mode, crosshair preload, extended range, vanilla fades, and individual fade sliders.
- Engine loading-speed patches: animation-loop freeze, background-load budget raise, fade trims, process-priority boost during loads.
