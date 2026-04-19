#pragma once

// Thin wrapper over CrimsonDesertCore's AOB resolver. Re-exports the
// framework types/functions into the Transmog namespace via `using`
// declarations so existing call sites stay unchanged, aliases the shared
// candidate tables from CDCore::Anchors by reference, and carries the
// LiveTransmog-only candidate tables.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/aob_resolver.hpp>
#include <cdcore/anchors.hpp>

#include <cstdint>

namespace Transmog
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
    // sub_14076D520 — EH and LT share this hook target. LT historically
    // named the table `k_vecCandidates`; an alias is kept under that name.
    inline constexpr auto &k_visualEquipChangeCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_vecCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    // sub_14075BBF0 — unified as "BatchEquip".
    inline constexpr auto &k_batchEquipCandidates =
        CDCore::Anchors::k_batchEquipCandidates;

    // --- LiveTransmog-only candidate tables ------------------------------

    /**
     * @brief SafeTearDown (sub_14075FE60) — scene-graph tear-down that
     *        retires a matched part without mutating the authoritative
     *        equip table at *(a1+0x78). Used by the two-phase transmog
     *        apply in real_part_tear_down.
     *
     * P1 extends 3 bytes past the prologue to disambiguate from
     * sub_140BD7270 (the 33-byte prologue alone has 2 matches).
     */
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        // P1 — full prologue. Stack-save disp32 (`50 FF FF FF`) and
        // stack-alloc size (`B0 01 00 00`) wildcarded per §2. The
        // `41 0F B7 F8` (movzx edi, r8w) tail is function-specific.
        {"SafeTearDown_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        // P2 — post-alloca anchor. Stack-alloc imm32 wildcarded.
        {"SafeTearDown_P2_PostAlloca",
         "48 81 EC ?? ?? ?? ?? 41 0F B7 F8 48 8B F1",
         ResolveMode::Direct, -0x1A, 0},

        // P3 — deep-body anchor at the global-pointer call site. The
        // `48 83 C1 60` is a game-ABI struct offset (stable ABI), kept
        // literal. Stack disp32 in `lea rdx,[rbp+disp32]` and the
        // rel32 `jz` target are wildcarded per §2 (both compiler-
        // owned). The trailing stack disp `E0 00 00 00` is kept
        // literal to disambiguate from 4 sibling functions that share
        // the same `add rcx,60 / lea rdx / call / test / jz / movzx /
        // mov [rbp+X]` body shape — CE aob_scan 2026-04-19: 5 hits
        // without it, 1 hit with it. The `B9 FF FF 00 00 66 3B C8`
        // tail (mov ecx,0xFFFF ; cmp cx,ax) is a semantic sentinel
        // check specific to this function.
        {"SafeTearDown_P3_GlobalPtrCall",
         "48 83 C1 60 48 8D 95 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
         "48 85 C0 0F 84 ?? ?? ?? ?? 0F B7 00 66 89 85 E0 00 00 00 "
         "B9 FF FF 00 00 66 3B C8",
         ResolveMode::Direct, -0x4A, 0},
    };

    /**
     * @brief SubTranslator (sub_14076D950) — SlotPopulator item-id
     *        translator. Not hooked; used as the first hop of the chain
     *        that walks to qword_145CEF370 (iteminfo global) so we can
     *        build the stable item-name table at init. See
     *        item_name_table.cpp for the full 4-step chain.
     */
    inline constexpr AddrCandidate k_subTranslatorCandidates[] = {
        // P1 — full prologue. Stack disp8 (`B9`), stack-alloc size
        // (`00 01 00 00`), and the two local-variable disp8 values in
        // `lea rdx,[rbp+X]` / `lea rcx,[rbp+Y]` wildcarded per §2. The
        // `41 B8 01 00 00 00` (mov r8d, 1) is a SEMANTIC argument
        // flag, kept literal.
        {"SubTranslator_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??",
         ResolveMode::Direct, 0, 0},

        // P2 — post-alloca scratch-buffer prep anchor. Same disp8
        // wildcarding as P1.
        {"SubTranslator_P2_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P3 — deeper anchor without the `mov rdi, rcx` leading move.
        // Without the `90 48 8B D0 48 8B CF E8` post-call suffix this
        // shortened pattern matches 5 sibling call sites (same
        // `mov r8d,1 / lea rdx / lea rcx / call` boilerplate); the
        // suffix pins the specific follow-up into this function's
        // second call (CE aob_scan 2026-04-19).
        {"SubTranslator_P3_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},
    };

    /**
     * @brief InitSwapEntry (sub_141D451B0) — initializes a 0x80-byte swap
     *        entry structure to default sentinel values (-1 / 0). Called
     *        by the mod immediately before each SlotPopulator invocation.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_141D451B0(__int64 dest)
     *
     * Converted from hardcoded `gameBase + 0x1D451B0` to AOB on
     * 2026-04-11 to survive code-drift in earlier .text sections.
     */
    inline constexpr AddrCandidate k_initSwapEntryCandidates[] = {
        // P1 — true prologue (mov [rsp+8],rcx / push rbx / sub rsp,20h /
        // mov rbx,rcx / mov rax,-1 / mov [rcx],rax / mov ecx,0FFFFh).
        {"InitSwapEntry_P1_FullPrologue",
         "48 89 4C 24 08 53 48 83 EC 20 48 8B D9 "
         "48 C7 C0 FF FF FF FF 48 89 01 B9 FF FF 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 — post-prologue field-init chain (mov [rbx+20h],rdx / ... /
        // lea rax,[rbx+58h]).
        {"InitSwapEntry_P2_FieldInitChain",
         "48 89 53 20 48 89 53 28 48 89 53 30 66 89 53 38 "
         "48 89 53 40 48 89 53 48 66 89 53 50 48 8D 43 58",
         ResolveMode::Direct, -0x26, 0},

        // P3 — xor edx,edx + field-init chain (slightly tighter than P2).
        {"InitSwapEntry_P3_XorInitChain",
         "33 D2 48 89 53 20 48 89 53 28 48 89 53 30 "
         "66 89 53 38 48 89 53 40 48 89 53 48 66 89 53 50",
         ResolveMode::Direct, -0x24, 0},
    };

    /**
     * @brief SlotPopulator (sub_14076C960) — populates the slot array
     *        (a1+440) with item visual data then calls VisualEquipChange.
     *        This is the function the server equip handler invokes to
     *        trigger a full visual equip with mesh loading.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14076C960(
     *       __int64 a1,
     *       unsigned __int16* a2_itemData,
     *       __int64 a3_swapEntry)
     *
     * a2 is a 16-byte structure:
     *   +0:  uint16 item ID
     *   +2:  byte   flag (2 = normal equip)
     *   +4:  int32  (-1)
     *   +12: uint16 secondary slot (0xFFFF to skip)
     *
     * History:
     *   2026-04-19 P2 rebuilt against v1.03.01 bytes (old pattern anchored
     *   on `lea rcx, [rip+disp32]` which no longer sits immediately after
     *   the `xor edi, edi` — compiler reshuffled). P3 added for the
     *   required ≥3-candidate cascade; anchors on the distinctive
     *   `mov r14d, -1` sentinel plus the following `mov ebx, 0xFFFF`.
     */
    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        // P1 — full prologue through the register shuffle (mov r12,rdx;
        // mov r13,rcx; xor edi,edi).
        {"SlotPopulator_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? 00 00 00 "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        // P2 — post-alloca anchor: register shuffle + the mov-edi-to-stack
        // + `mov r14d, -1` sentinel + `movzx eax, r14w`. Offset -0x22
        // backs up to function start.
        {"SlotPopulator_P2_PostAlloca",
         "4C 8B E2 4C 8B E9 33 FF 89 7D ?? 41 BE FF FF FF FF 41 0F B7 C6",
         ResolveMode::Direct, -0x22, 0},

        // P3 — deeper anchor on the `mov r14d,-1` sentinel + the inline
        // `mov [rbp+6Fh],ax ; mov ebx,0FFFFh` follow-up. Skips the
        // register shuffle entirely and pins the post-init block. Offset
        // -0x2D backs up to function start.
        {"SlotPopulator_P3_SentinelInit",
         "41 BE FF FF FF FF 41 0F B7 C6 66 89 45 6F BB FF FF 00 00",
         ResolveMode::Direct, -0x2D, 0},
    };

    /**
     * @brief Patch site: CondPrefab evaluator secondary hash check
     *        (sub_141D5F470 at +0xC8, 0x141D5F538 on v1.02.00).
     *
     * The `jz` that jumps on character-class hash match. NPC items lack
     * Kliff's class in their secondary hash array, so this `jz` never
     * fires → evaluator returns empty → no mesh.
     *
     * Toggling this single byte from `0x74` (jz rel8) to `0xEB` (jmp
     * rel8) forces the match, making the evaluator emit resource names
     * for NPC items.
     *
     * Patterns wildcard the fragile bytes (rel8 jump distances, struct
     * offsets) and append the trailing `EB` (jmp opcode of the OUTER
     * loop exit — NOT the patch target) to disambiguate from a
     * structurally identical loop at +0x70 in the same function.
     *
     * @note Branch-encoding constraint (aob-signatures.md §8):
     *       Unlike every other AOB in this project, the `74` rel8 opcode
     *       in these patterns is **intentionally literal and must NOT be
     *       wildcarded** — it is the exact byte the mod flips at
     *       runtime. A future compiler that emits this jz in its 6-byte
     *       `0F 84 rel32` form would require a full re-RE of the patch
     *       strategy (not a pattern tweak); every candidate below would
     *       stop matching and the feature would fail-soft via the
     *       scanner's null return. Anchors on the outer `72 ?? EB` loop
     *       structure share the same constraint for the same reason.
     */
    inline constexpr AddrCandidate k_charClassBypassCandidates[] = {
        // P1 — `mov r8,[rbx+??] + movzx r9d,[rax+??] + inner loop + jmp`.
        // Patch byte at match + 0x11.
        {"CharClassBypass_P1_MovzxCmpLoop",
         "4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x11, 0},

        // P2 — `test edx,edx + jz + mov + movzx + cmp+jz + loop + jmp`.
        // Patch byte at match + 0x15.
        {"CharClassBypass_P2_TestMovzxCmp",
         "85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x15, 0},

        // P3 — `xor ecx,ecx + test + jz + full inner loop + jmp`. Deepest
        // anchor, most context. Patch byte at match + 0x17.
        {"CharClassBypass_P3_XorTestCmpLoop",
         "33 C9 85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x17, 0},
    };

} // namespace Transmog
