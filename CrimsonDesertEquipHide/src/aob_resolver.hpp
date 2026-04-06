#pragma once

#include <DetourModKit.hpp>

#include <cstdint>
#include <vector>

namespace EquipHide
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
        ptrdiff_t offsetToHook; // bytes from AOB match to the actual hook point
    };

    struct CompiledCandidate
    {
        const AobCandidate *source;
        DMK::Scanner::CompiledPattern compiled;
    };

    /** @brief Resolve an address via cascading AOB patterns. */
    uintptr_t resolve_address(
        const AddrCandidate *candidates, std::size_t count,
        const char *label);

    /** @brief Scan all executable regions for a hook target. */
    uintptr_t scan_for_hook_target(
        const std::vector<CompiledCandidate> &candidates,
        const AobCandidate *&matchedSource);

    // --- AOB candidate tables ---

    inline constexpr AddrCandidate k_worldSystemCandidates[] = {
        {"WS_P1_SmallFunc",
         "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3",
         ResolveMode::RipRelative, 7, 11},

        {"WS_P2_StructField",
         "80 B8 ?? ?? ?? ?? 00 75 ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ??",
         ResolveMode::RipRelative, 12, 16},

        {"WS_P3_InnerLoad",
         "48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    inline constexpr AddrCandidate k_childActorVtblCandidates[] = {
        {"VT_P1_AllocCtor",
         "48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ??",
         ResolveMode::RipRelative, 16, 20},

        {"VT_P2_CtorStore",
         "48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 06 EB ?? 4C",
         ResolveMode::RipRelative, 12, 16},

        {"VT_P3_WiderCtorStore",
         "45 31 ED 48 85 F6 74 ?? 48 8B 55 ?? 48 89 F1 E8 ?? ?? ?? ?? 90 48 8D 05",
         ResolveMode::RipRelative, 24, 28},
    };

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

    inline constexpr AddrCandidate k_mapInsertCandidates[] = {
        {"MI_P1_FullPrologue",
         "4C 89 4C 24 20 53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, 0, 0},

        {"MI_P2_InnerBody",
         "44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA",
         ResolveMode::Direct, -0x11, 0},

        {"MI_P3_PrologueBody",
         "53 55 56 57 41 54 41 55 48 83 EC 28 44 8B 11 48 8B D9 4D 8B E1 41 8B F0 4C 8B EA 41 8B CA 45 85 D2",
         ResolveMode::Direct, -5, 0},
    };

    /**
     * @brief Hook target: sub_14081D3C0 — PartInOut transition function.
     *
     * Hook point: movzx eax, byte ptr [r13+1Ch]; cmp al, 3
     *
     * Register layout at hook point:
     *   R10 = pointer to part hash DWORD (IndexedStringA ID)
     *   R13 = pointer to PartInOutSocket struct (Visible byte at +0x1C)
     *   R8B = exclusion-list flag
     *   [RBP+0x67] = a4 (transition type byte, saved from R9B at prologue)
     *
     * Cascading AOB patterns (tried in order until one matches).
     */
    inline constexpr AobCandidate k_hookSiteCandidates[] = {
        {"P1_DirectSite",
         "41 0F B6 45 1C 3C 03 74 ?? 45 84 C0 75 ?? 84 C0",
         0},

        {"P2_WiderContext",
         "45 32 C0 48 8B 4D ?? 48 8B 41 ?? 8B 49 ?? 48 C1 E1 04 48 03 C8 48 3B C1 74 ?? 41 8B 12",
         0x36},

        {"P3_PrecedingGate",
         "41 B0 01 41 0F B6 45 1C 3C 03 74 ?? 45 84 C0",
         3},
    };

    /**
     * @brief Inline hook: PartInOut direct-show bypass (sub_14081DC20).
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14081DC20(
     *       __int64 a1,           // RCX  descriptor context
     *       char    a2,           // DL   transition flag
     *       uint64_t partHashPtr, // R8   pointer to DWORD part hash
     *       float   blend,        // XMM3 animation blend
     *       __int64 a5..a9)       // stack params
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

    /**
     * @brief AOB candidates for sub_1423FDEB0 — Postfix rule evaluator.
     *
     * Virtual function at vtable[4] of objects with vtable 0x144CC8248.
     * Evaluates whether a postfix rule matches currently equipped items.
     * Returns 1 = rule matches (hide hair), 0 = no match (keep hair).
     */
    inline constexpr AddrCandidate k_postfixEvalCandidates[] = {
        {"PFE_P1_PrologueAndBody",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 "
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3",
         ResolveMode::Direct, 0, 0},

        {"PFE_P2_UniqueBody",
         "48 83 EC ?? 4C 8B FA 48 8B 5A ?? 8B 42 ?? 48 8D 3C C3 48 3B",
         ResolveMode::Direct, -0x14, 0},

        {"PFE_P3_LoopInit",
         "45 33 F6 44 89 74 24 ?? C7 44 24 ?? ?? 00 00 00 49 8B 5F ?? 41 8B 47 ??",
         ResolveMode::Direct, -0x6F, 0},
    };


    /**
     * @brief Inline hook: VisualEquipChange (sub_14076D520).
     *
     * Bottleneck for all visual equipment changes (equip and unequip).
     * Called from the network handler for TrocTrAddVisualEquipItemAck.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14076D520(
     *       __int64 bodyComp,    // RCX  ClientFrameEventActorComponent*
     *       int16_t slotId,      // DX   equipment slot
     *       int16_t itemId,      // R8W  new item (0xFFFF = removing)
     *       __int64 itemData)    // R9   item data pointer
     */
    inline constexpr AddrCandidate k_visualEquipChangeCandidates[] = {
        {"VEC_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
         "55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ??",
         ResolveMode::Direct, 0, 0},

        {"VEC_P2_PushFrame",
         "55 57 41 54 41 56 41 57 "
         "48 8D AC 24 E0 EA FF FF B8 20 16 00 00",
         ResolveMode::Direct, -0x10, 0},

        // Post-alloca register shuffle: sub rsp,rax; mov rsi,r9; movzx ebx,r8w; movzx edi,dx; mov r14,rcx
        {"VEC_P3_PostAlloca",
         "48 2B E0 49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1",
         ResolveMode::Direct, -0x2A, 0},
    };

    /**
     * @brief Inline hook: VisualEquipSwap (sub_14075BBF0).
     *
     * Handles direct item-to-item swaps (bypasses VisualEquipChange).
     * Both paths converge on sub_14075C420 (EquipChangeDispatch).
     */
    inline constexpr AddrCandidate k_visualEquipSwapCandidates[] = {
        {"VES_P1_FullPrologue",
         "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 30 ED FF FF B8 D0 13 00 00",
         ResolveMode::Direct, 0, 0},

        {"VES_P2_PushFrame",
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 30 ED FF FF B8 D0 13 00 00",
         ResolveMode::Direct, -5, 0},

        // Post-alloca register shuffle: sub rsp,rax; mov r15,r8; mov r12,rdx; mov rsi,rcx; mov r14,[rcx+??]
        {"VES_P3_PostAlloca",
         "48 2B E0 4D 8B F8 4C 8B E2 48 8B F1 4C 8B 71 ??",
         ResolveMode::Direct, -0x22, 0},
    };

} // namespace EquipHide
