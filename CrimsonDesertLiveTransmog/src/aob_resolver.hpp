#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors
// aliases. Resolution itself is delegated to DetourModKit's cascading
// scanner; resolve_address() flattens the std::expected return into the
// uintptr_t-or-zero shape used throughout the call sites.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>
#include <cdcore/prologue_check.hpp>

#include <DetourModKit.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace Transmog
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    using CDCore::sanity_check_function_prologue;

    // Shared candidate tables (aliased by reference from CDCore so the
    // single source of truth lives in cdcore/anchors.hpp).
    inline constexpr auto &k_worldSystemCandidates =
        CDCore::Anchors::k_worldSystemCandidates;
    inline constexpr auto &k_playerStaticCandidates =
        CDCore::Anchors::k_playerStaticCandidates;
    inline constexpr auto &k_mapLookupCandidates =
        CDCore::Anchors::k_mapLookupCandidates;
    inline constexpr auto &k_partAddShowCandidates =
        CDCore::Anchors::k_partAddShowCandidates;
    inline constexpr auto &k_visualEquipChangeCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_vecCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_batchEquipCandidates =
        CDCore::Anchors::k_batchEquipCandidates;
    inline constexpr auto &k_radialSwapKeyCandidates =
        CDCore::Anchors::k_radialSwapKeyCandidates;

    namespace detail
    {
        /**
         * @brief Resolves the first matching candidate from a cascade and
         *        returns the absolute address, or 0 on failure.
         * @details The underlying cascade already logs the success line; on
         *          failure this helper emits a single Warning so caller
         *          code can stay focused on conditional feature wiring.
         *          For call sites that need the precise ResolveError,
         *          call DetourModKit::Scanner::resolve_cascade directly.
         */
        [[nodiscard]] inline std::uintptr_t resolve_cascade_or_zero(
            std::span<const AddrCandidate> candidates,
            std::string_view label)
        {
            auto hit =
                DetourModKit::Scanner::resolve_cascade_with_prologue_fallback(
                    candidates, label);
            if (hit.has_value())
                return hit->address;

            DetourModKit::Logger::get_instance().warning(
                "{} resolve cascade failed: {}",
                label,
                DetourModKit::Scanner::resolve_error_to_string(hit.error()));
            return 0;
        }
    } // namespace detail

    /**
     * @brief Resolve a candidate cascade to an absolute address, or 0.
     */
    [[nodiscard]] inline std::uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label)
    {
        return detail::resolve_cascade_or_zero(
            std::span<const AddrCandidate>{candidates, count},
            label ? std::string_view{label} : std::string_view{});
    }

    template <std::size_t N>
    [[nodiscard]] inline std::uintptr_t resolve_address(
        const AddrCandidate (&arr)[N], const char *label)
    {
        return detail::resolve_cascade_or_zero(
            std::span<const AddrCandidate>{arr, N},
            label ? std::string_view{label} : std::string_view{});
    }

    // --- LiveTransmog-only candidate tables ------------------------------

    /**
     * @brief SafeTearDown (sub_14075FE60) -- scene-graph tear-down that
     *        retires a matched part without mutating the authoritative
     *        equip table at *(a1+0x78). Used by the two-phase transmog
     *        apply in real_part_tear_down.
     *
     * P1 extends 3 bytes past the prologue to disambiguate from
     * sub_140BD7270 (the 33-byte prologue alone has 2 matches).
     */
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        // P1 -- full prologue. Stack-save disp32 (50 FF FF FF) and
        // stack-alloc size (B0 01 00 00) wildcarded per signing rules.
        // The 41 0F B7 F8 (movzx edi, r8w) tail is function-specific.
        {"SafeTearDown_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor. Stack-alloc imm32 wildcarded.
        {"SafeTearDown_P2_PostAlloca",
         "48 81 EC ?? ?? ?? ?? 41 0F B7 F8 48 8B F1",
         ResolveMode::Direct, -0x1A, 0},

        // P3 -- deep-body anchor at the global-pointer call site. The
        // 48 83 C1 60 is a game-ABI struct offset (stable ABI), kept
        // literal. Stack disp32 in lea rdx,[rbp+disp32] and the
        // rel32 jz target are wildcarded (both compiler-owned). The
        // trailing stack disp E0 00 00 00 is kept literal to
        // disambiguate from 4 sibling functions that share the same
        // add rcx,60 / lea rdx / call / test / jz / movzx / mov [rbp+X]
        // body shape (CE aob_scan 2026-04-19: 5 hits without it, 1 hit
        // with). The B9 FF FF 00 00 66 3B C8 tail is a semantic
        // sentinel check specific to this function.
        {"SafeTearDown_P3_GlobalPtrCall",
         "48 83 C1 60 48 8D 95 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
         "48 85 C0 0F 84 ?? ?? ?? ?? 0F B7 00 66 89 85 E0 00 00 00 "
         "B9 FF FF 00 00 66 3B C8",
         ResolveMode::Direct, -0x4A, 0},
    };

    /**
     * @brief SubTranslator (sub_14076D950) -- SlotPopulator item-id
     *        translator. Not hooked; used as the first hop of the chain
     *        that walks to qword_145CEF370 (iteminfo global) so we can
     *        build the stable item-name table at init. See
     *        item_name_table.cpp for the full 4-step chain.
     */
    inline constexpr AddrCandidate k_subTranslatorCandidates[] = {
        // P0 -- v1.05.00 full prologue. The second lea encodes
        // rsp-relative (`48 8D 4C 24 ??`, 4 bytes) instead of the
        // v1.04 rbp-relative form (`48 8D 4D ??`, 3 bytes), shifting
        // the body by 1 byte. Body shape (mov r8d=1, lea rdx=[rbp+X],
        // lea rcx=[rsp+Y]) is otherwise unchanged. 1 unique hit on
        // v1.05.00 at 0x140799CA9. dispOffset 0 returns the prologue
        // start; this candidate is consumed by ItemNameTable::build()
        // as a chain anchor (no hook installed here).
        {"SubTranslator_P0_v105_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??",
         ResolveMode::Direct, 0, 0},

        // P0b -- v1.05.00 post-alloca anchor. Same disp8 wildcarding
        // as P0 but no head sentinels. Walk-back matches v1.04 P2
        // (-0x19): the encoding shift sits inside the pattern, not
        // before the anchor.
        {"SubTranslator_P0b_v105_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P0c -- v1.05.00 deeper anchor: mov-r8d-1 + lea pair followed
        // by the unique 90 48 8B D0 48 8B CF E8 post-call tail. 1
        // unique hit on v1.05.00 at 0x140799CAC; function starts at
        // 0x140799C90, so walk-back -0x1C (matches v1.04 P3 semantics).
        {"SubTranslator_P0c_v105_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},

        // P1 -- v1.04.00 full prologue. Stack disp8 (B9), stack-alloc
        // size (00 01 00 00), and the two local-variable disp8 values
        // in lea rdx,[rbp+X] / lea rcx,[rbp+Y] wildcarded. The
        // 41 B8 01 00 00 00 (mov r8d, 1) is a SEMANTIC argument flag
        // and is kept literal. Inert on v1.05.00 (lea encoding shift).
        {"SubTranslator_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- v1.04.00 post-alloca scratch-buffer prep anchor. Same
        // disp8 wildcarding as P1. Inert on v1.05.00.
        {"SubTranslator_P2_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P3 -- v1.04.00 deeper anchor without the leading `mov rdi,
        // rcx`. Without the 90 48 8B D0 48 8B CF E8 post-call suffix
        // this shortened pattern matches 5 sibling call sites (same
        // mov r8d,1 / lea rdx / lea rcx / call boilerplate); the
        // suffix pins this function's second call (CE aob_scan
        // 2026-04-19). Inert on v1.05.00.
        {"SubTranslator_P3_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},
    };

    /**
     * @brief InitSwapEntry (sub_141D451B0) -- initializes a 0x80-byte swap
     *        entry structure to default sentinel values (-1 / 0). Called
     *        by the mod immediately before each SlotPopulator invocation.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_141D451B0(__int64 dest)
     *
     * Converted from hardcoded gameBase + 0x1D451B0 to AOB on
     * 2026-04-11 to survive code-drift in earlier .text sections.
     */
    inline constexpr AddrCandidate k_initSwapEntryCandidates[] = {
        // P1 -- true prologue (mov [rsp+8],rcx / push rbx / sub rsp,20h /
        // mov rbx,rcx / mov rax,-1 / mov [rcx],rax / mov ecx,0FFFFh).
        {"InitSwapEntry_P1_FullPrologue",
         "48 89 4C 24 08 53 48 83 EC 20 48 8B D9 "
         "48 C7 C0 FF FF FF FF 48 89 01 B9 FF FF 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue field-init chain (mov [rbx+20h],rdx / ... /
        // lea rax,[rbx+58h]).
        {"InitSwapEntry_P2_FieldInitChain",
         "48 89 53 20 48 89 53 28 48 89 53 30 66 89 53 38 "
         "48 89 53 40 48 89 53 48 66 89 53 50 48 8D 43 58",
         ResolveMode::Direct, -0x26, 0},

        // P3 -- xor edx,edx + field-init chain (slightly tighter than P2).
        {"InitSwapEntry_P3_XorInitChain",
         "33 D2 48 89 53 20 48 89 53 28 48 89 53 30 "
         "66 89 53 38 48 89 53 40 48 89 53 48 66 89 53 50",
         ResolveMode::Direct, -0x24, 0},
    };

    /**
     * @brief SlotPopulator (sub_14076C960) -- populates the slot array
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
     */
    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        // P1 -- full prologue through the register shuffle (mov r12,rdx;
        // mov r13,rcx; xor edi,edi).
        {"SlotPopulator_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? 00 00 00 "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor: register shuffle + the mov-edi-to-stack
        // + mov r14d, -1 sentinel + movzx eax, r14w. Offset -0x22
        // backs up to function start.
        {"SlotPopulator_P2_PostAlloca",
         "4C 8B E2 4C 8B E9 33 FF 89 7D ?? 41 BE FF FF FF FF 41 0F B7 C6",
         ResolveMode::Direct, -0x22, 0},

        // P3 -- deeper anchor on the mov r14d,-1 sentinel + the inline
        // mov [rbp+6Fh],ax ; mov ebx,0FFFFh follow-up. Skips the
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
     * The jz that jumps on character-class hash match. NPC items lack
     * Kliff's class in their secondary hash array, so this jz never
     * fires, evaluator returns empty, no mesh.
     *
     * Toggling this single byte from 0x74 (jz rel8) to 0xEB (jmp rel8)
     * forces the match, making the evaluator emit resource names for
     * NPC items.
     *
     * Patterns wildcard the fragile bytes (rel8 jump distances, struct
     * offsets) and append the trailing EB (jmp opcode of the OUTER
     * loop exit, NOT the patch target) to disambiguate from a
     * structurally identical loop at +0x70 in the same function.
     *
     * Branch-encoding constraint (aob-signatures.md section 8):
     *       Unlike every other AOB in this project, the 74 rel8 opcode
     *       in these patterns is intentionally literal and must NOT be
     *       wildcarded -- it is the exact byte the mod flips at
     *       runtime. A future compiler that emits this jz in its 6-byte
     *       0F 84 rel32 form would require a full re-RE of the patch
     *       strategy (not a pattern tweak); every candidate below would
     *       stop matching and the feature would fail-soft via the
     *       scanner's null return. Anchors on the outer 72 ?? EB loop
     *       structure share the same constraint for the same reason.
     */
    inline constexpr AddrCandidate k_charClassBypassCandidates[] = {
        // P1 -- mov r8,[rbx+??] + movzx r9d,[rax+??] + inner loop + jmp.
        // Patch byte at match + 0x11.
        {"CharClassBypass_P1_MovzxCmpLoop",
         "4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x11, 0},

        // P2 -- test edx,edx + jz + mov + movzx + cmp+jz + loop + jmp.
        // Patch byte at match + 0x15.
        {"CharClassBypass_P2_TestMovzxCmp",
         "85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x15, 0},

        // P3 -- xor ecx,ecx + test + jz + full inner loop + jmp. Deepest
        // anchor, most context. Patch byte at match + 0x17.
        {"CharClassBypass_P3_XorTestCmpLoop",
         "33 C9 85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x17, 0},
    };

} // namespace Transmog
