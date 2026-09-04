#ifndef CDCORE_ANCHORS_HPP
#define CDCORE_ANCHORS_HPP

// ---------------------------------------------------------------------------
// Shared AOB candidate tables used by multiple Crimson Desert mods.
//
// Naming convention (unified across mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
//     RoleName         = semantic role of the resolved address
//                        (WorldSystem, MapLookup, VisualEquipChange, ...)
//     P<N>             = cascade tier (P1 = tightest / most-specific,
//                        higher N = wider / deeper fallback)
//     AnchorDescriptor = what the candidate anchors on
//                        (FullPrologue, PostAlloca, BodyAnchor, ...)
//
// Each candidate is uniqueness-verified against the live .text section at authoring time. Cascades exist so that a
// partial recompilation or a sibling-DLL hook patch that invalidates the tightest signature still resolves.
//
// Authoring rules (from external/DetourModKit/docs/misc/aob-signatures.md):
//   - Sign CODE, not DATA. Anchor on instruction semantics, not on linker outputs.
//   - Wildcard every immediate operand, RIP-relative disp32, rel8/rel32 jump target, struct offset the compiler can
//     renumber.
//   - Keep signatures as short as will return a unique hit (7-16 bytes is the common sweet spot, and 12-32 bytes
//     when disambiguation needs it).
//   - Never anchor on a short `Jcc rel8` -- compilers flip freely between `74 xx` and `0F 84 xx xx xx xx` across
//     patches.
//   - Each `resolve_address()` returns (match_address + dispOffset). For Direct mode a negative dispOffset walks
//     backward to land on the function start when the anchor sits deeper in the body.
//
// Consumers expose these arrays inside their own mod namespace via a reference alias:
//
//     namespace EquipHide {
//         inline constexpr auto &k_worldSystemCandidates =
//             CDCore::Anchors::k_worldSystemCandidates;
//     }
//
// so call sites remain unchanged.
// ---------------------------------------------------------------------------

#include <DetourModKit/scanner.hpp>

namespace CDCore::Anchors
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    // -----------------------------------------------------------------------
    // WorldSystem -- static pointer for the root world-system global.
    //
    // Every row anchors on a `mov reg, [rip+disp32]` that loads this global, inside code the mod chases to obtain the
    // player actor component. The disp32 offset and the instruction end differ per row and are stated on each.
    //
    // Walk (runtime-data). The +0x30 / +0x58 / +0xD8 manager-chain offsets are owned by CDCore::ActorChainOffsets
    // (controlled_char.hpp), the single authority shared with the LT/EH controlled-actor polls:
    //   *(wsPtr) -> *(+0x30) -> *(+0x58) -> *(+0xD8) = actor
    //   actor    -> *(+104)  -> *(+56)             = component
    //
    // 3-tier cascade. P2_StructField is a structurally different sibling site that pulls the same pointer via a
    // different prologue shape.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_worldSystemCandidates[] = {
        // P1 -- the accessor body, at the site where it is inlined into its caller. There is no standalone getter
        // function to anchor on: a whole-function row is not available for this global, so do not go looking for
        // one. Per aob-signatures.md section 2.3, an anchor whose function is inlined has to move to the code that
        // survived, and this is that code.
        //
        // Shape: load the global, walk `+0xD8` to the container, take `[+0x20]`, and branch on whether its count at
        // `+0x28` is zero -- empty yields a null, otherwise the element at `[+0x20][+0x10]` -- then virtual-call
        // `[rax+0x40]` on the result. The load plus the container walk alone is a common idiom with dozens of
        // matches, so the window has to run through the empty-check branch and the vcall to be unique. Both rel8
        // targets are wildcarded. A compiler that widens either short jump to `0F 8x rel32` breaks this row -- that
        // is what P2 is for, since it crosses no branch of its own.
        {"WorldSystem_P1_InlinedGetterThroughVCall",
         "48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00 48 8B 41 20 83 78 28 00 "
         "77 ?? 33 C9 EB ?? 48 8B 40 20 48 8B 48 10 48 8B 01 FF 50 40",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- alternative sibling site:
        //   cmp byte [rax+disp32], 0
        //   <2-byte branch: jne rel8 or first 2 bytes of jne rel32>
        //   mov rax, [rip+disp32]    <- the resolved instruction
        //   mov rcx, [rax+0xD8]      <- game-ABI disambiguator
        //
        // The 2-byte branch slot is wildcarded rather than hard-coded as `75 ??`. Per aob-signatures.md section 9 the
        // compiler can flip a short Jcc to the 6-byte `0F 85 rel32` form, and that flip changes the opcode byte. Two
        // wildcard bytes tolerate the 2-byte shape for any opcode, but the pattern still fails on a 6-byte flip. In
        // that case P3 takes over, since it crosses no branch at all. The trailing `48 8B 88 D8 00 00 00` pins
        // the specific WorldSystem follow-on (`mov rcx, [rax+0xD8]`). 0xD8 is a game-struct ABI offset that is stable
        // within a build.
        {"WorldSystem_P2_StructField", "80 B8 ?? ?? ?? ?? 00 ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- shortest anchor: a `mov rcx, [rip+disp32] ; mov rcx,[rcx+X] ; call ; test al,al ; sete al` site. It
        // shares no bytes with P1 or P2 and crosses no branch, so it survives both the branch-widening that sinks P1
        // and the entry-block rewrite that sinks P2.
        //
        // This is the shape of the standalone getter that P1's site inlines, so it only resolves on a build that
        // still emits one somewhere. Treat it as an opportunistic tier, not a guaranteed fallback, and re-verify it
        // in the disassembler rather than assuming the cascade has three live rows.
        {"WorldSystem_P3_InnerLoad", "48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    // -----------------------------------------------------------------------
    // MapLookup -- IndexedStringA global-table lookup routine.
    //
    // Not hooked. Its address is used as the RIP anchor for the `mov rax, [rip+disp32]` at offset +20 which points at
    // the IndexedStringA global. Both mods walk that global to build their CD_*/part-name tables.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_mapLookupCandidates[] = {
        // P1 -- full function prologue + first body instruction. The `83 79 04 00` (cmp [rcx+4], 0) check is
        // distinctive. The 2-byte early-out branch slot is wildcarded (see the section 9 branch-encoding note in the
        // WorldSystem P2 comment above).
        {"MapLookup_P1_FullPrologue",
         "48 83 EC 08 83 79 04 00 4C 8B C1 ?? ?? 33 C0 48 83 C4 08 C3 48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A",
         ResolveMode::Direct, 0, 0},

        // P2 -- hash-body anchor (deeper in the function). Re-anchors when the prologue layout changes. Offset -0x24
        // walks back to function start. The 2-byte jz on zero-count is wildcarded (same branch-encoding caveat as
        // above).
        {"MapLookup_P2_HashBody", "8B 48 ?? 48 03 D2 44 8B 5C D1 ?? 41 8B 08 85 C9 ?? ?? 33 D2 41 8B C3 F7 F1",
         ResolveMode::Direct, -0x24, 0},

        // P3 -- hash-loop anchor (even deeper). Last-resort fallback. Offset -0x3D walks back to function start.
        {"MapLookup_P3_HashLoop", "44 8B CA 33 D2 49 C1 E1 08 4D 03 48 ?? 45 8B 11 45 85 D2", ResolveMode::Direct,
         -0x3D, 0},
    };

    // -----------------------------------------------------------------------
    // PartAddShow -- PartInOut direct-show bypass.
    //
    // Both mods hook this function to suppress stale real-part frames that flash through during state transitions
    // (glide exit, landings, effect spawns) where the game bypasses the PartInOutSocket vis=2 mask.
    //
    // Signature (x64 __fastcall):
    //   __int64 PartAddShow(
    //       __int64 a1,           // RCX  descriptor context
    //       char    a2,           // DL   transition flag
    //       uint64_t partHashPtr, // R8   pointer to DWORD part hash
    //       float   blend,        // XMM3 animation blend
    //       __int64 a5..a9)       // stack params
    //
    // P1 is tightened past the bare prologue. A scan of the register-save run alone also hits a Windows module
    // (kernel DLL) function with a different body, so P1 runs on through the same array/count setup P2 anchors on,
    // which is what selects the game function uniquely.
    //
    // The show-list array and count displacements move inside the descriptor between builds. When they move, the
    // count load `mov r9d,[rcx+disp]` can also grow from a disp8 form to a disp32 form. That growth changes the
    // instruction length and breaks any pattern tail that follows it. Both displacements are wildcarded below. The
    // hook is a part-hash filter and does not read those fields, so such a move needs an AOB update only, not a code
    // change.
    //
    // CALL FREQUENCY. This function is conditional and does not run every frame. It fires in bursts of one call per
    // entry in the character's show list, a few times per second, and only while the show path is active. Do NOT
    // conclude the anchor is dead because a breakpoint reports no hits during a short idle sample. Leave the
    // breakpoint armed and trigger a state transition first.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_partAddShowCandidates[] = {
        // P1 -- full prologue through the array/count setup, including the post-alloca moves. The bare prologue also
        // matches a Windows DLL function with a different body, so those moves are what make the row unique.
        //
        // The register-save set (`push rbx / rdi / r15`, five bytes) and the register the show-list array base lands
        // in are both compiler-owned and move together. P2's walk-back is measured against that push run, so it has
        // to be re-measured whenever the run changes length.
        {"PartAddShow_P1_FullPrologue", "40 53 57 41 57 48 83 EC ?? 48 8B 59 ?? 4D 8B F8 44 8B 89 ?? ?? ?? ?? 48 8B F9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue anchor (sub rsp / mov rbx,[rcx+X] / mov r15,r8 / mov r9d,[rcx+disp32]). Offset -5 backs
        // up to function start.
        {"PartAddShow_P2_PostPrologue", "48 83 EC ?? 48 8B 59 ?? 4D 8B F8 44 8B 89 ?? ?? ?? ??", ResolveMode::Direct,
         -5, 0},

        // P3 -- show-list walk setup, entirely past the prologue and past the count load that both other rows depend
        // on. Shape: capture the this-pointer, take the count into eax, scale it by the 0x10 entry stride, add the
        // array base, spill xmm6, park the blend argument in xmm6, then compare base against end for the empty-list
        // guard. The 0x10 stride and the register roles carry the uniqueness budget, and the only wildcarded byte is
        // the xmm6 spill slot. This row survives a further shift of the array and count displacements, which is the
        // known failure mode of the other two rows. Anchors at function start + 0x17.
        //
        // The scale/add pair and the xmm6 spill can be scheduled in either order. A row that stretches to pin more
        // of that ordering is pinning a compiler scheduling choice, so keep the window tight around the two halves
        // that actually carry meaning: the strided walk setup and the blend argument being parked.
        {"PartAddShow_P3_ShowListWalkSetup",
         "48 8B F9 41 8B C1 48 C1 E0 04 48 03 C3 C5 F8 29 74 24 ?? C5 F8 28 F3 48 3B D8", ResolveMode::Direct, -0x17,
         0},
    };

    // -----------------------------------------------------------------------
    // VisualEquipChange -- bottleneck for all visual equipment changes (equip and unequip). Called from the network
    // handler for TrocTrAddVisualEquipItemAck.
    //
    // Signature (x64 __fastcall):
    //   __int64 VisualEquipChange(
    //       __int64 bodyComp,    // RCX  ClientFrameEventActorComponent*
    //       int16_t slotId,      // DX   equipment slot
    //       int16_t itemId,      // R8W  new item (0xFFFF = removing)
    //       __int64 itemData)    // R9   item data pointer
    //
    // 4-tier cascade. P1 is the shared prologue, identical in both mods. P2 is a "push-frame" anchor unique to the
    // prologue shape of this function. P3 is the post-alloca register-shuffle anchor. P4 is a deeper body anchor past
    // the `lea rcx,[rbp+X]`.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_visualEquipChangeCandidates[] = {
        // P1 -- full prologue from `mov [rsp+0x10], rbx` through the `B8 ?? ?? ?? ??` (mov eax, imm32 = __chkstk
        // function-size marker). Stack frame size and function-size hint are wildcarded -- both are compiler-owned and
        // drift between builds (section 2).
        {"VisualEquipChange_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
         "55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- push-frame anchor (pushes + lea + mov eax,imm + __chkstk call + sub rsp,rax + first two post-alloca
        // register moves). The wildcarded stack-size and function-size slots alone match several unrelated prologues.
        // The `48 2B E0 49 8B F1 41 0F B7 D8` tail (the VEC register shuffle through `movzx ebx, r8w`) restores
        // uniqueness without re-introducing a hardcoded stack frame. Offset -0x10 backs up to function start.
        {"VisualEquipChange_P2_PushFrame",
         "55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
         "E8 ?? ?? ?? ?? 48 2B E0 49 8B F1 41 0F B7 D8",
         ResolveMode::Direct, -0x10, 0},

        // P3 -- post-alloca register shuffle (sub rsp,rax; mov rsi,r9; movzx ebx,r8w; movzx edi,dx; mov r14,rcx) plus
        // the deeper `lea rcx, [rbp+disp32]` and `E8` call. Stack disp32 wildcarded per section 2. Offset -0x2A backs
        // up to function start.
        {"VisualEquipChange_P3_PostAlloca",
         "48 2B E0 49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D ?? ?? ?? ?? E8",
         ResolveMode::Direct, -0x2A, 0},

        // P4 -- deepest fallback: same post-alloca shuffle without the leading `sub rsp,rax`, anchored 3 bytes deeper
        // (mov rsi,r9). Stack disp32 in the `lea rcx,[rbp+disp32]` wildcarded. Offset -0x2D backs up to function start.
        {"VisualEquipChange_P4_PreLeaBody",
         "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D ?? ?? ?? ??",
         ResolveMode::Direct, -0x2D, 0},
    };

    // -----------------------------------------------------------------------
    // BatchEquip -- the function that fires when the player equips armor. Walks the swap entry list handed in through
    // a4, matches each entry against the actor authority table at a1+0x80, and calls the equip-change dispatcher to
    // load meshes. Also the bottleneck for item-to-item visual swaps, which bypass VisualEquipChange. Named BatchEquip
    // here. The EquipHide cascade code calls the same role VisualEquipSwap.
    //
    // Signature (x64 __fastcall):
    //   _DWORD* BatchEquip(
    //       _QWORD*   a1,
    //       _DWORD*   a2,
    //       __int64** a3_old,
    //       __int64** a4_new)
    //
    // 3-tier cascade: P1 = full prologue, P2 = arg shuffle plus scratch prep, P3 = deepest body anchor, past every
    // argument-carrying instruction.
    //
    // The arg shuffle right after __chkstk is four moves in a fixed ORDER, though not into fixed registers: a3,
    // then a2, then a1, then [a1+8], each parked in whichever register allocation picked. The order and the `08`
    // displacement of the last load are the stable part; the destinations are not.
    // The compiler rotates WHICH register each one lands in. That rewrites the ModRM byte, and -- when the target is
    // a non-extended register (rsi, rdi, rbx) rather than r8-r15 -- the REX prefix as well: `4C 8B E2` becomes
    // `48 8B F1`. A row that pins the REX byte therefore dies on a rotation that crosses the extended/legacy line,
    // taking P1 and P2 down together and leaving the cascade on P3 alone. Both rows take the REX byte as a
    // per-nibble token (`4?`) instead, which keeps the one nibble the rotation cannot touch and wildcards the bit
    // that encodes extended-vs-legacy. The `8B` opcodes, the four-move shape and the `08` displacement of the [a1+8]
    // load carry the uniqueness. A rotation never changes instruction length, so the -0x22 and -0x32 walk-backs
    // stay valid across one. Frame and function-size immediates stay wildcarded per section 2.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_batchEquipCandidates[] = {
        // P1 -- full prologue: save rbx, push 7 callee-saves, lea rbp, mov eax=__chkstk size, call __chkstk, sub
        // rsp,rax, then the four-move arg shuffle. The wildcarded stack-size and function-size slots alone match
        // several unrelated prologues. Per section 2, the `48 2B E0` plus the REX/opcode skeleton of the four moves
        // restores uniqueness without a hardcoded stack frame and without pinning a register allocation.
        {"BatchEquip_P1_FullPrologue",
         "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
         "E8 ?? ?? ?? ?? 48 2B E0 4? 8B ?? 4? 8B ?? 4? 8B ?? 4? 8B ?? 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- arg shuffle (sub rsp,rax; mov r?,r8; mov r?,rdx; mov r?,rcx; mov r?,[rcx+8]) plus the first on-stack
        // scratch descriptor (lea rax,[rbp+X]; mov [rbp+Y],rax; xor ecx,ecx). Stack disp8 offsets are wildcarded per
        // section 2. Offset -0x22 backs up to function start.
        //
        // A push-frame-only candidate here is non-unique once its stack disp32 is wildcarded. P1 already covers the
        // push-frame region.
        {"BatchEquip_P2_PostAlloca",
         "48 2B E0 4? 8B ?? 4? 8B ?? 4? 8B ?? 4? 8B ?? 08 "
         "48 8D 45 ?? 48 89 45 ?? 33 C9",
         ResolveMode::Direct, -0x22, 0},

        // P3 -- deepest fallback: the two on-stack scratch descriptors the function builds before it reads any
        // argument. Shape per descriptor: lea rax,[rbp+X]; mov [rbp+Y],rax (buffer pointer); mov [rbp+Z],ecx (count,
        // zeroed by the shared xor ecx,ecx); mov dword [rbp+W],imm (capacity). Both capacity immediates are SEMANTIC
        // and stay literal per the section 2 exception; every stack displacement is wildcarded. This row survives an
        // arg-register rotation and a REX change in the shuffle, which is the known failure mode of P1 and P2. Offset
        // -0x32 backs up to function start.
        {"BatchEquip_P3_BodyScratchInit",
         "48 8D 45 ?? 48 89 45 ?? 33 C9 89 4D ?? C7 45 ?? 02 00 00 00 "
         "48 8D 85 ?? ?? ?? ?? 48 89 85 ?? ?? ?? ?? 89 8D ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 14 00 00 00",
         ResolveMode::Direct, -0x32, 0},
    };

    // -----------------------------------------------------------------------
    // ClientActorManagerGlobal -- module-static slot holding the published pa::ClientActorManager* singleton. Source of
    // truth for the entire controlled-character resolver chain:
    //
    //   [global] -> mgr (pa::ClientActorManager)
    //   mgr  +0x58 -> userActor (pa::ClientUserActor)
    //   user +0x08 -> subMgr
    //   sub  +0x30 -> Kliff CCOIA (always present)
    //   sub  +0x38 -> currently-controlled CCOIA
    //
    // A game update that re-lays-out pa::ClientActorManager moves the userActor field (mgr+0x58) and the CCOIA
    // actor-array descriptor (mgr+0x130, capacity at mgr+0x13C) together by the same delta. The userActor offset is
    // owned by CDCore::ActorChainOffsets (controlled_char.hpp). The array offsets live with the snapshot walk in
    // controlled_char.cpp.
    //
    // The module-relative offset of the slot drifts between game patches, so a hardcoded offset reads unrelated
    // `.data` on the wrong build. That failure is SILENT: a stale offset can land inside a packed string table, and
    // the dereference then yields ASCII content instead of a heap pointer. Always resolve the slot through the
    // cascade below. There are exactly TWO candidates, because the global has exactly two referencing instructions
    // in the image (see the note under P2). Both sit in the same lazy-init function and both resolve to the same
    // global slot, so this cascade is thinner than the usual three tiers and cannot be widened.
    //
    // Note that P2 crosses a short conditional jump, so a branch-encoding flip leaves only P1 standing.
    //
    // Both candidates use RipRelative mode. Each yields the absolute address of the slot whose qword is the manager
    // pointer.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_clientActorManagerGlobalCandidates[] = {
        // P1 -- publish-store + sibling sub-pointer assignments:
        //   mov [rip+disp32], reg         ; <-- publishes the manager, slot +0
        //   lea reg2, [reg+subobj]
        //   mov [rip+disp32], reg2        ; sibling slot +8
        //   lea reg2, [reg+subobj]
        //   mov [rip+disp32], reg2        ; sibling slot +16
        //   movzx eax, byte [rbp+X]
        //   mov [rip+disp32], al          ; state flag, sibling slot +24
        //
        // Nothing here pins a destination register or a sub-object offset. The REX byte of each of those three
        // instructions is kept as a per-nibble `4?` token: the high nibble is what makes the store and the leas
        // 64-bit operations at all, and dropping it would let the row match a 32-bit or non-REX encoding whose
        // operand layout puts the "disp32" bytes somewhere else entirely.
        //
        // The `lea` immediates are offsets inside the manager
        // and move whenever it is re-laid-out; per section 2 an operand the engine is free to renumber does not
        // belong in a signature body, and these do not even move with the actor-array descriptor -- they have moved
        // in the OPPOSITE direction from it, so there is no single delta to re-derive them from. The publish
        // register is likewise compiler-owned, which is why the store's REX and ModRM and the leas' base register
        // are wildcards too.
        //
        // What carries the match is the instruction skeleton: a publish store of the incoming pointer, two
        // `lea`+store pairs publishing sub-pointers into consecutive global slots, then a byte-sized state flag into
        // the fourth. No other publish block in the image emits that tail. Every wildcarded instruction keeps a
        // fixed length, so the disp32 of the publish store stays at match+3 and the instruction ends at match+7.
        // require_unique keeps the row honest if a future build duplicates the shape.
        {"ClientActorManagerGlobal_P1_PublishStore",
         "4? 89 ?? ?? ?? ?? ?? 4? 8D ?? ?? ?? ?? ?? "
         "48 89 05 ?? ?? ?? ?? 4? 8D ?? ?? ?? ?? ?? "
         "48 89 05 ?? ?? ?? ?? "
         "0F B6 85 ?? ?? ?? ?? 88 05",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- the only READ of the slot: the same lazy-init function later leas it as an outbound argument.
        //   <loop tail: add rsi,0x10 ; sub r14,1 ; jne>
        //   lea rdx, cs:[rip+slot]        ; 48 8D 15 + disp32
        //   lea rcx, [rbp+disp8]          ; 48 8D 4D + disp8
        //   call helper ; nop
        //   mov byte [rsp+disp8], 0
        //   vpxor xmm0,xmm0,xmm0
        // Both halves of the window earn their length. The post-call tail is needed because the leading
        // lea/lea/call shape alone matches many unrelated sites, and the leading loop tail is needed because the
        // lea/lea/call/nop/vpxor block itself occurs TWICE in this function. The disp32 of the `lea rdx` lives at
        // match+13 and that instruction ends at match+17.
        {"ClientActorManagerGlobal_P2_LeaCallBodyDisp8",
         "48 83 C6 10 49 83 EE 01 75 ?? "
         "48 8D 15 ?? ?? ?? ?? 48 8D 4D ?? "
         "E8 ?? ?? ?? ?? 90 C6 44 24 ?? 00 "
         "C5 F9 EF C0",
         ResolveMode::RipRelative, 13, 17},

        // There is deliberately NO third row.
        //
        // This global has exactly two referencing instructions in the whole image: the publish store P1 anchors on,
        // and the single `lea rdx` P2 anchors on. The second, byte-identical lea/lea/call/nop/vpxor block noted
        // above is tempting as a third row, but it loads a NEIGHBORING global rather than this one. A row cut from
        // it resolves cleanly, passes every plausibility check, and yields the wrong pointer. Verify the resolved
        // TARGET, not just the match count, before adding anything here.
    };

} // namespace CDCore::Anchors

#endif // CDCORE_ANCHORS_HPP
