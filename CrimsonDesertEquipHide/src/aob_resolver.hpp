#ifndef EQUIPHIDE_AOB_RESOLVER_HPP
#define EQUIPHIDE_AOB_RESOLVER_HPP

/**
 * @file aob_resolver.hpp
 * @brief EquipHide-local AOB candidate ladders plus the declarative anchor registry for the mod.
 *
 * The roles both mods share live in cdcore/anchors.hpp. Only the EquipHide-only roles are declared here. Every
 * table, shared or local, enters the registry below as the `site` of an AnchorKind::RipGlobal entry, and
 * resolve_all_anchors() resolves the whole table in one parallel pass at startup. anchor_address() then hands each
 * resolved address, or 0 on a ladder miss, to the call sites.
 *
 * A mid-body hook site is a Direct row whose walk-back lands on the exact instruction the hook must mid-hook.
 *
 * Naming convention (unified across both mods):
 *   <RoleName>_P<N>_<AnchorDescriptor>
 * See cdcore/anchors.hpp for the full convention and authoring rules.
 */

#include <cdcore/anchors.hpp>

#include <DetourModKit/anchor.hpp>

#include <cstdint>
#include <span>

namespace EquipHide
{
    using CDCore::anchors::Candidate;
    using CDCore::anchors::Pattern;

    /**
     * @brief ChildActor (pa::ClientChildOnlyInGameActor) vtable.
     * @details Resolves by RTTI mangled name first (patch-stable, self-healing), with three nearby RIP-relative byte
     *          loads as fallback. Each byte candidate resolves to the `lea rax, [rip+disp32]` that loads the vtable
     *          base. That base is also the primary (COL.offset == 0) vtable that the RTTI backend returns for the
     *          class, so every tier resolves the same pointer.
     * @note A 4-row ladder resolving the vtable address (data in .rdata, not code).
     */
    inline const Candidate CHILD_ACTOR_VTBL_CANDIDATES[] = {

        // Branch-encoding caveat (aob-signatures.md section 9, the short Jcc rel8 rule): P2 keeps the EB opcode of the
        // trailing 2-byte jmp-over-fallback that follows the vtable store, and wildcards only its rel8 operand. A row
        // that wildcards the EB opcode too loses uniqueness, and the scan reports several matches. If a future
        // compiler flips this jmp to the 6-byte E9 rel32 form, P2 stops matching: P1 and P3 pick up the slack. P1 is
        // truncated, so it does not cross the jmp and stays encoding-independent. P3 wildcards the preceding `74 ??`
        // jz pair as `?? ??`, so it declares only a 2-byte slot, not the rel8 opcode literal.

        // Primary - resolve the vtable by its RTTI mangled name. The AllocCtor stores ClientChildOnlyInGameActor's
        // primary vtable into the new object, so the RTTI backend yields the exact pointer the byte tiers below
        // recover. The mangled name is patch-stable, so this tier self-heals across the vtable relocations and
        // jmp-encoding flips the byte anchors are sensitive to. require_unique does not apply: the RTTI backend is
        // unique-only and fails closed on an ambiguous name, so the ladder falls through to the byte tiers.
        Candidate::rtti_vtable("ChildActorVtbl_RTTI", ".?AVClientChildOnlyInGameActor@pa@@"),

        // P1 - truncated: ends at the `mov [rsi], rax` that stores the vtable. Does NOT cross the trailing `EB 03`
        // jmp, so it survives a Jcc-encoding flip.
        //
        // 48 8B 55 ??            mov rdx, [rbp+d8]
        // 48 89 F1               mov rcx, rsi
        // E8 ?? ?? ?? ??         call <rel32>
        // 90                     nop
        // 48 8D 05 ?? ?? ?? ??   lea rax, [rip+d32]   <- result offset
        // 48 89 06               mov [rsi], rax
        Candidate::rip_relative(
            "ChildActorVtbl_P1_AllocCtor",
            Pattern::literal("48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ?? 48 89 06"),
            3,
            7
        ),

        // P2 - retains the trailing `EB ?? 4C`. Without the 4C byte of the post-jmp `mov rsi, r13` continuation, the
        // lead-in is structurally shared with other ctor sites and the window is no longer unique. Tied to the 2-byte
        // jmp encoding.
        //
        // 48 89 F1               mov rcx, rsi
        // E8 ?? ?? ?? ??         call <rel32>
        // 90                     nop
        // 48 8D 05 ?? ?? ?? ??   lea rax, [rip+d32]   <- result offset
        // 48 89 06               mov [rsi], rax
        // EB ??                  jmp <rel8>
        // 4C                     mov rsi, r13 (truncated)
        Candidate::rip_relative(
            "ChildActorVtbl_P2_CtorStore",
            Pattern::literal("48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C"),
            3,
            7
        ),

        // P3 - the row wildcards the rel8 jz at offset +6 to `?? ??` (both bytes). It does not hardcode the opcode
        // literal, and it still only matches the 2-byte form (it fails on `0F 84 rel32`). The trailing `?? ?? ?? ??`
        // is the lea's own disp32: the RipRelative tier decode-verifies the instruction at the marker, so the
        // matched suffix has to span the displacement it authorizes.
        //
        // 45 31 ED               xor r13d, r13d
        // 48 85 F6               test rsi, rsi
        // ?? ??                  je <rel8>
        // 48 8B 55 ??            mov rdx, [rbp+d8]
        // 48 89 F1               mov rcx, rsi
        // E8 ?? ?? ?? ??         call <rel32>
        // 90                     nop
        // 48 8D 05 ?? ?? ?? ??   lea rax, [rip+d32]   <- result offset
        Candidate::rip_relative(
            "ChildActorVtbl_P3_WiderCtorStore",
            Pattern::literal("45 31 ED 48 85 F6 ?? ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ??"),
            3,
            7
        ),
    };

    /**
     * @brief IndexedStringA map insert routine, the companion to CDCore::anchors::map_lookup().
     * @details Map layout every row depends on: bucket modulus at +0, live count at +4, capacity at +8, bucket array
     *          at +0x10 (256-byte buckets, index `(key % modulus) << 8`), entry-pointer array at +0x18.
     * @warning Do NOT anchor a row on the bucket arithmetic alone. This hash map is heavily templated. The
     *          `div modulus / shl 8 / add [map+0x10]` probe matches hundreds of sites in the image, and the
     *          `inc [map+4]` plus `[map+0x18]` write-back pair matches as many. What singles out THIS instantiation
     *          is its argument shuffle, four arguments parked in a specific order.
     * @note A 3-tier ladder resolving the function entry.
     */
    inline const Candidate MAP_INSERT_CANDIDATES[] = {

        // P1 - full prologue through the argument shuffle.
        // The five callee-saved pushes and the shuffle that follows are the most specific window available. The row
        // wildcards the stack-allocation imm8 because the compiler sizes the frame. No branch sits inside the window,
        // so the displacement cannot move when a branch encoding changes. The match lands on the function start.
        //
        // The window opens with the empty-map test folded into the modulus read (`cmp dword [rcx],0`) and closes on
        // the r9/r8d/rdx/rcx parking. The parking ORDER is the identifier. The map pointer's own register is not.
        //
        // 40 53         push rbx
        // 56            push rsi
        // 57            push rdi
        // 41 54         push r12
        // 41 55         push r13
        // 48 83 EC ??   sub rsp, imm8
        // 83 39 00      cmp dword [rcx], 0x0
        // 4D 8B E1      mov r12, r9
        // 41 8B F0      mov esi, r8d
        // 4C 8B EA      mov r13, rdx
        // 48 8B F9      mov rdi, rcx
        Candidate::direct(
            "MapInsert_P1_FullPrologue",
            Pattern::literal("40 53 56 57 41 54 41 55 48 83 EC ?? 83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9")
        ),

        // P2 - argument shuffle only, with no prologue head at all. Independent of the register-save set, which is
        // the part of the prologue that moves most. Walk back 0x0C bytes to the function start.
        //
        // 83 39 00   cmp dword [rcx], 0x0
        // 4D 8B E1   mov r12, r9
        // 41 8B F0   mov esi, r8d
        // 4C 8B EA   mov r13, rdx
        // 48 8B F9   mov rdi, rcx
        Candidate::direct(
            "MapInsert_P2_ArgShuffleBody",
            Pattern::literal("83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9"),
            -0x0C
        ),

        // P3 - past the shuffle and past the empty-map early-out, opening on the re-read of the modulus and the
        // bucket-array lea that the probe loop runs on. The row wildcards the mid-function callee-saved spill slot.
        //
        // This row shares no PATTERN bytes with P1 or P2, so a rewrite of those bytes cannot take all three down. It
        // is not independent of the entry block, though. The -0x1D walk-back spans that block and the empty-map jcc.
        // An added instruction there, or a jcc widened from rel8 to rel32, still matches the pattern but resolves the
        // row SHORT. Re-measure it whenever the entry block changes.
        //
        // 41 8B D0         mov edx, r8d
        // E8 ?? ?? ?? ??   call <rel32>
        // 44 8B 17         mov r10d, [rdi]
        // 4C 89 74 24 ??   mov [rsp+d8], r14
        // 4C 8D 77 10      lea r14, [rdi+0x10]
        // 45 85 D2         test r10d, r10d
        Candidate::direct(
            "MapInsert_P3_BucketArrayLea",
            Pattern::literal("41 8B D0 E8 ?? ?? ?? ?? 44 8B 17 4C 89 74 24 ?? 4C 8D 77 10 45 85 D2"),
            -0x1D
        ),
    };

    /**
     * @brief EquipVisCheck: the equipment-visibility decision the PartInOut transition runs per part.
     *
     * @details The decision is a leaf function of its own:
     *            float check(a1, const uint32_t *part_hash, PartInOut *pio, uint8_t inOut)
     *          It returns the alpha the caller then publishes: 1.0f show, 0.0f hide, -1.0f "no opinion", which the
     *          caller tests with `vcomiss xmm0,0 / jb` and skips the publish for. Every input arrives in a register;
     *          the function allocates no frame of its own.
     *
     *          Hook point: `movzx r11d, byte ptr [r8+0x20]`, the first instruction after the single
     *          `mov [rsp+8],rbx` spill, plus the `cmp r11b, 3` that follows the `mov rbx,rcx` context move.
     *
     *          Register layout at the hook point:
     *            RCX = the a1 visibility-control context, which the caller loads from its own frame slot and passes
     *                  in. It owns the exclusion array at a1+0x78 and its count at a1+0x80, with a 0x10-byte entry
     *                  stride.
     *            RDX = pointer to the part-hash DWORD. The exclusion walk dereferences it as `mov ecx,[rdx]` before
     *                  comparing against each entry's first DWORD. A register the walk does not dereference is the
     *                  wrong one. Verify against the disassembly, do not assume.
     *            R8  = the PartInOut struct. The hooked movzx reads its visibility byte at +0x20, and the branch
     *                  after the exclusion walk reads its transition byte at +3.
     *            R9B = the In/Out selector the caller passes: 0 = In, 1 = Out. The engine consumes it at
     *                  `test r9b,r9b`, `cmp r9b,1` and `cmp r9b,r11b`. Writing the visibility byte alone does not
     *                  decide the outcome: visibility 2 hides only on the In pass (R9B = 0), visibility 1 only on the
     *                  Out pass.
     *            RBX, R10, R11 are scratch and hold nothing useful before the hooked instruction retires.
     *
     * @warning Do NOT write R8 to steer the decision: it carries the struct pointer, so a write replaces a live
     *          pointer with a small integer that the engine then dereferences. R9B is the only input that steers the
     *          outcome. See on_vis_check_impl in equip_hide.cpp.
     * @warning The compiler is free to inline this body back into its caller. If it does, the rows stop matching and
     *          the anchors have to move to the caller's frame, where the caller spills the same four values to fixed
     *          slots.
     *          That is a re-derivation, not a displacement fix: do not try to patch the offsets through it.
     * @note Ladder contract: each candidate must match exactly once in the scanned scope. A wide shape can still match
     *       once while its walk-back lands mid-instruction after a body shift. Verify the match count and the
     *       match-to-hook displacement together for each added candidate.
     * @note A 3-tier ladder resolving the mid-hook instruction.
     */
    inline const Candidate EQUIP_VIS_CHECK_CANDIDATES[] = {

        // P1 - the whole entry block: the single rbx spill, the visibility read, the context move, the sentinel
        // compare, its branch, and the exclusion-array header. No frame arithmetic sits inside the window because this
        // function allocates no frame, so nothing here moves when the compiler resizes a caller. The row wildcards
        // the branch displacement and the visibility field offset. The exclusion offsets are engine layout and stay
        // literal. The hook lands on the movzx at match + 5.
        //
        // 48 89 5C 24 08         mov [rsp+0x8], rbx
        // 45 0F B6 58 ??         movzx r11d, byte [r8+d8]
        // 48 8B D9               mov rbx, rcx
        // 41 80 FB 03            cmp r11b, 0x3
        // 0F 84 ?? ?? ?? ??      je <rel32>
        // 48 8B 41 78            mov rax, [rcx+0x78]
        // 44 8B 91 80 00 00 00   mov r10d, [rcx+0x80]
        Candidate::direct(
            "EquipVisCheck_P1_EntrySpillToExclusionHeader",
            Pattern::literal(
                "48 89 5C 24 08 45 0F B6 58 ?? 48 8B D9 41 80 FB 03 0F 84 ?? ?? ?? ?? 48 8B 41 78 44 8B 91 80 00 00 00"
            ),
            5
        ),

        // P2 - the same window without the prologue spill, so a change to the register the entry saves (or a move to
        // a push) cannot take this row down with P1. The match lands directly on the movzx, walk-back 0.
        //
        // 45 0F B6 58 ??         movzx r11d, byte [r8+d8]
        // 48 8B D9               mov rbx, rcx
        // 41 80 FB 03            cmp r11b, 0x3
        // 0F 84 ?? ?? ?? ??      je <rel32>
        // 48 8B 41 78            mov rax, [rcx+0x78]
        // 44 8B 91 80 00 00 00   mov r10d, [rcx+0x80]
        // 49 C1 E2 04            shl r10, 0x4
        Candidate::direct(
            "EquipVisCheck_P2_VisReadToExclusionHeader",
            Pattern::literal(
                "45 0F B6 58 ?? 48 8B D9 41 80 FB 03 0F 84 ?? ?? ?? ?? 48 8B 41 78 44 8B 91 80 00 00 00 49 C1 E2 04"
            )
        ),

        // P3 - the exclusion walk alone: array base at a1+0x78, count at a1+0x80, the 0x10-byte stride shift, the
        // empty-list test, and the first key compare through the hash pointer. It shares no pattern byte with P1 or
        // P2, so a rewrite of the entry block leaves it standing. It is not independent of that block, though. The
        // -0x12 walk-back spans the sentinel compare and its rel32 branch, so a wider or narrower branch resolves the
        // row SHORT. Re-measure whenever the entry block changes. Order it last.
        //
        // 48 8B 41 78            mov rax, [rcx+0x78]
        // 44 8B 91 80 00 00 00   mov r10d, [rcx+0x80]
        // 49 C1 E2 04            shl r10, 0x4
        // 4C 03 D0               add r10, rax
        // 49 3B C2               cmp rax, r10
        // 74 ??                  je <rel8>
        // 8B 0A                  mov ecx, [rdx]
        // 39 08                  cmp [rax], ecx
        Candidate::direct(
            "EquipVisCheck_P3_ExclusionWalk",
            Pattern::literal("48 8B 41 78 44 8B 91 80 00 00 00 49 C1 E2 04 4C 03 D0 49 3B C2 74 ?? 8B 0A 39 08"),
            -0x12
        ),
    };

    /**
     * @brief PostfixEval: the postfix rule evaluator.
     * @details Evaluates whether a postfix rule matches the currently equipped items. Returns 1 = rule matches (hide
     *          hair), 0 = no match (keep hair). The engine reaches it through a named method-binding table rather than
     *          a COL-tagged C++ vtable, so there is no RTTI identity to name-resolve and the prologue ladder is the
     *          strongest available anchor.
     * @note A 3-tier ladder resolving the function entry.
     */
    inline const Candidate POSTFIX_EVAL_CANDIDATES[] = {

        // P1 - full prologue through the first body instruction.
        // The prologue alone is not unique: the same three shadow stores, push set, frame allocation and xmm spill
        // pair appear in other functions in the module. The `mov r15, rdx` that follows is what separates this
        // function from them, so the window has to reach it. The row wildcards the frame size and both spill slots
        // because the compiler assigns them. The match lands on the function start.
        //
        // 48 89 5C 24 08      mov [rsp+0x8], rbx
        // 48 89 6C 24 10      mov [rsp+0x10], rbp
        // 48 89 74 24 18      mov [rsp+0x18], rsi
        // 57                  push rdi
        // 41 56               push r14
        // 41 57               push r15
        // 48 83 EC ??         sub rsp, imm8
        // C5 F8 29 74 24 ??   vmovaps [rsp+d8], xmm6
        // C5 F8 29 7C 24 ??   vmovaps [rsp+d8], xmm7
        // 4C 8B FA            mov r15, rdx
        Candidate::direct(
            "PostfixEval_P1_FullPrologue",
            Pattern::literal(
                "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC ?? C5 F8 29 74 24 ?? "
                "C5 F8 29 7C 24 ?? 4C 8B FA"
            )
        ),

        // P2 - push set, frame allocation, xmm spills, first body instruction.
        // Drops the three shadow stores, which are the part of the prologue a recompile is most likely to reshape: an
        // earlier build spilled a fourth register here instead of pushing it. Walk back 0x0F to the function start.
        //
        // 57                  push rdi
        // 41 56               push r14
        // 41 57               push r15
        // 48 83 EC ??         sub rsp, imm8
        // C5 F8 29 74 24 ??   vmovaps [rsp+d8], xmm6
        // C5 F8 29 7C 24 ??   vmovaps [rsp+d8], xmm7
        // 4C 8B FA            mov r15, rdx
        Candidate::direct(
            "PostfixEval_P2_PushSetToBody",
            Pattern::literal("57 41 56 41 57 48 83 EC ?? C5 F8 29 74 24 ?? C5 F8 29 7C 24 ?? 4C 8B FA"),
            -0x0F
        ),

        // P3 - equipped-item vector header only.
        // Loads the entry array from the rule context, loads the count, scales it by the 0x10 entry stride and forms
        // the end pointer. Independent of the whole prologue. The 0x10 stride encoded by `shl rdi, 4` is the same
        // stride bald_fix.cpp walks the container with, so this window also documents that contract. Walk back 0x24 to
        // the function start.
        //
        // 4C 8B FA      mov r15, rdx
        // 48 8B 5A ??   mov rbx, [rdx+d8]
        // 8B 7A ??      mov edi, [rdx+d8]
        // 48 C1 E7 04   shl rdi, 0x4
        // 48 03 FB      add rdi, rbx
        Candidate::direct(
            "PostfixEval_P3_ItemVectorHeader",
            Pattern::literal("4C 8B FA 48 8B 5A ?? 8B 7A ?? 48 C1 E7 04 48 03 FB"),
            -0x24
        ),
    };

    /**
     * @brief Return-address landmark inside createPrefabFromPartPrefab, the engine-registered profiling label for the
     *        prefab instantiation routine.
     *
     * @details That function instantiates a renderable prefab from a PartPrefab, then calls the rule-eval entry point.
     *          The rule-eval pipeline inside that call reaches PostfixEval on the freshly-built instance. The landmark
     *          is the return address of that call, the byte immediately after it.
     *
     *          Player PostfixEval invocations come from the equipment-visibility update loop elsewhere in the binary
     *          and never include this return address on their stack. bald_fix uses stack presence of this landmark to
     *          reject prefab-instantiation-path calls. It does not cache ctx pointers and it does not depend on
     *          frequency heuristics.
     * @warning This landmark carries a MEANING beyond its address: the row has to name the rule-eval call
     *          specifically, so re-verify the bald fix in game after any change here.
     * @warning Do NOT identify the call by its position in the function. It is not reliably the last call, and the
     *          compiler schedules local cleanup between it and the epilogue, so a "call followed by the frame reload"
     *          row picks up a destructor's return address instead. That resolves cleanly and silently makes the bald
     *          fix reject every NPC. Identify it by its ARGUMENT SETUP instead.
     * @note A 1-row ladder resolving the return address.
     */
    inline const Candidate NPC_PFE_RETURN_ADDR_CANDIDATES[] = {

        // P1 - the rule-eval call's argument setup, then the call itself. The landmark is the byte after the call, at
        // match+0x1E. The setup loads the world singleton from a module global, walks it to a large sub-object
        // displacement (in the +0x40000 family that LiveTransmog's LoaderRegistry ladder also walks), and passes an
        // outbound `lea r8,[rbp-X]` result slot. The row wildcards the global's RIP displacement, the sub-object
        // displacement and the outbound frame slot. It keeps the trailing `mov eax,0x1FD` literal because that
        // profiling-scope id, which the engine emits right after the call, is what makes the window unique.
        //
        // Longer term this target wants a StringXref tier on the function's own profiling label,
        // "createPrefabFromPartPrefab", with XrefReturn::EnclosingFunction. That literal is what actually names this
        // function, and it survives the code motion that keeps invalidating byte rows here.
        //
        // 48 83 C2 40            add rdx, 0x40
        // 48 8B 05 ?? ?? ?? ??   mov rax, [rip+d32]
        // 48 8B 08               mov rcx, [rax]
        // 4C 8D 45 ??            lea r8, [rbp-d8]
        // 48 8B 89 ?? ?? ?? ??   mov rcx, [rcx+d32]
        // E8 ?? ?? ?? ??         call <rel32>
        // B8 FD 01 00 00         mov eax, 0x1FD
        Candidate::direct(
            "NpcPfeReturnAddr_P1_RuleEvalCallLandmark",
            Pattern::literal(
                "48 83 C2 40 48 8B 05 ?? ?? ?? ?? 48 8B 08 4C 8D 45 ?? 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                "B8 FD 01 00 00"
            ),
            0x1E
        ),
    };

    /**
     * @enum AnchorId
     * @brief Every address the mod resolves at startup. The enumerator order IS the registry table order.
     * @warning Index the results through these enumerators and never through a literal: inserting a row otherwise
     *          silently re-points every later consumer.
     */
    enum class AnchorId : std::size_t
    {
        /// Module-static slot for the root world-system global (data).
        WorldSystem,
        /// pa::ClientChildOnlyInGameActor vtable (data, .rdata).
        ChildActorVtbl,
        /// IndexedStringA lookup routine entry (code).
        MapLookup,
        /// IndexedStringA insert routine entry (code).
        MapInsert,
        /// The mid-body EquipVisCheck instruction the visibility mid-hook patches (code).
        EquipVisCheck,
        /// PartAddShow function entry (code).
        PartAddShow,
        /// PostfixEval function entry (code).
        PostfixEval,
        /// Return-address landmark inside createPrefabFromPartPrefab (code).
        NpcPfeReturnAddr,
        /// VisualEquipChange function entry (code).
        VisualEquipChange,
        /// BatchEquip function entry, which this mod calls VisualEquipSwap (code).
        VisualEquipSwap,
        /// Sentinel: the number of anchors in the registry table.
        Count
    };

    /**
     * @brief Resolves the whole anchor table in one parallel pass and records every address.
     * @details Grades the candidate patterns offline first, then resolves. Each entry's validator rejects a value
     *          outside the host image. A code entry additionally rejects a site whose first byte is zero fill, INT3
     *          padding, or a near return, so a match that landed past the end of a function degrades to a clean miss
     *          instead of a hook on an unrelated address.
     * @note Setup/control-plane only: the pass spawns a worker pool. Never call it under the loader lock.
     */
    void resolve_all_anchors();

    /**
     * @brief The address recorded for @p id, or 0 when its ladder missed.
     * @note Callback-safe after resolve_all_anchors() returns: a plain read of write-once storage.
     */
    [[nodiscard]] std::uintptr_t anchor_address(AnchorId id) noexcept;

    /**
     * @brief The per-anchor drift report, for the startup quality summary and the diagnostics snapshot.
     * @note Callback-safe after resolve_all_anchors() returns.
     */
    [[nodiscard]] std::span<const DetourModKit::anchor::ResolvedAnchor> anchor_report() noexcept;

} // namespace EquipHide

#endif // EQUIPHIDE_AOB_RESOLVER_HPP
