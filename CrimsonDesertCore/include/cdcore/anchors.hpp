#ifndef CDCORE_ANCHORS_HPP
#define CDCORE_ANCHORS_HPP

// Shared AOB candidate ladders used by more than one Crimson Desert mod.
//
// Each table is a ladder of ordered candidates rather than a single signature, so a game patch that shifts code only
// has to leave ONE row intact for the role to keep resolving. The tables enter each mod's DetourModKit anchor
// registry (see the mod's own aob_resolver.hpp) as the `site` of an AnchorKind::RipGlobal entry, and the registry
// resolves the whole table in one parallel pass at startup.
//
// Naming convention (unified across mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
//     RoleName         = semantic role of the resolved address
//                        (WorldSystem, MapLookup, VisualEquipChange, ...)
//     P<N>             = ladder tier (P1 = tightest / most-specific, higher N = wider / deeper fallback)
//     AnchorDescriptor = what the candidate anchors on (FullPrologue, PostAlloca, BodyAnchor, ...)
//
// Each candidate is uniqueness-verified against the live .text section at authoring time. Candidate order is
// most-specific first, and the resolver is first-match-wins, so a tight anchor wins before a looser fallback. Every
// ladder resolves under require_unique, so a pattern that matches more than once inside the image is skipped as
// ambiguous rather than resolving blindly, and a full miss is a clean failure (0) rather than a guess.
//
// Authoring rules (from external/DetourModKit/docs/misc/aob-signatures.md):
//   - Sign CODE, not DATA. Anchor on instruction semantics, not on linker outputs.
//   - Wildcard every immediate operand, RIP-relative disp32, rel8/rel32 jump target, and struct offset the compiler
//     can renumber.
//   - Keep signatures as short as will return a unique hit (7-16 bytes is the common sweet spot, and 12-32 bytes
//     when disambiguation needs it).
//   - Never anchor on a short `Jcc rel8` - compilers flip freely between `74 xx` and `0F 84 xx xx xx xx` across
//     patches.
//   - A Direct row returns (match address + walk_back). A negative walk_back lands on the function start when the
//     anchor sits deeper in the body.
//   - A RipRelative row's match must START at the referencing instruction, because the resolver decode-verifies the
//     instruction at the match point and caps its length at the x86-64 maximum of 15 bytes. Put the `|` result
//     marker immediately before that instruction whenever the window needs lead-in context, then state
//     displacement_at and instruction_length relative to the marker.
//
// The arrays have static storage duration, so an anchor registry may borrow a span over one for the process
// lifetime.

#include <DetourModKit/scan.hpp>

namespace CDCore::Anchors
{
    using DetourModKit::scan::Candidate;
    using DetourModKit::scan::Pattern;

    /**
     * @brief WorldSystem: the module-static slot holding the root world-system global.
     * @details Every row anchors on a `mov reg, [rip+disp32]` that loads this global, inside code the mod chases to
     *          obtain the player actor component.
     *
     *          Walk (runtime data). The +0x30 / +0x58 / +0xD8 manager-chain offsets are owned by
     *          CDCore::ActorChainOffsets (controlled_char.hpp), the single authority shared with the LT/EH
     *          controlled-actor polls:
     *            *(wsPtr) -> *(+0x30) -> *(+0x58) -> *(+0xD8) = actor
     *            actor    -> *(+104)  -> *(+56)              = component
     * @note A 3-tier ladder that resolves the slot address (data, not code).
     */
    inline const Candidate k_worldSystemCandidates[] = {

        // P1 - the accessor body, at the site where it is inlined into its caller. There is no standalone getter
        // function to anchor on: a whole-function row is not available for this global, so do not go looking for one.
        // Per aob-signatures.md section 2.3, an anchor whose function is inlined has to move to the code that
        // survived, and this is that code.
        //
        // Shape: load the global, walk `+0xD8` to the container, take `[+0x20]`, and branch on whether its count at
        // `+0x28` is zero - empty yields a null, otherwise the element at `[+0x20][+0x10]` - then virtual-call
        // `[rax+0x40]` on the result. The load plus the container walk alone is a common idiom with dozens of matches,
        // so the window has to run through the empty-check branch and the vcall to be unique. Both rel8 targets are
        // wildcarded. A compiler that widens either short jump to `0F 8x rel32` breaks this row - that is what P2 is
        // for, since it crosses no branch of its own.
        Candidate::rip_relative(
            "WorldSystem_P1_InlinedGetterThroughVCall",
            Pattern::literal(
                "48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00 48 8B 41 20 83 78 28 00 "
                "77 ?? 33 C9 EB ?? 48 8B 40 20 48 8B 48 10 48 8B 01 FF 50 40"
            ),
            3,
            7
        ),

        // P2 - alternative sibling site:
        //   cmp byte [rax+disp32], 0
        //   <2-byte branch: jne rel8 or the first 2 bytes of jne rel32>
        //   mov rax, [rip+disp32]    <- the resolved instruction, marked with `|`
        //   mov rcx, [rax+0xD8]      <- game-ABI disambiguator
        //
        // The 2-byte branch slot is wildcarded rather than hard-coded as `75 ??`. Per aob-signatures.md section 9 the
        // compiler can flip a short Jcc to the 6-byte `0F 85 rel32` form, and that flip changes the opcode byte. Two
        // wildcard bytes tolerate the 2-byte shape for any opcode, but the pattern still fails on a 6-byte flip. In
        // that case P3 takes over, since it crosses no branch at all. The trailing `48 8B 88 D8 00 00 00` pins the
        // specific WorldSystem follow-on (`mov rcx, [rax+0xD8]`). 0xD8 is a game-struct ABI offset that is stable
        // within a build.
        Candidate::rip_relative(
            "WorldSystem_P2_StructField",
            Pattern::literal("80 B8 ?? ?? ?? ?? 00 ?? ?? | 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00"),
            3,
            7
        ),

        // P3 - a consumer site that walks the global to `+0x58` and passes the result straight into a call. It shares
        // no bytes with P1 or P2, and it crosses neither a branch nor a call, so it survives both the branch-widening
        // that sinks P1 and the entry-block rewrite that sinks P2. Only the outbound frame slot is wildcarded.
        //
        // The window has to reach `mov rcx,rbx` to be usable. Stopping at the `lea` leaves a shape that also matches a
        // neighboring global's walk, and that near-miss resolves to a DIFFERENT global rather than a failure, which
        // require_unique cannot catch because each site matches only once. Verify the decoded target, not only the
        // match count, whenever this row is re-derived.
        Candidate::rip_relative(
            "WorldSystem_P3_ContainerWalkToCall",
            Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 8B 58 58 48 8D 54 24 ?? 48 8B CB"),
            3,
            7
        ),
    };

    /**
     * @brief MapLookup: the IndexedStringA global-table lookup routine.
     * @details Not hooked. Its address anchors the `mov rax, [rip+disp32]` at +20 that points at the IndexedStringA
     *          global. Both mods walk that global to build their CD_-prefixed part-name tables.
     * @note A 2-tier ladder that resolves the function entry. The third authored tier cannot live in this ladder:
     *         see @ref k_mapLookupCallSiteCandidates.
     */
    inline const Candidate k_mapLookupCandidates[] = {

        // P1 - full function prologue plus the first body instruction. The `83 79 04 00` (cmp [rcx+4], 0) check is
        // distinctive. The 2-byte early-out branch slot is wildcarded (see the section 9 branch-encoding note in the
        // WorldSystem P2 comment above).
        Candidate::direct(
            "MapLookup_P1_FullPrologue",
            Pattern::literal(
                "48 83 EC 08 83 79 04 00 4C 8B C1 ?? ?? 33 C0 48 83 C4 08 C3 "
                "48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A"
            )
        ),

        // P2 - hash-body anchor (deeper in the function). Re-anchors when the prologue layout changes. Offset -0x24
        // walks back to function start. The 2-byte jz on zero-count is wildcarded (same branch-encoding caveat as
        // above).
        Candidate::direct(
            "MapLookup_P2_HashBody",
            Pattern::literal("8B 48 ?? 48 03 D2 44 8B 5C D1 ?? 41 8B 08 85 C9 ?? ?? 33 D2 41 8B C3 F7 F1"),
            -0x24
        ),
    };

    /**
     * @brief MapLookup deep fallback: the unique CALLER whose `E8 rel32` reaches the map primitive.
     * @details This map primitive is a templated instantiation and the linker emits more than one copy, so no row
     *          inside the function body can be unique. The way out for a cloned function is to anchor a caller that
     *          IS unique and follow its `E8 rel32` to the callee.
     *
     *          A relative CALL is a branch, not a RIP-relative memory operand, so the RipRelative candidate tier
     *          rejects it: that tier decode-verifies a RIP memory operand at the match. The row therefore resolves
     *          the call instruction itself, and the caller follows the displacement with
     *          scan::resolve_rip_relative(call, 1, 5).
     * @note A 1-row ladder that resolves the address of the `E8` instruction.
     */
    inline const Candidate k_mapLookupCallSiteCandidates[] = {

        // The caller is identified by its argument setup: a module global, `+0x28` to the owner, then the large
        // `+0x10778` walk to the map. That displacement is the distinctive part and stays literal. The `|` marker puts
        // the resolved address on the `E8` itself so the caller can follow its rel32.
        Candidate::direct(
            "MapLookup_P3_CallerArgSetup",
            Pattern::literal("48 8B 05 ?? ?? ?? ?? 48 8B 48 28 48 8B 89 78 07 01 00 | E8 ?? ?? ?? ?? 4C 8B E8")
        ),
    };

    /**
     * @brief PartAddShow: the PartInOut direct-show bypass.
     * @details Both mods hook this function to suppress stale real-part frames that flash through during state
     *          transitions (glide exit, landings, effect spawns) where the game bypasses the PartInOutSocket vis=2
     *          mask.
     *
     *          Signature (x64 __fastcall):
     *            __int64 PartAddShow(
     *                __int64  a1,           // RCX  descriptor context
     *                char     a2,           // DL   transition flag
     *                uint64_t partHashPtr,  // R8   pointer to DWORD part hash
     *                float    blend,        // XMM3 animation blend
     *                __int64  a5..a9)       // stack params
     * @note CALL FREQUENCY. This function is conditional and does not run every frame. It fires in bursts of one call
     *       per entry in the character's show list, a few times per second, and only while the show path is active. Do
     *       NOT conclude the anchor is dead because a breakpoint reports no hits during a short idle sample. Leave the
     *       breakpoint armed and trigger a state transition first.
     * @note A 3-tier ladder that resolves the function entry.
     */
    inline const Candidate k_partAddShowCandidates[] = {

        // P1 is tightened past the bare prologue. A scan of the register-save run alone also hits a Windows module
        // (kernel DLL) function with a different body, so P1 runs on through the same array/count setup P2 anchors on,
        // which is what selects the game function uniquely.
        //
        // The show-list array and count displacements move inside the descriptor between builds. When they move, the
        // count load `mov r9d,[rcx+disp]` can also grow from a disp8 form to a disp32 form. That growth changes the
        // instruction length and breaks any pattern tail that follows it. Both displacements are wildcarded. The hook
        // is a part-hash filter and does not read those fields, so such a move needs an AOB update only, not a code
        // change.
        //
        // The register-save set (`push rbx / rdi / r15`, five bytes) and the register the show-list array base lands in
        // are both compiler-owned and move together. P2's walk-back is measured against that push run, so it has to be
        // re-measured whenever the run changes length.
        Candidate::direct(
            "PartAddShow_P1_FullPrologue",
            Pattern::literal("40 53 57 41 57 48 83 EC ?? 48 8B 59 ?? 4D 8B F8 44 8B 89 ?? ?? ?? ?? 48 8B F9")
        ),

        // P2 - post-prologue anchor (sub rsp / mov rbx,[rcx+X] / mov r15,r8 / mov r9d,[rcx+disp32]). Offset -5 backs
        // up to function start.
        Candidate::direct(
            "PartAddShow_P2_PostPrologue",
            Pattern::literal("48 83 EC ?? 48 8B 59 ?? 4D 8B F8 44 8B 89 ?? ?? ?? ??"),
            -5
        ),

        // P3 - show-list walk setup, entirely past the prologue and past the count load that both other rows depend
        // on. Shape: capture the this-pointer, take the count into eax, scale it by the 0x10 entry stride, add the
        // array base, spill xmm6, park the blend argument in xmm6, then compare base against end for the empty-list
        // guard. The 0x10 stride and the register roles carry the uniqueness budget, and the only wildcarded byte is
        // the xmm6 spill slot. This row survives a further shift of the array and count displacements, which is the
        // known failure mode of the other two rows. Anchors at function start + 0x17.
        //
        // The scale/add pair and the xmm6 spill can be scheduled in either order. A row that stretches to pin more of
        // that ordering is pinning a compiler scheduling choice, so keep the window tight around the two halves that
        // actually carry meaning: the strided walk setup and the blend argument being parked.
        Candidate::direct(
            "PartAddShow_P3_ShowListWalkSetup",
            Pattern::literal("48 8B F9 41 8B C1 48 C1 E0 04 48 03 C3 C5 F8 29 74 24 ?? C5 F8 28 F3 48 3B D8"),
            -0x17
        ),
    };

    /**
     * @brief VisualEquipChange: the bottleneck for all visual equipment changes (equip and unequip).
     * @details Called from the network handler for TrocTrAddVisualEquipItemAck.
     *
     *          Signature (x64 __fastcall):
     *            __int64 VisualEquipChange(
     *                __int64 bodyComp,    // RCX  ClientFrameEventActorComponent*
     *                int16_t slotId,      // DX   equipment slot
     *                int16_t itemId,      // R8W  new item (0xFFFF = removing)
     *                __int64 itemData)    // R9   item data pointer
     * @note A 4-tier ladder that resolves the function entry.
     */
    inline const Candidate k_visualEquipChangeCandidates[] = {

        // P1 - full prologue from `mov [rsp+0x10], rbx` through the `B8 ?? ?? ?? ??` (mov eax, imm32 = __chkstk
        // function-size marker). Stack frame size and function-size hint are wildcarded: both are compiler-owned and
        // drift between builds (section 2).
        Candidate::direct(
            "VisualEquipChange_P1_FullPrologue",
            Pattern::literal(
                "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
                "55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
                "B8 ?? ?? ?? ??"
            )
        ),

        // P2 - push-frame anchor (pushes + lea + mov eax,imm + __chkstk call + sub rsp,rax + the first two
        // post-alloca register moves). The wildcarded stack-size and function-size slots alone match several unrelated
        // prologues. The `48 2B E0 49 8B F1 41 0F B7 D8` tail (the VEC register shuffle through `movzx ebx, r8w`)
        // restores uniqueness without re-introducing a hardcoded stack frame. Offset -0x10 backs up to function start.
        Candidate::direct(
            "VisualEquipChange_P2_PushFrame",
            Pattern::literal(
                "55 57 41 54 41 56 41 57 "
                "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 2B E0 49 8B F1 41 0F B7 D8"
            ),
            -0x10
        ),

        // P3 - post-alloca register shuffle (sub rsp,rax; mov rsi,r9; movzx ebx,r8w; movzx edi,dx; mov r14,rcx) plus
        // the deeper `lea rcx, [rbp+disp32]` and `E8` call. Stack disp32 wildcarded per section 2. Offset -0x2A backs
        // up to function start.
        Candidate::direct(
            "VisualEquipChange_P3_PostAlloca",
            Pattern::literal(
                "48 2B E0 49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
                "48 8D 8D ?? ?? ?? ?? E8"
            ),
            -0x2A
        ),

        // P4 - deepest fallback: the same post-alloca shuffle without the leading `sub rsp,rax`, anchored 3 bytes
        // deeper (mov rsi,r9). Stack disp32 in the `lea rcx,[rbp+disp32]` wildcarded. Offset -0x2D backs up to
        // function start.
        Candidate::direct(
            "VisualEquipChange_P4_PreLeaBody",
            Pattern::literal(
                "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
                "48 8D 8D ?? ?? ?? ??"
            ),
            -0x2D
        ),
    };

    /**
     * @brief BatchEquip: the function that fires when the player equips armor.
     * @details Walks the swap entry list handed in through a4, matches each entry against the actor authority table
     *          at a1+0x80, and calls the equip-change dispatcher to load meshes. Also the bottleneck for
     *          item-to-item visual swaps, which bypass VisualEquipChange. The EquipHide cascade code calls the same
     *          role VisualEquipSwap.
     *
     *          Signature (x64 __fastcall):
     *            _DWORD *BatchEquip(_QWORD *a1, _DWORD *a2, __int64 **a3_old, __int64 **a4_new)
     * @note A 3-tier ladder that resolves the function entry.
     */
    inline const Candidate k_batchEquipCandidates[] = {

        // The arg shuffle right after __chkstk is four moves in a fixed ORDER, though not into fixed registers: a3,
        // then a2, then a1, then [a1+8], each parked in whichever register allocation picked. The order and the `08`
        // displacement of the last load are the stable part. The destinations are not.
        //
        // The compiler rotates WHICH register each one lands in. That rewrites the ModRM byte, and - when the target
        // is a non-extended register (rsi, rdi, rbx) rather than r8-r15 - the REX prefix as well: `4C 8B E2` becomes
        // `48 8B F1`. A row that pins the REX byte therefore dies on a rotation that crosses the extended/legacy line,
        // taking P1 and P2 down together and leaving the ladder on P3 alone. Both rows take the REX byte as a
        // per-nibble token (`4?`) instead, which keeps the one nibble the rotation cannot touch and wildcards the bit
        // that encodes extended-vs-legacy. The `8B` opcodes, the four-move shape and the `08` displacement of the
        // [a1+8] load carry the uniqueness. A rotation never changes instruction length, so the -0x22 and -0x32
        // walk-backs stay valid across one. Frame and function-size immediates stay wildcarded per section 2.

        // P1 - full prologue: save rbx, push 7 callee-saves, lea rbp, mov eax=__chkstk size, call __chkstk, sub
        // rsp,rax, then the four-move arg shuffle.
        Candidate::direct(
            "BatchEquip_P1_FullPrologue",
            Pattern::literal(
                "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
                "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
                "E8 ?? ?? ?? ?? 48 2B E0 4? 8B ?? 4? 8B ?? 4? 8B ?? 4? 8B ?? 08"
            )
        ),

        // P2 - arg shuffle (sub rsp,rax; mov r?,r8; mov r?,rdx; mov r?,rcx; mov r?,[rcx+8]) plus the first on-stack
        // scratch descriptor (lea rax,[rbp+X]; mov [rbp+Y],rax; xor ecx,ecx). Stack disp8 offsets are wildcarded per
        // section 2. Offset -0x22 backs up to function start.
        //
        // A push-frame-only candidate here is non-unique once its stack disp32 is wildcarded. P1 already covers the
        // push-frame region.
        Candidate::direct(
            "BatchEquip_P2_PostAlloca",
            Pattern::literal(
                "48 2B E0 4? 8B ?? 4? 8B ?? 4? 8B ?? 4? 8B ?? 08 "
                "48 8D 45 ?? 48 89 45 ?? 33 C9"
            ),
            -0x22
        ),

        // P3 - deepest fallback: the two on-stack scratch descriptors the function builds before it reads any
        // argument. Shape per descriptor: lea rax,[rbp+X]; mov [rbp+Y],rax (buffer pointer); mov [rbp+Z],ecx (count,
        // zeroed by the shared xor ecx,ecx); mov dword [rbp+W],imm (capacity). Both capacity immediates are SEMANTIC
        // and stay literal per the section 2 exception. Every stack displacement is wildcarded. This row survives an
        // arg-register rotation and a REX change in the shuffle, which is the known failure mode of P1 and P2. Offset
        // -0x32 backs up to function start.
        Candidate::direct(
            "BatchEquip_P3_BodyScratchInit",
            Pattern::literal(
                "48 8D 45 ?? 48 89 45 ?? 33 C9 89 4D ?? C7 45 ?? 02 00 00 00 "
                "48 8D 85 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 89 8D ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 14 00 00 00"
            ),
            -0x32
        ),
    };

    /**
     * @brief ClientActorManagerGlobal: the module-static slot holding the published pa::ClientActorManager* singleton.
     * @details Source of truth for the entire controlled-character resolver chain:
     *            [global] -> mgr (pa::ClientActorManager)
     *            mgr  +0x58 -> userActor (pa::ClientUserActor)
     *            user +0x08 -> subMgr
     *            sub  +0x30 -> Kliff CCOIA (always present)
     *            sub  +0x38 -> currently-controlled CCOIA
     *
     *          A game update that re-lays-out pa::ClientActorManager moves the userActor field (mgr+0x58) and the
     *          CCOIA actor-array descriptor independently, so neither offset can be derived from the other. The
     *          userActor offset is owned by CDCore::ActorChainOffsets (controlled_char.hpp). The descriptor offsets
     *          live with the snapshot walk in controlled_char.cpp, which re-derives them from the live manager.
     * @warning A hardcoded module-relative offset for this slot reads unrelated `.data` on the wrong build, and that
     *          failure is SILENT: a stale offset can land inside a packed string table, and the dereference then
     *          yields ASCII content instead of a heap pointer. Always resolve the slot through this ladder.
     * @note A 2-row ladder that resolves the slot address. There are exactly two rows because the global carries
     *         exactly two references in the image, so the ladder cannot be widened.
     */
    inline const Candidate k_clientActorManagerGlobalCandidates[] = {

        // P1 - publish-store + sibling sub-pointer assignments:
        //   mov [rip+disp32], reg         ; <- publishes the manager, slot +0
        //   lea reg2, [reg+subobj]
        //   mov [rip+disp32], reg2        ; sibling slot +8
        //   lea reg2, [reg+subobj]
        //   mov [rip+disp32], reg2        ; sibling slot +16
        //   movzx eax, byte [rbp+X]
        //   mov [rip+disp32], al          ; state flag, sibling slot +24
        //
        // Nothing here pins a destination register or a sub-object offset. The REX byte of each of those three
        // instructions is kept as a per-nibble `4?` token: the high nibble is what makes the store and the leas 64-bit
        // operations at all, and a drop of it lets the row match a 32-bit or non-REX encoding whose operand layout
        // puts the "disp32" bytes somewhere else entirely.
        //
        // The `lea` immediates are offsets inside the manager and move whenever it is re-laid-out. Per section 2 an
        // operand the engine is free to renumber does not belong in a signature body, and these do not even move with
        // the actor-array descriptor - they move in the OPPOSITE direction from it, so there is no single delta to
        // re-derive them from. The publish register is likewise compiler-owned, which is why the store's REX and
        // ModRM and the leas' base register are wildcards too.
        //
        // What carries the match is the instruction skeleton: a publish store of the incoming pointer, two `lea`+store
        // pairs publishing sub-pointers into consecutive global slots, then a byte-sized state flag into the fourth. No
        // other publish block in the image emits that tail. Every wildcarded instruction keeps a fixed length, so the
        // disp32 of the publish store stays at match+3 and the instruction ends at match+7. require_unique keeps the
        // row honest if a future build duplicates the shape.
        Candidate::rip_relative(
            "ClientActorManagerGlobal_P1_PublishStore",
            Pattern::literal(
                "4? 89 ?? ?? ?? ?? ?? 4? 8D ?? ?? ?? ?? ?? "
                "48 89 05 ?? ?? ?? ?? 4? 8D ?? ?? ?? ?? ?? "
                "48 89 05 ?? ?? ?? ?? "
                "0F B6 85 ?? ?? ?? ?? 88 05"
            ),
            3,
            7
        ),

        // P2 - the only READ of the slot: the same lazy-init function later leas it as an outbound argument.
        //   <loop tail: add rsi,0x10 ; sub r14,1 ; jne>
        //   lea rdx, cs:[rip+slot]        ; 48 8D 15 + disp32, the resolved instruction, marked with `|`
        //   lea rcx, [rbp+disp8]          ; 48 8D 4D + disp8
        //   call helper ; nop
        //   mov byte [rsp+disp8], 0
        //   vpxor xmm0,xmm0,xmm0
        //
        // Both halves of the window earn their length. The post-call tail is needed because the leading lea/lea/call
        // shape alone matches many unrelated sites, and the leading loop tail is needed because the
        // lea/lea/call/nop/vpxor block itself occurs TWICE in this function.
        Candidate::rip_relative(
            "ClientActorManagerGlobal_P2_LeaCallBodyDisp8",
            Pattern::literal(
                "48 83 C6 10 49 83 EE 01 75 ?? "
                "| 48 8D 15 ?? ?? ?? ?? 48 8D 4D ?? "
                "E8 ?? ?? ?? ?? 90 C6 44 24 ?? 00 "
                "C5 F9 EF C0"
            ),
            3,
            7
        ),

        // There is deliberately NO third row.
        //
        // This global has exactly two referencing instructions in the whole image: the publish store P1 anchors on, and
        // the single `lea rdx` P2 anchors on. The second, byte-identical lea/lea/call/nop/vpxor block noted above is
        // tempting as a third row, but it loads a NEIGHBORING global rather than this one. A row cut from it resolves
        // cleanly, passes every plausibility check, and yields the wrong pointer. Verify the resolved TARGET, not only
        // the match count, before a third row lands here.
    };

} // namespace CDCore::Anchors

#endif // CDCORE_ANCHORS_HPP
