#pragma once

#include <DetourModKit.hpp>

#include <cstdint>
#include <vector>

namespace Transmog
{
    enum class ResolveMode : uint8_t
    {
        Direct,
        RipRelative
    };

    struct AddrCandidate
    {
        const char *name;
        const char *pattern;
        ResolveMode mode;
        ptrdiff_t dispOffset;
        ptrdiff_t instrEndOffset;
    };

    struct AobCandidate
    {
        const char *name;
        const char *pattern;
        ptrdiff_t offsetToHook;
    };

    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label);

    uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource);

    /**
     * @brief First-byte prologue sanity check for a callable function.
     *
     * Reads up to 1 byte from @p addr under SEH guard. Uses a
     * **blacklist** approach (reject known poison, accept everything
     * else) so the gate is patch-proof: future game updates that
     * introduce new legal prologue shapes won't trigger false
     * rejections.
     *
     * Rejected bytes (treated as scan-corruption signals):
     *   0x00       — uninitialized memory / zero-fill BSS / NULL page
     *   0xCC       — int3 breakpoint / alignment pad / debugger trap
     *   0xC2       — `ret imm16` (bare-ret stub, not a callable body)
     *   0xC3       — `ret` (bare-ret stub, not a callable body)
     *
     * Accepted: everything else. This covers every legal x86-64
     * instruction byte that could begin a function, including all
     * REX prefixes (0x40-0x4F), push/pop row (0x50-0x5F), mov/lea/
     * sub/xor/test/cmp opcodes, 0x0F two-byte prefix, 0x66
     * operand-size prefix, 0xE8/0xE9/0xEB jmp/call, etc.
     *
     * Use this to gate every site that stores an AOB-resolved address
     * into a callable function pointer so a false-positive scan
     * cannot turn into a wild branch.
     *
     * History:
     *   2026-04-11 initial: narrow whitelist (0x40, 0x41, 0x48, 0x49,
     *     0x53, 0x55, 0x56, 0x57, 0xE9). Missed 0x4C/0x4D (REX.WR)
     *     and falsely rejected SlotPopulator (`0x14076C960`) because
     *     its real prologue starts with `4C 89 44 24 18` (mov
     *     [rsp+18h], r8). User saw the log line
     *     "SlotPopulator resolved to 0x14076C960 but prologue byte
     *     looks wrong — rejecting, transmog apply disabled".
     *   2026-04-11 fix: flipped to blacklist approach so any future
     *     game patch that adds a new prologue shape we didn't
     *     predict is still accepted.
     */
    [[nodiscard]] bool sanity_check_function_prologue(uintptr_t addr) noexcept;

    // --- AOB candidate tables ---

    /**
     * @brief sub_14075FE60 — safe scene-graph tear-down.
     *
     * Takes (a1, hashDWORD, slotTag). Walks the scene graph via
     * sub_1425EBAE0 to retire the matched part without mutating the
     * authoritative equip table at *(a1+0x78). Used by the two-phase
     * transmog apply in real_part_tear_down.
     *
     * All 3 candidates verified unique via IDA Pro `find_bytes` against
     * v1.02.00. The 33-byte prologue alone is NOT unique (2 matches —
     * collides with sub_140BD7270), so P1 extends it to 36 bytes.
     * P2 and P3 anchor deeper in the body to survive prologue drift.
     */
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        {"STD_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
         "48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        {"STD_P2_PostAlloca",
         "48 81 EC B0 01 00 00 41 0F B7 F8 48 8B F1",
         ResolveMode::Direct, -0x1A, 0},

        {"STD_P3_GlobalPtrCall",
         "48 83 C1 60 48 8D 95 F8 00 00 00 E8 ?? ?? ?? ?? "
         "48 85 C0 0F 84 02 02 00 00 0F B7 00 66 89 85 E0 00 00 00",
         ResolveMode::Direct, -0x4A, 0},
    };

    /**
     * @brief IndexedStringA map-lookup routine. Not hooked — its address is
     *        used purely as the RIP anchor for the `mov rax, [rip+disp]` at
     *        offset +20 which points at the IndexedStringA global pointer.
     *        Ported verbatim from CrimsonDesertEquipHide/aob_resolver.hpp.
     */
    inline constexpr AddrCandidate k_mapLookupCandidates[] = {
        {"ML_P1_FullPrologue",
         "48 83 EC 08 83 79 04 00 4C 8B C1 75 ?? 33 C0 48 83 C4 08 C3 48 8B 05 ?? ?? ?? ?? 48 89 1C 24 8B 1A",
         ResolveMode::Direct, 0, 0},

        {"ML_P2_HashBody",
         "8B 48 ?? 48 03 D2 44 8B 5C D1 ?? 41 8B 08 85 C9 74 ?? 33 D2 41 8B C3 F7 F1",
         ResolveMode::Direct, -0x24, 0},

        {"ML_P3_HashLoop",
         "44 8B CA 33 D2 49 C1 E1 08 4D 03 48 ?? 45 8B 11 45 85 D2",
         ResolveMode::Direct, -0x3D, 0},
    };

    /**
     * @brief Hook target: sub_14076D520 — VisualEquipChange.
     * Catches unequip events to re-apply transmog.
     */
    inline constexpr AddrCandidate k_vecCandidates[] = {
        {"VEC_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
         "55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        // P2: anchor after __chkstk unwind and into the initial body moves
        // (sub rsp, rax / mov rsi, r9 / movzx ebx, r8w / movzx edi, dx /
        //  mov r14, rcx / lea rcx, [rbp+1F0h]). IDA-verified unique 1-hit at
        // 0x14076D54A (offset 0x2A from the function start).
        {"VEC_P2_PostChkstk",
         "48 2B E0 49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D F0 01 00 00 E8",
         ResolveMode::Direct, -0x2A, 0},

        // P3: deeper body anchor just after the sub rsp, rax. IDA-verified
        // unique 1-hit at 0x14076D54D (offset 0x2D from the function start).
        {"VEC_P3_PreLeaBody",
         "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 "
         "48 8D 8D F0 01 00 00",
         ResolveMode::Direct, -0x2D, 0},
    };

    /**
     * @brief Static pointer: WorldSystem global.
     * Used to resolve the player actor component on game load via:
     *   *(wsPtr) -> *(+0x30) -> *(+0x28) -> *(+0xD0) = actor
     *   actor -> *(+104) -> *(+56) = component
     */
    inline constexpr AddrCandidate k_worldSystemCandidates[] = {
        {"WS_P1_SmallFunc",
         "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3",
         ResolveMode::RipRelative, 7, 11},

        {"WS_P2_InnerLoad",
         "48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    /**
     * @brief Address anchor: sub_14076D950 — SlotPopulator item-id translator.
     *
     * Not hooked. Used as the first hop of the resolution chain that walks
     * to `qword_145CEF370` (the iteminfo global pointer) so we can build
     * the stable item-name table at init. See item_name_table.cpp for the
     * full 4-step chain.
     *
     * Prologue is unique — verified 1-hit against v1.02.00.
     */
    inline constexpr AddrCandidate k_subTranslatorCandidates[] = {
        {"ST_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 B9 48 81 EC 00 01 00 00 "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 6F 48 8D 4D 87",
         ResolveMode::Direct, 0, 0},

        // P2: anchors post-alloca at the scratch-buffer preparation
        // (mov rdi, rcx / mov r8d, 1 / lea rdx, [rbp-91h] / lea rcx, [rbp-79h]
        //  / call ...). IDA-verified unique 1-hit at 0x14076D969 (offset
        // 0x19 from the function start).
        {"ST_P2_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 6F 48 8D 4D 87 E8",
         ResolveMode::Direct, -0x19, 0},

        // P3: deeper anchor right after the mov rdi, rcx, without the rdi
        // move. IDA-verified unique 1-hit at 0x14076D96C (offset 0x1C from
        // the function start).
        {"ST_P3_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 6F 48 8D 4D 87 E8",
         ResolveMode::Direct, -0x1C, 0},
    };

    /**
     * @brief Helper: sub_141D451B0 — InitSwapEntry.
     *
     * Initializes a 0x80-byte swap entry structure to default sentinel
     * values (all -1 / zeros). Called by the mod immediately before each
     * SlotPopulator invocation to zero the third argument (the swap entry
     * scratch struct on stack).
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_141D451B0(__int64 dest)
     *
     * Previously bound via a hardcoded module-base RVA (`gameBase +
     * 0x1D451B0`). Converted to AOB resolution 2026-04-11 to survive any
     * code drift in earlier .text sections across future game patches.
     *
     * All 3 candidates verified unique via IDA Pro `find_bytes` against
     * v1.02.00.
     */
    inline constexpr AddrCandidate k_initSwapEntryCandidates[] = {
        // P1: true prologue. mov [rsp+8], rcx / push rbx / sub rsp, 20h /
        // mov rbx, rcx / mov rax, -1 / mov [rcx], rax / mov ecx, 0FFFFh.
        // Unique 1-hit at 0x141D451B0 (function start).
        {"ISE_P1_FullPrologue",
         "48 89 4C 24 08 53 48 83 EC 20 48 8B D9 "
         "48 C7 C0 FF FF FF FF 48 89 01 B9 FF FF 00 00",
         ResolveMode::Direct, 0, 0},

        // P2: post-prologue field-init chain (mov [rbx+20h], rdx / ... /
        // lea rax, [rbx+58h]). Unique 1-hit at 0x141D451D6 (offset 0x26).
        {"ISE_P2_FieldInitChain",
         "48 89 53 20 48 89 53 28 48 89 53 30 66 89 53 38 "
         "48 89 53 40 48 89 53 48 66 89 53 50 48 8D 43 58",
         ResolveMode::Direct, -0x26, 0},

        // P3: xor edx, edx + field-init chain. Unique 1-hit at 0x141D451D4
        // (offset 0x24).
        {"ISE_P3_XorInitChain",
         "33 D2 48 89 53 20 48 89 53 28 48 89 53 30 "
         "66 89 53 38 48 89 53 40 48 89 53 48 66 89 53 50",
         ResolveMode::Direct, -0x24, 0},
    };

    /**
     * @brief Hook target: sub_14076C960 — Slot visual populator.
     *
     * Populates the slot array (a1+440) with item visual data then
     * calls VisualEquipChange. This is the function called by the server
     * equip handler to trigger a full visual equip with mesh loading.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14076C960(__int64 a1, unsigned __int16* a2_itemData, __int64 a3_swapEntry)
     *
     * a2 is a 16-byte structure:
     *   +0:  uint16 item ID
     *   +2:  byte   flag (2 = normal equip)
     *   +4:  int32  (-1)
     *   +12: uint16 secondary slot (0xFFFF to skip)
     */
    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        {"SP_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? 00 00 00 "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        {"SP_P2_PostAlloca",
         "4C 8B E2 4C 8B E9 33 FF 48 8D 0D",
         ResolveMode::Direct, -0x22, 0},
    };

    /**
     * @brief Hook target: sub_14075BBF0 — Batch equip visual processor.
     *
     * THE function that fires when the player equips armor. Reads the entry
     * table at a1+120, builds 216-byte dispatch entries, and calls
     * EquipChangeDispatch (sub_14075C420) to load meshes.
     *
     * Signature (x64 __fastcall):
     *   _DWORD* sub_14075BBF0(_QWORD* a1, _DWORD* a2, __int64** a3_old, __int64** a4_new)
     */
    inline constexpr AddrCandidate k_batchEquipCandidates[] = {
        // P1: true prologue — most stable across minor compiler shuffles.
        // mov [rsp+10h], rbx / push rbp rsi rdi r12 r13 r14 r15 /
        // lea rbp, [rsp-12D0h] / mov eax, 13D0h. IDA-verified unique
        // 1-hit at 0x14075BBF0 (offset 0x00, the function start).
        {"BE_P1_FullPrologue",
         "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 30 ED FF FF B8 D0 13 00 00",
         ResolveMode::Direct, 0, 0},

        // P2: post-alloca sequence (formerly P1). 4D 8B F8 (mov r15,r8) +
        // 4C 8B E2 (mov r12,rdx) + 48 8B F1 (mov rsi,rcx) + 4C 8B 71 08
        // (mov r14,[rcx+8]). IDA-verified unique 1-hit at 0x14075BC12
        // (offset 0x22).
        {"BE_P2_PostAlloca",
         "48 2B E0 4D 8B F8 4C 8B E2 48 8B F1 4C 8B 71 08 "
         "48 8D 45 30 48 89 45 20 33 C9",
         ResolveMode::Direct, -0x22, 0},

        // P3: deeper body anchor past the zero init of the 16-byte on-stack
        // scratch slot (mov [rbp+28h], ecx / mov dword ptr [rbp+2Ch], 2).
        // IDA-verified unique 1-hit at 0x14075BC15 (offset 0x25).
        {"BE_P3_BodyZeroInit",
         "4D 8B F8 4C 8B E2 48 8B F1 4C 8B 71 08 "
         "48 8D 45 30 48 89 45 20 33 C9 89 4D 28 C7 45 2C 02 00 00 00",
         ResolveMode::Direct, -0x25, 0},
    };

    /**
     * @brief Patch site: CondPrefab evaluator secondary hash check.
     *
     * sub_141D5F470 at +0xC8 (0x141D5F538 on v1.02.00): the `jz` that
     * jumps on character-class hash match. NPC items lack Kliff's class
     * in their secondary hash array, so this `jz` never fires → evaluator
     * returns empty → no mesh.
     *
     * Toggling this single byte from `0x74` (jz) to `0xEB` (jmp) forces
     * the match, making the evaluator emit resource names for NPC items.
     *
     * The AOB anchors on the surrounding instruction sequence:
     *   66 45 3B 0C 48   cmp  r9w, [r8+rcx*2]
     *   74 08             jz   +8              ← PATCH BYTE (offset +5)
     *   FF C1             inc  ecx
     *   3B CA             cmp  ecx, edx
     *   72 F3             jb   loop_top
     *
     * The patch byte is at AOB match + 5. Resolution mode is Direct
     * with offsetToHook = +5 so the resolved address points at the jz.
     */
    inline constexpr AddrCandidate k_charClassBypassCandidates[] = {
        // All patterns wildcard fragile bytes (rel8 jump distances,
        // struct offsets) and append the trailing `EB` (jmp opcode)
        // to disambiguate from a structurally identical loop at +0x70
        // in the same function.

        // P1: mov r8,[rbx+??] + movzx r9d,[rax+??] + inner loop + jmp.
        // IDA find_bytes verified unique 1-hit at 0x141D5F527.
        // Patch byte (the jz) is at match + 0x11.
        {"CCB_P1_MovzxCmpLoop",
         "4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x11, 0},

        // P2: test edx,edx + jz + mov + movzx + cmp+jz + loop + jmp.
        // IDA find_bytes verified unique 1-hit at 0x141D5F523.
        // Patch byte is at match + 0x15.
        {"CCB_P2_TestMovzxCmp",
         "85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x15, 0},

        // P3: xor ecx,ecx + test + jz + full inner loop + jmp.
        // IDA find_bytes verified unique 1-hit at 0x141D5F521.
        // Deepest anchor, most context. Patch byte at match + 0x17.
        {"CCB_P3_XorTestCmpLoop",
         "33 C9 85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x17, 0},
    };

    /**
     * @brief Inline hook: PartInOut direct-show bypass (sub_14081DC20).
     *        Copied verbatim from CrimsonDesertEquipHide/aob_resolver.hpp.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14081DC20(
     *       __int64 a1,           // RCX  descriptor context
     *       char    a2,           // DL   transition flag
     *       uint64_t partHashPtr, // R8   pointer to DWORD part hash
     *       float   blend,        // XMM3 animation blend
     *       __int64 a5..a9)       // stack params
     *
     * Used by live-transmog to suppress stale real-part frames that flash
     * through during state transitions (glide exit, landings, effect spawns)
     * where the game bypasses the PartInOutSocket vis=2 mask.
     */
    inline constexpr AddrCandidate k_partAddShowCandidates[] = {
        {"PAS_P1_Prologue",
         "40 55 56 57 41 55 48 83 EC ?? 48 8B 79 ??",
         ResolveMode::Direct, 0, 0},

        // Post-prologue: sub rsp,??; mov rdi,[rcx+??]; mov r13,r8; mov r9d,[rcx+??]
        {"PAS_P2_PostPrologue",
         "48 83 EC ?? 48 8B 79 ?? 4D 8B E8 44 8B 49 ??",
         ResolveMode::Direct, -6, 0},

        // SIMD save + shift: movaps [rsp+??],xmm6; shl rax,04; movaps xmm6,xmm3; add rax,rdi
        {"PAS_P3_SimdBody",
         "0F 29 74 24 ?? 48 C1 E0 04 0F 28 F3 48 03 C7",
         ResolveMode::Direct, -0x1B, 0},
    };

} // namespace Transmog
