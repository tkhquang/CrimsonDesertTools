#pragma once

// Thin wrapper over CrimsonDesertCore's AOB resolver. Re-exports the
// framework types/functions into the EquipHide namespace via `using`
// declarations so existing call sites stay unchanged, aliases the shared
// candidate tables from CDCore::Anchors by reference, and carries the
// EquipHide-only candidate tables.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/aob_resolver.hpp>
#include <cdcore/anchors.hpp>

#include <cstdint>

namespace EquipHide
{
    using CDCore::AddrCandidate;
    using CDCore::AobCandidate;
    using CDCore::CompiledCandidate;
    using CDCore::ResolveMode;

    using CDCore::resolve_address;
    using CDCore::scan_for_hook_target;
    using CDCore::sanity_check_function_prologue;

    // --- Shared candidate tables (aliased by reference from CDCore) -------

    inline constexpr auto &k_worldSystemCandidates =
        CDCore::Anchors::k_worldSystemCandidates;
    inline constexpr auto &k_mapLookupCandidates =
        CDCore::Anchors::k_mapLookupCandidates;
    inline constexpr auto &k_partAddShowCandidates =
        CDCore::Anchors::k_partAddShowCandidates;
    // sub_14076D520 -- EH calls this "VisualEquipChange" (so does LT); keep
    // the unified name.
    inline constexpr auto &k_visualEquipChangeCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    // sub_14075BBF0 -- EH historically called this "VisualEquipSwap", LT
    // called it "BatchEquip". Unified as "BatchEquip" (LT's name) since it
    // better describes the function's role (entry-table scan + dispatch).
    // An alias is provided under the old EH name so existing EH call sites
    // keep working.
    inline constexpr auto &k_batchEquipCandidates =
        CDCore::Anchors::k_batchEquipCandidates;
    inline constexpr auto &k_visualEquipSwapCandidates =
        CDCore::Anchors::k_batchEquipCandidates;

    // --- EquipHide-only candidate tables ---------------------------------

    /**
     * @brief ChildActor vtable -- static data pointer resolved via three
     *        nearby RIP-relative loads. All three candidates resolve to the
     *        same `lea rax, [rip+disp32]` that loads the vtable base; the
     *        cascade picks whichever anchor shape survives the current build.
     *
     * @note Branch-encoding caveat (aob-signatures.md §8):
     *       P2 anchors on the trailing `EB 03` (jmp-over-fallback) that
     *       follows the vtable store. Wildcarding its opcode loses
     *       uniqueness on the live build (5 matches without it). If a
     *       future compiler flips this jmp to the 6-byte `E9 rel32` form,
     *       P2 will stop matching -- P1 and P3 pick up the slack. P1 no
     *       longer crosses the jmp (truncated 2026-04-19) so it is
     *       encoding-independent. P3 wildcards the preceding `74 ??` jz
     *       pair as `?? ??` so it only declares a 2-byte slot, not the
     *       rel8 opcode literal.
     */
    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        // P1 -- truncated 2026-04-19: ends at the `mov [rsi], rax` that
        // stores the vtable. Does NOT cross the trailing `EB 03` jmp, so
        // it survives a Jcc-encoding flip.
        {"ChildActorVtbl_P1_AllocCtor",
         "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06",
         ResolveMode::RipRelative, 16, 20},

        // P2 -- retains the trailing `EB ?? 4C`: without the `4C` byte
        // of the post-jmp `mov rsi, r13` continuation, the lead-in is
        // structurally shared with 4 other ctor sites (CE aob_scan
        // 2026-04-19: 5 hits). Tied to the 2-byte jmp encoding.
        {"ChildActorVtbl_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- rel8 jz at offset +6 is wildcarded to `?? ??` (both bytes).
        // Does not hardcode the opcode literal, but still only matches
        // the 2-byte form (fails on `0F 84 rel32`).
        {"ChildActorVtbl_P3_WiderCtorStore",
         "45 31 ED 48 85 F6 ?? ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05",
         ResolveMode::RipRelative, 24, 28},
    };

    /**
     * @brief IndexedStringA map insert routine -- companion to MapLookup.
     */
    inline constexpr AddrCandidate k_mapInsertCandidates[] = {
        {"MapInsert_P1_FullPrologue",
         "4C 89 4C 24 20 53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, 0, 0},

        {"MapInsert_P2_InnerBody",
         "44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, -0x11, 0},

        {"MapInsert_P3_PrologueBody",
         "53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA 41 8B CA 45 85 D2",
         ResolveMode::Direct, -5, 0},
    };

    /**
     * @brief Hook target: PartInOut transition function (sub_14081DB90 /
     *        sub_140826900 on v1.03.01).
     *
     * Hook point: movzx eax, byte ptr [<vis_reg>+1Ch]; cmp al, 3
     *
     * Register layout at hook point (v1.03.01):
     *   R10 = pointer to part hash DWORD (IndexedStringA ID)
     *   RAX = pointer to PartInOutSocket struct (loaded from [RBP+5Fh])
     *   R8B = exclusion-list flag
     *   [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *   [RBP+0x4F] = a1 context pointer
     *
     * Register layout at hook point (v1.02.00 -- legacy):
     *   R10 = pointer to part hash DWORD
     *   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
     *   R8B = exclusion-list flag
     *   [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *   [RBP+0x4F] = a1 context pointer
     *
     * Cross-version cascade. v1.03.01 candidates come first (current live
     * game returns 1 hit for each); v1.02.00 candidates are retained for
     * users still on the older build and return 0 on the v1.03.01 game --
     * that's expected, not broken.
     */
    // @note Branch-encoding caveat (aob-signatures.md §8):
    //       The hook point is `movzx eax, byte [<vis>+0x1C] ; cmp al, 3`.
    //       All candidates below previously extended past that compare
    //       into the `74 ??` / `75 ??` conditional branches that follow.
    //       Each candidate has been truncated 2026-04-19 to terminate at
    //       or before the first rel8 branch so the signatures no longer
    //       hardcode a rel8 opcode. `offsetToHook` still points at the
    //       `movzx` instruction from the same match position (truncation
    //       only shortens the verified suffix, not the match start).
    inline constexpr AobCandidate k_hookSiteCandidates[] = {
        // PartInOut struct pointer in RAX, loaded from [RBP+5F].
        // P1 -- ends at `cmp al, 3`, does not cross the jz.
        {"PartInOut_P1_DirectSite",
         "48 8B 45 5F 0F B6 40 1C 3C 03",
         4},

        // P2 -- ends at `cmp rax, rcx`, does not cross the following jz.
        {"PartInOut_P2_WiderContext",
         "45 32 C0 49 8B 45 ?? 41 8B 4D ?? 48 C1 E1 04 48 03 C8 48 3B C1",
         0x30},

        // P3 -- ends at `cmp al, 3`, does not cross the jz.
        {"PartInOut_P3_PrecedingGate",
         "41 B0 01 48 8B 45 5F 0F B6 40 1C 3C 03",
         7},
    };

    /**
     * @brief AOB candidates for the Postfix rule evaluator.
     *
     * Virtual function at vtable[4] of objects with vtable 0x144CC8248.
     * Evaluates whether a postfix rule matches currently equipped items.
     * Returns 1 = rule matches (hide hair), 0 = no match (keep hair).
     *
     * Prologue shape (0x1A bytes from entry to `sub rsp, XX`):
     *
     *   48 89 5C 24 08              mov [rsp+8],  rbx
     *   48 89 6C 24 10              mov [rsp+10], rbp
     *   48 89 74 24 18              mov [rsp+18], rsi
     *   48 89 7C 24 20              mov [rsp+20], rdi
     *   41 54                       push r12
     *   41 56                       push r14
     *   41 57                       push r15
     *   48 83 EC ??                 sub rsp, XX
     *
     * The first 6 body instructions that every candidate shares:
     *
     *   4C 8B FA                    mov r15, rdx
     *   48 8B 5A ??                 mov rbx, [rdx+disp8]
     *   8B 42 ??                    mov eax, [rdx+disp8]
     *   48 8D 3C C3                 lea rdi, [rbx+rax*8]
     *   48 3B DF                    cmp rbx, rdi
     *   74 ??                       je  rel8                (NOT in signature; rel8 per aob-signatures.md §8)
     *
     * Every candidate below lands install on the true function entry
     * (either via `offsetToHook = 0` or via a negative walk-back equal
     * to the prologue length). Install addresses in the middle of the
     * prologue must be avoided: SafetyHook disassembles from the
     * requested byte and can silently decode a mid-instruction
     * position as a shorter unrelated instruction. In this function's
     * prologue, starting one byte into `mov [rsp+10], rbp` decodes as
     * the REX.W-stripped 32-bit `mov [rsp+10], ebp`, whose truncated
     * save corrupts rbp on the function's epilogue reload and crashes
     * the caller.
     */
    inline constexpr AddrCandidate k_postfixEvalCandidates[] = {
        // P1 -- tight. Full prologue (4 shadow-space saves + 3 pushes +
        // `sub rsp`) chained into the first 4 body instructions. Most
        // selective; first to fail if the compiler reshuffles save
        // order or spills a different non-volatile register set.
        // `sub rsp` imm8 is wildcarded because it tracks local-variable
        // size; every other byte is structural for this prologue shape.
        {"PostfixEval_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        // P2 -- medium. Drops the four shadow-space register saves and
        // anchors on the 3-push sequence + `sub rsp` + body tail.
        // Survives save-order shifts but stays tied to the specific
        // r12/r14/r15 callee-spill set. Match lands on the first
        // byte of `push r12` (function entry + 0x14), so the walk-back
        // is -0x14.
        {"PostfixEval_P2_PushesAndBody",
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- loose. Body-only fallback anchored on the distinctive
        // `mov rbx, [rdx+disp8] ; mov eax, [rdx+disp8] ; lea rdi,
        // [rbx+rax*8]` load-pair (the C3 SIB byte encodes rbx+rax*8,
        // a rare index shape) plus the start of the loop-header
        // `cmp rbx, rdi`. Pattern terminates at `48 3B` so it does
        // not include the following `Jcc rel8` (aob-signatures.md
        // §8 rel8 rule). Walk-back of -0x1A covers the full 0x1A-byte
        // prologue; survives both prologue save-order and push-set
        // changes as long as the loop-header body stays put.
        {"PostfixEval_P3_UniqueBody",
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x1A, 0},
    };

    /**
     * @brief Return-address landmark inside `createPrefabFromPartPrefab`
     *        (engine-registered profiling label for sub_14261B6C0).
     *
     * That function instantiates a renderable prefab from a PartPrefab,
     * then tail-calls sub_1402E1430(v107) -- inside which the rule-eval
     * pipeline reaches PostfixEval on the freshly-built instance. The
     * return address on the byte right after that inner call is the
     * landmark (match+12, the first byte of the post-call `mov rbx,
     * [rsp+0x4A0]` epilogue load).
     *
     * Player PostfixEval invocations come from the equipment-visibility
     * update loop elsewhere in the binary and never include this return
     * address on their stack. bald_fix uses stack presence of this
     * landmark to reject prefab-instantiation-path calls without caching
     * ctx pointers or depending on frequency heuristics.
     *
     * Instruction window (all three candidates land on the first byte of
     * the `lea rcx, [rbp+0x210]`; consumer adds +12 to reach the
     * landmark):
     *
     *   48 8D 8D 10 02 00 00        lea rcx, [rbp+0x210]
     *   E8 ?? ?? ?? ??              call sub_1402E1430
     *   48 8B 9C 24 A0 04 00 00     mov rbx, [rsp+0x4A0]   ; landmark
     *   48 81 C4 60 04 00 00        add rsp, 0x460
     *   41 5F 41 5E 41 5D 41 5C
     *   5F 5E 5D C3                 pop r15..rbp ; ret
     *
     * The `mov rbx, [rsp+0x4A0]` stack-disp is the uniqueness anchor
     * (wildcarding it drops the scan from one hit to 10+). Every other
     * immediate is a compiler-movable constant and is wildcarded in P2
     * and P3 to tolerate stack-frame shifts across builds.
     */
    inline constexpr AddrCandidate k_npcPfeReturnAddrCandidates[] = {
        // P1 -- tight. Both frame offsets retained as literals for the
        // strictest match on the current build. First to fail if either
        // the `lea` frame disp or the `mov rbx` stack disp shifts.
        {"NpcPfeReturnAddr_P1_PostCallLandmark",
         "48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- wildcards the `lea` frame disp so a future build that
        // adds or removes locals (shifting the `[rbp+var_180]` offset)
        // still matches, and extends forward through the stack-teardown
        // `add rsp, <imm>` whose immediate is also wildcarded. Keeps the
        // landmark `A0 04 00 00` as a literal for uniqueness.
        {"NpcPfeReturnAddr_P2_LandmarkEpilogue",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P3 -- P2 extended through the non-volatile-register pop chain
        // and terminating `ret`. The `41 5F 41 5E 41 5D 41 5C 5F 5E 5D
        // C3` suffix ties the pattern to this specific callee-saved-set
        // (r12..r15 + rdi + rsi + rbp). Most resilient to frame-layout
        // shifts but will break if the compiler changes which registers
        // the function spills.
        {"NpcPfeReturnAddr_P3_WiderEpilogueChain",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ?? 41 5F 41 5E 41 5D 41 5C 5F 5E 5D C3",
         ResolveMode::Direct, 0, 0},
    };

} // namespace EquipHide
