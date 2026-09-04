#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors aliases. DetourModKit's cascading scanner does
// the resolution. resolve_address() flattens the std::expected return into the uintptr_t-or-zero shape that the call
// sites use.
//
// All candidate tables (entry-point and mid-body) use the unified AddrCandidate type. Mid-body sites encode the hook
// offset in disp_offset with ResolveMode::Direct. The resolver returns match + disp_offset, which lands on the exact
// instruction that the hook must mid-hook.
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
         * @details The cascade already logs the success line. On failure, this helper emits one Warning line, so
         *          caller code can stay focused on conditional feature wiring. For call sites that need the precise
         *          ResolveError, call DetourModKit::Scanner::resolve_cascade directly.
         */
        [[nodiscard]] inline std::uintptr_t resolve_cascade_or_zero(std::span<const AddrCandidate> candidates,
                                                                    std::string_view label)
        {
            // Host-EXE scope: every target resolves inside CrimsonDesert.exe. The scan and the require_unique count
            // use Memory::host_module_range() as their bound. That bound stops a generic-shaped candidate from
            // first-matching inside a sibling mod or overlay elsewhere in the process image. Keep the
            // prologue-fallback variant -- it re-matches a sibling-stomped prologue, and its rebuilt jump destination
            // stays unbounded, so the cascade still recovers a trampoline outside the EXE.
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
     * @brief ChildActor (pa::ClientChildOnlyInGameActor) vtable. The cascade resolves it by RTTI mangled name first
     *        (patch-stable, self-healing), with three nearby RIP-relative byte loads as fallback. Each byte candidate
     *        resolves to the lea rax, [rip+disp32] that loads the vtable base. That base is also the primary
     *        (COL.offset == 0) vtable that vtable_for_type returns for that class. The cascade picks whichever tier
     *        survives the current build.
     *
     * Branch-encoding caveat (aob-signatures.md section 9, the short Jcc rel8 rule):
     *       P2 keeps the EB opcode of the trailing 2-byte jmp-over-fallback that follows the vtable store, and
     *       wildcards only its rel8 operand. If you wildcard the EB opcode too, the window loses uniqueness and
     *       the scan reports several matches. If a future compiler flips this jmp to the 6-byte E9 rel32 form, P2
     *       stops matching -- P1 and P3 pick up the slack. P1 is truncated, so it does not cross the jmp and stays
     *       encoding-independent. P3 wildcards the preceding 74 ?? jz pair as ?? ??, so it declares only a 2-byte
     *       slot, not the rel8 opcode literal.
     */
    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        // Primary -- resolve the vtable by its RTTI mangled name. The AllocCtor stores ClientChildOnlyInGameActor's
        // primary vtable into the new object, so vtable_for_type on that class name yields the exact pointer the byte
        // tiers below recover. The mangled name is patch-stable, so this tier self-heals across the vtable relocations
        // and jmp-encoding flips the byte anchors are sensitive to. require_unique does not apply. The RTTI backend
        // is unique-only and fails closed on an ambiguous name, so the cascade falls through to the byte tiers.
        {"ChildActorVtbl_RTTI", ".?AVClientChildOnlyInGameActor@pa@@", ResolveMode::RttiVtable},

        // P1 -- truncated: ends at the mov [rsi], rax that stores the vtable. Does NOT cross the trailing EB 03 jmp, so
        // it survives a Jcc-encoding flip.
        {"ChildActorVtbl_P1_AllocCtor", "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06",
         ResolveMode::RipRelative, 16, 20},

        // P2 -- retains the trailing EB ?? 4C. Without the 4C byte of the post-jmp mov rsi, r13 continuation, the
        // lead-in is structurally shared with other ctor sites and the window is no longer unique. Tied to the 2-byte
        // jmp encoding.
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
    // Do NOT try to anchor a row on the bucket arithmetic alone. This hash map is heavily templated: the
    // `div modulus / shl 8 / add [map+0x10]` probe matches hundreds of sites in the image, and so does the
    // `inc [map+4]` plus `[map+0x18]` write-back pair. What singles out THIS instantiation is its argument shuffle,
    // four arguments parked in a specific order, which is why P1 and P2 are built around it. P3 is the exception and
    // pays for it: it reaches the bucket-array lea instead, and needs the mid-function spill and the call ahead of it
    // to stay unique.
    //
    // Map layout every row depends on: bucket modulus at +0, live count at +4, capacity at +8, bucket array at +0x10
    // (256-byte buckets, index `(key % modulus) << 8`), entry-pointer array at +0x18.
    inline constexpr AddrCandidate k_mapInsertCandidates[] = {
        // P1 -- full prologue through the argument shuffle.
        // The five callee-saved pushes and the shuffle that follows are the most specific window available. The
        // stack-allocation imm8 is wildcarded because the compiler sizes the frame. No branch sits inside the window,
        // so the displacement cannot move when a branch encoding changes. Match lands on the function start.
        //
        // The window opens with the empty-map test folded into the modulus read (`cmp dword [rcx],0`) and closes on
        // the r9/r8d/rdx/rcx parking. The parking order is the identifier; the map pointer's own register is not.
        {"MapInsert_P1_FullPrologue",
         "40 53 56 57 41 54 41 55 48 83 EC ?? 83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9",
         ResolveMode::Direct, 0, 0},

        // P2 -- argument shuffle only, with no prologue head at all.
        // Independent of the register-save set, which is the part of the prologue that moves most. Walk back 0x0C
        // bytes to the function start.
        {"MapInsert_P2_ArgShuffleBody", "83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9", ResolveMode::Direct, -0x0C, 0},

        // P3 -- past the shuffle and past the empty-map early-out, opening on the re-read of the modulus and the
        // bucket-array lea that the probe loop runs on. The mid-function callee-saved spill slot is wildcarded.
        //
        // This row shares no PATTERN bytes with P1 or P2, so a rewrite of those bytes cannot take all three down.
        // It is not independent of the entry block, though: its -0x1D walk-back is measured across that block and
        // across the empty-map jcc, so an added instruction there, or a widening of that jcc from rel8 to rel32,
        // leaves the row matching and resolving SHORT. Re-measure it whenever the entry block changes.
        {"MapInsert_P3_BucketArrayLea",
         "41 8B D0 E8 ?? ?? ?? ?? 44 8B 17 4C 89 74 24 ?? 4C 8D 77 10 45 85 D2", ResolveMode::Direct, -0x1D, 0},
    };

    /**
     * @brief Hook target: the visibility decision inside the PartInOut transition function.
     *
     * Hook point: `movzx eax, byte ptr [rcx+0x20]` followed by `cmp al, 3`.
     *
     * Register layout at the hook point:
     *   RCX = PartInOutSocket struct. The instruction before the hook loads it from [RBP+0x5F].
     *   R15 = pointer to the part-hash DWORD, read at both comparison sites: the exclusion-list walk loads the key
     *         as `mov edx,[r15]` and the transition dispatch loads it as `mov r8d,[r15]`. Both are loads THROUGH
     *         the pointer, which is the test for identifying it -- a register only one of the two sites
     *         dereferences is the wrong one.
     *   R13 = the exclusion walk's loop counter (`mov ecx,r13d` ... `inc ecx`), not a pointer. Reading it as the
     *         hash pointer yields a small integer that `plausible_userspace_ptr` rejects, so the handler returns
     *         early and the cascade fix goes dead with the hook still reporting installed.
     *   R8B = exclusion-list flag. The engine consumes it at `test r8b,r8b` and `cmp [rcx+3],r8b`.
     *   [RBP+0x4F] = the a1 context pointer. It feeds the exclusion array at a1+0x78 and the count at a1+0x80,
     *         with a 0x10-byte entry stride.
     *   The socket visibility byte is at +0x20.
     *
     * Read the socket pointer from RCX, not from RAX. At the hook instant RAX holds the exclusion-list walk cursor,
     * which is a live heap pointer. It passes the plausible-pointer guard, so a wrong read does not fail loudly. It
     * writes the visibility byte into an unrelated struct instead. See on_vis_check_impl in equip_hide.cpp.
     *
     * Cascade contract: each candidate must match exactly once in the scanned scope. A wide shape can still match
     * once while its displacement lands mid-instruction after a body shift. Verify the match count and the
     * match-to-hook displacement together when you add a candidate.
     */
    inline constexpr AddrCandidate k_hookSiteCandidates[] = {
        // P1 -- exclusion-flag store, socket load, visibility read, decision compare.
        // No branch sits between the first byte and the hook point, so a change of branch encoding cannot move the
        // displacement. The two disp8 operands are wildcarded because the compiler assigns the frame slot and the
        // visibility field offset. Hook lands on the movzx at match + 7.
        {"PartInOut_P1_FlagStoreToVisRead", "41 B0 01 48 8B 4D ?? 0F B6 41 ?? 3C 03", ResolveMode::Direct, 7, 0},

        // P2 -- socket load, visibility read, decision compare.
        // Same branch-free property as P1 with less context, so it survives a reshuffle of the flag store.
        // Hook lands on the movzx at match + 4.
        {"PartInOut_P2_SocketLoadVisRead", "48 8B 4D ?? 0F B6 41 ?? 3C 03", ResolveMode::Direct, 4, 0},

        // P3 -- exclusion-list setup: array base at a1+0x78, count at a1+0x80, the 0x10-byte stride shift.
        // It anchors upstream of the decision block that P1 and P2 both depend on, so it survives a rewrite of that
        // block. The displacement spans the exclusion loop, which contains short branches, so a change of branch
        // encoding moves the hook point. Order it last for that reason. Hook lands on the movzx at match + 0x31.
        {"PartInOut_P3_ExclusionListSetup", "48 8B 41 78 8B 89 80 00 00 00 48 C1 E1 04", ResolveMode::Direct, 0x31, 0},
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
        // P1 -- full prologue through the first body instruction.
        // The prologue alone is not unique: the same three shadow stores, push set, frame allocation and xmm spill
        // pair appear in other functions in the module. The `mov r15, rdx` that follows is what separates this
        // function from them, so the window has to reach it. The frame size and both spill slots are wildcarded
        // because the compiler assigns them. Match lands on the function start.
        {"PostfixEval_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 "
         "48 83 EC ?? C5 F8 29 74 24 ?? C5 F8 29 7C 24 ?? 4C 8B FA",
         ResolveMode::Direct, 0, 0},

        // P2 -- push set, frame allocation, xmm spills, first body instruction.
        // Drops the three shadow stores, which are the part of the prologue a recompile is most likely to reshape:
        // an earlier build spilled a fourth register here instead of pushing it. Walk back 0x0F to the function start.
        {"PostfixEval_P2_PushSetToBody",
         "57 41 56 41 57 48 83 EC ?? C5 F8 29 74 24 ?? C5 F8 29 7C 24 ?? 4C 8B FA", ResolveMode::Direct, -0x0F, 0},

        // P3 -- equipped-item vector header only.
        // Loads the entry array from the rule context, loads the count, scales it by the 0x10 entry stride and forms
        // the end pointer. Independent of the whole prologue. The 0x10 stride encoded by `shl rdi, 4` is the same
        // stride bald_fix.cpp walks the container with, so this window also documents that contract.
        // Walk back 0x24 to the function start.
        {"PostfixEval_P3_ItemVectorHeader", "4C 8B FA 48 8B 5A ?? 8B 7A ?? 48 C1 E7 04 48 03 FB",
         ResolveMode::Direct, -0x24, 0},
    };

    /**
     * @brief Return-address landmark inside createPrefabFromPartPrefab, the engine-registered profiling label for the
     *        prefab instantiation routine.
     *
     * That function instantiates a renderable prefab from a PartPrefab, then calls the rule-eval entry point. The
     * rule-eval pipeline inside that call reaches PostfixEval on the freshly-built instance. The landmark is the
     * return address of that call, i.e. the byte immediately after it (see the row's own disp_offset).
     *
     * Player PostfixEval invocations come from the equipment-visibility update loop elsewhere in the binary and never
     * include this return address on their stack. bald_fix uses stack presence of this landmark to reject
     * prefab-instantiation-path calls. It does not cache ctx pointers and it does not depend on frequency heuristics.
     */
    // This landmark is a MEANING, not just an address: the row has to name the rule-eval call specifically, so
    // re-verify the bald fix in game after any change here.
    //
    // Do NOT identify the call by its position in the function. It is not reliably the last call, and the compiler
    // schedules local cleanup between it and the epilogue, so a "call followed by the frame reload" row picks up a
    // destructor's return address instead. That resolves cleanly and silently makes the bald fix reject every NPC.
    //
    // Identify it by its ARGUMENT SETUP instead: the world singleton is loaded from a module global, walked to a
    // large sub-object displacement (in the +0x40000 family that k_loaderRegistryCandidates in LT also walks), and
    // passed with an outbound `lea r8,[rbp-X]` result slot.
    //
    // Longer term this target wants a ResolveMode::StringXref tier on the function's own profiling label,
    // "createPrefabFromPartPrefab", with XrefReturn::EnclosingFunction: that literal is what actually names this
    // function, and it survives the code motion that keeps invalidating byte rows here.
    inline constexpr AddrCandidate k_npcPfeReturnAddrCandidates[] = {
        // P1 -- the rule-eval call's argument setup, then the call itself. The landmark is the byte after the call,
        // at match+0x1E. The global's RIP displacement, the sub-object displacement and the outbound frame slot are
        // wildcarded; the trailing `mov eax,0x1FD` is the profiling-scope id the engine emits right after the call
        // and is kept literal because it is what makes the window unique.
        {"NpcPfeReturnAddr_P1_RuleEvalCallLandmark",
         "48 83 C2 40 48 8B 05 ?? ?? ?? ?? 48 8B 08 4C 8D 45 ?? "
         "48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? B8 FD 01 00 00",
         ResolveMode::Direct, 0x1E, 0},
    };

} // namespace EquipHide
