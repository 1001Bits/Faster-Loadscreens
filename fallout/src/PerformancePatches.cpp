#include "PCH.h"
#include "PerformancePatches.h"
#include "VRCompositorHelper.h"
#include "D3D11Compositor.h"

#include <mutex>

namespace VRLoadingScreens
{
    namespace
    {
        // Exact signatures verified against the supported executables in the
        // Combined Ghidra project. A byte patch must match the complete signature
        // immediately before the write; matching only an opcode is not ownership.
        constexpr std::uint8_t kVRSetForegroundModelCall[] = {
            0xE8, 0x40, 0x15, 0x00, 0x00
        };
        constexpr std::uint8_t kOGInitModelPrologue[] = {
            0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8D
        };
        constexpr std::uint8_t kNGSetModelPrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18,
            0x48, 0x89, 0x74, 0x24, 0x20
        };
        constexpr std::uint8_t kNGDisableAnimationOriginal[] = {
            0x41, 0x83, 0x7E, 0x68, 0x02
        };
        constexpr std::uint8_t kConditionalLoadingBranch[] = {
            0x75, 0x1E
        };
        constexpr std::uint8_t kVRTimerUntieSignature[] = {
            0x08, 0x00, 0x00, 0x00, 0x48, 0x8B,
            0xC1, 0x48, 0x3B, 0xCA, 0x48, 0x0F
        };
        constexpr std::uint8_t kVRFPSClampSignature[] = {
            0x08, 0x0F, 0x57, 0xC0, 0x0F, 0x2E,
            0xD0, 0x74, 0x38, 0x0F, 0x28, 0xCA
        };
        constexpr std::uint8_t kVRTimerUntied[] = { 0x00 };
        constexpr std::uint8_t kVRFPSClampDisabled[] = { 0x38 };

        // An Address Library ID alone is not sufficient ownership for a code
        // patch: this CommonLib fork can resolve an absent ID to a neighbouring
        // entry. Require the exact, independently verified RVA for each supported
        // NG executable before inspecting or changing any bytes at that site.
        constexpr std::uintptr_t kNGSetModelRva_1_11_221 = 0x99A1A0;
        constexpr std::uintptr_t kNGSetModelRva_1_11_240 = 0x99A4B0;
        constexpr std::uintptr_t kNGDisableAnimationRva_1_11_221 = 0xBD70A0;
        constexpr std::uintptr_t kNGDisableAnimationRva_1_11_240 = 0xBD7430;
        constexpr std::uintptr_t kNGDisableBlackRva_1_11_221 = 0x10665F0;
        constexpr std::uintptr_t kNGDisableBlackRva_1_11_240 = 0x1066980;

        // Serializes this plugin's compare/write/restore sequences. It cannot make
        // another DLL participate, so every operation still performs its own
        // exact byte comparison immediately before writing.
        std::mutex s_codePatchMutex;

        enum class PatchWriteResult
        {
            kApplied,
            kExpectedMismatch,
            kAccessViolation,
            kPostconditionMismatch
        };

        // SEH-only helpers stay free of destructible locals. expectedSize may be
        // larger than replacementSize (for example, a one-byte RET guarded by a
        // ten-byte function prologue). The untouched signature tail is checked
        // again after the write.
        PatchWriteResult SehWriteBytesIfExact(
            std::uintptr_t addr,
            const std::uint8_t* expected,
            std::size_t expectedSize,
            const std::uint8_t* replacement,
            std::size_t replacementSize) noexcept
        {
            __try {
                if (!addr || !expected || !replacement ||
                    replacementSize == 0 || replacementSize > expectedSize) {
                    return PatchWriteResult::kExpectedMismatch;
                }

                if (std::memcmp(
                        reinterpret_cast<const void*>(addr),
                        expected,
                        expectedSize) != 0) {
                    return PatchWriteResult::kExpectedMismatch;
                }

                REL::safe_write(addr, replacement, replacementSize);
                FlushInstructionCache(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(addr),
                    expectedSize);

                if (std::memcmp(
                        reinterpret_cast<const void*>(addr),
                        replacement,
                        replacementSize) != 0 ||
                    (expectedSize > replacementSize &&
                     std::memcmp(
                         reinterpret_cast<const void*>(addr + replacementSize),
                         expected + replacementSize,
                         expectedSize - replacementSize) != 0)) {
                    return PatchWriteResult::kPostconditionMismatch;
                }
                return PatchWriteResult::kApplied;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return PatchWriteResult::kAccessViolation;
            }
        }

        bool SehReadBytes(
            std::uintptr_t addr,
            std::uint8_t* destination,
            std::size_t size) noexcept
        {
            __try {
                if (!addr || !destination || size == 0) {
                    return false;
                }
                std::memcpy(
                    destination,
                    reinterpret_cast<const void*>(addr),
                    size);
                return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        const char* PatchResultName(PatchWriteResult result) noexcept
        {
            switch (result) {
            case PatchWriteResult::kApplied:
                return "applied";
            case PatchWriteResult::kExpectedMismatch:
                return "expected bytes not owned";
            case PatchWriteResult::kAccessViolation:
                return "access violation";
            case PatchWriteResult::kPostconditionMismatch:
                return "postcondition mismatch";
            default:
                return "unknown";
            }
        }

        bool IsExactNGAddressLibraryRva(
            const char* site,
            std::uint32_t id,
            std::uintptr_t resolvedAddress,
            std::uintptr_t expectedRva221,
            std::uintptr_t expectedRva240)
        {
            const auto version = REL::Module::get().version();
            std::uintptr_t expectedRva = 0;
            if (version[0] == 1 && version[1] == 11 &&
                version[2] == 221 && version[3] == 0) {
                expectedRva = expectedRva221;
            } else if (version[0] == 1 && version[1] == 11 &&
                       version[2] == 240 && version[3] == 0) {
                expectedRva = expectedRva240;
            } else {
                logger::warn(
                    "NG: {} disabled: runtime {}.{}.{}.{} has no exact "
                    "Address Library RVA mapping for ID {}",
                    site, version[0], version[1], version[2], version[3], id);
                return false;
            }

            const auto moduleBase = REL::Module::get().base();
            if (!resolvedAddress || resolvedAddress < moduleBase) {
                logger::warn(
                    "NG: {} disabled: Address Library ID {} resolved outside "
                    "the executable image (address={:x}, base={:x})",
                    site, id, resolvedAddress, moduleBase);
                return false;
            }

            const auto resolvedRva = resolvedAddress - moduleBase;
            if (resolvedRva != expectedRva) {
                logger::warn(
                    "NG: {} disabled: Address Library ID {} resolved to RVA "
                    "{:x}, expected exact RVA {:x} for {}.{}.{}.{}",
                    site, id, resolvedRva, expectedRva,
                    version[0], version[1], version[2], version[3]);
                return false;
            }
            return true;
        }
    }

    // ========================================================================
    // Performance patches
    // ========================================================================

    void PerformancePatches::Apply(const PerformanceConfig& config)
    {
        // Capture vsync-toggle intent so the DEFERRED arm in OnLoadingMenuOpen honours
        // the config flag (it previously re-armed regardless, making the flag a no-op).
        s_vsyncToggleEnabled.store(config.disableVSyncWhileLoading);

        bool isVR = REL::Module::IsVR();
        bool isNG = !isVR && REL::Module::IsNG();

        // NG address-library IDs that we byte-patch were verified on specific NG
        // builds. On this fork an ID ABSENT from the loaded versionlib silently
        // resolves to a NEIGHBOUR's offset on non-VR (no exact-match check), so a
        // future NG patch could make a REL::ID byte-write land on a random function
        // and corrupt code. Gate the NG REL::ID byte-writes to the builds actually
        // verified; an unrecognised NG build skips them (loses the speed patch, never
        // corrupts). Exact resolved-RVA and byte-signature checks provide the two
        // additional ownership gates on supported NG builds.
        bool ngVerified = false;
        if (isNG) {
            const auto ver = REL::Module::get().version();
            ngVerified =
                ver[0] == 1 && ver[1] == 11 && ver[3] == 0 &&
                (ver[2] == 221 || ver[2] == 240);
            if (!ngVerified) {
                logger::warn("NG: build {}.{}.{} not in the verified set — skipping NG REL::ID byte-patches "
                             "(speed patches off, no corruption risk)", ver[0], ver[1], ver[2]);
            }
        }

        logger::info("Runtime detection:");
        try {
            auto v = REL::Module::get().version();
            int v0 = static_cast<int>(v[0]);
            int v1 = static_cast<int>(v[1]);
            int v2 = static_cast<int>(v[2]);
            int v3 = static_cast<int>(v[3]);
            int rt = static_cast<int>(REL::Module::GetRuntime());
            logger::info("Module: version={}.{}.{}.{} runtime={} isVR={} isNG={}",
                v0, v1, v2, v3, rt, isVR ? 1 : 0, isNG ? 1 : 0);
        } catch (const std::exception& e) {
            logger::warn("Module diag threw: {}", e.what());
        } catch (...) {
            logger::warn("Module diag threw unknown exception");
        }

        // ================================================================
        // OG NOP5 VEH: catches access violation from corrupted epilogue return
        // Fixes the stack and redirects to the proper return address
        // ================================================================
        // NOP5 replaces "add rsp,0x20; pop rdi" before RET.
        // RET pops [RSP] (garbage from frame) and jumps to it → access violation.
        // VEH restores: RSP += 0x18 (remaining frame), pop RDI, pop return addr, continue.

        // ================================================================
        // Disable 3D model on loading screens
        // VR: NOP5 the SetForegroundModel CALL in AdvanceMovie (prevents per-frame model update)
        // Flat: RET at InitModel entry (prevents model creation entirely, enables luminance key)
        // ================================================================
        if (config.disable3DModel) {
            if (isVR) {
                // Preserve BackgroundScreenModel::InitModels. Fallout requires
                // the state it constructs even when no model will be presented;
                // RET-patching that initializer leaves a null object which the
                // startup LoadingMenu dereferences at VR RVA 0x9E4F3D.
                //
                // Suppress only the verified per-frame foreground-selection
                // call. Apply() runs at kGameDataReady, before the interactive
                // main menu can begin the first timed save load.
                REL::Relocation<std::uintptr_t> target{
                    REL::Offset(SetForegroundModel_Call_Offset_OG)
                };
                PatchWriteResult callResult;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    callResult = SehWriteBytesIfExact(
                        target.address(),
                        kVRSetForegroundModelCall,
                        sizeof(kVRSetForegroundModelCall),
                        NOP5,
                        sizeof(NOP5));
                }
                if (callResult == PatchWriteResult::kApplied) {
                    logger::info(
                        "VR: SetForegroundModel CALL NOP5 applied at {:x}",
                        target.address());
                } else {
                    logger::warn(
                        "VR: SetForegroundModel CALL NOP5 skipped at {:x}: {}",
                        target.address(), PatchResultName(callResult));
                }
            } else if (isNG && ngVerified) {
                // NG/AE: RET BackgroundScreenModel's per-load "set model"
                // function (NG ID 2219591; verified at RVA 0x99a1a0 on
                // 1.11.221 and 0x99a4b0 on 1.11.240). Both callers are LoadingMenu
                // virtual methods (Func0 / Func4) that fire on each menu open
                // to tell the renderer which model to show. By RETting at
                // entry we skip the vtable[0x1d0] "show" call so the model
                // never gets registered with Interface3D::Renderer.
                //
                // Why not `BackgroundScreenModel::InitModels` (ID 2219592):
                // that one runs once at startup via `Main::Init_UI_Systems`,
                // BEFORE our plugin's Apply() at kGameDataReady — too late.
                //
                // Why not the OG-style `LoadingMenu::InitModel` (ID 724763):
                // NG's address library maps that ID to an unrelated TES/
                // power-grid function, so the OG patch corrupts random game
                // code on NG (see commit history for details).
                REL::Relocation<std::uintptr_t> setModel{
                    REL::ID(BackgroundScreenModel_SetModel_ID_NG) };
                if (IsExactNGAddressLibraryRva(
                        "BackgroundScreenModel::SetModel RET",
                        BackgroundScreenModel_SetModel_ID_NG,
                        setModel.address(),
                        kNGSetModelRva_1_11_221,
                        kNGSetModelRva_1_11_240)) {
                    PatchWriteResult result;
                    {
                        std::lock_guard lock(s_codePatchMutex);
                        result = SehWriteBytesIfExact(
                            setModel.address(),
                            kNGSetModelPrologue,
                            sizeof(kNGSetModelPrologue),
                            RET,
                            sizeof(RET));
                    }
                    if (result == PatchWriteResult::kApplied) {
                        logger::info(
                            "NG: BackgroundScreenModel::SetModel RET applied at {:x} (NG ID {})",
                            setModel.address(), BackgroundScreenModel_SetModel_ID_NG);
                    } else {
                        logger::warn(
                            "NG: BackgroundScreenModel::SetModel RET skipped at {:x}: {}",
                            setModel.address(), PatchResultName(result));
                    }
                }
            } else if (!isNG) {
                // OG flat: RET InitModel only (no 3D model, tips still render).
                // Dynamic animation loop NOP in LoadingScreenManager handles speed.
                // MUST be gated on !isNG: the OG InitModel_ID (724763) maps to an
                // unrelated function on NG's address library, so an UNVERIFIED NG
                // build (isNG && !ngVerified — skipped the NG branch above) must NOT
                // fall through to here and write RET into mis-resolved NG code. It
                // simply gets no disable-3D-model patch (the safe degrade).
                REL::Relocation<std::uintptr_t> initModel{ REL::ID(InitModel_ID) };
                PatchWriteResult result;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    result = SehWriteBytesIfExact(
                        initModel.address(),
                        kOGInitModelPrologue,
                        sizeof(kOGInitModelPrologue),
                        RET,
                        sizeof(RET));
                }
                if (result == PatchWriteResult::kApplied) {
                    logger::info("Flat: InitModel RET applied at {:x}", initModel.address());
                } else {
                    logger::warn(
                        "Flat: InitModel RET skipped at {:x}: {}",
                        initModel.address(), PatchResultName(result));
                }
            }
        }

        // NG/AE animation ownership is independent of model suppression.
        // HFPF's DisableAnimationOnLoadingScreens overlaps only this NOP5; callers
        // may disable this flag while retaining the unique SetModel RET above.
        if (config.disableAnimationOnLoadingScreens && isNG && ngVerified) {
            REL::Relocation<std::uintptr_t> loadingFunc{
                REL::ID(DisableAnimation_ID_NG) };
            if (IsExactNGAddressLibraryRva(
                    "DisableAnimation",
                    DisableAnimation_ID_NG,
                    loadingFunc.address(),
                    kNGDisableAnimationRva_1_11_221,
                    kNGDisableAnimationRva_1_11_240)) {
                const auto patchAddr =
                    loadingFunc.address() + DisableAnimation_Offset_NG;
                std::uint8_t current[sizeof(kNGDisableAnimationOriginal)]{};
                const bool readable =
                    SehReadBytes(patchAddr, current, sizeof(current));
                const bool exact =
                    readable &&
                    std::memcmp(
                        current,
                        kNGDisableAnimationOriginal,
                        sizeof(current)) == 0;
                if (!exact) {
                    logger::warn(
                        "NG: DisableAnimation site disabled at {:x}: exact "
                        "41 83 7E 68 02 signature not owned",
                        patchAddr);
                } else {
                    {
                        std::lock_guard lock(s_codePatchMutex);
                        std::memcpy(
                            s_ngDisableAnimOrigBytes,
                            kNGDisableAnimationOriginal,
                            sizeof(s_ngDisableAnimOrigBytes));
                        s_ngDisableAnimAddr = patchAddr;
                        s_ngDisableAnimSaved = true;
                    }
                    logger::info(
                        "NG: DisableAnimation exact site verified at {:x} "
                        "(ID {} + {:x}); per-load apply deferred to mode gate",
                        patchAddr, DisableAnimation_ID_NG,
                        DisableAnimation_Offset_NG);
                }
            }
        }

        // ================================================================
        // DisableBlackLoadingScreens — patch conditional JZ/JNZ to unconditional JMP
        // Prevents game from showing completely black loading screen (no text/model).
        //
        // SAFETY: require the complete verified `75 1E` instruction before
        // changing only its opcode to EB. OG and VR use different RVAs; the old
        // shared VR RVA points at an unrelated event-system CALL on OG.
        //
        // This is intentionally not part of the default speed preset. It skips
        // LoadingMenu destination-state assignment (presentation routing), not
        // save/cell I/O, and has caused unsafe transition behaviour in VR.
        // ================================================================
        if (config.disableBlackLoadingScreens) {
            auto tryPatchCondJmp = [](std::uintptr_t addr, const char* tag) {
                PatchWriteResult result;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    result = SehWriteBytesIfExact(
                        addr,
                        kConditionalLoadingBranch,
                        sizeof(kConditionalLoadingBranch),
                        JMP_SHORT,
                        sizeof(JMP_SHORT));
                }
                if (result == PatchWriteResult::kApplied) {
                    logger::info(
                        "{}: DisableBlackLoadingScreens at {:x} "
                        "(75 1E -> EB 1E)",
                        tag, addr);
                } else {
                    logger::warn(
                        "{}: DisableBlackLoadingScreens SKIPPED — bytes at "
                        "{:x} are not our exact 75 1E instruction: {}",
                        tag, addr, PatchResultName(result));
                }
            };
            if (isNG && ngVerified) {
                REL::Relocation<std::uintptr_t> target{
                    REL::ID(DisableBlackLoading_ID_NG) };
                if (IsExactNGAddressLibraryRva(
                        "DisableBlackLoadingScreens",
                        DisableBlackLoading_ID_NG,
                        target.address(),
                        kNGDisableBlackRva_1_11_221,
                        kNGDisableBlackRva_1_11_240)) {
                    tryPatchCondJmp(
                        target.address() + DisableBlackLoading_Offset_NG,
                        "NG");
                }
            } else if (!isNG) {
                const auto offset = isVR
                    ? DisableBlackLoadingScreens_Offset_VR
                    : DisableBlackLoadingScreens_Offset_OG;
                REL::Relocation<std::uintptr_t> target{ REL::Offset(offset) };
                tryPatchCondJmp(target.address(), isVR ? "VR" : "OG");
            }
        }

        // ================================================================
        // VSync disable during loading
        // ================================================================
        if (config.disableVSyncWhileLoading) {
            if (isVR) {
                REL::Relocation<void**> rendererDataPtr{ REL::Offset(BSGraphics_RendererData_Offset_VR) };
                void* rendererData = *rendererDataPtr;
                if (rendererData) {
                    auto* p = reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uintptr_t>(rendererData) + PresentInterval_Offset);
                    s_presentIntervalPtr.store(p);
                    s_originalPresentInterval.store(*p);
                    s_vsyncEnabled.store(true);
                    logger::info("VR: VSync toggle ready (presentInterval={})",
                        s_originalPresentInterval.load());
                }
            } else if (isNG) {
                // NG: RE::BSGraphics::RendererData::GetSingleton() crashes (wrong address library ID
                // returns garbage pointer). Skip here — OnLoadingMenuOpen retries the safe path.
                logger::info("NG: VSync toggle deferred (singleton unsafe at init)");
            } else {
                // OG: try now if device available, else defer to OnLoadingMenuOpen
                if (VRCompositorHelper::GetD3D11Device()) {
                    auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
                    if (rendererData) {
                        auto* p = &rendererData->presentInterval;
                        s_presentIntervalPtr.store(p);
                        s_originalPresentInterval.store(*p);
                        s_vsyncEnabled.store(true);
                        logger::info("Flat: VSync toggle ready (presentInterval={})",
                            s_originalPresentInterval.load());
                    }
                } else {
                    logger::info("Flat: VSync toggle deferred (device not yet available)");
                }
            }
        }

        // ================================================================
        // iFPSClamp disable
        // ================================================================
        if (config.disableiFPSClamp && !isNG && !isVR) {
            // RE::GetINISetting crashes on NG 1.11.191 (internal -1 sentinel pointer).
            // OG applies this here. March VR deferred both the code byte and the
            // iFPSClamp setting until the first successful save had closed, so
            // ApplyTimerPatches owns VR's setting write as well.
            [&]() {
                __try {
                    if (auto* setting = RE::GetINISetting("iFPSClamp:General")) {
                        setting->SetInt(0);
                        logger::info("iFPSClamp:General set to 0");
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    logger::warn("iFPSClamp disable skipped (access violation)");
                }
            }();
        }

        // ================================================================
        // OneThreadWhileLoading — limit to single CPU core during loading
        // Reduces context switching, allows loading thread to max out one core.
        // ================================================================
        if (config.oneThreadWhileLoading) {
            HANDLE hProcess = GetCurrentProcess();
            DWORD_PTR procMask, sysMask;
            if (GetProcessAffinityMask(hProcess, &procMask, &sysMask) && procMask != 0) {
                s_normalAffinityMask = procMask;
                s_oneThreadEnabled.store(true);
                logger::info("OneThreadWhileLoading enabled (normalMask={:x})", s_normalAffinityMask);
            } else {
                logger::warn("OneThreadWhileLoading unavailable: could not query process affinity");
            }
        }

        // The old load-scoped PresentThread patch replaced a live ten-byte store
        // and later restored it with two ordinary memcpy-style writes. Exact byte
        // checks do not make a ten-byte instruction rewrite atomic: a worker can
        // execute a torn instruction. Until this is implemented as a proven hook
        // or atomic transition, fail closed and leave Bethesda's code untouched.
        if (config.yieldCPUDuringLoading && !isVR) {
            logger::warn(
                "PresentThread FixCPUThreads-equivalent disabled: refusing "
                "unsafe live ten-byte NOP/restore");
        }

        // NG/AE: no MinHook hooks — loading detection uses kPreLoadGame/kPostLoadGame.
        // DisableAnimation NOP5 handles speed, no need to hook AdvanceMovie/InitModel.

        // Publish active only after all setup completed. If Apply throws, the caller
        // catches it and per-load handlers remain inert instead of operating on a
        // half-initialized patch set.
        s_patchesActive.store(true, std::memory_order_release);
        logger::info(
            "Performance patches applied (VR={}, NG={}, presentThreadPatch={})",
            isVR, isNG, false);
    }

    void PerformancePatches::ApplyTimerPatches(const PerformanceConfig& config)
    {
        if (!REL::Module::IsVR()) return;

        static std::atomic<bool> s_attempted{ false };
        if (s_attempted.exchange(true, std::memory_order_acq_rel)) return;

        if (config.untieSpeedFromFPS) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(UntieSpeedFromFPS_Offset_VR) };
            PatchWriteResult result;
            {
                std::lock_guard lock(s_codePatchMutex);
                result = SehWriteBytesIfExact(
                    target.address(),
                    kVRTimerUntieSignature,
                    sizeof(kVRTimerUntieSignature),
                    kVRTimerUntied,
                    sizeof(kVRTimerUntied));
            }
            if (result == PatchWriteResult::kApplied) {
                logger::info(
                    "VR: UntieSpeedFromFPS patched at {:x}",
                    target.address());
            } else {
                logger::warn(
                    "VR: UntieSpeedFromFPS skipped at {:x}: {}",
                    target.address(), PatchResultName(result));
            }
        }

        if (config.disableiFPSClamp) {
            REL::Relocation<std::uintptr_t> target{ REL::Offset(DisableiFPSClamp_Offset_VR) };
            PatchWriteResult result;
            {
                std::lock_guard lock(s_codePatchMutex);
                result = SehWriteBytesIfExact(
                    target.address(),
                    kVRFPSClampSignature,
                    sizeof(kVRFPSClampSignature),
                    kVRFPSClampDisabled,
                    sizeof(kVRFPSClampDisabled));
            }
            if (result == PatchWriteResult::kApplied) {
                logger::info(
                    "VR: DisableiFPSClamp patched at {:x}",
                    target.address());

                if (auto* setting = RE::GetINISetting("iFPSClamp:General")) {
                    setting->SetInt(0);
                }
            } else {
                logger::warn(
                    "VR: DisableiFPSClamp skipped at {:x}: {}",
                    target.address(), PatchResultName(result));
            }
        }
    }

    void PerformancePatches::OnLoadingMenuOpen()
    {
        // Baseline (benchmark=0): Apply() never ran, so do nothing — no vsync
        // disable, no affinity change, no NG anim NOP. Keeps the measurement vanilla.
        if (!s_patchesActive.load()) return;

        // Retry deferred VSync setup if not initialized yet
        // Skip on NG — RendererData::GetSingleton may crash (report_and_fail)
        if (s_vsyncToggleEnabled.load() &&
            !s_vsyncEnabled.load() && !s_presentIntervalPtr.load() && !REL::Module::IsVR() && !REL::Module::IsNG()) {
            try {
                auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
                if (rendererData) {
                    auto* p = &rendererData->presentInterval;
                    s_presentIntervalPtr.store(p);
                    s_originalPresentInterval.store(*p);
                    s_vsyncEnabled.store(true);
                    logger::info("Flat: VSync toggle ready (deferred, presentInterval={})",
                        s_originalPresentInterval.load());
                }
            } catch (...) {}
        }

        if (auto* p = s_presentIntervalPtr.load(); s_vsyncEnabled.load() && p) {
            auto orig = *p;
            s_originalPresentInterval.store(orig);
            *p = 0;
            logger::info("VSync disabled for loading (was {})", orig);
        }

        if (s_oneThreadEnabled.load()) {
            // Use the lowest CPU that is actually allowed for this process;
            // bit zero is not guaranteed to be present in a constrained job.
            const DWORD_PTR singleCore = s_normalAffinityMask & (~s_normalAffinityMask + 1);
            if (SetProcessAffinityMask(GetCurrentProcess(), singleCore)) {
                logger::info("Loading: limited to CPU mask {:x}", singleCore);
            } else {
                logger::warn("Loading: failed to apply single-core affinity");
            }
        }

        // NG DisableAnimation NOP5 — NG's ONLY loading-render speed patch (NG has
        // no animation-loop NOP). Mode gating:
        //   0 (Black) / 2 (Background): apply immediately — no tips to render.
        //   3 (Bg+Tips):  DEFER — keep the render alive so the SWF draws the tips,
        //                 then apply via ApplyNGDisableAnimDeferred() from Update()
        //                 once AdvanceMovie is frozen (tips captured). This reclaims
        //                 the speedup for the tail of the load; previously mode 3
        //                 (the shipped default) silently got NONE of this patch.
        //   1 (Native):   never apply — native tips must render the whole load.
        if (D3D11Compositor::GetSingleton().IsRenderReady()) {
            int mode = D3D11Compositor::GetSingleton().GetFlatMode();
            if (mode == 0 || mode == 2) {
                PatchWriteResult result = PatchWriteResult::kExpectedMismatch;
                std::uintptr_t address = 0;
                bool attempted = false;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    if (s_ngDisableAnimSaved && !s_ngDisableAnimApplied) {
                        attempted = true;
                        address = s_ngDisableAnimAddr;
                        result = SehWriteBytesIfExact(
                            address,
                            s_ngDisableAnimOrigBytes,
                            sizeof(s_ngDisableAnimOrigBytes),
                            NOP5,
                            sizeof(NOP5));
                        if (result == PatchWriteResult::kApplied) {
                            s_ngDisableAnimApplied = true;
                        } else if (
                            result == PatchWriteResult::kExpectedMismatch ||
                            result == PatchWriteResult::kPostconditionMismatch) {
                            // Another owner changed the site. Relinquish it
                            // permanently rather than adopting or later restoring it.
                            s_ngDisableAnimSaved = false;
                            s_ngDisableAnimApplied = false;
                            s_ngDisableAnimAddr = 0;
                        }
                    }
                }
                if (attempted && result == PatchWriteResult::kApplied) {
                    logger::info(
                        "NG: DisableAnimation NOP5 applied at {:x} (mode={})",
                        address, mode);
                } else if (attempted) {
                    logger::warn(
                        "NG: DisableAnimation NOP5 skipped at {:x}: {}",
                        address, PatchResultName(result));
                }
            } else if (mode == 3) {
                bool available = false;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    available =
                        s_ngDisableAnimSaved && !s_ngDisableAnimApplied;
                }
                if (available) {
                    logger::info(
                        "NG: DisableAnimation NOP5 deferred (mode 3) "
                        "until tips captured");
                }
            } else {
                bool available = false;
                {
                    std::lock_guard lock(s_codePatchMutex);
                    available =
                        s_ngDisableAnimSaved && !s_ngDisableAnimApplied;
                }
                if (available) {
                    logger::info(
                        "NG: DisableAnimation kept off (mode={}) so "
                        "native tips render",
                        mode);
                }
            }
        }
    }

    // Deferred apply of the NG DisableAnimation NOP5 — called from
    // LoadingScreenManager::Update() on NG mode 3 once AdvanceMovie has been
    // frozen (tips drawn + captured). Idempotent: no-op if not saved / already
    // applied. OnLoadingMenuClose restores it via the existing s_ngDisableAnimApplied path.
    void PerformancePatches::ApplyNGDisableAnimDeferred()
    {
        if (!D3D11Compositor::GetSingleton().IsRenderReady()) return;

        PatchWriteResult result = PatchWriteResult::kExpectedMismatch;
        std::uintptr_t address = 0;
        bool attempted = false;
        {
            std::lock_guard lock(s_codePatchMutex);
            if (s_ngDisableAnimSaved && !s_ngDisableAnimApplied) {
                attempted = true;
                address = s_ngDisableAnimAddr;
                result = SehWriteBytesIfExact(
                    address,
                    s_ngDisableAnimOrigBytes,
                    sizeof(s_ngDisableAnimOrigBytes),
                    NOP5,
                    sizeof(NOP5));
                if (result == PatchWriteResult::kApplied) {
                    s_ngDisableAnimApplied = true;
                } else if (
                    result == PatchWriteResult::kExpectedMismatch ||
                    result == PatchWriteResult::kPostconditionMismatch) {
                    s_ngDisableAnimSaved = false;
                    s_ngDisableAnimApplied = false;
                    s_ngDisableAnimAddr = 0;
                }
            }
        }
        if (attempted && result == PatchWriteResult::kApplied) {
            logger::info(
                "NG: DisableAnimation NOP5 applied "
                "(deferred, post tips-capture) at {:x}",
                address);
        } else if (attempted) {
            logger::warn(
                "NG: DisableAnimation deferred NOP5 skipped at {:x}: {}",
                address, PatchResultName(result));
        }
    }

    void PerformancePatches::OnLoadingMenuClose()
    {
        if (!s_patchesActive.load()) return;  // baseline: nothing to restore

        if (auto* p = s_presentIntervalPtr.load(); s_vsyncEnabled.load() && p) {
            auto orig = s_originalPresentInterval.load();
            *p = orig;
            logger::info("VSync restored to {}", orig);
        }

        if (s_oneThreadEnabled.load()) {
            if (SetProcessAffinityMask(GetCurrentProcess(), s_normalAffinityMask)) {
                logger::info("Loading: restored CPU affinity ({:x})", s_normalAffinityMask);
            } else {
                logger::warn("Loading: failed to restore CPU affinity ({:x})", s_normalAffinityMask);
            }
        }

        // Restore NG DisableAnimation original bytes if we applied them this load.
        PatchWriteResult restoreResult = PatchWriteResult::kExpectedMismatch;
        std::uintptr_t restoreAddress = 0;
        bool restoreAttempted = false;
        {
            std::lock_guard lock(s_codePatchMutex);
            if (s_ngDisableAnimApplied) {
                restoreAttempted = true;
                restoreAddress = s_ngDisableAnimAddr;
                restoreResult = SehWriteBytesIfExact(
                    restoreAddress,
                    NOP5,
                    sizeof(NOP5),
                    s_ngDisableAnimOrigBytes,
                    sizeof(s_ngDisableAnimOrigBytes));
                if (restoreResult == PatchWriteResult::kApplied) {
                    s_ngDisableAnimApplied = false;
                } else if (
                    restoreResult == PatchWriteResult::kExpectedMismatch ||
                    restoreResult == PatchWriteResult::kPostconditionMismatch) {
                    // The bytes are no longer our exact NOP. Never overwrite the
                    // new owner with the stale startup snapshot.
                    s_ngDisableAnimSaved = false;
                    s_ngDisableAnimApplied = false;
                    s_ngDisableAnimAddr = 0;
                }
            }
        }
        if (restoreAttempted &&
            restoreResult == PatchWriteResult::kApplied) {
            logger::info(
                "NG: DisableAnimation restored at {:x}", restoreAddress);
        } else if (restoreAttempted) {
            logger::warn(
                "NG: DisableAnimation restore skipped at {:x}: {}",
                restoreAddress, PatchResultName(restoreResult));
        }
    }

}
