#ifndef CDCORE_ANCHORS_HPP
#define CDCORE_ANCHORS_HPP

// ---------------------------------------------------------------------------
// Shared AOB candidate tables used by multiple Crimson Desert mods.
//
// Naming convention (unified across mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
//     RoleName         = semantic role of the resolved address
//                        (WorldSystem, MapLookup, VisualEquipChange, ...)
//     P<N>             = cascade tier (P1 = tightest / most-specific;
//                        higher N = wider / deeper fallback)
//     AnchorDescriptor = what the candidate anchors on
//                        (FullPrologue, PostAlloca, BodyAnchor, ...)
//
// Every candidate here has been verified to return exactly one match in the
// live v1.03.01 .text section via `mcp__cheatengine__aob_scan` (see the
// uniqueness audit in the session log).
//
// Authoring rules (from external/DetourModKit/docs/misc/aob-signatures.md):
//   - Sign CODE, not DATA. Anchor on instruction semantics, not on linker
//     outputs.
//   - Wildcard every immediate operand, RIP-relative disp32, rel8/rel32
//     jump target, struct offset the compiler can renumber.
//   - Keep signatures as short as will return a unique hit (7-16 bytes is
//     the common sweet spot; 12-32 bytes when disambiguation needs it).
//   - Never anchor on a short `Jcc rel8` -- compilers flip freely between
//     `74 xx` and `0F 84 xx xx xx xx` across patches.
//   - Each `resolve_address()` returns (match_address + dispOffset). For
//     Direct mode a negative dispOffset walks backward to land on the
//     function start when the anchor sits deeper in the body.
//
// Consumers expose these arrays inside their own mod namespace via a
// reference alias:
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
    // Resolved via a small `48 83 EC 28 ... mov rcx, [rip+disp32]` getter that
    // the mod chases to obtain the player actor component. RipRelative mode:
    // the disp32 is read at match+7 and added to (match + 11).
    //
    // Walk (runtime-data):
    //   *(wsPtr) -> *(+0x30) -> *(+0x28) -> *(+0xD0) = actor
    //   actor    -> *(+104)  -> *(+56)             = component
    //
    // 3-tier cascade. P2_StructField is a structurally different sibling
    // site that pulls the same pointer via a different prologue shape.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_worldSystemCandidates[] = {
        // P1 -- whole-function anchor for the small getter (the tightest
        // signature we can build: it pins every byte of the function
        // prologue AND body).
        {"WorldSystem_P1_SmallFunc",
         "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3",
         ResolveMode::RipRelative, 7, 11},

        // P2 -- alternative sibling site:
        //   cmp byte [rax+disp32], 0
        //   <2-byte branch: jne rel8 or first 2 bytes of jne rel32>
        //   mov rax, [rip+disp32]    <- the one we resolve
        //   mov rcx, [rax+0xD8]      <- game-ABI disambiguator
        //
        // The 2-byte branch slot is wildcarded rather than hard-coded as
        // `75 ??`: per aob-signatures.md §8 the compiler may flip a short
        // Jcc to the 6-byte `0F 85 rel32` form, which would change the
        // opcode byte. Wildcarding both bytes tolerates the 2-byte shape
        // regardless of opcode, but the pattern still fails on a 6-byte
        // flip -- in that case WS_P1 / WS_P3 take over (they don't cross
        // a branch). The trailing `48 8B 88 D8 00 00 00` pins the
        // specific WorldSystem follow-on (`mov rcx, [rax+0xD8]`); 0xD8
        // is a game-struct ABI offset, stable within a build.
        {"WorldSystem_P2_StructField",
         "80 B8 ?? ?? ?? ?? 00 ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 D8 00 00 00",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- shortest anchor: the `mov rcx, [rip+disp32] ; mov rcx,[rcx+X]
        // ; call ; ...` tail of the small getter, without the prologue.
        // Used when a compiler shuffle trims the `sub rsp, 28` from P1.
        {"WorldSystem_P3_InnerLoad",
         "48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    // -----------------------------------------------------------------------
    // MapLookup -- IndexedStringA global-table lookup routine.
    //
    // Not hooked. Its address is used as the RIP anchor for the
    // `mov rax, [rip+disp32]` at offset +20 which points at the
    // IndexedStringA global. Both mods walk that global to build their
    // CD_*/part-name tables.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_mapLookupCandidates[] = {
        // P1 -- full function prologue + first body instruction. The
        // `83 79 04 00` (cmp [rcx+4], 0) check is distinctive. The
        // 2-byte early-out branch slot is wildcarded (see §8 branch-
        // encoding note in the WorldSystem P2 comment above).
        {"MapLookup_P1_FullPrologue",
         "48 83 EC 08 83 79 04 00 4C 8B C1 ?? ?? 33 C0 48 83 C4 08 C3 48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A",
         ResolveMode::Direct, 0, 0},

        // P2 -- hash-body anchor (deeper in the function). Re-anchors when
        // the prologue layout changes. Offset -0x24 walks back to func
        // start. The 2-byte jz on zero-count is wildcarded (same branch-
        // encoding caveat as above).
        {"MapLookup_P2_HashBody",
         "8B 48 ?? 48 03 D2 44 8B 5C D1 ?? 41 8B 08 85 C9 ?? ?? 33 D2 41 8B C3 F7 F1",
         ResolveMode::Direct, -0x24, 0},

        // P3 -- hash-loop anchor (even deeper). Last-resort fallback; offset
        // -0x3D walks back to func start.
        {"MapLookup_P3_HashLoop",
         "44 8B CA 33 D2 49 C1 E1 08 4D 03 48 ?? 45 8B 11 45 85 D2",
         ResolveMode::Direct, -0x3D, 0},
    };

    // -----------------------------------------------------------------------
    // PartAddShow (sub_14081DC20) -- PartInOut direct-show bypass.
    //
    // Both mods hook this function to suppress stale real-part frames that
    // flash through during state transitions (glide exit, landings, effect
    // spawns) where the game bypasses the PartInOutSocket vis=2 mask.
    //
    // Signature (x64 __fastcall):
    //   __int64 sub_14081DC20(
    //       __int64 a1,           // RCX  descriptor context
    //       char    a2,           // DL   transition flag
    //       uint64_t partHashPtr, // R8   pointer to DWORD part hash
    //       float   blend,        // XMM3 animation blend
    //       __int64 a5..a9)       // stack params
    //
    // History:
    //   2026-04-19 P1 tightened after CE aob_scan found a second hit in a
    //   Windows module (kernel DLL). Appended the P2 discriminator
    //   (`4D 8B E8 44 8B 49 ??`) so the pattern uniquely selects the game.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_partAddShowCandidates[] = {
        // P1 -- full prologue through the r13/r9d setup. Extended 2026-04-19
        // to include post-alloca moves because the 14-byte prologue alone
        // also matched a Windows DLL function with a different body.
        {"PartAddShow_P1_FullPrologue",
         "40 55 56 57 41 55 48 83 EC ?? 48 8B 79 ?? 4D 8B E8 44 8B 49 ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue anchor (sub rsp / mov rdi,[rcx+X] / mov r13,r8
        // / mov r9d,[rcx+X]). Offset -6 backs up to function start.
        {"PartAddShow_P2_PostPrologue",
         "48 83 EC ?? 48 8B 79 ?? 4D 8B E8 44 8B 49 ??",
         ResolveMode::Direct, -6, 0},

        // P3 -- SIMD-save / shift anchor deeper in the body. Offset -0x1B
        // backs up to function start. Anchors on `movaps [rsp+X],xmm6 ;
        // shl rax,04 ; movaps xmm6,xmm3 ; add rax,rdi` which is
        // structurally rare.
        {"PartAddShow_P3_SimdBody",
         "0F 29 74 24 ?? 48 C1 E0 04 0F 28 F3 48 03 C7",
         ResolveMode::Direct, -0x1B, 0},
    };

    // -----------------------------------------------------------------------
    // VisualEquipChange (sub_14076D520) -- bottleneck for all visual equipment
    // changes (equip and unequip). Called from the network handler for
    // TrocTrAddVisualEquipItemAck.
    //
    // Signature (x64 __fastcall):
    //   __int64 sub_14076D520(
    //       __int64 bodyComp,    // RCX  ClientFrameEventActorComponent*
    //       int16_t slotId,      // DX   equipment slot
    //       int16_t itemId,      // R8W  new item (0xFFFF = removing)
    //       __int64 itemData)    // R9   item data pointer
    //
    // 4-tier cascade: P1 is the shared prologue (both mods had identical
    // P1s). P2 is a "push-frame" anchor unique to the function's
    // specific prologue shape. P3 is the post-alloca register-shuffle
    // anchor. P4 is a deeper body anchor past the `lea rcx,[rbp+X]`.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_visualEquipChangeCandidates[] = {
        // P1 -- full prologue from `mov [rsp+0x10], rbx` through the
        // `B8 ?? ?? ?? ??` (mov eax, imm32 = __chkstk function-size
        // marker). Stack frame size and function-size hint are
        // wildcarded -- both are compiler-owned and drift between
        // builds (§2).
        {"VisualEquipChange_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
         "55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- push-frame anchor (pushes + lea + mov eax,imm + __chkstk
        // call + sub rsp,rax + first two post-alloca register moves).
        // Extended 2026-04-19: the wildcarded stack-size and function-
        // size slots alone matched 5 unrelated prologues; adding the
        // `48 2B E0 49 8B F1 41 0F B7 D8` tail (specific VEC register
        // shuffle through `movzx ebx, r8w`) restores uniqueness without
        // re-introducing a hardcoded stack frame. Offset -0x10 backs up
        // to function start.
        {"VisualEquipChange_P2_PushFrame",
         "55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
         "E8 ?? ?? ?? ?? 48 2B E0 49 8B F1 41 0F B7 D8",
         ResolveMode::Direct, -0x10, 0},

        // P3 -- post-alloca register shuffle (sub rsp,rax; mov rsi,r9;
        // movzx ebx,r8w; movzx edi,dx; mov r14,rcx) + deeper
        // `lea rcx, [rbp+disp32]` + `E8` call. Stack disp32 wildcarded
        // per §2. Offset -0x2A backs up to function start.
        {"VisualEquipChange_P3_PostAlloca",
         "48 2B E0 49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D ?? ?? ?? ?? E8",
         ResolveMode::Direct, -0x2A, 0},

        // P4 -- deepest fallback: same post-alloca shuffle without the
        // leading `sub rsp,rax`, anchored 3 bytes deeper (mov rsi,r9).
        // Stack disp32 in the `lea rcx,[rbp+disp32]` wildcarded. Offset
        // -0x2D backs up to function start.
        {"VisualEquipChange_P4_PreLeaBody",
         "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D ?? ?? ?? ??",
         ResolveMode::Direct, -0x2D, 0},
    };

    // -----------------------------------------------------------------------
    // BatchEquip (sub_14075BBF0) -- the function that fires when the player
    // equips armor. Reads the entry table at a1+120, builds 216-byte
    // dispatch entries, and calls EquipChangeDispatch (sub_14075C420) to
    // load meshes. Also the bottleneck for item-to-item visual swaps
    // (bypasses VisualEquipChange) -- EquipHide called this role
    // "VisualEquipSwap" historically; "BatchEquip" is the newer / clearer
    // name used here.
    //
    // Signature (x64 __fastcall):
    //   _DWORD* sub_14075BBF0(
    //       _QWORD*   a1,
    //       _DWORD*   a2,
    //       __int64** a3_old,
    //       __int64** a4_new)
    //
    // 4-tier cascade: P1 = full prologue, P2 = push frame only, P3 = post-
    // alloca register shuffle, P4 = deepest body anchor past the on-stack
    // scratch zero-init.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_batchEquipCandidates[] = {
        // P1 -- full prologue: save rbx, push 7 callee-saves, lea rbp,
        // mov eax=__chkstk size, call __chkstk, sub rsp,rax, first 3
        // post-alloca register moves. Extended 2026-04-19 into the
        // body shuffle -- the wildcarded stack-size and function-size
        // slots alone matched 5 unrelated prologues; the
        // `48 2B E0 4D 8B F8 4C 8B E2 48 8B F1` tail restores
        // uniqueness without a hardcoded stack frame (§2).
        {"BatchEquip_P1_FullPrologue",
         "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? "
         "E8 ?? ?? ?? ?? 48 2B E0 4D 8B F8 4C 8B E2 48 8B F1",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca register shuffle (sub rsp,rax; mov r15,r8;
        // mov r12,rdx; mov rsi,rcx; mov r14,[rcx+8]) + the on-stack
        // scratch prep (lea rax,[rbp+X]; mov [rbp+Y],rax; xor ecx,ecx).
        // Stack disp8 offsets wildcarded per §2. Offset -0x22 backs up
        // to function start.
        //
        // (The old "BatchEquip_P2_PushFrame" was dropped 2026-04-19:
        //  wildcarding its stack-frame disp32 reduced it to 3 non-
        //  unique hits, and any extension into the body duplicated P2
        //  above. P1 already covers the push-frame region.)
        {"BatchEquip_P2_PostAlloca",
         "48 2B E0 4D 8B F8 4C 8B E2 48 8B F1 4C 8B 71 08 "
         "48 8D 45 ?? 48 89 45 ?? 33 C9",
         ResolveMode::Direct, -0x22, 0},

        // P3 -- deepest fallback: starts from `mov r15,r8` (after the
        // sub rsp,rax) and extends past the on-stack scratch zero-init
        // (mov [rbp+X],ecx; mov dword [rbp+Y], 2). Stack disp8 offsets
        // wildcarded; the `02 00 00 00` tail is SEMANTIC (the value-2
        // init flag), kept literal per §2 exception for semantic
        // constants. Offset -0x25 backs up to function start.
        {"BatchEquip_P3_BodyZeroInit",
         "4D 8B F8 4C 8B E2 48 8B F1 4C 8B 71 08 "
         "48 8D 45 ?? 48 89 45 ?? 33 C9 89 4D ?? C7 45 ?? 02 00 00 00",
         ResolveMode::Direct, -0x25, 0},
    };

    // -----------------------------------------------------------------------
    // Player -- static pointer for the server-side party-state root global.
    //
    // Resolves to qword_145F0D580 in the v1.04.00 .text. The dereference of
    // that qword is a heap-allocated root object (no specific RTTI) whose
    // chain is:
    //
    //   *(player_static)        -> root container
    //   *(root  + 0x18)         -> pa::NwVirtualAsyncSession
    //   *(nwSes + 0xA0)         -> pa::ServerUserActor
    //   *(srvUA + 0xD0)         -> pa::ServerChildOnlyInGameActor
    //                              (the party container)
    //
    // Inside the party container the three protagonist slots are inline
    // structs at fixed offsets with stride 0x100:
    //
    //   party + 0x68            = Kliff slot
    //   party + 0x168           = Damiane slot
    //   party + 0x268           = Oongka slot
    //
    // Per slot the active-flag byte is at slot+0x2C (1 = currently
    // controlled, 0 = passive). The character is identified by which slot
    // offset, NEVER by a stored character ID. The +0x40 dword is an
    // observability handle and is volatile; it is logged for diagnostics
    // but not used for identity.
    //
    // The CE-validated state-transition table (5 transitions across radial
    // swaps and a save-load) confirmed the invariant that exactly one slot
    // has +0x2C == 1 while the player is in-world; zero slots are active
    // during cutscenes / loading screens. The top three pointers in the
    // chain (root, NwVirtualAsyncSession, ServerUserActor) stay identical
    // across save-load; only the final party container reallocates.
    //
    // Why this static and not the older WorldSystem holder at +0x5F0D210:
    // the WorldSystem chain reaches a CLIENT-side ChildOnlyInGameActor pool
    // whose per-actor +0x50 byte was the prior identity discriminator. On
    // v1.04.00 that byte ceased to discriminate (every protagonist reads
    // 0x08), and actor-pool reuse made the field unstable across radial
    // swaps. The Player static reaches the SERVER-side party state, which
    // identifies the controlled character by slot offset (a structural
    // invariant) instead of by a value the engine can repurpose.
    //
    // 3-tier cascade. Stable read sites verified unique on v1.04.00 via
    // mcp__cheatengine__aob_scan (each returns exactly 1 hit).
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_playerStaticCandidates[] = {
        // P1 -- the unique writer site at sub_1420D0470.
        //   4C 89 3D <disp32>          mov [rip+disp32], r15
        //   48 8B 8F F8 00 00 00       mov rcx, [rdi+0xF8]
        //   41 BC 04 02 00 00          mov r12d, 0x204
        //   48 85 C9 ??                test rcx, rcx ; jcc rel8
        //
        // Writer encodings on globals are typically rare (one or two per
        // module) and the immediately-following load from [rdi+0xF8] +
        // mov-r12d-0x204 sequence is semantically distinctive. The 0x204
        // is a SEMANTIC constant (an initialisation tag) and is kept
        // literal per the §2 exception; the rel8 of the trailing jcc is
        // wildcarded because branch distance can shift.
        {"PlayerStatic_P1_WriterSite",
         "4C 89 3D ?? ?? ?? ?? 48 8B 8F F8 00 00 00 "
         "41 BC 04 02 00 00 48 85 C9 ??",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- read-into-r12 at sub_1420D3700+0x39.
        //   4C 8B 25 <disp32>          mov r12, [rip+disp32]
        //   8B D5                      mov edx, ebp
        //   4C 8B F0                   mov r14, rax
        //   E8 <disp32>                call <subroutine>
        //   48 8B D8                   mov rbx, rax
        //   48 85 C0 ??                test rax, rax ; jcc rel8
        //
        // The 4C 8B 25 prefix (load into r12) is uncommon; combined with
        // the immediate mov-edx-ebp / mov-r14-rax / call / mov-rbx-rax /
        // test-rax-rax tail it pins this single read site. Both rel32
        // displacements are wildcarded (linker-owned).
        {"PlayerStatic_P2_ReadIntoR12",
         "4C 8B 25 ?? ?? ?? ?? 8B D5 4C 8B F0 "
         "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 ??",
         ResolveMode::RipRelative, 3, 7},

        // P3 -- read-into-rcx + stack-slot test at sub_140819600+0x14F.
        //   48 8B 0D <disp32>          mov rcx, [rip+disp32]
        //   E8 <disp32>                call <subroutine>
        //   83 7C 24 ?? 00             cmp dword [rsp+disp8], 0
        //   74 ??                      jz rel8
        //   48 8B 8E E0 00 00 00       mov rcx, [rsi+0xE0]
        //
        // The 48 8B 0D / E8 mov-then-call shape is common, but the tail
        // (cmp dword [rsp+disp8], 0; jz rel8; mov rcx, [rsi+0xE0]) pins
        // this site. 0xE0 is a game-struct ABI offset, kept literal. The
        // stack disp8 in the cmp and the rel8 jz target are wildcarded.
        {"PlayerStatic_P3_ReadStackTest",
         "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
         "83 7C 24 ?? 00 74 ?? 48 8B 8E E0 00 00 00",
         ResolveMode::RipRelative, 3, 7},
    };

    // -----------------------------------------------------------------------
    // RadialSwapKey -- mid-body anchor inside the radial-UI character-swap
    // handler (sub_1422019A0). At this site EAX has just been loaded with
    // the requested characterinfo key from the radial input pointer in RSI:
    //
    //   ; sub_1422019A0 fragment, loads the destination key and forwards to
    //   ; sub_141B0C2B0 which commits the new actor to User+0xD8.
    //   8B 06              mov eax, [rsi]            ; ANCHOR
    //   89 45 78           mov [rbp+0x78], eax       ; HOOK +2 lands here
    //   48 8B 0D ?? ?? ?? ?? mov rcx, [rip+disp32]   ; lookup table base
    //   48 83 C1 60        add rcx, 60h
    //   48 8D 55 78        lea rdx, [rbp+0x78]
    //   E8 ?? ?? ?? ??     call sub_1402FB1C0        ; hash table lookup
    //   B9 FF FF 00 00     mov ecx, 0FFFFh           ; sentinel
    //   48 85 C0           test rax, rax
    //   74 05              jz <skip>
    //   0F B7 18           movzx ebx, word ptr [rax]
    //   EB 02              jmp +2
    //
    // CE-verified live (v1.04.00, 2026-05-02) across four protagonist
    // transitions: EAX = 0x01 (Kliff), 0x04 (Damiane), 0x06 (Oongka).
    //
    // The hook offset is +2 (the byte length of `mov eax, [rsi]`) so the
    // mid-hook lands on the immediately-following `mov [rbp+0x78], eax`,
    // i.e. the instruction at which the captured key is fully realised in
    // the EAX register and not yet shadowed by the lookup-table call. EAX
    // is the source of the captured value; reading it from the SafetyHook
    // Context64 as `ctx.rax & 0xFFFF'FFFFu` gives the raw key.
    //
    // Coverage caveat: scripted cutscene auto-switches that drive the
    // player back to Kliff do NOT pass through this handler (verified
    // live: zero hits on a forced Kliff return). The +0x2C slot decode
    // and the resolver's last-known-good cache cover those paths.
    //
    // 5-tier cascade ordered for cross-mod coordination.
    //
    // Cross-mod model: CrimsonDesertCore is a STATIC library, so each Logic
    // DLL (LiveTransmog, EquipHide) carries its own copy of CDCore state and
    // installs its own SafetyHook MidHook against this site. SafetyHook's
    // internal chaining at the JMP-target site lets two MidHooks coexist --
    // each consumer's trampoline runs independently on every radial swap.
    // The price is that whichever consumer initialises FIRST overwrites the
    // hook target's prelude bytes with the SafetyHook stub (a 5-byte JMP at
    // match+2 plus zero or more NOP-padded partial-instruction bytes); the
    // sibling consumer that scans SECOND no longer sees the original
    // `89 45 78 48 8B 0D ...` shape at offset +2 through +11 of the match.
    //
    // Patch survival:
    //   - SafetyHook's MidHook overwrites COMPLETE instructions starting at
    //     the hook target. Bytes 0..1 of the match (`8B 06`, the leading
    //     mov-eax-from-rsi) are LEFT INTACT because the hook target is at
    //     match+2.
    //   - Bytes match+2 through match+11 (the `89 45 78 48 8B 0D ?? ?? ?? ??`
    //     window -- 3 bytes of `mov [rbp+0x78], eax` plus 7 bytes of
    //     `mov rcx, [rip+disp32]`) are FULLY OVERWRITTEN by the JMP+pad.
    //   - Bytes match+12 onward (`48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? B9
    //     FF FF 00 00 48 85 C0 74 05 0F B7 18 EB 02`) are UNCHANGED -- they
    //     sit past the displaced range and are reached only via the
    //     SafetyHook trampoline's epilogue.
    //
    // P1 / P2 anchor exclusively in the post-patch-invariant tail
    // (match+12 onward) so they match the live binary regardless of whether
    // a sibling has already patched the prelude. dispOffset = -10 walks back
    // from the new anchor's start (which is at original-match+12) to the
    // hook target (at original-match+2): -10 = +2 - 12. P3 / P4 / P5 are
    // the original prelude-anchored candidates retained for the
    // pre-any-patch first-load case where every byte still matches; on a
    // post-patched site they fail gracefully and the cascade falls through
    // to P1 / P2.
    //
    // resolve_cascade iterates declaration order and returns first match
    // (see scanner.cpp scan_candidates), so the post-patch-tolerant
    // candidates MUST come first to guarantee the second consumer resolves
    // the same hook target the first consumer already patched.
    //
    // The `0F B7 18 EB 02` (movzx ebx, word ptr [rax]; jmp +2) tail is the
    // unique disambiguator: a structurally identical shape at sub_1421D5990
    // in a different handler is followed by an index-write
    // (`48 89 B5 28 03 00 00`) instead, so any candidate that includes
    // bytes past the call locks onto this site exclusively.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_radialSwapKeyCandidates[] = {
        // P1 -- post-patch-invariant tail with literal rel8 jz. Anchors
        // entirely in the bytes past the SafetyHook displaced window
        // (match+12 onward of the original prelude shape):
        //   add rcx, 0x60; lea rdx,[rbp+0x78]; call <disp32>;
        //   mov ecx, 0xFFFF; test rax,rax; jz +5; movzx ebx,[rax];
        //   jmp +2.
        // The unique movzx-ebx + jmp-+2 tail pins this site (see comment
        // block above for the sibling-handler disambiguator). dispOffset
        // -10 walks back to the original hook target at match+2 of the
        // prelude shape (= start-of-this-anchor + 2 - 12).
        {"RadialSwapKey_P1_PostPatchFull",
         "48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? "
         "B9 FF FF 00 00 48 85 C0 74 05 0F B7 18 EB 02",
         ResolveMode::Direct, -10, 0},

        // P2 -- same post-patch-invariant tail with the rel8 jz target
        // wildcarded. Survives a recompile that shifts the branch
        // distance while the surrounding instruction stream is preserved.
        // Falls back to P1 only when an exact-byte match wins first; same
        // dispOffset semantics.
        {"RadialSwapKey_P2_PostPatchWildcardRel8",
         "48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? "
         "B9 FF FF 00 00 48 85 C0 74 ?? 0F B7 18 EB 02",
         ResolveMode::Direct, -10, 0},

        // P3 -- pre-patch full-prelude anchor (was P1). Matches only on a
        // first-load scan against an unpatched site. Anchors on the full
        // prelude (mov eax,[rsi]; mov [rbp+0x78],eax; mov rcx,[rip+disp32];
        // add rcx,0x60; lea rdx,[rbp+0x78]; call <disp32>) followed by the
        // unique tail (mov ecx,0xFFFF; test rax,rax; jz +5; movzx ebx,[rax];
        // jmp +2). dispOffset +2 walks forward to the hook target. After
        // a sibling has patched, bytes match+2..+11 differ, so this
        // candidate fails gracefully and the cascade falls through to P1.
        {"RadialSwapKey_P3_FullSequence",
         "8B 06 89 45 78 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? "
         "B9 FF FF 00 00 48 85 C0 74 05 0F B7 18 EB 02",
         ResolveMode::Direct, 2, 0},

        // P4 -- pre-patch prelude with wildcarded rel8 jz target (was P2).
        // Same first-load-only matching characteristic as P3; tolerates a
        // future build that shifts the jz branch distance.
        {"RadialSwapKey_P4_WildcardRel8",
         "8B 06 89 45 78 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? "
         "B9 FF FF 00 00 48 85 C0 74 ?? 0F B7 18 EB 02",
         ResolveMode::Direct, 2, 0},

        // P5 -- minimal-tail prelude anchor (was P3). Drops the post-call
        // test/movzx tail and ends at the `mov ecx, 0xFFFF` sentinel-load.
        // Verified unique on v1.04.00. Last-resort first-load fallback if
        // both P3 and P4 fail to a future tail rewrite. Also fails on a
        // post-patched site (the prelude bytes are gone), at which point
        // P1 / P2 carry the resolution.
        {"RadialSwapKey_P5_MinimalTail",
         "8B 06 89 45 78 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 60 48 8D 55 78 E8 ?? ?? ?? ?? "
         "B9 FF FF 00 00",
         ResolveMode::Direct, 2, 0},
    };

} // namespace CDCore::Anchors

#endif // CDCORE_ANCHORS_HPP
