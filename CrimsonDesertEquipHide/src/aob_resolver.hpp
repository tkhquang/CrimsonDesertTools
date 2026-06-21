#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors aliases. Resolution itself is delegated to
// DetourModKit's cascading scanner; resolve_address() flattens the std::expected return into the uintptr_t-or-zero
// shape used throughout the call sites.
//
// All candidate tables (entry-point and mid-body) use the unified AddrCandidate type. Mid-body sites encode the hook
// offset in disp_offset with ResolveMode::Direct; the resolver returns match + disp_offset, landing on the exact
// instruction the hook must mid-hook.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>

#include <DetourModKit.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace EquipHide
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    // --- Shared candidate tables (aliased by reference from CDCore) -------

    inline constexpr auto &k_worldSystemCandidates = CDCore::Anchors::k_worldSystemCandidates;
    inline constexpr auto &k_mapLookupCandidates = CDCore::Anchors::k_mapLookupCandidates;
    inline constexpr auto &k_partAddShowCandidates = CDCore::Anchors::k_partAddShowCandidates;
    inline constexpr auto &k_visualEquipChangeCandidates = CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_batchEquipCandidates = CDCore::Anchors::k_batchEquipCandidates;
    // EH-local alias for the unified BatchEquip candidate table.
    inline constexpr auto &k_visualEquipSwapCandidates = CDCore::Anchors::k_batchEquipCandidates;

    namespace detail
    {
        /**
         * @brief Resolves the first matching candidate from a cascade and returns the absolute address, or 0 on
         *        failure.
         * @details The underlying cascade already logs the success line; on failure this helper emits a single Warning
         *          so caller code can stay focused on conditional feature wiring. For call sites that need the precise
         *          ResolveError, call DetourModKit::Scanner::resolve_cascade directly.
         */
        [[nodiscard]] inline std::uintptr_t resolve_cascade_or_zero(std::span<const AddrCandidate> candidates,
                                                                    std::string_view label)
        {
            // Host-EXE scope: every target resolves inside CrimsonDesert.exe, so bounding the scan (and the
            // require_unique count) to
            // Memory::host_module_range() stops a generic-shaped candidate from first-matching inside a sibling mod or
            // overlay elsewhere in the process image. The prologue-fallback variant is retained -- it re-matches a
            // sibling-stomped prologue, and its rebuilt jump destination stays unbounded, so a trampoline outside the
            // EXE is still recovered.
            auto hit = DetourModKit::Scanner::resolve_cascade_in_host_module_with_prologue_fallback(candidates, label);
            if (hit.has_value())
                return hit->address;

            DetourModKit::Logger::get_instance().warning("{} resolve cascade failed: {}", label,
                                                         DetourModKit::Scanner::resolve_error_to_string(hit.error()));
            return 0;
        }
    } // namespace detail

    /**
     * @brief Resolve a candidate cascade to an absolute address, or 0.
     */
    [[nodiscard]] inline std::uintptr_t resolve_address(const AddrCandidate *candidates, std::size_t count,
                                                        const char *label)
    {
        return detail::resolve_cascade_or_zero(std::span<const AddrCandidate>{candidates, count},
                                               label ? std::string_view{label} : std::string_view{});
    }

    template <std::size_t N>
    [[nodiscard]] inline std::uintptr_t resolve_address(const AddrCandidate (&arr)[N], const char *label)
    {
        return detail::resolve_cascade_or_zero(std::span<const AddrCandidate>{arr, N},
                                               label ? std::string_view{label} : std::string_view{});
    }

    // --- EquipHide-only candidate tables ---------------------------------

    /**
     * @brief ChildActor (pa::ClientChildOnlyInGameActor) vtable. Resolved by RTTI mangled name first (patch-stable,
     *        self-healing), with three nearby RIP-relative byte loads as fallback. Each byte candidate resolves to the
     *        lea rax, [rip+disp32] that loads the vtable base, which is also the primary (COL.offset == 0) vtable that
     *        vtable_for_type returns for that class; the cascade picks whichever tier survives the current build.
     *
     * Branch-encoding caveat (aob-signatures.md section 8):
     *       P2 anchors on the trailing EB 03 (jmp-over-fallback) that follows the vtable store. Wildcarding its opcode
     *       loses uniqueness on the live build (5 matches without it). If a future compiler flips this jmp to the
     *       6-byte E9 rel32 form, P2 will stop matching -- P1 and P3 pick up the slack. P1 is truncated so it does not
     *       cross the jmp, making it encoding-independent. P3 wildcards the preceding 74 ?? jz pair as ?? ?? so it only
     *       declares a 2-byte slot, not the rel8 opcode literal.
     */
    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        // Primary -- resolve the vtable by its RTTI mangled name. The AllocCtor stores ClientChildOnlyInGameActor's
        // primary vtable into the new object, so vtable_for_type on that class name yields the exact pointer the byte
        // tiers below recover. The mangled name is patch-stable, so this tier self-heals across the vtable relocations
        // and jmp-encoding flips the byte anchors are sensitive to. require_unique does not apply: the RTTI backend is
        // unique-only and fails closed on an ambiguous name, falling through to the byte tiers.
        {"ChildActorVtbl_RTTI", ".?AVClientChildOnlyInGameActor@pa@@", ResolveMode::RttiVtable},

        // P1 -- truncated: ends at the mov [rsi], rax that stores the vtable. Does NOT cross the trailing EB 03 jmp, so
        // it survives a Jcc-encoding flip.
        {"ChildActorVtbl_P1_AllocCtor", "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06",
         ResolveMode::RipRelative, 16, 20},

        // P2 -- retains the trailing EB ?? 4C: without the 4C byte of the post-jmp mov rsi, r13 continuation, the
        // lead-in is structurally shared with 4 other ctor sites (5 matches total). Tied to the 2-byte jmp encoding.
        {"ChildActorVtbl_P2_CtorStore", "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- rel8 jz at offset +6 is wildcarded to ?? ?? (both bytes). Does not hardcode the opcode literal, but
        // still only matches the 2-byte form (fails on 0F 84 rel32).
        {"ChildActorVtbl_P3_WiderCtorStore", "45 31 ED 48 85 F6 ?? ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05",
         ResolveMode::RipRelative, 24, 28},
    };

    /**
     * @brief IndexedStringA map insert routine -- companion to MapLookup.
     */
    inline constexpr AddrCandidate k_mapInsertCandidates[] = {
        {"MapInsert_P1_FullPrologue",
         "4C 89 4C 24 20 53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, 0, 0},

        // P2 -- inner body anchor (post-prologue arg shuffle through the
        // following `test r10d, r10d ; jne ; mov edx,r8d ; mov rcx,rbx ;
        // call helper ; mov r10d,[rbx] ; mov ecx,r10d ; xor ebp,ebp ;
        // test ecx,ecx`). Original 15-byte body was unique on earlier builds but matched twice on v1.08 -- a sibling
        // actor-component init at 0x142469B1C ran the exact same arg-shuffle plus the exact same `test/jne/call`
        // follow-on. The differentiator is the `33 ED` (xor ebp, ebp) just before the `test ecx, ecx` -- the
        // false-positive site jumps straight to `test ecx, ecx` without zeroing ebp first. Extending the suffix through
        // that discriminator restores uniqueness. The intervening rel8 short branch and rel32 call target are
        // wildcarded (compiler-owned values, see aob-signatures.md section 2). disp_offset stays -0x11 (walks back to
        // function start from the first byte of P2).
        {"MapInsert_P2_InnerBody",
         "44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA "
         "41 8B CA 45 85 D2 75 ?? 41 8B D0 48 8B CB "
         "E8 ?? ?? ?? ?? 44 8B 13 41 8B CA 33 ED 85 C9",
         ResolveMode::Direct, -0x11, 0},

        {"MapInsert_P3_PrologueBody",
         "53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA 41 8B CA 45 85 D2",
         ResolveMode::Direct, -5, 0},
    };

    /**
     * @brief Hook target: PartInOut transition function (sub_14081DB90 / sub_140826900 on v1.03.01).
     *
     * Hook point: movzx eax, byte ptr [<vis_reg>+1Ch]; cmp al, 3
     *
     * Register layout at hook point (v1.03.01):
     *   R10 = pointer to part hash DWORD (IndexedStringA ID) RAX = pointer to PartInOutSocket struct (loaded from
     *   [RBP+5Fh]) R8B = exclusion-list flag [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *   [RBP+0x4F] = a1 context pointer
     *
     * Register layout at hook point (v1.08.00 and later):
     *   The visibility byte is read directly as `movzx eax, byte [r12+disp8]; cmp al, 3`. The `44 24` SIB reads as
     *   `[rsp]`, but the leading `41` REX.B prefix extends the base to R12, so this is a struct field load (R12 holds
     *   the PartInOut decision struct), not a stack-slot spill. The `mov r8b, 1` that sets the exclusion-list flag
     *   still precedes the read.
     *
     * Cascade contract: 1-hit-only on the current target build. Re-adding a legacy tier requires per-version CE
     * verification of both match count AND the match-to-hook displacement against the target build, because a
     * wider-context shape can still resolve once with its disp_offset landing mid-instruction when the function body
     * shifts.
     */
    inline constexpr AddrCandidate k_hookSiteCandidates[] = {
        // PN1 -- v1.08.00. The PartInOutSocket arg-passing convention changed: the visibility byte is no longer fetched
        // through `mov rax, [rbp+0x5F] ; movzx eax, byte [rax+0x1C]` (which anchored P0..P3). The byte is now read
        // directly from the decision struct: `movzx eax, byte [r12+0x20] ; cmp al, 3`. The `44 24` SIB reads as
        // `[rsp]`, but the leading `41` REX.B extends the base to R12, so the disp8 is a struct field offset, not a
        // stack slot. The `mov r8b, 1` (set-exclusion-list flag) idiom that precedes the load is retained, so we anchor
        // on the pair: `41 B0 01 41 0F B6 44 24 ?? 3C 03`. The struct disp8 is wildcarded so a future re-layout still
        // matches. Hook lands on the movzx at match + 3 (skip the `41 B0 01`). 1 hit on v1.08.00, 0 on v1.04.00 /
        // v1.05.00.
        {"PartInOut_PN1_v108_StackArgVisRead", "41 B0 01 41 0F B6 44 24 ?? 3C 03", ResolveMode::Direct, 3, 0},

        // PN2 -- v1.08.00 wider: prefixed with the `EB 03` short jmp that lands on the `mov r8b, 1` from the
        // loop-not-found branch. Provides extra structural pinning if a future recompile shuffles the `mov r8b, 1` away
        // from PN1. Hook still lands on the movzx, at match + 5. 1 hit on v1.08.00.
        {"PartInOut_PN2_v108_LoopExitJoin", "EB 03 41 B0 01 41 0F B6 44 24 ?? 3C 03", ResolveMode::Direct, 5, 0},
    };

    /**
     * @brief AOB candidates for the Postfix rule evaluator.
     *
     * Evaluates whether a postfix rule matches the currently equipped items. Returns 1 = rule matches (hide hair), 0 =
     * no match (keep hair). The engine reaches it through a named method-binding table rather than a COL-tagged C++
     * vtable, so there is no RTTI identity to name-resolve -- the prologue cascade below is the strongest available
     * anchor.
     */
    inline constexpr AddrCandidate k_postfixEvalCandidates[] = {
        // P1 -- tight. Full prologue (4 shadow-space saves + 3 pushes + sub rsp) chained into the first 4 body
        // instructions. Most selective; first to fail if the compiler reshuffles save order or spills a different
        // non-volatile register set. sub rsp imm8 is wildcarded because it tracks local-variable size; every other byte
        // is structural for this prologue shape.
        {"PostfixEval_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        // P2 -- medium. Drops the four shadow-space register saves and anchors on the 3-push sequence + sub rsp + body
        // tail. Survives save-order shifts but stays tied to the specific r12/r14/r15 callee-spill set. Match lands on
        // the first byte of push r12 (function entry + 0x14), so the walk-back is -0x14.
        {"PostfixEval_P2_PushesAndBody",
         "41 54 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- loose. Body-only fallback anchored on the distinctive mov rbx, [rdx+disp8] ; mov eax, [rdx+disp8] ; lea
        // rdi, [rbx+rax*8] load-pair (the C3 SIB byte encodes rbx+rax*8, a rare index shape) plus the start of the
        // loop-header cmp rbx, rdi. Pattern terminates at 48 3B so it does not include the following Jcc rel8
        // (aob-signatures.md section 8 rel8 rule). Walk-back of -0x1A covers the full 0x1A-byte prologue; survives both
        // prologue save-order and push-set changes as long as the loop-header body stays put.
        {"PostfixEval_P3_UniqueBody", "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x1A, 0},
    };

    /**
     * @brief Return-address landmark inside createPrefabFromPartPrefab (engine-registered profiling label for
     *        sub_14261B6C0).
     *
     * That function instantiates a renderable prefab from a PartPrefab, then tail-calls sub_1402E1430(v107) -- inside
     * which the rule-eval pipeline reaches PostfixEval on the freshly-built instance. The return address on the byte
     * right after that inner call is the landmark (match+12, the first byte of the post-call mov rbx, [rsp+0x4A0]
     * epilogue load).
     *
     * Player PostfixEval invocations come from the equipment-visibility update loop elsewhere in the binary and never
     * include this return address on their stack. bald_fix uses stack presence of this landmark to reject
     * prefab-instantiation-path calls without caching ctx pointers or depending on frequency heuristics.
     */
    inline constexpr AddrCandidate k_npcPfeReturnAddrCandidates[] = {
        // P1 -- tight. Both frame offsets retained as literals for the strictest match on the current build. First to
        // fail if either the lea frame disp or the mov rbx stack disp shifts.
        {"NpcPfeReturnAddr_P1_PostCallLandmark", "48 8D 8D 10 02 00 00 E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- wildcards the lea frame disp so a future build that adds or removes locals (shifting the [rbp+var_180]
        // offset) still matches, and extends forward through the stack-teardown add rsp, <imm> whose immediate is also
        // wildcarded. Keeps the landmark A0 04 00 00 as a literal for uniqueness.
        {"NpcPfeReturnAddr_P2_LandmarkEpilogue",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P3 -- P2 extended through the non-volatile-register pop chain and terminating ret. The 41 5F 41 5E 41 5D 41
        // 5C 5F 5E 5D
        // C3 suffix ties the pattern to this specific callee-saved-set (r12..r15 + rdi + rsi + rbp). Most resilient to
        // frame-layout shifts but will break if the compiler changes which registers the function spills.
        {"NpcPfeReturnAddr_P3_WiderEpilogueChain",
         "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 9C 24 A0 04 00 00 "
         "48 81 C4 ?? ?? ?? ?? 41 5F 41 5E 41 5D 41 5C 5F 5E 5D C3",
         ResolveMode::Direct, 0, 0},
    };

} // namespace EquipHide
