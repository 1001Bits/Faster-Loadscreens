#pragma once

namespace VRLoadingScreens
{
    // Guard for a latent vanilla Fallout 4 VR crash on worldspace transitions
    // (reproduced at the Diamond City exit gate; two byte-identical Buffout
    // logs, Fallout4VR.exe+0x35BFA5, "mov r8, [r15+0x58]", read of 0x2B0).
    //
    // The engine's own call chain, every link a verified direct call:
    //
    //   0xD83DE0 Main per-frame update
    //     -> 0xF03BD0 queued-teleport processor
    //       -> 0xF2BC20 MoveTo
    //         -> 0xF063E0 detach actor from world
    //           -> 0xF6990  TES purge
    //             -> 0x100880 attached-cell purge loop  (no filters at all)
    //               -> 0x11B800 TESObjectCELL unload
    //                 -> 0x47B9B0 worldspace cell-unregister   <-- hooked here
    //                   -> 0x35BF10 hashmap probe              <-- faults
    //
    // A TESObjectCELL in TES->attachedCells (TES+0x68) had cellFlags(+0x40)==0
    // and worldSpace(+0xC8)==0. 0x47B9B0 receives that null worldspace in rcx,
    // keeps it in rbp, and its ONLY guard re-derives the same field from the
    // cell and compares it to itself:
    //
    //   0x47B9FD  mov rax, [rdx+0xC8]     ; cell->worldSpace  (NULL)
    //   0x47BA04  cmp rax, rbp            ; NULL == NULL
    //   0x47BA07  jne bail                ; not taken
    //   0x47BA09  lea rcx, [rbp+0x258]    ; 0x258
    //   0x47BA10  call 0x35BF10           ; -> mov r8,[r15+0x58] -> read 0x2B0
    //
    // so the check can never fire for the one case that matters. R15 = 0x258
    // and the 0x2B0 fault address in the crash log are that arithmetic exactly.
    //
    // The guard below performs the null check the engine omits here and applies
    // elsewhere on the same field (0x2042C4). On null it takes the engine's own
    // guard-failure path: return without doing anything. That path (0x47BA6E)
    // is a pure epilogue, and the function's only side effect before the fault
    // is an allocator tag written at 0x47B9E8 — after our hook point — so
    // returning at entry is exactly equivalent and cannot leak engine state.
    //
    // This cannot change any case that works today: it only intercepts calls
    // that would otherwise dereference null. It converts the crash into a
    // leaked hashmap entry plus a log line naming the offending cell, which is
    // also the diagnostic that tells us where the bad cell came from:
    //   vtable 0 or non-TESObjectCELL -> recycled/freed memory
    //   real vtable with a real FormID -> an initialisation-order bug
    class CellWorldspaceGuard
    {
    public:
        // VR 1.2.72 only. Idempotent; safe to call more than once.
        static bool Install();
        // Number of null-worldspace unregisters intercepted this session.
        static int InterceptCount();
    };
}
