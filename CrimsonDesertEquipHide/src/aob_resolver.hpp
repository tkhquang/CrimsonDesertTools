#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors
// aliases. Resolution itself is delegated to DetourModKit's cascading
// scanner; resolve_address() flattens the std::expected return into the
// uintptr_t-or-zero shape used throughout the call sites.
//
// All candidate tables (entry-point and mid-body) use the unified
// AddrCandidate type. Mid-body sites encode the hook offset in
// disp_offset with ResolveMode::Direct; the resolver returns
// match + disp_offset, landing on the exact instruction the hook
// must mid-hook.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>
#include <cdcore/prologue_check.hpp>

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace EquipHide
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    using CDCore::sanity_check_function_prologue;

    // --- Shared candidate tables (aliased by reference from CDCore) -------

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
    inline constexpr auto &k_batchEquipCandidates =
        CDCore::Anchors::k_batchEquipCandidates;
    inline constexpr auto &k_radialSwapKeyCandidates =
        CDCore::Anchors::k_radialSwapKeyCandidates;
    // Historical EH name. BatchEquip is the unified label.
    inline constexpr auto &k_visualEquipSwapCandidates =
        CDCore::Anchors::k_batchEquipCandidates;

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

    // --- EquipHide-only candidate tables ---------------------------------

    /**
     * @brief ChildActor vtable -- static data pointer resolved via three
     *        nearby RIP-relative loads. All three candidates resolve to the
     *        same lea rax, [rip+disp32] that loads the vtable base; the
     *        cascade picks whichever anchor shape survives the current build.
     *
     * Branch-encoding caveat (aob-signatures.md section 8):
     *       P2 anchors on the trailing EB 03 (jmp-over-fallback) that
     *       follows the vtable store. Wildcarding its opcode loses
     *       uniqueness on the live build (5 matches without it). If a
     *       future compiler flips this jmp to the 6-byte E9 rel32 form,
     *       P2 will stop matching -- P1 and P3 pick up the slack. P1 no
     *       longer crosses the jmp (truncated 2026-04-19) so it is
     *       encoding-independent. P3 wildcards the preceding 74 ?? jz
     *       pair as ?? ?? so it only declares a 2-byte slot, not the
     *       rel8 opcode literal.
     */
    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        // P1 -- truncated 2026-04-19: ends at the mov [rsi], rax that
        // stores the vtable. Does NOT cross the trailing EB 03 jmp, so
        // it survives a Jcc-encoding flip.
        {"ChildActorVtbl_P1_AllocCtor",
         "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06",
         ResolveMode::RipRelative, 16, 20},

        // P2 -- retains the trailing EB ?? 4C: without the 4C byte
        // of the post-jmp mov rsi, r13 continuation, the lead-in is
        // structurally shared with 4 other ctor sites (CE aob_scan
        // 2026-04-19: 5 hits). Tied to the 2-byte jmp encoding.
        {"ChildActorVtbl_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- rel8 jz at offset +6 is wildcarded to ?? ?? (both bytes).
        // Does not hardcode the opcode literal, but still only matches
        // the 2-byte form (fails on 0F 84 rel32).
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
     *
     * Branch-encoding caveat (aob-signatures.md section 8):
     *       The hook point is movzx eax, byte [<vis>+0x1C] ; cmp al, 3.
     *       All candidates below previously extended past that compare
     *       into the 74 ?? / 75 ?? conditional branches that follow.
     *       Each candidate has been truncated 2026-04-19 to terminate at
     *       or before the first rel8 branch so the signatures no longer
     *       hardcode a rel8 opcode. offsetToHook still points at the
     *       movzx instruction from the same match position (truncation
     *       only shortens the verified suffix, not the match start).
     */
    inline constexpr AddrCandidate k_hookSiteCandidates[] = {
        // P1 -- ends at cmp al, 3, does not cross the jz.
        // Hook lands on the cmp (match + 4).
        {"PartInOut_P1_DirectSite",
         "48 8B 45 5F 0F B6 40 1C 3C 03",
         ResolveMode::Direct, 4, 0},

        // P2 -- ends at cmp rax, rcx, does not cross the following jz.
        // Hook lands +0x30 inside the wider preamble.
        {"PartInOut_P2_WiderContext",
         "45 32 C0 49 8B 45 ?? 41 8B 4D ?? 48 C1 E1 04 48 03 C8 48 3B C1",
         ResolveMode::Direct, 0x30, 0},

        // P3 -- ends at cmp al, 3, does not cross the jz.
        // Hook lands on the cmp (match + 7).
        {"PartInOut_P3_PrecedingGate",
         "41 B0 01 48 8B 45 5F 0F B6 40 1C 3C 03",
         ResolveMode::Direct, 7, 0},
    };

    /**
     * @brief AOB candidates for the Postfix rule evaluator.
     *
     * Virtual function at vtable[4] of objects with vtable 0x144CC8248.
     * Evaluates whether a postfix rule matches currently equipped items.
     * Returns 1 = rule matches (hide hair), 0 = no match (keep hair).
     */
    inline constexpr AddrCandidate k_postfixEvalCandidates[] = {
        // P1 -- tight. Full prologue (4 shadow-space saves + 3 pushes +
        // sub rsp) chained into the first 4 body instructions. Most
        // selective; first to fail if the compiler reshuffles save
        // order or spills a different non-volatile register set.
        // sub rsp imm8 is wildcarded because it tracks local-variable
        // size; every other byte is structural for this prologue shape.
        {"PostfixEval_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        // P2 -- medium. Drops the four shadow-space register saves and
        // anchors on the 3-push sequence + sub rsp + body tail.
        // Survives save-order shifts but stays tied to the specific
        // r12/r14/r15 callee-spill set. Match lands on the first
        // byte of push r12 (function entry + 0x14), so the walk-back
        // is -0x14.
        {"PostfixEval_P2_PushesAndBody",
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- loose. Body-only fallback anchored on the distinctive
        // mov rbx, [rdx+disp8] ; mov eax, [rdx+disp8] ; lea rdi,
        // [rbx+rax*8] load-pair (the C3 SIB byte encodes rbx+rax*8,
        // a rare index shape) plus the start of the loop-header
        // cmp rbx, rdi. Pattern terminates at 48 3B so it does
        // not include the following Jcc rel8 (aob-signatures.md
        // section 8 rel8 rule). Walk-back of -0x1A covers the full
        // 0x1A-byte prologue; survives both prologue save-order and
        // push-set changes as long as the loop-header body stays put.
        {"PostfixEval_P3_UniqueBody",
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x1A, 0},
    };

    /**
     * @brief Return-address landmark inside createPrefabFromPartPrefab
     *        (engine-registered profiling label for sub_14261B6C0).
     *
     * That function instantiates a renderable prefab from a PartPrefab,
     * then tail-calls sub_1402E1430(v107) -- inside which the rule-eval
     * pipeline reaches PostfixEval on the freshly-built instance. The
     * return address on the byte right after that inner call is the
     * landmark (match+12, the first byte of the post-call mov rbx,
     * [rsp+0x4A0] epilogue load).
     *
     * Player PostfixEval invocations come from the equipment-visibility
     * update loop elsewhere in the binary and never include this return
     * address on their stack. bald_fix uses stack presence of this
     * landmark to reject prefab-instantiation-path calls without caching
     * ctx pointers or depending on frequency heuristics.
     */
    inline constexpr AddrCandidate k_npcPfeReturnAddrCandidates[] = {
        // P1 -- tight. Both frame offsets retained as literals for the
        // strictest match on the current build. First to fail if either
        // the lea frame disp or the mov rbx stack disp shifts.
        {"NpcPfeReturnAddr_P1_PostCallLandmark",
         "48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- wildcards the lea frame disp so a future build that
        // adds or removes locals (shifting the [rbp+var_180] offset)
        // still matches, and extends forward through the stack-teardown
        // add rsp, <imm> whose immediate is also wildcarded. Keeps the
        // landmark A0 04 00 00 as a literal for uniqueness.
        {"NpcPfeReturnAddr_P2_LandmarkEpilogue",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P3 -- P2 extended through the non-volatile-register pop chain
        // and terminating ret. The 41 5F 41 5E 41 5D 41 5C 5F 5E 5D
        // C3 suffix ties the pattern to this specific callee-saved-set
        // (r12..r15 + rdi + rsi + rbp). Most resilient to frame-layout
        // shifts but will break if the compiler changes which registers
        // the function spills.
        {"NpcPfeReturnAddr_P3_WiderEpilogueChain",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ?? 41 5F 41 5E 41 5D 41 5C 5F 5E 5D C3",
         ResolveMode::Direct, 0, 0},
    };

} // namespace EquipHide
