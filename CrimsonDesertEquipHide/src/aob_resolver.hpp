#pragma once

// EquipHide-local AOB candidate ladders plus the declarative anchor registry for the mod.
//
// The roles both mods share live in cdcore/anchors.hpp; only the EquipHide-only roles are declared here. Every
// table, shared or local, enters the registry below as the `site` of an AnchorKind::RipGlobal entry, and
// resolve_all_anchors() resolves the whole table in one parallel pass at startup. anchor_address() then hands each
// resolved address, or 0 on a ladder miss, to the call sites.
//
// A mid-body hook site is a Direct row whose walk-back lands on the exact instruction the hook must mid-hook.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>

#include <DetourModKit/anchor.hpp>

#include <cstdint>
#include <span>

namespace EquipHide
{
    using CDCore::Anchors::Candidate;
    using CDCore::Anchors::Pattern;

    /**
     * @brief ChildActor (pa::ClientChildOnlyInGameActor) vtable.
     * @details Resolves by RTTI mangled name first (patch-stable, self-healing), with three nearby RIP-relative byte
     *          loads as fallback. Each byte candidate resolves to the `lea rax, [rip+disp32]` that loads the vtable
     *          base. That base is also the primary (COL.offset == 0) vtable that the RTTI backend returns for the
     *          class, so every tier resolves the same pointer.
     * @note A 4-row ladder resolving the vtable address (data in .rdata, not code).
     */
    inline const Candidate k_childActorVtblCandidates[] = {

        // Branch-encoding caveat (aob-signatures.md section 9, the short Jcc rel8 rule): P2 keeps the EB opcode of the
        // trailing 2-byte jmp-over-fallback that follows the vtable store, and wildcards only its rel8 operand. If you
        // wildcard the EB opcode too, the window loses uniqueness and the scan reports several matches. If a future
        // compiler flips this jmp to the 6-byte E9 rel32 form, P2 stops matching: P1 and P3 pick up the slack. P1 is
        // truncated, so it does not cross the jmp and stays encoding-independent. P3 wildcards the preceding `74 ??`
        // jz pair as `?? ??`, so it declares only a 2-byte slot, not the rel8 opcode literal.

        // Primary -- resolve the vtable by its RTTI mangled name. The AllocCtor stores ClientChildOnlyInGameActor's
        // primary vtable into the new object, so the RTTI backend yields the exact pointer the byte tiers below
        // recover. The mangled name is patch-stable, so this tier self-heals across the vtable relocations and
        // jmp-encoding flips the byte anchors are sensitive to. require_unique does not apply: the RTTI backend is
        // unique-only and fails closed on an ambiguous name, so the ladder falls through to the byte tiers.
        Candidate::rtti_vtable("ChildActorVtbl_RTTI", ".?AVClientChildOnlyInGameActor@pa@@"),

        // P1 -- truncated: ends at the `mov [rsi], rax` that stores the vtable. Does NOT cross the trailing `EB 03`
        // jmp, so it survives a Jcc-encoding flip.
        Candidate::rip_relative(
            "ChildActorVtbl_P1_AllocCtor",
            Pattern::literal("48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ?? 48 89 06"),
            3,
            7
        ),

        // P2 -- retains the trailing `EB ?? 4C`. Without the 4C byte of the post-jmp `mov rsi, r13` continuation, the
        // lead-in is structurally shared with other ctor sites and the window is no longer unique. Tied to the 2-byte
        // jmp encoding.
        Candidate::rip_relative(
            "ChildActorVtbl_P2_CtorStore",
            Pattern::literal("48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C"),
            3,
            7
        ),

        // P3 -- the rel8 jz at offset +6 is wildcarded to `?? ??` (both bytes). Does not hardcode the opcode literal,
        // but still only matches the 2-byte form (fails on `0F 84 rel32`). The trailing `?? ?? ?? ??` is the lea's own
        // disp32: the RipRelative tier decode-verifies the instruction at the marker, so the matched suffix has to
        // span the displacement it authorizes.
        Candidate::rip_relative(
            "ChildActorVtbl_P3_WiderCtorStore",
            Pattern::literal("45 31 ED 48 85 F6 ?? ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 | 48 8D 05 ?? ?? ?? ??"),
            3,
            7
        ),
    };

    /**
     * @brief IndexedStringA map insert routine, the companion to CDCore::Anchors::map_lookup().
     * @details Map layout every row depends on: bucket modulus at +0, live count at +4, capacity at +8, bucket array
     *          at +0x10 (256-byte buckets, index `(key % modulus) << 8`), entry-pointer array at +0x18.
     * @warning Do NOT anchor a row on the bucket arithmetic alone. This hash map is heavily templated: the
     *          `div modulus / shl 8 / add [map+0x10]` probe matches hundreds of sites in the image, and so does the
     *          `inc [map+4]` plus `[map+0x18]` write-back pair. What singles out THIS instantiation is its argument
     *          shuffle, four arguments parked in a specific order.
     * @note A 3-tier ladder resolving the function entry.
     */
    inline const Candidate k_mapInsertCandidates[] = {

        // P1 -- full prologue through the argument shuffle.
        // The five callee-saved pushes and the shuffle that follows are the most specific window available. The
        // stack-allocation imm8 is wildcarded because the compiler sizes the frame. No branch sits inside the window,
        // so the displacement cannot move when a branch encoding changes. The match lands on the function start.
        //
        // The window opens with the empty-map test folded into the modulus read (`cmp dword [rcx],0`) and closes on
        // the r9/r8d/rdx/rcx parking. The parking ORDER is the identifier; the map pointer's own register is not.
        Candidate::direct(
            "MapInsert_P1_FullPrologue",
            Pattern::literal("40 53 56 57 41 54 41 55 48 83 EC ?? 83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9")
        ),

        // P2 -- argument shuffle only, with no prologue head at all. Independent of the register-save set, which is
        // the part of the prologue that moves most. Walk back 0x0C bytes to the function start.
        Candidate::direct(
            "MapInsert_P2_ArgShuffleBody",
            Pattern::literal("83 39 00 4D 8B E1 41 8B F0 4C 8B EA 48 8B F9"),
            -0x0C
        ),

        // P3 -- past the shuffle and past the empty-map early-out, opening on the re-read of the modulus and the
        // bucket-array lea that the probe loop runs on. The mid-function callee-saved spill slot is wildcarded.
        //
        // This row shares no PATTERN bytes with P1 or P2, so a rewrite of those bytes cannot take all three down. It
        // is not independent of the entry block, though: its -0x1D walk-back is measured across that block and across
        // the empty-map jcc, so an added instruction there, or a widening of that jcc from rel8 to rel32, leaves the
        // row matching and resolving SHORT. Re-measure it whenever the entry block changes.
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
     *            float check(a1, const uint32_t *partHash, PartInOut *pio, uint8_t inOut)
     *          It returns the alpha the caller then publishes: 1.0f show, 0.0f hide, -1.0f "no opinion", which the
     *          caller tests with `vcomiss xmm0,0 / jb` and skips the publish for. Every input arrives in a register;
     *          the function allocates no frame of its own.
     *
     *          Hook point: `movzx r11d, byte ptr [r8+0x20]` followed by `cmp r11b, 3`, the first two instructions
     *          after the single `mov [rsp+8],rbx` spill.
     *
     *          Register layout at the hook point:
     *            RCX = the a1 visibility-control context, which the caller loads from its own frame slot and passes
     *                  in. It owns the exclusion array at a1+0x78 and its count at a1+0x80, with a 0x10-byte entry
     *                  stride.
     *            RDX = pointer to the part-hash DWORD. The exclusion walk dereferences it as `mov ecx,[rdx]` before
     *                  comparing against each entry's first DWORD. A register the walk does not dereference is the
     *                  wrong one; verify against the disassembly rather than assuming.
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
     *          the anchors have to move to the caller's frame, where the same four values are spilled to fixed slots.
     *          That is a re-derivation, not a displacement fix: do not try to patch the offsets through it.
     * @note Ladder contract: each candidate must match exactly once in the scanned scope. A wide shape can still match
     *       once while its walk-back lands mid-instruction after a body shift. Verify the match count and the
     *       match-to-hook displacement together when you add a candidate.
     * @note A 3-tier ladder resolving the mid-hook instruction.
     */
    inline const Candidate k_equipVisCheckCandidates[] = {

        // P1 -- the whole entry block: the single rbx spill, the visibility read, the context move, the sentinel
        // compare, its branch, and the exclusion-array header. No frame arithmetic sits inside the window because this
        // function allocates no frame, so nothing here moves when the compiler resizes a caller. The branch
        // displacement and the visibility field offset are wildcarded; the exclusion offsets are engine layout and
        // stay literal. The hook lands on the movzx at match + 5.
        Candidate::direct(
            "EquipVisCheck_P1_EntrySpillToExclusionHeader",
            Pattern::literal(
                "48 89 5C 24 08 45 0F B6 58 ?? 48 8B D9 41 80 FB 03 0F 84 ?? ?? ?? ?? "
                "48 8B 41 78 44 8B 91 80 00 00 00"
            ),
            5
        ),

        // P2 -- the same window with the prologue spill dropped, so a change to which register is saved on entry (or a
        // move to a push) cannot take this row down with P1. The match lands directly on the movzx, walk-back 0.
        Candidate::direct(
            "EquipVisCheck_P2_VisReadToExclusionHeader",
            Pattern::literal(
                "45 0F B6 58 ?? 48 8B D9 41 80 FB 03 0F 84 ?? ?? ?? ?? "
                "48 8B 41 78 44 8B 91 80 00 00 00 49 C1 E2 04"
            )
        ),

        // P3 -- the exclusion walk alone: array base at a1+0x78, count at a1+0x80, the 0x10-byte stride shift, the
        // empty-list test, and the first key compare through the hash pointer. It shares no pattern byte with P1 or
        // P2, so a rewrite of the entry block leaves it standing. It is not independent of that block, though: the
        // -0x12 walk-back is measured across the sentinel compare and its rel32 branch, so widening or narrowing that
        // branch resolves the row SHORT. Re-measure whenever the entry block changes; order it last.
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
    inline const Candidate k_postfixEvalCandidates[] = {

        // P1 -- full prologue through the first body instruction.
        // The prologue alone is not unique: the same three shadow stores, push set, frame allocation and xmm spill
        // pair appear in other functions in the module. The `mov r15, rdx` that follows is what separates this
        // function from them, so the window has to reach it. The frame size and both spill slots are wildcarded
        // because the compiler assigns them. The match lands on the function start.
        Candidate::direct(
            "PostfixEval_P1_FullPrologue",
            Pattern::literal(
                "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 "
                "48 83 EC ?? C5 F8 29 74 24 ?? C5 F8 29 7C 24 ?? 4C 8B FA"
            )
        ),

        // P2 -- push set, frame allocation, xmm spills, first body instruction.
        // Drops the three shadow stores, which are the part of the prologue a recompile is most likely to reshape: an
        // earlier build spilled a fourth register here instead of pushing it. Walk back 0x0F to the function start.
        Candidate::direct(
            "PostfixEval_P2_PushSetToBody",
            Pattern::literal("57 41 56 41 57 48 83 EC ?? C5 F8 29 74 24 ?? C5 F8 29 7C 24 ?? 4C 8B FA"),
            -0x0F
        ),

        // P3 -- equipped-item vector header only.
        // Loads the entry array from the rule context, loads the count, scales it by the 0x10 entry stride and forms
        // the end pointer. Independent of the whole prologue. The 0x10 stride encoded by `shl rdi, 4` is the same
        // stride bald_fix.cpp walks the container with, so this window also documents that contract. Walk back 0x24 to
        // the function start.
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
     * @warning This landmark is a MEANING, not just an address: the row has to name the rule-eval call specifically,
     *          so re-verify the bald fix in game after any change here.
     * @warning Do NOT identify the call by its position in the function. It is not reliably the last call, and the
     *          compiler schedules local cleanup between it and the epilogue, so a "call followed by the frame reload"
     *          row picks up a destructor's return address instead. That resolves cleanly and silently makes the bald
     *          fix reject every NPC. Identify it by its ARGUMENT SETUP instead.
     * @note A 1-row ladder resolving the return address.
     */
    inline const Candidate k_npcPfeReturnAddrCandidates[] = {

        // P1 -- the rule-eval call's argument setup, then the call itself. The landmark is the byte after the call, at
        // match+0x1E. The world singleton is loaded from a module global, walked to a large sub-object displacement
        // (in the +0x40000 family that LiveTransmog's LoaderRegistry ladder also walks), and passed with an outbound
        // `lea r8,[rbp-X]` result slot. The global's RIP displacement, the sub-object displacement and the outbound
        // frame slot are wildcarded; the trailing `mov eax,0x1FD` is the profiling-scope id the engine emits right
        // after the call and is kept literal because it is what makes the window unique.
        //
        // Longer term this target wants a StringXref tier on the function's own profiling label,
        // "createPrefabFromPartPrefab", with XrefReturn::EnclosingFunction: that literal is what actually names this
        // function, and it survives the code motion that keeps invalidating byte rows here.
        Candidate::direct(
            "NpcPfeReturnAddr_P1_RuleEvalCallLandmark",
            Pattern::literal(
                "48 83 C2 40 48 8B 05 ?? ?? ?? ?? 48 8B 08 4C 8D 45 ?? "
                "48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? B8 FD 01 00 00"
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
        Count
    };

    /**
     * @brief Resolves the whole anchor table in one parallel pass and records every address.
     * @details Grades the candidate patterns offline first, then resolves. Each entry's validator rejects a value
     *          outside the host image, and a code entry additionally rejects a site whose first byte cannot begin an
     *          instruction, so a freak match degrades to a clean miss instead of a hook on an unrelated address.
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
