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
    // sub_14076D520 — EH calls this "VisualEquipChange" (so does LT); keep
    // the unified name.
    inline constexpr auto &k_visualEquipChangeCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    // sub_14075BBF0 — EH historically called this "VisualEquipSwap", LT
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
     * @brief ChildActor vtable — static data pointer resolved via three
     *        nearby RIP-relative loads. All three candidates resolve to the
     *        same `lea rax, [rip+disp32]` that loads the vtable base; the
     *        cascade picks whichever anchor shape survives the current build.
     *
     * @note Branch-encoding caveat (aob-signatures.md §8):
     *       P2 anchors on the trailing `EB 03` (jmp-over-fallback) that
     *       follows the vtable store. Wildcarding its opcode loses
     *       uniqueness on the live build (5 matches without it). If a
     *       future compiler flips this jmp to the 6-byte `E9 rel32` form,
     *       P2 will stop matching — P1 and P3 pick up the slack. P1 no
     *       longer crosses the jmp (truncated 2026-04-19) so it is
     *       encoding-independent. P3 wildcards the preceding `74 ??` jz
     *       pair as `?? ??` so it only declares a 2-byte slot, not the
     *       rel8 opcode literal.
     */
    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        // P1 — truncated 2026-04-19: ends at the `mov [rsi], rax` that
        // stores the vtable. Does NOT cross the trailing `EB 03` jmp, so
        // it survives a Jcc-encoding flip.
        {"ChildActorVtbl_P1_AllocCtor",
         "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06",
         ResolveMode::RipRelative, 16, 20},

        // P2 — retains the trailing `EB ?? 4C`: without the `4C` byte
        // of the post-jmp `mov rsi, r13` continuation, the lead-in is
        // structurally shared with 4 other ctor sites (CE aob_scan
        // 2026-04-19: 5 hits). Tied to the 2-byte jmp encoding.
        {"ChildActorVtbl_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        // P3 — rel8 jz at offset +6 is wildcarded to `?? ??` (both bytes).
        // Does not hardcode the opcode literal, but still only matches
        // the 2-byte form (fails on `0F 84 rel32`).
        {"ChildActorVtbl_P3_WiderCtorStore",
         "45 31 ED 48 85 F6 ?? ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05",
         ResolveMode::RipRelative, 24, 28},
    };

    /**
     * @brief IndexedStringA map insert routine — companion to MapLookup.
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
     * Register layout at hook point (v1.02.00 — legacy):
     *   R10 = pointer to part hash DWORD
     *   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
     *   R8B = exclusion-list flag
     *   [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *   [RBP+0x4F] = a1 context pointer
     *
     * Cross-version cascade. v1.03.01 candidates come first (current live
     * game returns 1 hit for each); v1.02.00 candidates are retained for
     * users still on the older build and return 0 on the v1.03.01 game —
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
        // --- v1.03.01 (PartInOut struct pointer in RAX, loaded from [RBP+5F]) ---
        // P1 — ends at `cmp al, 3`, does not cross the jz.
        {"PartInOut_v103_P1_DirectSite",
         "48 8B 45 5F 0F B6 40 1C 3C 03",
         4},

        // P2 — ends at `cmp rax, rcx`, does not cross the following jz.
        {"PartInOut_v103_P2_WiderContext",
         "45 32 C0 49 8B 45 ?? 41 8B 4D ?? 48 C1 E1 04 48 03 C8 48 3B C1",
         0x30},

        // P3 — ends at `cmp al, 3`, does not cross the jz.
        {"PartInOut_v103_P3_PrecedingGate",
         "41 B0 01 48 8B 45 5F 0F B6 40 1C 3C 03",
         7},

        // --- v1.02.00 legacy (PartInOut struct pointer in R13) ---
        // CE returns 0 hits for all three on v1.03.01 by design — the
        // live game has migrated the struct-pointer register and these
        // candidates serve as fallbacks for anyone still on v1.02.00.
        // Same truncation rule applied: terminate at or before the first
        // rel8 branch to avoid §8 violations.
        //
        // P1 — movzx + cmp + {2-byte branch} + test r8b + {2-byte branch}
        //      + test al. Branch opcode bytes wildcarded as `?? ??`
        //      rather than literal `74`/`75` to satisfy §8 (no rel8
        //      literal in the body); still only matches the 2-byte
        //      encoding of both branches.
        {"PartInOut_v102_P1_DirectSite",
         "41 0F B6 45 1C 3C 03 ?? ?? 45 84 C0 ?? ?? 84 C0",
         0},

        // P2 — ends at `cmp rax, rcx`, mirrors v103_P2 truncation.
        {"PartInOut_v102_P2_WiderContext",
         "45 32 C0 48 8B 4D ?? 48 8B 41 ?? 8B 49 ?? 48 C1 E1 04 48 03 C8 48 3B C1",
         0x36},

        // P3 — `mov r8b,1` prelude + movzx + cmp + {2-byte branch} +
        //      test r8b. Same §8 treatment as P1.
        {"PartInOut_v102_P3_PrecedingGate",
         "41 B0 01 41 0F B6 45 1C 3C 03 ?? ?? 45 84 C0",
         3},
    };

    /**
     * @brief AOB candidates for sub_1423FDEB0 — Postfix rule evaluator.
     *
     * Virtual function at vtable[4] of objects with vtable 0x144CC8248.
     * Evaluates whether a postfix rule matches currently equipped items.
     * Returns 1 = rule matches (hide hair), 0 = no match (keep hair).
     */
    inline constexpr AddrCandidate k_postfixEvalCandidates[] = {
        {"PostfixEval_P1_PrologueAndBody",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        {"PostfixEval_P2_UniqueBody",
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x14, 0},

        {"PostfixEval_P3_LoopInit",
         "45 33 F6 44 89 74 24 ?? C7 44 24 ?? ?? 00 00 00 49 8B 5F ?? 41 8B 47 ??",
         ResolveMode::Direct, -0x6F, 0},
    };

} // namespace EquipHide
