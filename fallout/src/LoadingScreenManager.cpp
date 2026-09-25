#include "PCH.h"
#include "LoadingScreenManager.h"
#include "CellWorldspaceGuard.h"
#include "RuntimePolicy.h"
#include "VRCompositorHelper.h"
#include "D3D11Compositor.h"
#include "PerformancePatches.h"

namespace VRLoadingScreens
{
    // Loading loop exit: CMP [RSI+0x68],2 (4 bytes) + JZ back-to-top (6 bytes) = 10 bytes
    // Same RVA in VR and OG 1.10.163 (confirmed via Ghidra)
    static constexpr std::uintptr_t AnimationLoop_CMP_Offset = 0xd07573;

    static constexpr std::uint8_t NOP10[] = {
        0x0F, 0x1F, 0x44, 0x00, 0x00,
        0x0F, 0x1F, 0x44, 0x00, 0x00
    };

    // March-compatible VR post-close handoff. Restore Fallout's native loop at
    // CLOSE, let it render underneath the opaque artwork for 200 ms, then remove
    // the complete custom stack in one game-thread step. Every successful save
    // load retains that stack across one real self-MoveTo transition.

    // ---------------------------------------------------------------------
    // VR playspace reposition-hold repair (Fallout 4 VR 1.2.72 only).
    //
    // Root cause of the long-standing "body stops following stick turning
    // after loading a save" bug, established by disassembly:
    //
    //   The VR playspace singleton (pointer global at RVA 0x59429C0) holds a
    //   reposition freeze counter at +0x240. Every transition-begin
    //   (FUN_140f2b1f0, called by the save-load path at three sites inside
    //   BGSSaveLoadGlobalData::LoadLocationData) raises it via BeginReposition
    //   (0x141ba84a0), NULLs the cached player-3D node at +0x248, and stages
    //   the destination room-yaw matrix into +0x1E0. EndReposition
    //   (0x141ba84f0) decrements it, and
    //   the call that brings it to zero commits the staged yaw into the live
    //   matrix at +0x210 and sets the committed flag +0x26D, which is what
    //   releases the engine's own 3-stage handshake in the per-frame rig
    //   updater and re-links body heading to the room yaw.
    //
    //   On the save-load path the only balancing release is a side effect of
    //   PlayerCharacter::InitLoadGame and the loading pump's thread bracket;
    //   when those land in the wrong order the load ends with the counter
    //   still above zero. Turning keeps writing the live matrix (so the view
    //   still turns) while the body-follow machinery waits forever on a
    //   handshake that can no longer start. `player.moveto player` cured it
    //   because its executor calls EndReposition directly.
    //
    // FIELD CORRECTION (v2.1.4). The counter is NOT what leaks. Live logs at
    // a save-load close read: counter=0, committed=1, cachedNode=0,
    // DETACHED=1. The stuck field is the playspace's "detached" byte (+0x271),
    // and it has exactly four writers:
    //     0x140d07349  detached := 1   loading-pump PROLOGUE
    //     0x140d07584  detached := 0   loading-pump EPILOGUE
    //     0x140f2b26c  detached := 1   transition-begin (the save-load path)
    //     0x140f10a1e  detached := 0   per-frame handshake stage 3, which
    //                                  pairs the clear with player+0x12A3 |= 2
    // This plugin's animation-loop NOP makes the pump body run once and fall
    // straight through, so each pump signal produces an immediate
    // prologue(1)/epilogue(0) pair at the START of the load instead of holding
    // the flag for its duration. When transition-begin then sets the flag and
    // no further pump signal arrives, nothing clears it: the handshake's own
    // clear is unreachable in this state (it is gated further up that
    // function). Vanilla spins the loop for the whole load, so its clear
    // always lands after. The NOP turns a guaranteed ordering into a race,
    // which is why this bug has shadowed the mod since v1.0 and is ours.
    //
    // Why it disconnects the body: GetViewRotation (0x141ba7c60) branches on
    // +0x271 in its FIRST instruction and, when set, returns the RAW HMD pose
    // without composing the artificial-turn room yaw (+0x210). The per-frame
    // body code then forces the actor heading to that raw value, so stick
    // turning moves the view while the body and hands stay pinned to the
    // physical head direction. Clearing the byte restores composition, which
    // is exactly what the pump epilogue does at its natural time.
    //
    // The repair is therefore: clear a stuck detached flag, and drain the
    // counter if it too was left positive.
    // CRITICAL: EndReposition commits the staged matrix over the live one
    // whenever the counter is already zero, which would discard the player's
    // accumulated turn, so it must be called ONLY while the counter is
    // positive and never once more after it reaches zero.
    // ---------------------------------------------------------------------
    // Post-load world-presentation state (Fallout 4 VR 1.2.72 only).
    //
    // CORRECTED after two failed attempts. There is NO monoscopic window in
    // VR: per-eye projection/eye-to-head setup is never disabled by anything
    // load-related, and Main::RenderFrame (0x140d831f0) IS the VR path but is
    // a single top-level pass with both eyes baked into one world draw, so
    // nothing up there ever reads as "mono vs stereo".
    //
    // The byte an earlier gate waited on, renderer object (RVA 0x6239340)
    // byte[3], is the `iScopeEnabled:VR` weapon-scope flag (setting name read
    // out of the binary; sole setter 0x141d947a0 from 0x140efaace). It is 0 in
    // all normal play by construction, so that gate was structurally incapable
    // of firing and silently turned its own timeout into the hold length.
    //
    // What actually exists is the engine's FREEZE-FRAME micro-load: after a
    // load, if the new cell's combined objects are not yet resident,
    // Main::OnIdle sets g_freezeRequested (0x140d84049) and FUN_140d87a80
    // captures one still and re-blits it every frame, head-locked, until the
    // priority-4 IO flush clears it (0x140d83fef). A head-locked still is what
    // reads as "the world briefly shown in 2D before it switches to 3D".
    // Vanilla hides it behind a LoadingMenu this plugin suppresses.
    //
    // The title-screen flash was a different branch entirely: with player 3D
    // still null, Main::RenderFrame takes its menu-only path (0x140d83821) and
    // draws the MainMenu movie with no world at all.
    //
    // These are the correct, PROVABLY-CHANGING signals (complete .text xref):
    //   g_renderWorld3D   0x5B042F4  byte
    //   g_frameWasFrozen  0x5B041CA  byte, ONE writer (0x140d87bfd), written
    //                                every world frame; 0 in live rendering
    //   g_freezeRequested 0x5B041CB  byte, writers 0x140d84049 (=1) /
    //                                0x140d83fef (=0)
    //   g_microLoadPending 0x5B04560 byte
    // The freeze publish happens BEFORE the eye submit, so sampling from the
    // Submit callback describes the frame being submitted.
    constexpr std::uintptr_t VRRenderWorld3D_Offset_VR   = 0x5b042f4;
    constexpr std::uintptr_t VRFrameWasFrozen_Offset_VR  = 0x5b041ca;
    constexpr std::uintptr_t VRFreezeRequested_Offset_VR = 0x5b041cb;
    constexpr std::uintptr_t VRMicroLoadPending_Offset_VR = 0x5b04560;

    constexpr std::uintptr_t VRPlayspacePointer_Offset_VR      = 0x59429c0;
    constexpr std::uintptr_t VRPlayspaceEndReposition_Offset_VR = 0x1ba84f0;
    constexpr std::ptrdiff_t kVRPlayspaceRepositionCounter = 0x240;
    constexpr std::ptrdiff_t kVRPlayspaceCachedNode        = 0x248;
    constexpr std::ptrdiff_t kVRPlayspaceCommitVeto        = 0x26c;
    constexpr std::ptrdiff_t kVRPlayspaceCommitted         = 0x26d;
    constexpr std::ptrdiff_t kVRPlayspaceDetached          = 0x271;
    constexpr std::ptrdiff_t kVRPlayspaceHandshake         = 0x273;
    constexpr std::ptrdiff_t kVRPlayspaceUpdateEnabled     = 0x275;
    // The staged and live room-yaw blocks EndReposition copies between: three
    // consecutive 16-byte NiMatrix3 rows each (+0x1E0..+0x20F -> +0x210..0x23F).
    constexpr std::ptrdiff_t kVRPlayspaceStagedYaw = 0x1e0;
    constexpr std::ptrdiff_t kVRPlayspaceLiveYaw   = 0x210;
    constexpr std::size_t    kVRPlayspaceYawBytes  = 0x30;
    // A leaked hold is a small positive count. Anything larger means the
    // layout assumption is wrong, so refuse rather than spin on live state.
    constexpr int kVRPlayspaceMaxDrain = 8;
    static_assert(Policy::IsVRPlayspaceSnapshotSane(
        kVRPlayspaceMaxDrain, 1));
    static_assert(!Policy::IsVRPlayspaceSnapshotSane(
        kVRPlayspaceMaxDrain + 1, 1));
    // The post-close callback is not reliably HMD-rate: the latest field run
    // needed 7.7 seconds to collect 60 samples. Use elapsed steady-clock time
    // plus a small minimum sample count, preserving the intended dwell without
    // silently stretching the transition by many seconds.
    constexpr int kVRPlayspaceDwellMs =
        static_cast<int>(Policy::kVRPlayspaceRepairDwellMs);
    constexpr int kVRPlayspaceDwellMinSamples =
        Policy::kVRPlayspaceRepairMinimumSamples;

    // Post-close stereo-signal probes (diagnostic): a handful of samples
    // spread across the window where the engine is expected to resume stereo.
    constexpr int kVRStereoProbeCount = 4;
    constexpr int kVRStereoProbeIntervalMs = 250;

    namespace
    {

    struct VRPlayspaceSnapshot
    {
        std::uintptr_t base = 0;
        std::int32_t   counter = 0;
        std::uintptr_t cachedNode = 0;
        std::uint8_t   commitVeto = 0;
        std::uint8_t   committed = 0;
        std::uint8_t   detached = 0;
        std::uint8_t   handshake = 0;
        std::uint8_t   updateEnabled = 0;
        bool           valid = false;
    };

    [[nodiscard]] bool VRPlayspaceSupported() noexcept
    {
        if (!REL::Module::IsVR()) {
            return false;
        }
        const auto version = REL::Module::get().version();
        return version[0] == 1 && version[1] == 2 && version[2] == 72;
    }

    // Structured exception handling keeps a wrong-address read from taking the
    // process down; no unwinding objects may live in this frame.
    [[nodiscard]] bool TryReadVRPlayspace(
        std::uintptr_t a_base, VRPlayspaceSnapshot& a_out) noexcept
    {
        __try {
            a_out.base = a_base;
            a_out.counter = *reinterpret_cast<const std::int32_t*>(
                a_base + kVRPlayspaceRepositionCounter);
            a_out.cachedNode = *reinterpret_cast<const std::uintptr_t*>(
                a_base + kVRPlayspaceCachedNode);
            a_out.commitVeto = *reinterpret_cast<const std::uint8_t*>(
                a_base + kVRPlayspaceCommitVeto);
            a_out.committed = *reinterpret_cast<const std::uint8_t*>(
                a_base + kVRPlayspaceCommitted);
            a_out.detached = *reinterpret_cast<const std::uint8_t*>(
                a_base + kVRPlayspaceDetached);
            a_out.handshake = *reinterpret_cast<const std::uint8_t*>(
                a_base + kVRPlayspaceHandshake);
            a_out.updateEnabled = *reinterpret_cast<const std::uint8_t*>(
                a_base + kVRPlayspaceUpdateEnabled);
            a_out.valid = true;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a_out.valid = false;
            return false;
        }
    }

    [[nodiscard]] bool TryWriteByteSafe(
        std::uintptr_t a_address, std::uint8_t a_value) noexcept
    {
        __try {
            *reinterpret_cast<std::uint8_t*>(a_address) = a_value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    [[nodiscard]] bool TryReadByteSafe(
        std::uintptr_t a_address, std::uint8_t& a_out) noexcept
    {
        __try {
            a_out = *reinterpret_cast<const std::uint8_t*>(a_address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    [[nodiscard]] std::uint8_t ReadVRFlagByte(
        std::uintptr_t a_offset) noexcept
    {
        REL::Relocation<std::uintptr_t> flag{ REL::Offset(a_offset) };
        std::uint8_t value = 0;
        return TryReadByteSafe(flag.address(), value)
            ? value
            : static_cast<std::uint8_t>(0xFF);
    }

    // Diagnostic snapshot of the real post-load presentation state.
    //
    // a_includePlayer3D is OPT-IN and must only be passed from the game thread.
    // PlayerCharacter::Get3D() walks loadedData, which the engine is actively
    // rebuilding across the whole post-close window; reading it from the OpenVR
    // submit thread is an unsynchronized traversal of an object being swapped
    // under us, and unlike the flag bytes below it has no SEH guard. 2.1.7 did
    // exactly that on every render tick after CLOSE, under a "diagnostics only,
    // no behaviour change" label, and reintroduced the end-of-load flash that
    // 2.1.6 was confirmed clean of. The flag bytes are plain globals and safe
    // from any thread.
    [[nodiscard]] std::string DescribeVRPresentationState(
        bool a_includePlayer3D = false) noexcept
    {
        if (!VRPlayspaceSupported()) {
            return "unsupported";
        }
        auto out = std::format(
            "world3D={} frozenFrame={} freezeReq={} microLoad={}",
            ReadVRFlagByte(VRRenderWorld3D_Offset_VR),
            ReadVRFlagByte(VRFrameWasFrozen_Offset_VR),
            ReadVRFlagByte(VRFreezeRequested_Offset_VR),
            ReadVRFlagByte(VRMicroLoadPending_Offset_VR));
        if (a_includePlayer3D) {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            out += std::format(
                " player3D={}", player && player->Get3D() ? 1 : 0);
        }
        return out;
    }

    // True when the engine is presenting the live world normally: world
    // rendering enabled and the last world frame was a real render rather than
    // a re-blitted freeze-frame still. Byte reads only — see the note above on
    // why the player-3D term cannot live on this path, which is called from the
    // render thread. Nothing gates presentation on this; it is a log signal.
    [[nodiscard]] bool VRWorldPresentationLive() noexcept
    {
        if (!VRPlayspaceSupported()) {
            return true;
        }
        const auto world = ReadVRFlagByte(VRRenderWorld3D_Offset_VR);
        const auto frozen = ReadVRFlagByte(VRFrameWasFrozen_Offset_VR);
        if (world == 0xFF || frozen == 0xFF) {
            // Unreadable: report live so a caller can never stall on it.
            return true;
        }
        return world != 0 && frozen == 0;
    }

    // Behavioural gate variant of the diagnostic above. Unlike
    // VRWorldPresentationLive it never fails open and includes every known
    // transition byte; unreadable state must wait for the bounded fallback.
    [[nodiscard]] bool TryReadVRWorldTransitionReady(bool& a_ready) noexcept
    {
        a_ready = false;
        if (!VRPlayspaceSupported()) {
            return false;
        }
        const auto world = ReadVRFlagByte(VRRenderWorld3D_Offset_VR);
        const auto frozen = ReadVRFlagByte(VRFrameWasFrozen_Offset_VR);
        const auto freezeRequested =
            ReadVRFlagByte(VRFreezeRequested_Offset_VR);
        const auto microLoad = ReadVRFlagByte(VRMicroLoadPending_Offset_VR);
        if (world == 0xFF || frozen == 0xFF || freezeRequested == 0xFF ||
            microLoad == 0xFF) {
            return false;
        }
        a_ready = world != 0 && frozen == 0 && freezeRequested == 0 &&
            microLoad == 0;
        return true;
    }

    [[nodiscard]] std::uintptr_t DerefPointerSafe(
        std::uintptr_t a_slot) noexcept
    {
        __try {
            return *reinterpret_cast<const std::uintptr_t*>(a_slot);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    [[nodiscard]] std::uintptr_t ReadVRPlayspaceBase() noexcept
    {
        if (!VRPlayspaceSupported()) {
            return 0;
        }
        REL::Relocation<std::uintptr_t*> slot{
            REL::Offset(VRPlayspacePointer_Offset_VR)
        };
        return DerefPointerSafe(
            reinterpret_cast<std::uintptr_t>(slot.get()));
    }

    // Copies the LIVE room-yaw over the STAGED one so the commit EndReposition
    // performs when the counter reaches zero writes live-over-live: the drain
    // then repairs the counter and the committed flag without moving the view
    // by a single degree. This also defuses any later stray engine
    // EndReposition arriving at counter zero, which would otherwise commit a
    // stale staged matrix and snap the player's heading.
    bool NeutralizeVRPlayspaceStagedYaw(std::uintptr_t a_base) noexcept
    {
        __try {
            std::memcpy(
                reinterpret_cast<void*>(a_base + kVRPlayspaceStagedYaw),
                reinterpret_cast<const void*>(a_base + kVRPlayspaceLiveYaw),
                kVRPlayspaceYawBytes);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Drains a leaked reposition hold. Runs on the game thread only. Returns
    // the number of EndReposition calls issued.
    int DrainVRPlayspaceHold(std::uintptr_t a_base) noexcept
    {
        using EndReposition_t = void (*)(std::uintptr_t);
        REL::Relocation<EndReposition_t> endReposition{
            REL::Offset(VRPlayspaceEndReposition_Offset_VR)
        };

        int calls = 0;
        while (calls < kVRPlayspaceMaxDrain) {
            VRPlayspaceSnapshot state{};
            // Re-read every iteration: the counter is main-thread state and
            // this loop is the only writer here, but a stale read must never
            // authorize the call that would commit at zero.
            if (!TryReadVRPlayspace(a_base, state) || state.counter <= 0) {
                break;
            }
            endReposition(a_base);
            ++calls;
        }
        return calls;
    }

    }  // namespace

    // Mode-3 exact-delta capture window. The capture arms on the first native
    // LoadingMenu movie draw, which the log shows arriving 9-170 ms after OPEN,
    // and a completed capture requests the animation-loop freeze itself. This
    // is only the ceiling for a capture that never completes, kept close to
    // v1.0's 500 ms first-load deadline so a broken capture cannot cost real
    // loading time. A dead pipeline still uses the v1.0 100/500 ms deadlines.

    static bool TryReadLoopBytes(std::uintptr_t address, std::uint8_t (&bytes)[10])
    {
        __try {
            if (!address) return false;
            std::memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool LoopBytesEqual(
        std::uintptr_t address, const std::uint8_t (&expected)[10])
    {
        std::uint8_t current[10]{};
        return TryReadLoopBytes(address, current) &&
            std::memcmp(current, expected, sizeof(current)) == 0;
    }

    // LoadingMenu.as exposes the animated bottom-right throbber as
    // VaultTecLogo_mc. Toggle that one display object instead of shipping a
    // replacement SWF, so native mode and any custom-pipeline fallback retain
    // Bethesda's original UI. NG/AE uses a separate native-boundary path because
    // this CommonLib fork cannot safely retrieve its UI singleton.
    static bool SetNativeLoadingSpinnerVisible(bool visible)
    {
        if (REL::Module::IsNG() && !REL::Module::IsVR()) {
            return false;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        auto menu = ui->GetMenu(RE::LoadingMenu::MENU_NAME);
        if (!menu || !menu->menuObj.IsObject()) {
            return false;
        }
        RE::Scaleform::GFx::Value spinner;
        if (!menu->menuObj.GetMember("VaultTecLogo_mc", &spinner) ||
            !spinner.IsObject()) {
            return false;
        }
        const RE::Scaleform::GFx::Value value{ visible };
        return spinner.SetMember("visible", value);
    }

    static bool GetNativeLoadingSpinnerVisible(bool& visible)
    {
        if (REL::Module::IsNG() && !REL::Module::IsVR()) {
            return false;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        auto menu = ui->GetMenu(RE::LoadingMenu::MENU_NAME);
        if (!menu || !menu->menuObj.IsObject()) {
            return false;
        }
        RE::Scaleform::GFx::Value spinner;
        RE::Scaleform::GFx::Value value;
        if (!menu->menuObj.GetMember("VaultTecLogo_mc", &spinner) ||
            !spinner.IsObject() ||
            !spinner.GetMember("visible", &value) ||
            !value.IsBoolean()) {
            return false;
        }
        visible = value.GetBoolean();
        return true;
    }

    bool LoadingScreenManager::HideNativeLoadingSpinnerForCustomScreen()
    {
        // If restoration from a prior load is still pending, retain that load's
        // original value. Overwriting it with the currently hidden value would
        // make the suppression permanent.
        if (!m_spinnerRestorePending &&
            !GetNativeLoadingSpinnerVisible(m_spinnerOriginalVisible)) {
            return false;
        }
        if (!SetNativeLoadingSpinnerVisible(false)) {
            return false;
        }
        m_spinnerRestorePending = true;
        return true;
    }

    bool LoadingScreenManager::RestoreNativeLoadingSpinner()
    {
        if (!m_spinnerRestorePending) {
            return true;
        }
        if (!SetNativeLoadingSpinnerVisible(m_spinnerOriginalVisible)) {
            return false;
        }
        m_spinnerRestorePending = false;
        return true;
    }

    LoadingScreenManager::~LoadingScreenManager()
    {
        // Heartbeat FIRST. This manager's singleton is constructed before
        // D3D11Compositor's, so it is destroyed last - and the heartbeat loop
        // dereferences that already-destroyed compositor every tick. jthread's
        // own destructor would only stop it after this body runs, which is too
        // late.
        if (m_loadHeartbeatThread.joinable()) {
            m_loadHeartbeatThread.request_stop();
            m_loadHeartbeatThread.join();
        }
        if (m_bgPrepareThread.joinable()) {
            m_bgPrepareThread.request_stop();
            m_bgPrepareThread.join();
        }
        if (auto* texture = m_currentBgTexture.exchange(nullptr)) {
            VRCompositorHelper::ReleaseTexture(texture);
        }
        if (m_retiredBgTexture) {
            VRCompositorHelper::ReleaseTexture(m_retiredBgTexture);
            m_retiredBgTexture = nullptr;
        }
    }

    void LoadingScreenManager::SetGameSessionLoaded()
    {
        std::lock_guard lock(m_stateMutex);
        m_gameSessionLoaded = true;
    }

    void LoadingScreenManager::ClearGameSessionLoaded()
    {
        std::lock_guard lock(m_stateMutex);
        m_gameSessionLoaded = false;
        const bool marchPresentationRetained =
            m_vrMarchPhase != VRMarchPhase::kIdle ||
            m_vrPresentationReleaseGeneration != 0;
        m_vrMarchPhase = VRMarchPhase::kIdle;
        m_vrMarchSourceGeneration = 0;
        m_vrMarchChainedGeneration = 0;
        m_vrMarchCommandIssued = false;
        m_vrMarchDelayMs = 0;
        m_vrPresentationReleaseAt = {};
        m_vrPresentationReleaseGeneration = 0;
        // The session this repair belonged to is gone (quit to main menu, or a
        // load that never attached). Drop it rather than leaving the per-frame
        // cell walk armed across the whole main menu.
        if (m_vrPlayspaceRepairPending.exchange(
                false, std::memory_order_acq_rel)) {
            logger::info(
                "VR playspace: pending repair dropped at session end");
        }
        m_vrPlayspaceRepairGeneration.store(0, std::memory_order_release);
        m_vrPlayspaceRepairReadyGeneration.store(
            0, std::memory_order_release);
        m_vrPlayspaceLastCounter = -1;
        m_vrPlayspaceStableSamples = 0;
        m_vrPlayspaceStableSince = {};
        m_vrPostCloseResolvedGeneration.store(
            0, std::memory_order_release);
        m_vrPostCloseHealthySamples = 0;
        m_vrPostCloseBackstopLogged = false;
        if (m_vrPostCloseGateGeneration.exchange(
                0, std::memory_order_acq_rel) != 0) {
            // MainMenu has become the legitimate owner again. Do not leave a
            // save handoff overlay covering it.
            VRCompositorHelper::EndLoadingPresentationNow();
            logger::info(
                "VR handoff: cancelled because the game session ended");
        }
        if (marchPresentationRetained) {
            m_bgWorkState.fetch_and(
                ~kBgLoadActive, std::memory_order_release);
            // MainMenu owns presentation again. A failed/missing MoveTo OPEN
            // must never leave March's retained artwork above the real menu.
            VRCompositorHelper::EndLoadingPresentationNow(0.0f, false);
            VRCompositorHelper::ClearSkybox();
            logger::info(
                "VR March handoff: retained presentation cancelled at "
                "session end");
        }
        if (m_isVR) {
            // The save suppression may intentionally outlive an already
            // completed visual release. MainMenu OPEN makes it invalid even
            // when the gate generation has already been cleared.
            D3D11Compositor::GetSingleton()
                .BeginPostCloseLoadingMenuSuppression(0);
        }
    }

    void LoadingScreenManager::SetBackgroundsEnabled(bool enabled)
    {
        bool startWorker = false;
        {
            std::lock_guard lock(m_stateMutex);
            const bool effectiveEnabled = enabled &&
                !m_timingOnly.load(std::memory_order_acquire);
            const bool wasEnabled = m_backgroundsEnabled.exchange(
                effectiveEnabled, std::memory_order_acq_rel);
            startWorker = effectiveEnabled && !wasEnabled &&
                !m_currentBgTexture.load(std::memory_order_acquire);
        }
        if (startWorker) {
            PrepareNextBackgroundAsync();
        }
    }

    void LoadingScreenManager::SetOverlayMode(int mode)
    {
        std::lock_guard lock(m_stateMutex);
        m_overlayMode = mode;
    }

    void LoadingScreenManager::SetOverlayAlpha(float alpha)
    {
        std::lock_guard lock(m_stateMutex);
        m_overlayAlpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    bool LoadingScreenManager::IsVRPresentationProbeEnabled()
    {
        return GetSingleton().m_vrPresentationProbeEnabled.load(
            std::memory_order_acquire);
    }

    void LoadingScreenManager::SetTimingOnly(bool timingOnly)
    {
        std::lock_guard lock(m_stateMutex);
        m_timingOnly.store(timingOnly, std::memory_order_release);
        if (timingOnly) {
            // Timing-only is a true vanilla baseline: prevent both initial and
            // later config/session callbacks from launching DDS IO or upload.
            m_backgroundsEnabled.store(false, std::memory_order_release);
        }
    }

    void LoadingScreenManager::OnD3DDeviceReady()
    {
        if (!VRCompositorHelper::GetD3D11Device() ||
            m_timingOnly.load(std::memory_order_acquire) ||
            !m_backgroundsEnabled.load(std::memory_order_acquire)) {
            return;
        }

        // Device publication can happen from the first flat Present or from
        // early VR initialization. Prepare is single-flight and load-gated, so
        // duplicate notifications remain cheap and safe.
        if (!m_currentBgTexture.load(std::memory_order_acquire) ||
            m_bgPrepareDeferred.load(std::memory_order_acquire)) {
            PrepareNextBackgroundAsync();
        }
    }

    void LoadingScreenManager::OnD3DDeviceChanged()
    {
        m_d3dDeviceGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_bgPrepareDeferred.store(true, std::memory_order_release);
        m_bgRetryAfterTicks.store(0, std::memory_order_relaxed);

        {
            std::lock_guard stateLock(m_stateMutex);
            const bool marchPresentationRetained =
                m_vrMarchPhase != VRMarchPhase::kIdle ||
                m_vrPresentationReleaseGeneration != 0;
            m_visualPipelineActiveThisLoad = false;
            m_tipsPipelineActiveThisLoad = false;
            m_tipsOverlayAttached = false;
            m_vrDisplayTex = nullptr;
            m_bgPoseRelockFrames = 0;
            m_pendingLoopNOP = false;
            // The helper has just invalidated/destroyed the presentation's
            // device-owned resources. No queued March action may manufacture a
            // transition or release against those retired handles.
            m_vrMarchPhase = VRMarchPhase::kIdle;
            m_vrMarchSourceGeneration = 0;
            m_vrMarchChainedGeneration = 0;
            m_vrMarchCommandIssued = false;
            m_vrMarchDelayMs = 0;
            m_vrPresentationReleaseAt = {};
            m_vrPresentationReleaseGeneration = 0;
            if (marchPresentationRetained &&
                !m_inLoadingScreen.load(std::memory_order_acquire)) {
                m_bgWorkState.fetch_and(
                    ~kBgLoadActive, std::memory_order_release);
            }
            // Device loss is reported by Present/Submit on the render thread.
            // Do not traverse Scaleform/UI from that thread; CLOSE (or the next
            // native OPEN) restores a spinner hidden by this load.
        }

        void* current = nullptr;
        void* retired = nullptr;
        {
            // Serialize with the worker's retirement/publication transaction so
            // an old-device texture can never be published after invalidation.
            std::lock_guard textureLock(m_textureStateMutex);
            current = m_currentBgTexture.exchange(
                nullptr, std::memory_order_acq_rel);
            retired = m_retiredBgTexture;
            m_retiredBgTexture = nullptr;
            m_backgroundGeneration.fetch_add(1, std::memory_order_release);
        }
        VRCompositorHelper::ReleaseTexture(current);
        VRCompositorHelper::ReleaseTexture(retired);
        logger::warn(
            "LoadingScreenManager: old-device backgrounds invalidated; "
            "native presentation retained until landscape prewarm completes");
    }

    void LoadingScreenManager::StartLoadHeartbeat()
    {
        if (m_loadHeartbeatThread.joinable()) return;
        m_loadHeartbeatThread = std::jthread([this](std::stop_token a_stop) {
            constexpr auto kTick = std::chrono::milliseconds(500);
            constexpr auto kReportEvery = std::chrono::milliseconds(2000);
            auto& comp = D3D11Compositor::GetSingleton();
            bool wasLoading = false;
            std::chrono::steady_clock::time_point loadStart{};
            std::chrono::steady_clock::time_point lastReport{};
            int lastSubmits = 0;
            while (!a_stop.stop_requested()) {
                // Stop-aware wait. A flat 500 ms sleep made shutdown wait up to
                // half a second on a thread that dereferences the compositor
                // singleton, which is destroyed BEFORE this manager.
                for (int i = 0; i < 10 && !a_stop.stop_requested(); ++i) {
                    std::this_thread::sleep_for(kTick / 10);
                }
                if (a_stop.stop_requested()) break;
                // The MANAGER's own load flag, not the compositor's. The
                // compositor flag is only raised when the custom visual
                // pipeline owns the load, so keying on it left this heartbeat
                // blind in the iBenchmarkMode=0 control run, VR native mode 1,
                // every fallback load, and ALL flat loads - i.e. precisely the
                // comparison arms the long-load investigation depends on.
                const bool loading =
                    m_inLoadingScreen.load(std::memory_order_acquire);
                const auto now = std::chrono::steady_clock::now();
                if (loading && !wasLoading) {
                    loadStart = now;
                    lastReport = now;
                    lastSubmits = comp.TotalEyeSubmits();
                    wasLoading = true;
                    continue;
                }
                if (!loading) {
                    wasLoading = false;
                    continue;
                }
                if (now - lastReport < kReportEvery) continue;
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - loadStart).count();
                const auto sinceReport =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - lastReport).count();
                const int submits = comp.TotalEyeSubmits();
                // Eye submits per second is the key number: it separates "the
                // engine is busy streaming" from "the engine is stalled", which
                // the plugin log could not previously distinguish at all.
                const double fps = sinceReport > 0
                    ? (submits - lastSubmits) * 1000.0 / (2.0 * sinceReport)
                    : 0.0;
                lastReport = now;
                lastSubmits = submits;
                // State tag + honest rate. In timing-only mode the Submit hook
                // is never installed, so TotalEyeSubmits stays 0 - reporting
                // that as "0.0 pairs/s" would read as the engine being frozen
                // in exactly the control run built to test that. n/a is the
                // truthful value there.
                const bool timingOnly =
                    m_timingOnly.load(std::memory_order_acquire);
                // SteamVR grid alpha, probe-gated. Cross-thread IVRCompositor
                // reads are treated with suspicion in this codebase, so it is
                // opt-in - but it is the measurement that finally showed the
                // compositor sitting in its app-hang environment at 0.638 while
                // the game submitted nothing, so seeing WHEN it fades in during
                // a load is worth one call every 2 s of a diagnostic run.
                const float gridAlpha =
                    m_vrPresentationProbeEnabled.load(std::memory_order_acquire)
                        ? VRCompositorHelper::CurrentGridAlpha()
                        : -1.0f;
                // m_eyeSubmitTotal is only incremented by the VR Submit hook,
                // but IsInitialized() is also true for both flat init paths -
                // so on flat this reported a real-looking "0.0 pairs/s", which
                // reads as a frozen engine. VR-only, or the number is n/a.
                const bool submitHookLive = m_isVR && comp.IsInitialized();
                logger::info(
                    "Load heartbeat +{} ms: {} mode={} state={} "
                    "eyePairs/s={} totalSubmits={} gridAlpha={}",
                    elapsed,
                    m_isVR ? DescribeVRPresentationState() : std::string("flat"),
                    comp.GetFlatMode(),
                    timingOnly ? "timing-only"
                        : submitHookLive ? "active" : "no-submit-hook",
                    submitHookLive ? fmt::format("{:.1f}", fps)
                                   : std::string("n/a"),
                    submits,
                    gridAlpha < 0.0f ? std::string("off")
                                     : fmt::format("{:.3f}", gridAlpha));
            }
        });
    }

    void LoadingScreenManager::Init(bool enableBackgrounds, int overlayMode,
                                     float overlayAlpha)
    {
        m_backgroundsEnabled.store(enableBackgrounds &&
            !m_timingOnly.load(std::memory_order_acquire), std::memory_order_release);
        m_overlayMode = overlayMode;
        m_overlayAlpha = overlayAlpha;
        m_isVR = REL::Module::IsVR();

        const bool startHeartbeat = Policy::ShouldStartLoadHeartbeat(
            m_isVR,
            m_vrPresentationProbeEnabled.load(std::memory_order_acquire));
        if (startHeartbeat) {
            StartLoadHeartbeat();
        } else {
            logger::info(
                "VR March parity: normal load heartbeat disabled "
                "(enable bVRPresentationProbe for diagnostics)");
        }

        const bool timingOnly = m_timingOnly.load(std::memory_order_acquire);
        if (!timingOnly) {
            ScanForTextures();
            if (m_texturePaths.empty()) {
                logger::warn("No loading screen DDS textures found in Data/Textures/LoadingScreens/");
            } else {
                logger::info("Found {} loading screen textures", m_texturePaths.size());
            }
        } else {
            m_texturePaths.clear();
            logger::info("Timing-only: skipped loading-screen asset scan");
        }

        // Animation loop NOP (breaks loading render loop for massive speedup)
        // Dynamically NOP the CMP+JE that controls the animation loop during loading
        if (m_isVR) {
            REL::Relocation<std::uintptr_t> animCmp{ REL::Offset(AnimationLoop_CMP_Offset) };
            m_loopAddress = animCmp.address();
        } else if (!REL::Module::IsNG()) {
            // OG flat: CMP [RDI+0x68],2; JE back (10 bytes) at RVA 0xCBFFCD
            // Found via pattern scan — only match for this loop condition in the binary
            m_loopAddress = REL::Module::get().base() + 0xCBFFCD;
        }

        if (m_loopAddress) {
            // Verify bytes before saving. Expected sequence (OG 1.10.163):
            //   83 7F 68 02      CMP DWORD PTR [RDI+0x68], 2     (4 bytes)
            //   0F 84 ?? ?? ?? ?? JE near back                   (6 bytes)
            // Tighter than the original 3-of-10-byte check; verifies the full
            // CMP encoding and the JE opcode so an unrelated function that
            // happens to start with `83 ?? 68 02` isn't silently NOP'd.
            std::uint8_t candidate[10]{};
            const bool readable = TryReadLoopBytes(m_loopAddress, candidate);
            if (!readable) {
                logger::warn("Animation loop address {:x} is unreadable; patch disabled",
                    m_loopAddress);
            }
            const auto* b = candidate;
            const bool cmpOk = readable && (b[0] == 0x83 &&
                                (b[1] == 0x7F || b[1] == 0x7E) &&
                                b[2] == 0x68 && b[3] == 0x02);
            // We replace ten bytes, so accepting a two-byte short JZ would
            // overwrite four bytes of the following instruction.
            const bool jeOk = (b[4] == 0x0F && b[5] == 0x84);
            if (cmpOk && jeOk) {
                std::memcpy(m_originalLoopBytes, candidate, sizeof(candidate));
                m_originalBytesSaved = true;
                logger::info("Animation loop saved at {:x}", m_loopAddress);
            } else if (readable) {
                logger::warn("Animation loop bytes mismatch at {:x}; dynamic loop patch disabled",
                    m_loopAddress);
                m_loopAddress = 0;
            } else {
                m_loopAddress = 0;
            }
        }

        if (!timingOnly) {
            // Initialize compositor (VR: OpenVR + overlays, flat: D3D device only)
            logger::info("Init: calling VRCompositorHelper::Initialize()...");
            const bool helperReady = VRCompositorHelper::Initialize();
            const bool overlayReady = !m_isVR ||
                (helperReady && VRCompositorHelper::InitializeOverlay());
            logger::info("Init: VRCompositorHelper ready={}, overlay ready={}",
                helperReady, overlayReady);
        }

        if (m_backgroundsEnabled.load(std::memory_order_acquire)) {
            // Never block startup or Present on disk IO/decompression. Flat has
            // no device yet and retries from its render callback once published.
            PrepareNextBackgroundAsync();
        }
    }

    void LoadingScreenManager::ScanForTextures()
    {
        m_texturePaths.clear();
        std::filesystem::path searchDir = "Data/Textures/LoadingScreens";
        std::error_code ec;
        if (!std::filesystem::exists(searchDir, ec) || ec) return;

        std::filesystem::directory_iterator it(
            searchDir, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            std::error_code typeEc;
            if (!entry.is_regular_file(typeEc) || typeEc) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext == ".dds") {
                m_texturePaths.push_back("LoadingScreens/" + entry.path().filename().string());
            }
        }
        std::sort(m_texturePaths.begin(), m_texturePaths.end());
    }

    std::string LoadingScreenManager::PickRandomTexture()
    {
        std::lock_guard lock(m_textureStateMutex);
        if (m_texturePaths.empty()) return "";
        if (m_texturePaths.size() == 1) {
            m_lastIndex = 0;
            return m_texturePaths[0];
        }

        // Pick from [0, size-1) and skip past the last index — avoids the
        // rejection-sampling loop and constructing a distribution each retry.
        std::uniform_int_distribution<int> dist(0, static_cast<int>(m_texturePaths.size()) - 2);
        int idx = dist(m_rng);
        if (m_lastIndex >= 0 && idx >= m_lastIndex) ++idx;

        m_lastIndex = idx;
        return m_texturePaths[idx];
    }

    bool LoadingScreenManager::ShouldDeferBackgroundWork(void* context)
    {
        auto* self = static_cast<LoadingScreenManager*>(context);
        if (!self) return false;

        const bool loading =
            (self->m_bgWorkState.load(std::memory_order_acquire) &
             kBgLoadActive) != 0;
        if (loading) {
            self->m_bgPrepareDeferred.store(true, std::memory_order_release);
        }
        return loading;
    }

    bool LoadingScreenManager::TryBeginBackgroundUpload(void* context)
    {
        auto* self = static_cast<LoadingScreenManager*>(context);
        if (!self) return false;

        std::uint32_t state =
            self->m_bgWorkState.load(std::memory_order_acquire);
        for (;;) {
            if ((state & kBgLoadActive) != 0) {
                self->m_bgPrepareDeferred.store(true, std::memory_order_release);
                return false;
            }
            if ((state & kBgUploadAdmitted) != 0) return false;
            if (self->m_bgWorkState.compare_exchange_weak(
                    state, state | kBgUploadAdmitted,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    void LoadingScreenManager::EndBackgroundUpload(void* context)
    {
        if (auto* self = static_cast<LoadingScreenManager*>(context)) {
            self->m_bgWorkState.fetch_and(
                ~kBgUploadAdmitted, std::memory_order_release);
        }
    }

    void LoadingScreenManager::PrepareNextBackgroundAsync()
    {
        if (m_texturePaths.empty() ||
            m_timingOnly.load(std::memory_order_acquire) ||
            !m_backgroundsEnabled.load(std::memory_order_acquire)) return;
        if ((m_bgWorkState.load(std::memory_order_acquire) &
             kBgLoadActive) != 0) {
            m_bgPrepareDeferred.store(true, std::memory_order_release);
            return;
        }
        if (!VRCompositorHelper::GetD3D11Device()) return;
        const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
        if (nowTicks < m_bgRetryAfterTicks.load(std::memory_order_relaxed)) return;
        if (m_bgPrepareInFlight.exchange(true)) return;  // single-flight
        struct InFlightReset
        {
            std::atomic<bool>& flag;
            bool active = true;
            ~InFlightReset()
            {
                if (active) flag.store(false, std::memory_order_release);
            }
        } launchGuard{ m_bgPrepareInFlight };
        m_bgPrepareDeferred.store(false, std::memory_order_release);

        const std::string texturePath = PickRandomTexture();
        {
            std::lock_guard lock(m_textureStateMutex);
            m_currentTexturePath = texturePath;
        }
        logger::info("Next background (async): {}", texturePath);
        const std::string fullPath = "Data/Textures/" + texturePath;
        const std::uint64_t deviceGeneration =
            m_d3dDeviceGeneration.load(std::memory_order_acquire);

        std::lock_guard threadLock(m_bgThreadMutex);
        try {
            m_bgPrepareThread = std::jthread(
                [this, fullPath, deviceGeneration](std::stop_token stopToken) {
                try {
                    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)) {
                        logger::warn("Background worker: failed to set BELOW_NORMAL priority ({})",
                            GetLastError());
                    }

                    if (stopToken.stop_requested() ||
                        ShouldDeferBackgroundWork(this)) {
                        m_bgPrepareInFlight.store(false, std::memory_order_release);
                        return;
                    }

                    // Release the texture retired by the previous swap after a
                    // complete display cycle has elapsed.
                    void* retiredToRelease = nullptr;
                    {
                        std::lock_guard textureLock(m_textureStateMutex);
                        retiredToRelease = m_retiredBgTexture;
                        m_retiredBgTexture = nullptr;
                    }
                    VRCompositorHelper::ReleaseTexture(retiredToRelease);
                    void* tex = VRCompositorHelper::LoadDDSTexture(
                        fullPath,
                        &LoadingScreenManager::ShouldDeferBackgroundWork, this,
                        &LoadingScreenManager::TryBeginBackgroundUpload,
                        &LoadingScreenManager::EndBackgroundUpload);

                    bool published = false;
                    bool retiredDevice = false;
                    // A load can open after CreateTexture2D, or even between the
                    // outer barrier check and texture publication. Keep the
                    // completed texture on this below-normal worker and retry;
                    // never swap m_currentBgTexture while LoadingMenu is active.
                    //
                    // OnLoadingMenuOpen raises kBgLoadActive while holding the
                    // same short-lived texture mutex. The inner re-check is
                    // therefore the publication linearization point: either the
                    // texture belongs to the new OPEN from its first frame, or
                    // it remains private until CLOSE.
                    while (tex && !stopToken.stop_requested() &&
                        m_backgroundsEnabled.load(std::memory_order_acquire)) {
                        while (!stopToken.stop_requested() &&
                            (m_bgWorkState.load(std::memory_order_acquire) &
                             kBgLoadActive) != 0) {
                            m_bgPrepareDeferred.store(
                                true, std::memory_order_release);
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                        }
                        if (stopToken.stop_requested()) break;

                        bool loadRacedPublication = false;
                        {
                            std::lock_guard textureLock(m_textureStateMutex);
                            if ((m_bgWorkState.load(
                                    std::memory_order_acquire) &
                                 kBgLoadActive) != 0) {
                                loadRacedPublication = true;
                            } else if (deviceGeneration !=
                                m_d3dDeviceGeneration.load(
                                    std::memory_order_acquire)) {
                                retiredDevice = true;
                            } else {
                                m_retiredBgTexture =
                                    m_currentBgTexture.exchange(
                                        tex, std::memory_order_acq_rel);
                                tex = nullptr;
                                m_backgroundGeneration.fetch_add(
                                    1, std::memory_order_release);
                                m_bgPrepareDeferred.store(
                                    false, std::memory_order_release);
                                m_bgRetryAfterTicks.store(
                                    0, std::memory_order_relaxed);
                                published = true;
                            }
                        }
                        if (published || retiredDevice) break;
                        if (loadRacedPublication) {
                            m_bgPrepareDeferred.store(
                                true, std::memory_order_release);
                            continue;
                        }
                        break;
                    }
                    if (published) {
                        logger::info(
                            "Loaded background texture (async): {}", fullPath);
                    } else if (tex) {
                        VRCompositorHelper::ReleaseTexture(tex);
                        if (retiredDevice ||
                            deviceGeneration !=
                                m_d3dDeviceGeneration.load(
                                    std::memory_order_acquire)) {
                            m_bgPrepareDeferred.store(
                                true, std::memory_order_release);
                            logger::info(
                                "Discarded background from retired D3D device: {}",
                                fullPath);
                        }
                    } else if (m_bgPrepareDeferred.load(std::memory_order_acquire)) {
                        logger::info("Background prepare deferred while LoadingMenu is active");
                    } else {
                        logger::warn("Async bg load failed: {}", fullPath);
                        m_bgRetryAfterTicks.store(
                            (std::chrono::steady_clock::now() + std::chrono::seconds(1))
                                .time_since_epoch().count(), std::memory_order_relaxed);
                    }
                } catch (const std::exception& e) {
                    logger::error("Async bg load threw for {}: {}", fullPath, e.what());
                    m_bgRetryAfterTicks.store(
                        (std::chrono::steady_clock::now() + std::chrono::seconds(1))
                            .time_since_epoch().count(), std::memory_order_relaxed);
                } catch (...) {
                    logger::error("Async bg load threw for {}", fullPath);
                    m_bgRetryAfterTicks.store(
                        (std::chrono::steady_clock::now() + std::chrono::seconds(1))
                            .time_since_epoch().count(), std::memory_order_relaxed);
                }
                m_bgPrepareInFlight.store(false, std::memory_order_release);
            });
            launchGuard.active = false;
        } catch (const std::exception& e) {
            m_bgPrepareInFlight.store(false, std::memory_order_release);
            logger::error("Failed to start background worker for {}: {}", fullPath, e.what());
        } catch (...) {
            m_bgPrepareInFlight.store(false, std::memory_order_release);
            logger::error("Failed to start background worker for {}", fullPath);
        }
    }

    void LoadingScreenManager::OnLoadingMenuOpen()
    {
        // Stop background IO/upload at the native boundary without exposing a
        // half-initialized visual lifecycle to render callbacks. Serialize the
        // barrier transition with the worker's final publication check: a DDS
        // can be published wholly before this OPEN, never during it.
        {
            std::lock_guard textureLock(m_textureStateMutex);
            m_bgWorkState.fetch_or(
                kBgLoadActive, std::memory_order_acq_rel);
        }
        std::lock_guard stateLock(m_stateMutex);
        auto now = std::chrono::steady_clock::now();
        auto sinceLastClose = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastCloseTime).count();
        m_loadStartTime = now;
        m_loopNOPApplied.store(false, std::memory_order_release);
        m_loadCount++;
        const auto previousLoadGeneration =
            m_loadGeneration.load(std::memory_order_acquire);
        const bool retainMarchPresentation =
            m_isVR &&
            m_vrMarchPhase == VRMarchPhase::kWaitingForMoveToOpen &&
            m_vrMarchSourceGeneration == previousLoadGeneration &&
            m_vrMarchCommandIssued;
        const auto loadGeneration =
            m_loadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_vrMarchDelayMs = 0;
        if (retainMarchPresentation) {
            m_vrMarchPhase = VRMarchPhase::kMoveToLoadOpen;
            m_vrMarchChainedGeneration = loadGeneration;
            m_vrPresentationReleaseAt = {};
            m_vrPresentationReleaseGeneration = 0;
            logger::info(
                "VR March handoff: second native LoadingMenu OPEN accepted "
                "(sourceGeneration={}, loadGeneration={}); retaining current "
                "artwork",
                previousLoadGeneration, loadGeneration);
        } else if (m_vrMarchPhase != VRMarchPhase::kIdle) {
            logger::warn(
                "VR March handoff: unexpected LoadingMenu OPEN; cancelling "
                "retained transition (phase={}, sourceGeneration={}, "
                "previousGeneration={}, commandIssued={})",
                static_cast<int>(m_vrMarchPhase),
                m_vrMarchSourceGeneration, previousLoadGeneration,
                m_vrMarchCommandIssued);
            m_vrMarchPhase = VRMarchPhase::kIdle;
            m_vrMarchSourceGeneration = 0;
            m_vrMarchChainedGeneration = 0;
            m_vrMarchCommandIssued = false;
            m_vrPresentationReleaseAt = {};
            m_vrPresentationReleaseGeneration = 0;
        } else {
            // Any ordinary OPEN supersedes a not-yet-dispatched 200 ms release
            // from the previous generation. Generation checks already make its
            // task harmless; clear it here so it is never queued or logged.
            m_vrPresentationReleaseAt = {};
            m_vrPresentationReleaseGeneration = 0;
        }
        // Reset the VR mode-3 NOP watchdog state for this load (deadlines are
        // measured from m_loadStartTime in Update()).
        m_vrNOPWatchdogFired = false;
        m_vrNOPRequested = false;
        m_postLoadGameSeenThisLoad.store(false, std::memory_order_release);
        // A new load supersedes an unconsumed repair: its own close re-arms.
        m_vrPlayspaceRepairPending.store(false, std::memory_order_release);
        m_vrPlayspaceRepairGeneration.store(0, std::memory_order_release);
        m_vrPlayspaceRepairReadyGeneration.store(
            0, std::memory_order_release);
        m_vrPlayspaceLastCounter = -1;
        m_vrPlayspaceStableSamples = 0;
        m_vrPlayspaceStableSince = {};
        m_vrPostCloseGateGeneration.store(0, std::memory_order_release);
        m_vrPostCloseResolvedGeneration.store(
            0, std::memory_order_release);
        m_vrPostCloseHealthySamples = 0;
        m_vrPostCloseBackstopLogged = false;

        // A native LoadingMenu OPEN is the sole visibility-on boundary. A prior
        // close always hid the overlays immediately, so each native loading
        // screen gets one matching custom-screen open and a freshly sampled pose.
        m_pendingPrepareNext = false;
        logger::info("LoadingMenu opened (load #{}, VR={}, sinceLastClose={}ms)",
            m_loadCount, m_isVR, sinceLastClose);

        m_visualPipelineActiveThisLoad = false;
        m_tipsPipelineActiveThisLoad = false;
        // Publish only after every per-load field used by Update's watchdog has
        // been reset. Update also takes m_stateMutex, so it cannot observe an
        // OPEN with the previous load's epoch.
        m_inLoadingScreen.store(true, std::memory_order_release);
        if (m_timingOnly.load(std::memory_order_acquire)) return;

        auto& comp = D3D11Compositor::GetSingleton();
        bool staleVROverlaysHidden = true;
        if (m_isVR) {
            if (retainMarchPresentation) {
                // March kept the same complete overlay stack across the
                // self-MoveTo transition. BeginLoadingPresentation below
                // reopens publication ownership without hiding a single pixel.
                staleVROverlaysHidden = true;
            } else {
                // Resolve any prior close's art before selecting this load's
                // mode. Helper-side active bits survive HideOverlay failures,
                // unlike the manager's per-load attachment bookkeeping.
                const bool staleVRTipsHidden =
                    VRCompositorHelper::HideTipsOverlay();
                const bool staleVRBackgroundHidden =
                    VRCompositorHelper::HideBackgroundOverlay();
                staleVROverlaysHidden =
                    staleVRTipsHidden && staleVRBackgroundHidden;
                // A normal native OPEN supersedes a pending post-close
                // release. The March reopen deliberately skips this because
                // CancelPostLoadingHandoff clears the held scene fade.
                VRCompositorHelper::CancelPostLoadingHandoff();
            }
            // Same rule for the previous close's LoadingMenu render
            // suppression, and it must be cleared HERE rather than only on the
            // fully successful custom path below: every native/fallback branch
            // returns early having made the native LoadingMenu the sole
            // presentation owner, and a surviving window would blank it.
            comp.BeginPostCloseLoadingMenuSuppression(0);
        }
        const bool renderReady = comp.IsRenderReady();
        // Overlay creation is normally completed before the first timed load.
        // A device replacement or transient OpenVR failure tears the handles
        // down, so retry once at the next native OPEN instead of permanently
        // disabling custom presentation for the rest of the process.
        const bool overlayReady = !m_isVR ||
            VRCompositorHelper::IsOverlayInitialized() ||
            VRCompositorHelper::InitializeOverlay();
        const int loadingMode = comp.GetFlatMode();
        D3D11Compositor::FlatNativeLoadingSelectionSnapshot
            upcomingFlatSelection{};
        const bool exactUpcomingFlatSelection = !m_isVR &&
            comp.TryGetFlatNativeLoadingSelection(upcomingFlatSelection);
        D3D11Compositor::VRNativeLoadingSelectionSnapshot
            upcomingVRSelection{};
        const bool exactUpcomingVRSelection = m_isVR &&
            !retainMarchPresentation &&
            comp.TryGetVRNativeLoadingSelectionForUpcomingOpen(
                upcomingVRSelection);
        const bool vrNativeMinimalBlack = m_isVR && loadingMode == 3 &&
            Policy::ShouldPresentSolidBlack(
                loadingMode,
                exactUpcomingVRSelection,
                exactUpcomingVRSelection ? upcomingVRSelection.content :
                    Policy::NativeLoadingContent::kUnknown);
        const bool flatNativeMinimalBlack = !m_isVR && loadingMode == 3 &&
            Policy::ShouldPresentSolidBlack(
                loadingMode,
                exactUpcomingFlatSelection,
                exactUpcomingFlatSelection ?
                    upcomingFlatSelection.selection.content :
                    Policy::NativeLoadingContent::kUnknown);
        const bool nativeMinimalBlack =
            vrNativeMinimalBlack || flatNativeMinimalBlack;
        const bool preserveFlatMinimalSelection =
            Policy::ShouldPreserveFlatMinimalSelectionWithoutTipCapture(
                m_isVR,
                m_inLoadingScreen.load(std::memory_order_acquire),
                loadingMode, exactUpcomingFlatSelection,
                exactUpcomingFlatSelection ?
                    upcomingFlatSelection.selection.content :
                    Policy::NativeLoadingContent::kUnknown);
        if (m_isVR) {
            // Freeze March's delay before kPostLoadGame can change the session
            // flag while this same native LoadingMenu remains open.
            m_vrMarchDelayMs = Policy::VRMarchRenderDelayMs(
                loadingMode, m_gameSessionLoaded);
        }
        const bool vrNativeMode = m_isVR && loadingMode == 1;
        // The owner/capture hooks are required only when mode 3 must extract
        // native tip/level pixels. Modes 0 and 2 are covered by the blocker and
        // must retain their v1.0 speed path even if the tips hook is unavailable.
        const bool vrHooksReady =
            !m_isVR || loadingMode != 3 || comp.AreVRMode3HooksReady();
        const bool customBackgroundRequired = loadingMode == 2 ||
            (loadingMode == 3 && !nativeMinimalBlack);
        const bool backgroundReady =
            !customBackgroundRequired ||
            (m_backgroundsEnabled.load(std::memory_order_acquire) &&
             m_currentBgTexture.load(std::memory_order_acquire) != nullptr);

        // A failed prior restoration can be retried now that LoadingMenu is
        // definitely open. Never force the spinner on: the loose SpinnerOnly
        // SWF intentionally defaults it off, and native mode must preserve that.
        if (m_spinnerRestorePending && !RestoreNativeLoadingSpinner()) {
            logger::warn(
                "LoadingMenu: prior VaultTecLogo_mc state still unavailable; "
                "preserving the pending original value");
        }

        m_visualPipelineActiveThisLoad =
            !vrNativeMode && renderReady && overlayReady && backgroundReady &&
            vrHooksReady && staleVROverlaysHidden;
        if (vrNativeMode) {
            comp.SetTipsExtractEnabled(false);
            comp.SetInLoadingScreen(false);
            const bool blockerHidden =
                VRCompositorHelper::HideBlockerOverlay();
            if (staleVROverlaysHidden && blockerHidden) {
                VRCompositorHelper::ClearSkybox();
            } else {
                VRCompositorHelper::BeginPostLoadingHandoff();
            }
            logger::info("VR: native loading-screen mode selected; custom visuals inactive");
            return;
        }
        if (!m_visualPipelineActiveThisLoad) {
            m_pendingLoopNOP = false;
            comp.SetTipsExtractEnabled(false);
            comp.SetInLoadingScreen(false);
            if (m_isVR) {
                const bool artHidden =
                    VRCompositorHelper::HideBackgroundOverlay();
                const bool blockerHidden =
                    VRCompositorHelper::HideBlockerOverlay();
                if (artHidden && blockerHidden) {
                    VRCompositorHelper::ClearSkybox();
                } else {
                    VRCompositorHelper::BeginPostLoadingHandoff();
                }
            }
            logger::warn("LoadingMenu: custom visuals unavailable "
                         "(renderReady={}, overlayReady={}, backgroundReady={}, "
                         "vrHooksReady={}, staleOverlaysHidden={}); "
                         "using native fallback",
                renderReady, overlayReady, backgroundReady, vrHooksReady,
                staleVROverlaysHidden);
            return;
        }

        // Flat draw-proof hooks need the same authoritative native OPEN/CLOSE
        // lifetime as the VR capture hooks. SendLoadingText can publish just
        // before OPEN, but DisplayMovie proof is accepted only inside this
        // bounded load epoch. CloseLoadingMenu clears it before disabling the
        // compositor or restoring any patch.
        if (!m_isVR) {
            comp.SetInLoadingScreen(true);
        }

        if (m_isVR && retainMarchPresentation) {
            // Exact March quick-reload behavior: reopen ownership around the
            // already-visible stack. Re-publish the exact retained texture
            // handle because OpenVR can discard the submitted image across the
            // synthetic self-MoveTo LoadingMenu even though our COM reference
            // remains alive. This is a texture-only operation: it neither
            // recaptures tips nor resamples/mutates the accepted world pose.
            m_tipsPipelineActiveThisLoad = false;
            comp.SetTipsExtractEnabled(false);
            VRCompositorHelper::BeginLoadingPresentation();
            const bool retainedTexturePublished = !m_vrDisplayTex ||
                VRCompositorHelper::UpdateBackgroundOverlayTexture(
                    m_vrDisplayTex, true);
            m_vrMarchDelayMs = Policy::VRMarchRenderDelayMs(
                loadingMode, m_gameSessionLoaded);
            m_bgPoseRelockFrames = 0;
            comp.SetInLoadingScreen(true);
            if (!retainedTexturePublished) {
                logger::warn(
                    "VR March handoff: retained artwork re-publication failed; "
                    "existing overlay left visible (mode={})",
                    loadingMode);
            } else {
                logger::info(
                    "VR March handoff: retained artwork re-published without "
                    "pose mutation (mode={}, texture={})",
                    loadingMode, m_vrDisplayTex ? "background-or-tip-level" : "none");
            }
            // The normal mode-3 watchdog requests the NOP at the in-session
            // 100 ms floor. Returning here prevents every ordinary OPEN path
            // below from replacing the retained presentation.
            return;
        }

        bool nativeSpinnerSuppressed =
            loadingMode != 3 || nativeMinimalBlack;
        bool mode3TipsCaptureEnabled = false;
        if (loadingMode == 3 && !nativeMinimalBlack) {
            nativeSpinnerSuppressed =
                HideNativeLoadingSpinnerForCustomScreen();
            mode3TipsCaptureEnabled =
                Policy::ShouldEnableMode3TipCapture(
                    m_isVR, loadingMode, nativeSpinnerSuppressed);
            if (!m_isVR && !nativeSpinnerSuppressed) {
                // NG cannot safely retrieve the UI singleton with this
                // CommonLib build. That is not a flat-capture failure: both
                // the live compositor and the persistent proof/replay texture
                // discard the complete bottom-right spinner region.
                logger::info(
                    "Flat tips: spinner isolation=GPU-mask "
                    "(GFx untouched); mode-3 capture enabled");
            } else {
                logger::info(
                    "LoadingMenu: custom tip/level spinner suppression = {}",
                    nativeSpinnerSuppressed ?
                        (m_isVR ? "GFx+delta" : "GFx+flat-shader") :
                        "VR blocker+shader");
            }
            if (!mode3TipsCaptureEnabled) {
                logger::warn(
                    "LoadingMenu: native spinner state could not be preserved; "
                    "mode-3 tip capture disabled for this load");
            }
        }

        // Flat consumes the owner-scoped before/after LoadingMenu delta as a
        // visible-pixel proof and persistent replay texture. Its dedicated GPU
        // shader removes the complete spinner corner before either proof or
        // replay publication, so flat capture remains safe when native GFx
        // access is unavailable. VR still requires the native hide above.
        if (!m_isVR) {
            if (preserveFlatMinimalSelection) {
                comp.DisableTipsCapturePreservingFlatNativeSelection();
            } else {
                comp.SetTipsExtractEnabled(mode3TipsCaptureEnabled);
            }
        }

        if (!m_isVR) {
            // Establish the replacement presenter before suppressing either of
            // Bethesda's producers below. SetEnabled can fail open while render
            // resources are unavailable, or while a prior AdvanceMovie RET has
            // not yet been restored. In either case this load must stay wholly
            // native: never leave an animation-loop NOP or a hidden spinner
            // behind when no custom Present owner exists.
            comp.SetEnabled(true);
            if (!comp.IsEnabled()) {
                m_pendingLoopNOP = false;
                comp.SetEnabled(false);
                comp.SetTipsExtractEnabled(false);
                m_visualPipelineActiveThisLoad = false;
                m_tipsPipelineActiveThisLoad = false;
                if (m_spinnerRestorePending) {
                    RestoreNativeLoadingSpinner();
                }
                logger::warn(
                    "Flat: compositor enable failed at native OPEN; "
                    "native presentation retained (flatMode={})",
                    comp.GetFlatMode());
                return;
            }
        }

        if (m_isVR) {
            // Mode is plumbed via SetFlatMode (called on every OPEN from main.cpp).
            // 0=Black, 1=Native (no 3D), 2=Background, 3=Background + Tips.
            // A full-frame exact movie delta is controller-safe, but would also
            // contain VaultTecLogo_mc if GFx suppression failed. In that rare
            // case retain the landscape background/blocker and fail closed to
            // background-only presentation for this load.
            const bool tipsEnabled = mode3TipsCaptureEnabled;
            const bool backgroundMode = loadingMode == 2 ||
                (loadingMode == 3 && !nativeMinimalBlack);
            m_tipsPipelineActiveThisLoad = tipsEnabled;

            if (m_tipsPipelineActiveThisLoad) {
                // Clear both current-load freshness flags before publishing the
                // hook-visible LoadingMenu state. A render callback can therefore
                // never suppress this load based on the prior load's capture.
                comp.SetTipsExtractEnabled(true);
            } else {
                comp.SetTipsExtractEnabled(false);
            }

            // Ensure a prior tips overlay cannot carry across a mode change.
            if (!tipsEnabled && m_tipsOverlayAttached) {
                VRCompositorHelper::HideTipsOverlay();
                m_tipsOverlayAttached = false;
            }

            // Visual ownership starts at the native OPEN boundary. March tied
            // ShowOverlays to its 500/100/0 ms NOP timer, which exposed
            // Bethesda's vanilla LoadingMenu for 506 ms on the measured first
            // load. Cover it immediately with blocker + plain custom art; the
            // exact tip/level composite replaces only this texture when ready.
            // The loading-speed NOP still observes March's original deadline.
            VRCompositorHelper::BeginLoadingPresentation();
            void* currentBg = m_currentBgTexture.load(std::memory_order_acquire);
            if (backgroundMode &&
                m_backgroundsEnabled.load(std::memory_order_acquire) && currentBg) {
                m_vrDisplayTex = currentBg;
            } else {
                m_vrDisplayTex = nullptr;
            }
            m_bgPoseRelockFrames = 0;
            comp.SetInLoadingScreen(true);
            const bool blockerShown =
                VRCompositorHelper::ShowBlockerOverlay();
            const bool backgroundShown = !backgroundMode ||
                (m_vrDisplayTex &&
                 VRCompositorHelper::ShowBackgroundOverlay(
                     m_vrDisplayTex, m_overlayMode, m_overlayAlpha));
            if (!blockerShown || !backgroundShown) {
                comp.SetTipsExtractEnabled(false);
                comp.SetInLoadingScreen(false);
                m_visualPipelineActiveThisLoad = false;
                m_tipsPipelineActiveThisLoad = false;
                m_tipsOverlayAttached = false;
                m_vrDisplayTex = nullptr;
                VRCompositorHelper::EndLoadingPresentationNow(
                    0.0f, false);
                if (m_spinnerRestorePending) {
                    RestoreNativeLoadingSpinner();
                }
                logger::warn(
                    "VR presentation attach failed at native OPEN "
                    "(blocker={}, background={}); native fallback retained",
                    blockerShown, backgroundShown);
                return;
            }
            logger::info(
                "VR custom presentation shown at native OPEN "
                "(mode={}, background={}, minimalBlack={}, tips={}, "
                "MarchNOPDelay={}ms)",
                loadingMode, backgroundMode, nativeMinimalBlack, tipsEnabled,
                m_vrMarchDelayMs);

            // VR animation-loop NOP (the v1.0 loading-speed core):
            // Update requests every custom mode at this captured 500/100/0 ms
            // boundary. Mode 1 intentionally keeps native rendering.
        } else if (!m_isVR && !REL::Module::IsNG() && m_originalBytesSaved) {
            // OG flat: the animation-loop NOP freezes the loading-screen render
            // to a single present, which makes mode-3 tips RACY — the lone
            // composited frame may land before the LoadingMenu SWF has drawn the
            // tip/level text (observed: tips on a warm first load, none on an
            // in-game reload).
            //   Modes 0/1/2: apply the NOP immediately (max speed).
            //   Mode 3:      DEFER the NOP. Keep the loop rendering so the SWF
            //                draws the tips and we composite multiple frames,
            //                then apply the NOP in Update() once the exact
            //                native selection, an owner-matched visible-pixel
            //                delta (when vanilla chose a tip), and a later
            //                composited Present authorize the real freeze.
            if (D3D11Compositor::GetSingleton().GetFlatMode() == 3) {
                m_pendingLoopNOP = true;
                logger::info("OG: Animation loop NOP deferred for mode 3 (until tips captured)");
            } else {
                if (LoopBytesEqual(m_loopAddress, m_originalLoopBytes)) {
                    REL::safe_write(m_loopAddress, NOP10, sizeof(NOP10));
                    m_loopNOPApplied.store(true, std::memory_order_release);
                    logger::info(
                        "OG: Animation loop NOP applied at {:x}",
                        m_loopAddress);
                } else {
                    logger::warn(
                        "OG: Animation loop NOP skipped at {:x}; "
                        "exact original-byte ownership was lost",
                        m_loopAddress);
                }
            }
        }

        if (!m_isVR) {
            // Mode 0: kill AdvanceMovie early → black screen, no GPU rendering
            // Mode 1: native loading screen (no 3D model, tips still render)
            // Mode 2/3: our compositor handles rendering
            if (comp.GetFlatMode() == 0) {
                comp.KillAdvanceMovie();
            }
            // The flat Present callback binds published texture generations on
            // the render thread; never replace its SRV from this menu thread.
            logger::info("Flat: compositor enabled (flatMode={})", comp.GetFlatMode());
        }
    }

    void LoadingScreenManager::RepairVRPlayspaceHoldOnGameThread(
        std::uint64_t a_loadGeneration)
    {
        // A queued task cannot be cancelled, so re-validate here: between the
        // dispatch and this call the player can have entered a NEW transition
        // (door, fast travel). Clearing the flag then would land mid-transition
        // and leave the pre-transition room yaw composed for that whole load.
        if (a_loadGeneration == 0 ||
            m_loadGeneration.load(std::memory_order_acquire) !=
                a_loadGeneration ||
            m_vrPlayspaceRepairGeneration.load(
                std::memory_order_acquire) != a_loadGeneration ||
            m_inLoadingScreen.load(std::memory_order_acquire)) {
            logger::info(
                "VR playspace: repair generation {} abandoned; ownership "
                "changed before it reached the game thread",
                a_loadGeneration);
            return;
        }
        const auto finishRepairEpoch = [this, a_loadGeneration]() {
            auto expected = a_loadGeneration;
            m_vrPlayspaceRepairGeneration.compare_exchange_strong(
                expected, 0,
                std::memory_order_acq_rel, std::memory_order_acquire);
        };
        const auto base = ReadVRPlayspaceBase();
        if (!base) {
            finishRepairEpoch();
            return;
        }
        VRPlayspaceSnapshot before{};
        if (!TryReadVRPlayspace(base, before)) {
            logger::warn("VR playspace: state unreadable at {:#x}", base);
            finishRepairEpoch();
            return;
        }
        if (!Policy::IsVRPlayspaceSnapshotSane(
                before.counter, before.detached)) {
            logger::error(
                "VR playspace: refusing repair for invalid state "
                "(generation={}, counter={}, detached={}); expected counter "
                "0..{} and detached 0/1",
                a_loadGeneration, before.counter, before.detached,
                kVRPlayspaceMaxDrain);
            finishRepairEpoch();
            return;
        }
        if (Policy::IsVRTransitionHealthy(
                before.counter, before.detached)) {
            // Healthy: the engine completed its own transition. Recorded so a
            // good load stays attributable in the field.
            logger::info(
                "VR playspace: state healthy at repair time "
                "(counter={}, detached={}, committed={}, cachedNode={:#x})",
                before.counter, before.detached, before.committed,
                before.cachedNode);
            finishRepairEpoch();
            return;
        }

        // Restore the pump epilogue's clear at its natural time. A plain byte
        // write: it moves nothing, it only re-enables room-yaw composition in
        // GetViewRotation so the body follows artificial turning again.
        bool detachedCleared = false;
        if (before.detached != 0) {
            detachedCleared = TryWriteByteSafe(
                base + kVRPlayspaceDetached, 0);
        }

        const bool neutralized = before.counter > 0
            ? NeutralizeVRPlayspaceStagedYaw(base)
            : false;
        const int calls = before.counter > 0
            ? DrainVRPlayspaceHold(base)
            : 0;
        VRPlayspaceSnapshot after{};
        // A post-drain read failure leaves the zeroed snapshot, which the
        // readable=false field below makes explicit.
        const bool readable = TryReadVRPlayspace(base, after);
        const bool repaired =
            readable && Policy::IsVRTransitionHealthy(
                after.counter, after.detached);
        const auto* verdict = repaired
            ? "post-load transition state repaired"
            : "post-load transition state NOT fully repaired";
        const auto message =
            "VR playspace: {} (counter {}->{}, detached {}->{}, "
            "detachedCleared={}, endCalls={}, yawNeutralized={}, "
            "committed {}->{}, handshake {}->{}, updateEnabled={}, "
            "cachedNode={:#x}, veto={}, readable={})";
        if (repaired) {
            logger::warn(message, verdict,
                before.counter, after.counter,
                before.detached, after.detached,
                detachedCleared, calls, neutralized,
                before.committed, after.committed,
                before.handshake, after.handshake,
                after.updateEnabled, after.cachedNode, after.commitVeto,
                readable);
        } else {
            logger::error(message, verdict,
                before.counter, after.counter,
                before.detached, after.detached,
                detachedCleared, calls, neutralized,
                before.committed, after.committed,
                before.handshake, after.handshake,
                after.updateEnabled, after.cachedNode, after.commitVeto,
                readable);
        }
        // Do not resolve the visual gate from playspace state alone. The next
        // render callbacks must also confirm the world-transition bytes are
        // readable/live across consecutive samples before artwork can leave.
        finishRepairEpoch();
    }

    void LoadingScreenManager::DispatchPendingVRPlayspaceRepair()
    {
        // Deliberately called from the frame callback AFTER Update() returns,
        // i.e. with no manager lock held: F4SE drains its task queue while
        // holding the queue lock, so queueing from inside m_stateMutex would
        // pair {m_stateMutex -> queueLock} here against {queueLock ->
        // m_stateMutex} on the game thread.
        const auto readyGeneration =
            m_vrPlayspaceRepairReadyGeneration.load(
                std::memory_order_acquire);
        if (readyGeneration == 0) {
            return;
        }
        auto* task = F4SE::GetTaskInterface();
        if (!task) {
            // Keep the request armed rather than silently dropping it; a later
            // frame can still dispatch it.
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_acq_rel)) {
                logger::warn(
                    "VR playspace: repair pending but the F4SE task interface "
                    "is unavailable; retrying on later frames");
            }
            return;
        }
        auto expectedReadyGeneration = readyGeneration;
        if (!m_vrPlayspaceRepairReadyGeneration.compare_exchange_strong(
                expectedReadyGeneration, 0,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        if (m_vrPlayspaceRepairGeneration.load(
                std::memory_order_acquire) != readyGeneration ||
            m_loadGeneration.load(std::memory_order_acquire) !=
                readyGeneration) {
            logger::info(
                "VR playspace: stale repair generation {} discarded before "
                "queueing",
                readyGeneration);
            return;
        }
        logger::info(
            "VR playspace: repair generation {} queued on the game thread",
            readyGeneration);
        task->AddTask([readyGeneration]() {
            LoadingScreenManager::GetSingleton()
                .RepairVRPlayspaceHoldOnGameThread(readyGeneration);
        });
    }

    void LoadingScreenManager::TickVRMarchMainLoop()
    {
        enum class Action : std::uint8_t
        {
            kNone,
            kMoveTo,
            kRelease
        };

        Action action = Action::kNone;
        std::uint64_t actionGeneration = 0;
        long long elapsedCloseMs = 0;
        {
            std::lock_guard stateLock(m_stateMutex);
            if (!m_isVR) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto loadGeneration =
                m_loadGeneration.load(std::memory_order_acquire);
            elapsedCloseMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(now - m_lastCloseTime).count();

            const bool waitingForMoveTo =
                m_vrMarchPhase == VRMarchPhase::kWaitingForMoveToOpen &&
                m_vrMarchSourceGeneration == loadGeneration;
            if (waitingForMoveTo && !m_vrMarchCommandIssued) {
                if (Policy::ShouldExecuteVRMarchMoveTo(
                        true, false,
                        m_inLoadingScreen.load(std::memory_order_acquire),
                        m_mainMenuOpen.load(std::memory_order_acquire),
                        false, false, 0, elapsedCloseMs)) {
                    // Publish before entering the engine. ExecuteCommand may
                    // emit LoadingMenu OPEN synchronously; that callback must
                    // recognize this generation without re-entering our lock.
                    m_vrMarchCommandIssued = true;
                    action = Action::kMoveTo;
                    actionGeneration = loadGeneration;
                }
            }

            if (action == Action::kNone &&
                m_vrMarchPhase == VRMarchPhase::kIdle &&
                m_vrPresentationReleaseGeneration == loadGeneration &&
                !m_inLoadingScreen.load(std::memory_order_acquire) &&
                m_vrPresentationReleaseAt.time_since_epoch().count() != 0 &&
                now >= m_vrPresentationReleaseAt) {
                m_vrMarchPhase = VRMarchPhase::kIdle;
                m_vrMarchSourceGeneration = 0;
                m_vrMarchChainedGeneration = 0;
                m_vrMarchCommandIssued = false;
                m_vrMarchDelayMs = 0;
                m_vrPresentationReleaseAt = {};
                m_vrPresentationReleaseGeneration = 0;
                action = Action::kRelease;
                actionGeneration = loadGeneration;
            }
        }

        if (action == Action::kMoveTo) {
            logger::info(
                "VR March handoff: executing 'player.moveto player' from the "
                "next direct main-loop update, +{}ms (generation={})",
                elapsedCloseMs, actionGeneration);
            // Never hold m_stateMutex here. Unlike F4SE::AddTask, this verified
            // main-loop callsite is outside the task queue's global lock and is
            // the execution context used by the known-good March build.
            RE::Console::ExecuteCommand("player.moveto player");
            logger::info(
                "VR March handoff: 'player.moveto player' returned "
                "(generation={})",
                actionGeneration);
            return;
        }
        if (action != Action::kRelease) {
            return;
        }

        // This is the March transaction: one main-frame-aligned teardown of
        // tips, background and blocker, followed by an instant scene clear.
        VRCompositorHelper::EndLoadingPresentationNow(0.0f, false);
        VRCompositorHelper::ClearSkybox();
        logger::info(
            "VR March handoff: custom presentation released in one step "
            "from the direct main-loop tail (generation={})",
            actionGeneration);
    }

    void LoadingScreenManager::OnLoadingMenuClose()
    {
        CloseLoadingMenu(true);
    }

    void LoadingScreenManager::CloseForChainedLoadingMenu()
    {
        CloseLoadingMenu(false);
    }

    void LoadingScreenManager::FinishChainedLoadingMenuGap()
    {
        m_bgWorkState.fetch_and(~kBgLoadActive, std::memory_order_release);
        if (m_timingOnly.load(std::memory_order_acquire)) {
            return;
        }
        if (m_backgroundsEnabled.load(std::memory_order_acquire) &&
            (!m_currentBgTexture.load(std::memory_order_acquire) ||
             m_bgPrepareDeferred.load(std::memory_order_acquire)) &&
            !m_bgPrepareInFlight.load(std::memory_order_acquire)) {
            PrepareNextBackgroundAsync();
        }
    }

    void LoadingScreenManager::CloseLoadingMenu(
        bool a_prepareNextBackground)
    {
        // Stop every custom-pixel producer at the native CLOSE boundary before
        // waiting for a render/update callback that may own the lifecycle mutex.
        // Publications are sealed here; whether the already-shown overlays are
        // removed immediately or held through the bounded post-close handoff is
        // decided under m_stateMutex below.
        auto& compositor = D3D11Compositor::GetSingleton();
        m_inLoadingScreen.store(false, std::memory_order_release);
        compositor.SetInLoadingScreen(false);
        if (m_isVR) {
            // Close the helper latch first. This is intentionally ahead of the
            // DDS barrier and query/resource locks: neither a newly published
            // image nor a delayed GPU retirement can replace the native load's
            // one chosen background at its tail.
            VRCompositorHelper::SealLoadingPresentation();
            // March restored Fallout's loop at the authoritative CLOSE before
            // doing any diagnostics or background bookkeeping. Native rendering
            // must be free to overwrite the title/menu eye buffers underneath
            // the still-opaque custom artwork.
            compositor.ResetDeferredState();
        }
        compositor.SetEnabled(false);
        // A chained AE Hide→Show gap is still inside an engine load. Keep the
        // barrier raised until the matching reopen (or the explicit no-Show
        // completion above), otherwise the DDS worker can start/upload/publish
        // in the narrow interval between CloseForChainedLoadingMenu() and OPEN.
        if (a_prepareNextBackground && !m_isVR) {
            m_bgWorkState.fetch_and(
                ~kBgLoadActive, std::memory_order_release);
        }
        compositor.SetTipsExtractEnabled(false);

        std::lock_guard stateLock(m_stateMutex);
        m_lastCloseTime = std::chrono::steady_clock::now();
        auto elapsed = m_lastCloseTime - m_loadStartTime;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        if (m_isVR) {
            m_bgPoseRelockFrames = 0;
            // The leaked reposition hold is a vanilla engine defect and has
            // nothing to do with whether our overlay owned the load, so arm the
            // repair OUTSIDE the visual-pipeline branch: native mode 1 and
            // every mid-load fallback need it just as much.
            m_vrStereoResumeLogged = false;
            m_vrPlayspaceReadFailLogged = false;
            m_vrStereoProbesLogged = 0;
            const bool saveLoadClose =
                m_postLoadGameSeenThisLoad.load(std::memory_order_acquire);
            const auto loadGeneration =
                m_loadGeneration.load(std::memory_order_acquire);
            const bool marchSecondClose =
                m_vrMarchPhase == VRMarchPhase::kMoveToLoadOpen &&
                m_vrMarchChainedGeneration == loadGeneration;
            const bool saveLoadNeedsMoveTo =
                saveLoadClose &&
                m_visualPipelineActiveThisLoad &&
                m_vrMarchPhase == VRMarchPhase::kIdle;
            if (a_prepareNextBackground && !saveLoadNeedsMoveTo) {
                // A successful save-load CLOSE deliberately keeps the background
                // barrier raised across the MoveTo gap. Every final VR close
                // releases it at the ordinary native boundary.
                m_bgWorkState.fetch_and(
                    ~kBgLoadActive, std::memory_order_release);
            }

            m_vrPlayspaceRepairPending.store(
                false, std::memory_order_release);
            m_vrPlayspaceRepairGeneration.store(
                0, std::memory_order_release);
            m_vrPlayspaceRepairReadyGeneration.store(
                0, std::memory_order_release);
            m_vrPlayspaceLastCounter = -1;
            m_vrPlayspaceStableSamples = 0;
            m_vrPlayspaceStableSince = {};
            m_vrPostCloseGateGeneration.store(
                0, std::memory_order_release);
            m_vrPostCloseResolvedGeneration.store(
                0, std::memory_order_release);
            m_vrPostCloseHealthySamples = 0;
            m_vrPostCloseBackstopLogged = false;

            if (m_visualPipelineActiveThisLoad) {
                // March allowed Fallout's LoadingMenu tail and normal renderer
                // to run underneath the still-opaque artwork. Explicitly clear
                // every older suppression window; native pixels are hidden by
                // our overlay stack, not by preventing the engine from drawing.
                D3D11Compositor::GetSingleton()
                    .BeginPostCloseLoadingMenuSuppression(0);

                m_vrPresentationReleaseGeneration = loadGeneration;

                if (saveLoadNeedsMoveTo) {
                    // The known-good March release did not uncover after a
                    // successful save CLOSE. Keep art/tips/blocker/scene fade
                    // intact and run one real second transition next frame.
                    m_vrMarchPhase =
                        VRMarchPhase::kWaitingForMoveToOpen;
                    m_vrMarchSourceGeneration = loadGeneration;
                    m_vrMarchChainedGeneration = 0;
                    m_vrMarchCommandIssued = false;
                    m_vrPresentationReleaseAt = {};
                    logger::info(
                        "VR March handoff: successful save CLOSE retained "
                        "(generation={}); player.moveto player pending on the "
                        "next direct main-loop update",
                        loadGeneration);
                } else {
                    if (marchSecondClose) {
                        logger::info(
                            "VR March handoff: second native transition "
                            "CLOSE (generation={}); scheduling final reveal",
                            loadGeneration);
                    }
                    m_vrMarchPhase = VRMarchPhase::kIdle;
                    m_vrMarchSourceGeneration = 0;
                    m_vrMarchChainedGeneration = 0;
                    m_vrMarchCommandIssued = false;
                    m_vrPresentationReleaseAt =
                        m_lastCloseTime +
                        std::chrono::milliseconds(
                            Policy::kVRMarchPostCloseHoldMs);
                    logger::info(
                        "VR March handoff: one-step reveal scheduled in {}ms "
                        "on the game thread (generation={})",
                        Policy::kVRMarchPostCloseHoldMs, loadGeneration);
                }
            } else {
                // Native/fallback presentation: identical to the old boundary.
                // An Update that was already inside m_stateMutex at CLOSE may
                // have completed one last pose re-lock after the seal above;
                // this idempotent cleanup runs once it has drained.
                m_vrMarchPhase = VRMarchPhase::kIdle;
                m_vrMarchSourceGeneration = 0;
                m_vrMarchChainedGeneration = 0;
                m_vrMarchCommandIssued = false;
                m_vrMarchDelayMs = 0;
                m_vrPresentationReleaseAt = {};
                m_vrPresentationReleaseGeneration = 0;
                VRCompositorHelper::EndLoadingPresentationNow(0.0f, false);
                VRCompositorHelper::ClearSkybox();
            }
            if (D3D11Compositor::GetSingleton().GetFlatMode() == 3) {
                D3D11Compositor::GetSingleton()
                    .LogTipsPipelineDiagnostics("close");
            }
        }
        if (m_isVR && m_visualPipelineActiveThisLoad &&
            m_vrMarchPhase !=
                VRMarchPhase::kWaitingForMoveToOpen) {
            m_tipsOverlayAttached = false;
            m_vrDisplayTex = nullptr;
        }
        if (m_spinnerRestorePending) {
            if (!RestoreNativeLoadingSpinner()) {
                logger::warn(
                    "LoadingMenu: could not restore VaultTecLogo_mc at close; "
                    "the next native open will retry");
            }
        }

        logger::info("Loading screen #{} closed — duration: {:.2f}s", m_loadCount, ms / 1000.0);
        // Surface the crash-guard counter on change: it increments exactly at
        // worldspace transitions (this code path), and an unreported intercept
        // is field evidence lost. Game thread only, so the static is safe.
        {
            static int s_lastGuardHits = 0;
            const int guardHits = CellWorldspaceGuard::InterceptCount();
            if (guardHits != s_lastGuardHits) {
                s_lastGuardHits = guardHits;
                logger::info(
                    "CellWorldspaceGuard: {} null-worldspace unregister(s) "
                    "intercepted this session (crashes averted)",
                    guardHits);
            }
        }

        // The VR speed patch was restored immediately at CLOSE above, before
        // any close-side work. Flat restoration remains below.

        if (m_timingOnly.load(std::memory_order_acquire)) return;

        // A first-load fallback must not strand the background pipeline. OG's
        // Present callback is intentionally inert while the compositor is off,
        // so schedule a missing/deferred texture here before the visual-pipeline
        // early return instead of relying on a later render tick.
        if (a_prepareNextBackground &&
            m_backgroundsEnabled.load(std::memory_order_acquire) &&
            m_vrMarchPhase !=
                VRMarchPhase::kWaitingForMoveToOpen &&
            (!m_currentBgTexture.load(std::memory_order_acquire) ||
             m_bgPrepareDeferred.load(std::memory_order_acquire)) &&
            !m_bgPrepareInFlight.load(std::memory_order_acquire)) {
            PrepareNextBackgroundAsync();
        }

        if (!m_visualPipelineActiveThisLoad) {
            m_pendingLoopNOP = false;
            m_tipsPipelineActiveThisLoad = false;
            return;
        }

        // Clear any pending deferred NOP (mode 3 load that closed before tips
        // were captured / AdvanceMovie froze) so it can't carry into next load.
        m_pendingLoopNOP = false;

        // Flat NOP restore (applied directly by us via m_loopNOPApplied).
        if (!m_isVR && m_loopNOPApplied.load(std::memory_order_acquire) && m_originalBytesSaved) {
            if (LoopBytesEqual(m_loopAddress, NOP10)) {
                REL::safe_write(
                    m_loopAddress, m_originalLoopBytes,
                    sizeof(m_originalLoopBytes));
                logger::info(
                    "Animation loop restored at {:x}", m_loopAddress);
            } else {
                logger::warn(
                    "Animation loop restore skipped at {:x}; "
                    "NOP ownership was lost",
                    m_loopAddress);
            }
            m_loopNOPApplied.store(false, std::memory_order_release);
        }

        // VR deferred-state reset above restored the exact captured loop bytes
        // under the same ownership mutex as Submit. Bethesda's own fade calls
        // were never replaced or delayed.
        if (m_isVR) {
            if (a_prepareNextBackground &&
                m_backgroundsEnabled.load(std::memory_order_acquire) &&
                m_vrMarchPhase !=
                    VRMarchPhase::kWaitingForMoveToOpen) {
                m_pendingPrepareNext = true;
            } else if (m_vrMarchPhase ==
                       VRMarchPhase::kWaitingForMoveToOpen) {
                logger::info(
                    "VR March handoff: next background preparation deferred "
                    "until the second native CLOSE");
            }
        } else {
            if (a_prepareNextBackground &&
                m_backgroundsEnabled.load(std::memory_order_acquire)) {
                // Async — the synchronous version blocked this thread (game
                // thread on OG via MenuWatcher, render thread on NG via
                // TickNGDeferred) for up to ~5.7s at load-end: disk contention
                // from the post-load streaming storm + software BC decode.
                // That also delayed PerformancePatches::OnLoadingMenuClose
                // (VSync restore etc.) which runs after us in the handler.
                PrepareNextBackgroundAsync();
            }
        }
        m_visualPipelineActiveThisLoad = false;
        m_tipsPipelineActiveThisLoad = false;
    }

    void LoadingScreenManager::Update()
    {
        std::lock_guard stateLock(m_stateMutex);
        if (m_timingOnly.load(std::memory_order_acquire)) return;

        // A worker that reached a load-active phase boundary exits immediately
        // and leaves this bit set. Retry from the normal render callback only
        // after LoadingMenu has closed; no menu callback ever joins the worker.
        if (!m_inLoadingScreen.load(std::memory_order_acquire) &&
            m_vrMarchPhase !=
                VRMarchPhase::kWaitingForMoveToOpen &&
            m_bgPrepareDeferred.load(std::memory_order_acquire) &&
            !m_bgPrepareInFlight.load(std::memory_order_acquire) &&
            m_backgroundsEnabled.load(std::memory_order_acquire)) {
            PrepareNextBackgroundAsync();
        }

        // OG mode 3 deferred animation-loop NOP: once AdvanceMovie has been
        // frozen after the native selection and its required draw/Present
        // proof, apply the loop NOP to reclaim the loading-speed gain for the
        // rest of the load. The last presented frame remains on screen.
        if (m_visualPipelineActiveThisLoad && m_pendingLoopNOP && !m_isVR &&
            m_originalBytesSaved && m_loopAddress
            && m_inLoadingScreen.load()
            && D3D11Compositor::GetSingleton().IsAdvanceMovieKilled())
        {
            if (LoopBytesEqual(m_loopAddress, m_originalLoopBytes)) {
                REL::safe_write(m_loopAddress, NOP10, sizeof(NOP10));
                m_loopNOPApplied.store(true, std::memory_order_release);
                logger::info(
                    "OG: Animation loop NOP applied "
                    "(deferred, post tips-capture) at {:x}",
                    m_loopAddress);
            } else {
                logger::warn(
                    "OG: deferred animation loop NOP skipped at {:x}; "
                    "exact original-byte ownership was lost",
                    m_loopAddress);
            }
            m_pendingLoopNOP = false;
        }

        // NG/AE mode 3 deferred DisableAnimation NOP5 — same reclaim as OG, but
        // NG's loading-render speed patch is the DisableAnimation epilogue NOP5
        // (NG has no animation-loop NOP). Without this, default-mode (3) NG/AE
        // loads never got NG's only loading-render speedup. The call is
        // idempotent, so re-invoking it each frame after the kill is cheap.
        if (m_visualPipelineActiveThisLoad && !m_isVR && REL::Module::IsNG() &&
            m_inLoadingScreen.load()
            && D3D11Compositor::GetSingleton().GetFlatMode() == 3
            && D3D11Compositor::GetSingleton().IsAdvanceMovieKilled())
        {
            PerformancePatches::ApplyNGDisableAnimDeferred();
        }

        if (m_isVR) {
            auto& comp = D3D11Compositor::GetSingleton();

            // Bounded post-close hold release. The callback can resume much
            // later and far below HMD rate, so all dwell/fallback decisions use
            // steady-clock time rather than assuming a frame cadence.
            if (!m_inLoadingScreen.load()) {
                const auto now = std::chrono::steady_clock::now();
                const auto sinceClose =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_lastCloseTime).count();
                const auto loadGeneration =
                    m_loadGeneration.load(std::memory_order_acquire);
                const bool repairPending =
                    m_vrPlayspaceRepairPending.load(
                        std::memory_order_acquire) &&
                    m_vrPlayspaceRepairGeneration.load(
                        std::memory_order_acquire) == loadGeneration;
                // Record when the engine returns to live world presentation,
                // and sample the state a few times across the post-close
                // window when the opt-in diagnostic is enabled.
                if (m_vrPresentationProbeEnabled.load(
                        std::memory_order_acquire) &&
                    m_vrStereoProbesLogged < kVRStereoProbeCount) {
                    if (VRWorldPresentationLive() && !m_vrStereoResumeLogged) {
                        m_vrStereoResumeLogged = true;
                        logger::info(
                            "VR presentation: live world resumed {} ms after "
                            "CLOSE",
                            sinceClose);
                    }
                    if (sinceClose >= (m_vrStereoProbesLogged + 1) *
                            kVRStereoProbeIntervalMs) {
                        ++m_vrStereoProbesLogged;
                        logger::info(
                            "VR presentation probe +{} ms: {}",
                            sinceClose, DescribeVRPresentationState());
                    }
                }
                // Sample only while the body-follow repair owns this
                // generation. Presentation release is deliberately independent
                // of these state bytes; they cannot classify submitted pixels.
                // Two earlier
                // "safety" conditions remain deliberately absent:
                //  - requiring the player cell to report attached 3D — that
                //    predicate never returns true on VR (2.1.1 logs: the
                //    attach-gated art hold ran to its backstop on EVERY load);
                //  - requiring cachedNode != 0 — the leaked-hold state IS
                //    cachedNode == NULL (the node re-cache is part of what
                //    never ran), so the guard excluded exactly the state it
                //    existed to repair.
                // A legitimate in-flight reposition resolves before the
                // 1.5-second wall dwell in every recorded healthy load. Only a
                // leaked signature stays unchanged across that time.
                if (repairPending) {
                    const auto base = ReadVRPlayspaceBase();
                    VRPlayspaceSnapshot sample{};
                    if (base && TryReadVRPlayspace(base, sample)) {
                        const bool sane =
                            Policy::IsVRPlayspaceSnapshotSane(
                                sample.counter, sample.detached);
                        const bool playspaceHealthy = sane &&
                            Policy::IsVRTransitionHealthy(
                                sample.counter, sample.detached);

                        if (!sane) {
                            if (repairPending &&
                                m_vrPlayspaceRepairPending.exchange(
                                    false, std::memory_order_acq_rel)) {
                                m_vrPlayspaceRepairGeneration.store(
                                    0, std::memory_order_release);
                                logger::error(
                                    "VR playspace: repair cancelled for invalid "
                                    "post-close state (counter={}, detached={})",
                                    sample.counter, sample.detached);
                            }
                            m_vrPlayspaceStableSamples = 0;
                            m_vrPlayspaceStableSince = {};
                        } else if (playspaceHealthy) {
                            // The engine completed it itself: nothing to repair.
                            if (m_vrPlayspaceRepairPending.exchange(
                                    false, std::memory_order_acq_rel)) {
                                m_vrPlayspaceRepairGeneration.store(
                                    0, std::memory_order_release);
                                logger::info(
                                    "VR playspace: transition completed by the "
                                    "engine (counter={}, detached={}, "
                                    "committed={}, cachedNode={:#x})",
                                    sample.counter, sample.detached,
                                    sample.committed, sample.cachedNode);
                            }
                            m_vrPlayspaceStableSamples = 0;
                            m_vrPlayspaceStableSince = {};
                        } else if (repairPending) {
                            // The stuck signature is EITHER field: a leaked
                            // reposition counter or the detached flag observed
                            // in the field. Both are already range-validated.
                            const std::int32_t signature = sample.counter |
                                (sample.detached != 0 ? 0x10000 : 0);
                            const bool sameSignature =
                                signature == m_vrPlayspaceLastCounter &&
                                m_vrPlayspaceStableSamples > 0;
                            if (sameSignature) {
                                ++m_vrPlayspaceStableSamples;
                            } else {
                                m_vrPlayspaceLastCounter = signature;
                                m_vrPlayspaceStableSamples = 1;
                                m_vrPlayspaceStableSince = now;
                            }
                            const auto stableElapsed =
                                m_vrPlayspaceStableSince ==
                                        std::chrono::steady_clock::time_point{}
                                    ? 0
                                    : std::chrono::duration_cast<
                                          std::chrono::milliseconds>(
                                          now - m_vrPlayspaceStableSince)
                                          .count();
                            if (Policy::ShouldScheduleVRPlayspaceRepair(
                                    sane, true, sameSignature,
                                    m_vrPlayspaceStableSamples,
                                    stableElapsed,
                                    kVRPlayspaceDwellMinSamples,
                                    kVRPlayspaceDwellMs) &&
                                m_vrPlayspaceRepairPending.exchange(
                                    false, std::memory_order_acq_rel)) {
                                m_vrPlayspaceRepairReadyGeneration.store(
                                    loadGeneration, std::memory_order_release);
                                logger::warn(
                                    "VR playspace: stuck signature stable for "
                                    "{} ms across {} samples; repair generation "
                                    "{} ready for dispatch",
                                    stableElapsed,
                                    m_vrPlayspaceStableSamples,
                                    loadGeneration);
                                m_vrPlayspaceStableSamples = 0;
                            }
                        }
                    } else if (!m_vrPlayspaceReadFailLogged) {
                        // Field diagnosability: a silent read failure was one
                        // of the ways the first shipping of this repair did
                        // nothing without a trace.
                        m_vrPlayspaceReadFailLogged = true;
                        logger::warn(
                            "VR playspace: repair pending but state is "
                            "unreadable (base={:#x})",
                            base);
                    }
                }

            }

            if (m_inLoadingScreen.load() && m_visualPipelineActiveThisLoad) {
                // Upgrade the already-visible plain custom art with the exact
                // current Scaleform tip/level delta. The in-place texture swap
                // preserves the OPEN-time overlay pose and never exposes the
                // native LoadingMenu underneath it.
                if (m_tipsPipelineActiveThisLoad &&
                    comp.GetFlatMode() == 3 &&
                    comp.IsTipsExtractEnabled() &&
                    comp.IsTipsTextureReady() &&
                    !m_tipsOverlayAttached &&
                    m_currentBgTexture.load(std::memory_order_acquire)) {
                    comp.SetBackgroundTexture(
                        m_currentBgTexture.load(std::memory_order_acquire));
                    if (void* combined = comp.CompositeTipsIntoBg()) {
                        VRCompositorHelper::HideTipsOverlay();
                        if (VRCompositorHelper::
                                UpdateBackgroundOverlayTexture(combined)) {
                            m_vrDisplayTex = combined;
                            m_tipsOverlayAttached = true;
                            logger::info(
                                "VR presentation: current tip/level composite "
                                "published without changing pose");
                        } else {
                            comp.SetTipsExtractEnabled(false);
                            m_tipsPipelineActiveThisLoad = false;
                            logger::warn(
                                "VR presentation: tip/level texture swap "
                                "failed; plain custom background retained");
                        }
                    } else {
                        comp.SetTipsExtractEnabled(false);
                        m_tipsPipelineActiveThisLoad = false;
                        logger::warn(
                            "VR March presentation: tip/level composite failed; "
                            "plain custom background retained");
                    }
                }

            }
            // March's 500/100 ms boundary remains the minimum freeze time. For
            // mode 3 it is no longer a blind capture deadline: Fallout4VR's
            // exact SendLoadingText selection tells us whether native actually
            // chose a tip. Verified native-minimal screens retain blocker-only
            // black and freeze at the March floor; tip screens keep Scaleform
            // live for the bounded 600 ms capture window (plus one bounded
            // pending-query grace), then fall back to custom art.
            if (m_inLoadingScreen.load() &&
                !m_vrNOPRequested &&
                comp.GetFlatMode() != 1 &&
                comp.IsRenderReady() &&
                m_originalBytesSaved &&
                m_loopAddress &&
                !comp.IsDeferredNOPApplied()) {
                const int mode = comp.GetFlatMode();
                const auto held =
                    std::chrono::steady_clock::now() - m_loadStartTime;
                const auto heldMs = std::chrono::duration_cast<
                    std::chrono::milliseconds>(held).count();
                bool requestNOP = false;
                auto vrTipsDecision =
                    Policy::VRTipsFreezeDecision::kWait;
                Policy::NativeLoadingContent nativeContent =
                    Policy::NativeLoadingContent::kUnknown;
                bool nativeSelectionSeen = false;
                bool proofPending = false;
                bool captureHopeless = false;
                bool captureUnavailable = false;
                const bool retainedMarchPresentation =
                    m_vrMarchPhase == VRMarchPhase::kMoveToLoadOpen &&
                    !m_tipsPipelineActiveThisLoad;

                if (mode == 3) {
                    D3D11Compositor::VRNativeLoadingSelectionSnapshot
                        nativeSelection{};
                    nativeSelectionSeen =
                        comp.TryGetVRNativeLoadingSelection(nativeSelection);
                    if (nativeSelectionSeen) {
                        nativeContent = nativeSelection.content;
                    }
                    proofPending = comp.IsTipsDeltaProofPending();
                    captureUnavailable = !retainedMarchPresentation &&
                        !m_tipsPipelineActiveThisLoad;
                    captureHopeless = comp.IsTipsCaptureHopeless() ||
                        captureUnavailable;
                    vrTipsDecision = Policy::DecideVRTipsFreeze(
                        retainedMarchPresentation,
                        nativeSelectionSeen,
                        nativeContent,
                        m_tipsOverlayAttached,
                        captureHopeless,
                        proofPending,
                        heldMs,
                        m_vrMarchDelayMs);
                    requestNOP = vrTipsDecision !=
                        Policy::VRTipsFreezeDecision::kWait;
                } else {
                    requestNOP = heldMs >= m_vrMarchDelayMs;
                }

                if (requestNOP) {
                    const bool backgroundFallback = mode == 3 &&
                        vrTipsDecision == Policy::VRTipsFreezeDecision::
                            kFreezeBackgroundOnly;
                    const bool verifiedMinimalBlack = backgroundFallback &&
                        nativeSelectionSeen &&
                        nativeContent ==
                            Policy::NativeLoadingContent::kBackgroundOnly;
                    if (backgroundFallback &&
                        (m_tipsPipelineActiveThisLoad || proofPending)) {
                        comp.SetTipsExtractEnabled(false);
                        m_tipsPipelineActiveThisLoad = false;
                    }

                    m_vrNOPWatchdogFired = true;
                    m_vrNOPRequested = true;
                    if (m_vrAnimationLoopNOPEnabled.load(
                            std::memory_order_acquire)) {
                        comp.RequestDeferredNOP(
                            m_loopAddress,
                            m_originalLoopBytes,
                            NOP10,
                            sizeof(NOP10),
                            m_loadGeneration.load(
                                std::memory_order_acquire));
                        logger::info(
                            "VR March freeze: animation-loop NOP requested "
                            "(mode={}, held={}ms, floor={}ms, nativeSeen={}, "
                            "nativeContent={}, currentTipLevel={}, "
                            "proofPending={}, hopeless={}, unavailable={}, "
                            "retained={})",
                            mode, heldMs, m_vrMarchDelayMs,
                            nativeSelectionSeen,
                            nativeContent ==
                                    Policy::NativeLoadingContent::kTipAndLevel ?
                                "tip+level" :
                                nativeContent == Policy::NativeLoadingContent::
                                        kBackgroundOnly ?
                                    "background-only" : "unknown",
                            m_tipsOverlayAttached, proofPending,
                            captureHopeless, captureUnavailable,
                            retainedMarchPresentation);
                    } else {
                        logger::info(
                            "VR March freeze: animation-loop NOP suppressed "
                            "by bVRAnimationLoopNOP=0 "
                            "(mode={}, held={}ms, floor={}ms)",
                            mode, heldMs, m_vrMarchDelayMs);
                    }
                    if (mode == 3) {
                        if (backgroundFallback) {
                            if (verifiedMinimalBlack) {
                                logger::info(
                                    "VR tips: verified native-minimal black "
                                    "retained (held={}ms)",
                                    heldMs);
                            } else {
                                logger::info(
                                    "VR tips: bounded custom-background "
                                    "fallback selected (selectionSeen={}, "
                                    "content={}, held={}ms, "
                                    "captureDeadline={}ms, "
                                    "queryDeadline={}ms)",
                                    nativeSelectionSeen,
                                    nativeContent ==
                                            Policy::NativeLoadingContent::
                                                kTipAndLevel ?
                                        "tip+level" :
                                        nativeContent ==
                                                Policy::NativeLoadingContent::
                                                    kBackgroundOnly ?
                                            "background-only" : "unknown",
                                    heldMs,
                                    Policy::kVRTipsCaptureDeadlineMs,
                                    Policy::kVRTipsPendingQueryDeadlineMs);
                            }
                        }
                        comp.LogTipsPipelineDiagnostics("march-freeze");
                    }
                }
            }
            // Prep next bg once outside of a loading cycle (avoids texture swap
            // during an active native LoadingMenu). Async — this Update runs on
            // the VR render thread (Submit callback); the synchronous load used
            // to block it for the disk-IO + BC-decode duration.
            if (!m_inLoadingScreen.load() && m_pendingPrepareNext &&
                m_backgroundsEnabled.load(std::memory_order_acquire)) {
                m_pendingPrepareNext = false;
                PrepareNextBackgroundAsync();
            }

        }
    }
}
