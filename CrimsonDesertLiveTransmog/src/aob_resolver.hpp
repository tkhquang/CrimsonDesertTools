#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors
// aliases. Resolution itself is delegated to DetourModKit's cascading
// scanner; resolve_address() flattens the std::expected return into the
// uintptr_t-or-zero shape used throughout the call sites.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>
#include <cdcore/prologue_check.hpp>

#include <DetourModKit.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Transmog
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    using CDCore::sanity_check_function_prologue;

    // Shared candidate tables (aliased by reference from CDCore so the
    // single source of truth lives in cdcore/anchors.hpp).
    inline constexpr auto &k_worldSystemCandidates =
        CDCore::Anchors::k_worldSystemCandidates;
    inline constexpr auto &k_mapLookupCandidates =
        CDCore::Anchors::k_mapLookupCandidates;
    inline constexpr auto &k_partAddShowCandidates =
        CDCore::Anchors::k_partAddShowCandidates;
    inline constexpr auto &k_visualEquipChangeCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_vecCandidates =
        CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_batchEquipCandidates =
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

    // --- LiveTransmog-only candidate tables ------------------------------

    /**
     * @brief SafeTearDown (sub_14075FE60) -- scene-graph tear-down that
     *        retires a matched part without mutating the authoritative
     *        equip table at *(a1+0x78). Used by the two-phase transmog
     *        apply in real_part_tear_down.
     *
     * P1 extends 3 bytes past the prologue to disambiguate from
     * sub_140BD7270 (the 33-byte prologue alone has 2 matches).
     */
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        // P1 -- full prologue. Stack-save disp32 (50 FF FF FF) and
        // stack-alloc size (B0 01 00 00) wildcarded per signing rules.
        // The 41 0F B7 F8 (movzx edi, r8w) tail is function-specific.
        {"SafeTearDown_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor. Stack-alloc imm32 wildcarded.
        {"SafeTearDown_P2_PostAlloca",
         "48 81 EC ?? ?? ?? ?? 41 0F B7 F8 48 8B F1",
         ResolveMode::Direct, -0x1A, 0},

        // P3 -- deep-body anchor at the global-pointer call site. The
        // 48 83 C1 60 is a game-ABI struct offset (stable ABI), kept
        // literal. Stack disp32 in lea rdx,[rbp+disp32] and the
        // rel32 jz target are wildcarded (both compiler-owned). The
        // trailing stack disp E0 00 00 00 is kept literal to
        // disambiguate from 4 sibling functions that share the same
        // add rcx,60 / lea rdx / call / test / jz / movzx / mov [rbp+X]
        // body shape (CE aob_scan 2026-04-19: 5 hits without it, 1 hit
        // with). The B9 FF FF 00 00 66 3B C8 tail is a semantic
        // sentinel check specific to this function.
        {"SafeTearDown_P3_GlobalPtrCall",
         "48 83 C1 60 48 8D 95 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
         "48 85 C0 0F 84 ?? ?? ?? ?? 0F B7 00 66 89 85 E0 00 00 00 "
         "B9 FF FF 00 00 66 3B C8",
         ResolveMode::Direct, -0x4A, 0},
    };

    /**
     * @brief SubTranslator (sub_14076D950) -- SlotPopulator item-id
     *        translator. Not hooked; used as the first hop of the chain
     *        that walks to qword_145CEF370 (iteminfo global) so we can
     *        build the stable item-name table at init. See
     *        item_name_table.cpp for the full 4-step chain.
     */
    inline constexpr AddrCandidate k_subTranslatorCandidates[] = {
        // P0 -- v1.05.00 full prologue. The second lea encodes
        // rsp-relative (`48 8D 4C 24 ??`, 4 bytes) instead of the
        // v1.04 rbp-relative form (`48 8D 4D ??`, 3 bytes), shifting
        // the body by 1 byte. Body shape (mov r8d=1, lea rdx=[rbp+X],
        // lea rcx=[rsp+Y]) is otherwise unchanged. 1 unique hit on
        // v1.05.00 at 0x140799CA9. dispOffset 0 returns the prologue
        // start; this candidate is consumed by ItemNameTable::build()
        // as a chain anchor (no hook installed here).
        {"SubTranslator_P0_v105_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??",
         ResolveMode::Direct, 0, 0},

        // P0b -- v1.05.00 post-alloca anchor. Same disp8 wildcarding
        // as P0 but no head sentinels. Walk-back matches v1.04 P2
        // (-0x19): the encoding shift sits inside the pattern, not
        // before the anchor.
        {"SubTranslator_P0b_v105_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P0c -- v1.05.00 deeper anchor: mov-r8d-1 + lea pair followed
        // by the unique 90 48 8B D0 48 8B CF E8 post-call tail. 1
        // unique hit on v1.05.00 at 0x140799CAC; function starts at
        // 0x140799C90, so walk-back -0x1C (matches v1.04 P3 semantics).
        {"SubTranslator_P0c_v105_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},

        // P1 -- v1.04.00 full prologue. Stack disp8 (B9), stack-alloc
        // size (00 01 00 00), and the two local-variable disp8 values
        // in lea rdx,[rbp+X] / lea rcx,[rbp+Y] wildcarded. The
        // 41 B8 01 00 00 00 (mov r8d, 1) is a SEMANTIC argument flag
        // and is kept literal. Inert on v1.05.00 (lea encoding shift).
        {"SubTranslator_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- v1.04.00 post-alloca scratch-buffer prep anchor. Same
        // disp8 wildcarding as P1. Inert on v1.05.00.
        {"SubTranslator_P2_PostAlloca",
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P3 -- v1.04.00 deeper anchor without the leading `mov rdi,
        // rcx`. Without the 90 48 8B D0 48 8B CF E8 post-call suffix
        // this shortened pattern matches 5 sibling call sites (same
        // mov r8d,1 / lea rdx / lea rcx / call boilerplate); the
        // suffix pins this function's second call (CE aob_scan
        // 2026-04-19). Inert on v1.05.00.
        {"SubTranslator_P3_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},
    };

    /**
     * @brief InitSwapEntry (sub_141D451B0) -- initializes a 0x80-byte swap
     *        entry structure to default sentinel values (-1 / 0). Called
     *        by the mod immediately before each SlotPopulator invocation.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_141D451B0(__int64 dest)
     *
     * Converted from hardcoded gameBase + 0x1D451B0 to AOB on
     * 2026-04-11 to survive code-drift in earlier .text sections.
     */
    inline constexpr AddrCandidate k_initSwapEntryCandidates[] = {
        // P1 -- true prologue (mov [rsp+8],rcx / push rbx / sub rsp,20h /
        // mov rbx,rcx / mov rax,-1 / mov [rcx],rax / mov ecx,0FFFFh).
        {"InitSwapEntry_P1_FullPrologue",
         "48 89 4C 24 08 53 48 83 EC 20 48 8B D9 "
         "48 C7 C0 FF FF FF FF 48 89 01 B9 FF FF 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue field-init chain (mov [rbx+20h],rdx / ... /
        // lea rax,[rbx+58h]).
        {"InitSwapEntry_P2_FieldInitChain",
         "48 89 53 20 48 89 53 28 48 89 53 30 66 89 53 38 "
         "48 89 53 40 48 89 53 48 66 89 53 50 48 8D 43 58",
         ResolveMode::Direct, -0x26, 0},

        // P3 -- xor edx,edx + field-init chain (slightly tighter than P2).
        {"InitSwapEntry_P3_XorInitChain",
         "33 D2 48 89 53 20 48 89 53 28 48 89 53 30 "
         "66 89 53 38 48 89 53 40 48 89 53 48 66 89 53 50",
         ResolveMode::Direct, -0x24, 0},
    };

    /**
     * @brief SlotPopulator (sub_14076C960) -- populates the slot array
     *        (a1+440) with item visual data then calls VisualEquipChange.
     *        This is the function the server equip handler invokes to
     *        trigger a full visual equip with mesh loading.
     *
     * Signature (x64 __fastcall):
     *   __int64 sub_14076C960(
     *       __int64 a1,
     *       unsigned __int16* a2_itemData,
     *       __int64 a3_swapEntry)
     *
     * a2 is a 16-byte structure:
     *   +0:  uint16 item ID
     *   +2:  byte   flag (2 = normal equip)
     *   +4:  int32  (-1)
     *   +12: uint16 secondary slot (0xFFFF to skip)
     */
    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        // P1 -- full prologue through the register shuffle (mov r12,rdx;
        // mov r13,rcx; xor edi,edi).
        {"SlotPopulator_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? 00 00 00 "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor: register shuffle + the mov-edi-to-stack
        // + mov r14d, -1 sentinel + movzx eax, r14w. Offset -0x22
        // backs up to function start.
        {"SlotPopulator_P2_PostAlloca",
         "4C 8B E2 4C 8B E9 33 FF 89 7D ?? 41 BE FF FF FF FF 41 0F B7 C6",
         ResolveMode::Direct, -0x22, 0},

        // P3 -- deeper anchor on the mov r14d,-1 sentinel + the inline
        // mov [rbp+6Fh],ax ; mov ebx,0FFFFh follow-up. Skips the
        // register shuffle entirely and pins the post-init block. Offset
        // -0x2D backs up to function start.
        {"SlotPopulator_P3_SentinelInit",
         "41 BE FF FF FF FF 41 0F B7 C6 66 89 45 6F BB FF FF 00 00",
         ResolveMode::Direct, -0x2D, 0},
    };

    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module data globals (audit 2026-05-08).
    //
    // The PrefabWrapperSwap module previously hardcoded six absolute
    // addresses derived from v1.05.01 RVAs. Each is now resolved through
    // a 3-candidate cascade:
    //
    //   StringInfoRegistry  : MEMORY[0x145EF1DE8] -- registry struct.
    //   StringInfoVtable    : 0x145BC4638        -- vtable sentinel filter.
    //   LoaderRegistry      : MEMORY[0x145DDF8B0] -- partprefab name->wrapper.
    //   ApptContainerVtable : 0x144D24358        -- partPrefabDataContainer
    //                                              vtable; used by lookup
    //                                              gating in
    //                                              lookup_prefab_metadata.
    //   NaturalPipeline     : sub_142711DF0 (RVA 0x2711DF0). Hooked.
    //   ApptNameLookup      : sub_1424DF420 (RVA 0x24DF420). Called direct.
    //
    // For data globals (registry/vtable) the cascade resolves through a
    // RIP-relative mov/lea instruction in a non-template caller. The
    // disp32 is wildcarded; the cascade returns absolute target via
    // ResolveMode::RipRelative.
    //
    // For the two function targets we use Direct mode against the
    // function prologue. Three independent anchors per function let a
    // future compiler shuffle the prologue without breaking resolution.
    //
    // Per feedback_aob_cascade_ordering: each P1 below is verified
    // n=1 in v1.05.01 .text. P2/P3 are also unique to allow recovery
    // when a future patch breaks P1.
    //
    // If these break: each candidate's comment names the anchor caller
    // (e.g. "xref in sub_1402F58A0"). Re-find the function by name
    // through IDA, locate the load instruction, copy the surrounding
    // 16--32 bytes, wildcard the disp32, and verify uniqueness with
    // mcp__ida-pro-mcp__find_bytes.
    // -----------------------------------------------------------------------

    /**
     * @brief StringInfoRegistry global: MEMORY[0x145EF1DE8].
     *
     * The 17-slot StringInfo registry struct. +0x08 holds count u32,
     * +0x50 holds the QWORD entry-array pointer. PrefabWrapperSwap walks
     * this registry to resolve prefab NAMES to entry wrapper-ptrs.
     *
     * All three candidates anchor on a `mov reg, [rip+disp32]` that
     * loads this address. Disp32 wildcarded; rest of the 16+ byte
     * window is unique-text in v1.05.01 .text.
     */
    inline constexpr AddrCandidate k_stringInfoRegistryCandidates[] = {
        // P1 -- xref in sub_141D81F90 at 0x141D8215E. Distinctive frame
        // shape: load char-table eax, store at [rbp+0x58], then load
        // registry into rcx, walk +0x60, lea rdx,[rbp+0x58], then call.
        // The local-variable disp8 (0x58) is kept literal to disambiguate.
        {"StringInfoRegistry_P1_LoadAddCallSite",
         "8B 45 B0 89 45 58 48 8B 0D ?? ?? ?? ?? 48 83 C1 60 48 8D 55 58",
         ResolveMode::RipRelative, 9, 13},

        // P2 -- xref in sub_142144B10 (large parser, 0x1d6f bytes). A
        // distinctive `mov [rbp-0x6A], al` + 32-bit local-store sequence
        // precedes a `mov r15, [rip+disp32]` registry load. The local
        // disp32 sequence (90 00 00 00) is kept literal -- it's the
        // local frame's outer-scope variable offset.
        {"StringInfoRegistry_P2_OuterScopeFrame",
         "88 45 96 89 8D 90 00 00 00 4C 8B 3D ?? ?? ?? ?? "
         "48 8D 95 90 00 00 00 49 8D 4F 60",
         ResolveMode::RipRelative, 12, 16},

        // P3 -- xref in sub_14074DD40. Conditional load+jump shape:
        // load r9d, test, jz, store dword to [rbp+0x70], then load
        // registry. The 8B 4F 74 (mov ecx, [rdi+0x74]) is a stable
        // game-struct field offset.
        {"StringInfoRegistry_P3_CondLoadStore",
         "8B 4F 74 85 C9 74 5C 89 4D 70 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 60 48 8D 55 70",
         ResolveMode::RipRelative, 13, 17},
    };

    /**
     * @brief StringInfoVtable sentinel: 0x145BC4638.
     *
     * Vtable pointer used as the +0x08 sentinel of every StringInfo
     * entry. PrefabWrapperSwap reads it to filter out non-StringInfo
     * heap rows during walk_string_info.
     *
     * All three candidates anchor on a `lea rax, [rip+disp32]` write.
     * Each candidate is in a different function so a single-function
     * recompile cannot break all three.
     */
    inline constexpr AddrCandidate k_stringInfoVtableCandidates[] = {
        // P1 -- xref in sub_1402F58A0 (0x17b0 bytes, large method).
        // `mov ecx, [rbp+0x1C0]; call rel32; lea r15, [rip+disp32];
        //  mov [rbp+0x1C0], r15`. The 0x1C0 stack-frame offset is
        // sufficiently distinctive in this 6kB function.
        {"StringInfoVtable_P1_LargeMethodAssign",
         "8B 8D C0 01 00 00 E8 ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? "
         "4C 89 BD C0 01 00 00",
         ResolveMode::RipRelative, 14, 18},

        // P2 -- xref in sub_1403174F0 (0x535 bytes). Init block:
        // `mov [rbp-0x50], eax; xor r12d, r12d; mov [rbp-0x48], r12;
        //  lea r13, [rip+disp32]; nop`. The trailing 0x90 nop is
        // alignment padding kept literal -- if a future build drops
        // the padding, P3 catches it.
        {"StringInfoVtable_P2_R13InitBlock",
         "89 45 B0 45 33 E4 4C 89 65 B8 4C 8D 2D ?? ?? ?? ?? 90 "
         "48 8B 45 C8",
         ResolveMode::RipRelative, 13, 17},

        // P3 -- xref in sub_14031AC50 (0xbfd bytes). Tail block:
        // `mov [rbp+0x40], r13; mov rsi, [rbp+0x10]; lea rdi, [rip+disp32]`.
        // The trailing `0F 1F 40 00` is a 4-byte nop the compiler
        // emits for branch-target alignment -- stable for this site.
        {"StringInfoVtable_P3_TailLeaRdi",
         "4C 89 6D 40 48 8B 75 10 48 8D 3D ?? ?? ?? ?? 0F 1F 40 00",
         ResolveMode::RipRelative, 11, 15},
    };

    /**
     * @brief IteminfoHolder global: address of qword_145F2A338.
     *
     * The engine's per-process iteminfo registry pointer. `*holder`
     * dereferences to the registry struct; `*holder + 0x08` holds the
     * u32 entry count, `*holder + 0x50` the QWORD entry-array pointer.
     * The runtime item-to-prefab bridge (itemmesh_dumper) reads it to
     * enumerate every loaded item descriptor.
     *
     * All three candidates anchor on a `mov rcx, [rip+disp32]` loading
     * this address inside a lookup primitive that follows the standard
     * `add rcx, 0x60; lea rdx, [frame]; call` sequence. The disp32 is
     * wildcarded; surrounding stack-frame offsets and tail-instruction
     * bytes carry the uniqueness budget.
     */
    inline constexpr AddrCandidate k_iteminfoHolderCandidates[] = {
        // P1 -- xref in sub_14063C850 at 0x14063CB69. Frame disp32
        // 0x240 on both the inbound store (`mov [rbp+0x240], eax`)
        // and the outbound lea (`lea rdx, [rbp+0x240]`) pins this site
        // to the sole 0x240-frame caller of the iteminfo lookup. The
        // `74 11 44 0F B7 20` tail (jz rel8 / movzx r12d,[rax]) is the
        // post-call success branch and is unique-text in v1.07.00 .text.
        {"IteminfoHolder_P1_Frame0x240LookupCall",
         "89 85 40 02 00 00 48 8B 0D ?? ?? ?? ?? 48 83 C1 60 "
         "48 8D 95 40 02 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 11 "
         "44 0F B7 20",
         ResolveMode::RipRelative, 9, 13},

        // P2 -- xref in sub_1407307D0 at 0x140730A25. Distinctive
        // dword-copy preamble `mov eax, [rbp+0xB0]; mov [rbp+0xA8], eax`
        // (two stable game-struct frame offsets) precedes the load.
        // Followed by the canonical lookup-call sequence with the same
        // 0xA8 frame disp echoed in the lea, then a jz-success tail.
        {"IteminfoHolder_P2_FrameB0CopyLookup",
         "8B 85 B0 00 00 00 89 85 A8 00 00 00 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 60 48 8D 95 A8 00 00 00 E8 ?? ?? ?? ?? "
         "48 85 C0 74",
         ResolveMode::RipRelative, 15, 19},

        // P3 -- xref in sub_14081A580 at 0x14081A5E6. rsp-relative
        // frame (`mov [rsp+0x50], eax`; `lea rdx, [rsp+0x50]`) rather
        // than rbp-based, plus the post-call success branch lands on a
        // jz rel32 (`0F 84 ?? ?? ?? ??`) followed by a u16 sentinel
        // check `0F B7 10 66 44 3B F2` (movzx edx,[rax]; cmp r14w,dx).
        // The sentinel byte sequence is the canonical "no match"
        // probe used by this family of lookups.
        {"IteminfoHolder_P3_RspFrameSentinelProbe",
         "8B 07 89 44 24 50 48 8B 0D ?? ?? ?? ?? 48 83 C1 60 "
         "48 8D 54 24 50 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
         "0F B7 10 66 44 3B F2",
         ResolveMode::RipRelative, 9, 13},
    };

    /**
     * @brief StringinfoHolder global: address of qword_145F2A368.
     *
     * Sibling to IteminfoHolder, located 0x30 bytes higher in the
     * engine's data section. Backs the engine's string-bag registry
     * (icon-prefab names, asset paths). Used by the runtime
     * item-to-prefab bridge (itemmesh_dumper) to translate the u16
     * stringSlot stored in `iteminfo[id].desc+0x90` into a c-string
     * wrapper.
     *
     * The generic lookup primitive (`mov reg, [rip+disp32]; add reg,
     * 0x60; lea rdx, [frame]; call`) appears in dozens of callers for
     * both holders. Each candidate below extends the window with
     * caller-specific bytes (frame offsets, register selectors,
     * sentinel writes) so the cascade cannot drift from stringinfo
     * to iteminfo on a future rebuild. P1 specifically uses an
     * r15-targeted load (REX.WR = 4C) while P2/P3 use the more common
     * rcx-targeted form.
     */
    inline constexpr AddrCandidate k_stringinfoHolderCandidates[] = {
        // P1 -- xref in sub_14031EC80 at 0x14031ED78. Distinguished by
        // an r15-targeted load (`mov r15, [rip+disp32]`, REX.WR = 4C)
        // instead of the usual rcx target, paired with a `lea rcx,
        // [r15+0x60]` (`49 8D 4F 60`) outbound. The trailing
        // `74 05 0F B7 30 EB 05` (jz rel8 / movzx esi,[rax] /
        // jmp rel8) is the success/fail two-arm join unique to this
        // caller.
        {"StringinfoHolder_P1_R15LookupTwoArm",
         "89 45 58 4C 8B 3D ?? ?? ?? ?? 48 8D 55 58 49 8D 4F 60 "
         "E8 ?? ?? ?? ?? 48 85 C0 74 05 0F B7 30 EB 05",
         ResolveMode::RipRelative, 6, 10},

        // P2 -- xref in sub_140373AD0 at 0x140373C6A. Preamble
        // `mov eax, [rsi+4]; mov [rbp+0x40], eax` followed by the
        // load and the canonical lookup-call. The post-call tail
        // `45 33 C0 48 85 C0 0F 84 ?? ?? ?? ?? 44 0F B7 38
        // B8 FF FF 00 00` is the no-match sentinel write of 0xFFFF
        // into r15w and is unique-text in v1.07.00 .text.
        {"StringinfoHolder_P2_EsiPlus4SentinelWrite",
         "8B 46 04 89 45 40 48 8B 0D ?? ?? ?? ?? 48 83 C1 60 "
         "48 8D 55 40 E8 ?? ?? ?? ?? 45 33 C0 48 85 C0 "
         "0F 84 ?? ?? ?? ?? 44 0F B7 38 B8 FF FF 00 00",
         ResolveMode::RipRelative, 9, 13},

        // P3 -- xref in sub_141CA66D0 at 0x141CA66F7. Distinctive
        // r11-relative outbound lea `lea rdx, [r11+0x10]`
        // (`49 8D 53 10`) -- the only family caller that passes the
        // probe key through r11 rather than the frame. The
        // `B8 FF FF 00 00` sentinel write is shared with P2 but the
        // upstream `8B 41 18 41 89 43 10` preamble (mov eax,[rcx+0x18]
        // / mov [r11+0x10], eax) is unique to this caller.
        {"StringinfoHolder_P3_R11ProbeKeySentinel",
         "8B 41 18 41 89 43 10 48 8B 0D ?? ?? ?? ?? 48 83 C1 60 "
         "49 8D 53 10 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
         "0F B7 38 B8 FF FF 00 00",
         ResolveMode::RipRelative, 10, 14},
    };

    /**
     * @brief LoaderRegistry singleton: MEMORY[0x145DDF8B0].
     *
     * Engine partprefab name->wrapper registry singleton. The ApptName-
     * Lookup function (sub_1424DF420, also AOB-resolved) dereferences
     * this and queries [+0x50]. PrefabWrapperSwap reads this on init for
     * heap-walk enumeration of prefab wrappers.
     */
    inline constexpr AddrCandidate k_loaderRegistryCandidates[] = {
        // P1 -- xref in sub_1424E44A0 at 0x1424E4771. Distinctive
        // 64-bit add-immediate `48 81 C1 D0 00 00 00` (add rcx, 0xD0)
        // that follows the registry load -- a stable game-struct
        // walk-offset.
        {"LoaderRegistry_P1_AddD0CallSite",
         "48 8B 0D ?? ?? ?? ?? 48 81 C1 D0 00 00 00 48 8D 56 20 E8",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- xref in sub_1424E44A0 at 0x1424E47AA (different site).
        // `mov [rbp+0x07], r15; mov r14, [rip+disp32]; add r14, ...`.
        // The 4C 89 7D 07 (mov [rbp+0x07], r15) shape is unusual and
        // distinguishes from the first call site.
        {"LoaderRegistry_P2_R14ReadAfterStore",
         "4C 89 7D 07 4C 8B 35 ?? ?? ?? ?? 49 83 C6",
         ResolveMode::RipRelative, 7, 11},

        // P3 -- xref in sub_142D1E220 at 0x142D1E7F1. This is a STORE
        // (`mov [rip+disp32], rbx`) that initializes the singleton at
        // engine-init time, NOT a load. The disp32 still resolves to
        // the singleton address. Distinctive surrounding context: an
        // inline EB 03 (jmp short) and a 32-bit struct-field compare
        // `48 3B 9E 60 00 04 00`.
        {"LoaderRegistry_P3_InitStoreSite",
         "48 89 03 48 89 1D ?? ?? ?? ?? EB 03 48 8B DF "
         "48 3B 9E 60 00 04 00",
         ResolveMode::RipRelative, 6, 10},
    };

    /**
     * @brief ApptContainerVtable: 0x144D24358 (partPrefabDataContainer).
     *
     * Per sub_141E2DBB0 (AppearanceTableLoader ctor), the loader
     * allocates two containers and assigns final vtables:
     *   a1[0] (_appearanceContainer)     -> &off_144D24308
     *   a1[1] (_partPrefabDataContainer) -> &off_144D24358   <-- our target
     *
     * Single-xref site (in sub_141E2DBB0). The vtable-write pattern is
     * a code-generator template emitted for ~9 sibling container types,
     * so anchoring on the lea+mov[rdi] sequence alone is not unique.
     *
     * Resolution strategy: AOB-resolve sub_141E2DBB0's prologue (which
     * IS unique), then the C++ resolver walks forward through the
     * function body to find the SECOND `48 8D 05 ?? ?? ?? ?? 48 89 07`
     * pair (the FIRST is the intermediate `_appearanceContainer` vtable;
     * the SECOND is our target `_partPrefabDataContainer` vtable). The
     * walk-forward logic runs inline inside prefab_wrapper_swap.cpp's
     * `init()` after this anchor resolves.
     *
     * If this breaks: re-AOB sub_141E2DBB0 by its prologue, then in
     * IDA find the second `lea rax, [rip+disp32]; mov [rdi], rax` pair
     * inside the function. The byte offset has shifted across builds
     * (was at +0x2BF in v1.05.01); the walk is bounded to the
     * function's first 0x400 bytes to avoid running off into the next
     * function.
     */
    inline constexpr AddrCandidate k_apptLoaderCtorCandidates[] = {
        // P1 -- full prologue (8 callee-saved regs + frame setup).
        // Distinctive in v1.05.01: 1 unique hit. The 41 54 41 55 41 56
        // 41 57 (push r12-r15) is the largest possible callee-save set,
        // typical of a 540-byte function with many locals.
        {"ApptLoaderCtor_P1_FullPrologue",
         "48 89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 "
         "48 83 EC 38 48 8B F1 45 33 F6 4C 89 31 4C 89 71 08 4C 89 71 10 "
         "4C 89 71 18 44 88 71 20",
         ResolveMode::Direct, 0, 0},

        // P2 -- mid-prologue + first field-init. Skips the `mov rcx,rdx
        // mov rax,rcx` boilerplate; anchors on the first XOR + the
        // sequence of zero-stores into [rcx+0..0x18].
        {"ApptLoaderCtor_P2_FieldInitChain",
         "45 33 F6 4C 89 31 4C 89 71 08 4C 89 71 10 4C 89 71 18 "
         "44 88 71 20 49 8B 00 48 89 41 10",
         ResolveMode::Direct, -0x1D, 0},

        // P3 -- mid-body anchor on the unique field-init shape that
        // copies a 32-byte payload from a3 into a1+0x10..0x20: the
        // 49 8B 00 / 48 89 41 10 / 49 8B 40 08 / 48 89 41 18 /
        // 41 0F B6 40 10 sequence is the inline copy of {qword,qword,
        // byte} from *a3. Unique to this loader ctor in v1.05.01.
        // Walk-back -0x2F lands on function start.
        {"ApptLoaderCtor_P3_PayloadCopy",
         "44 88 71 20 49 8B 00 48 89 41 10 49 8B 40 08 48 89 41 18 "
         "41 0F B6 40 10",
         ResolveMode::Direct, -0x2F, 0},
    };

    /**
     * @brief NaturalPipeline (sub_142711DF0) -- engine unlink fn.
     *
     * RVA 0x2711DF0. PrefabWrapperSwap installs a MidHook here to
     * substitute Kliff src wrappers with target wrappers in the engine's
     * unlink list (helm/cloak ghost cleanup, see ghost-helm-re memory).
     *
     * Function is a 6k-byte unlink pipeline with all 8 callee-saved
     * registers pushed, so prologue is highly distinctive.
     */
    inline constexpr AddrCandidate k_naturalPipelineCandidates[] = {
        // P1 -- full prologue + chkstk preamble. The 80 18 00 00 (mov
        // eax, 0x1880) is the stack reservation passed to __chkstk.
        // Stable for this exact function shape -- B8 imm32 will only
        // change if the stack-allocation grows past 4kb.
        {"NaturalPipeline_P1_FullPrologueChkstk",
         "48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 E8 FF FF "
         "B8 80 18 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill prologue. Anchors past the four arg-
        // home stores and on the lea rbp,[rsp-0x1780]+chkstk pair.
        // Walk-back -5 = past `48 89 5C 24 10` to function start.
        {"NaturalPipeline_P2_PostArgSpill",
         "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 E8 FF FF "
         "B8 80 18 00 00",
         ResolveMode::Direct, -0x05, 0},

        // P3 -- chkstk + stack adjustment + arg-shuffle. The
        // 48 2B E0 (sub rsp, rax) is the conventional __chkstk
        // post-call. Walk-back -0x27 to function start.
        {"NaturalPipeline_P3_PostChkstkArgShuffle",
         "B8 80 18 00 00 E8 ?? ?? ?? ?? 48 2B E0 4D 8B E8 48 8B DA "
         "4C 8B F9 41 83 78",
         ResolveMode::Direct, -0x27, 0},
    };

    /**
     * @brief ApptNameLookup (sub_1424DF420) -- name->wrapper primitive.
     *
     * RVA 0x24DF420. PrefabWrapperSwap calls this directly (not hooked)
     * to resolve partprefab names to wrapper-ptrs. Lowercases the name,
     * interns it, queries MEMORY[0x145DDF8B0]+0x50, returns entry+8 on
     * hit or 0 on miss.
     */
    inline constexpr AddrCandidate k_apptNameLookupCandidates[] = {
        // P1 -- full prologue + frame setup + first registry load.
        // The 48 8D 6C 24 A0 (lea rbp,[rsp-0x60]) and 48 81 EC 60 01
        // (sub rsp, 0x160) form a unique 0x160-byte stack frame. The
        // immediately-following `mov rsi, [rip+disp32]` loads the
        // LoaderRegistry singleton (also resolved separately).
        {"ApptNameLookup_P1_FullPrologue",
         "48 89 5C 24 10 48 89 4C 24 08 55 56 57 "
         "48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF 48 89 7D 28",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill, before frame setup. Walk-back -10 to
        // function start.
        {"ApptNameLookup_P2_PostArgSpill",
         "55 56 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF",
         ResolveMode::Direct, -0x0A, 0},

        // P3 -- frame setup + xor edi,edi + the local-var inits.
        // The 48 C7 45 30 05 01 00 00 (mov [rbp+0x30], 0x105) is a
        // semantic constant (initial buffer capacity) -- stable.
        // Walk-back -13 to function start.
        {"ApptNameLookup_P3_LocalInitConstant",
         "48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF 48 89 7D 28 "
         "48 C7 45 30 05 01 00 00",
         ResolveMode::Direct, -0x0D, 0},
    };

    /**
     * @brief Patch site: CondPrefab evaluator secondary hash check
     *        (sub_141D5F470 at +0xC8, 0x141D5F538 on v1.02.00).
     *
     * The jz that jumps on character-class hash match. NPC items lack
     * Kliff's class in their secondary hash array, so this jz never
     * fires, evaluator returns empty, no mesh.
     *
     * Toggling this single byte from 0x74 (jz rel8) to 0xEB (jmp rel8)
     * forces the match, making the evaluator emit resource names for
     * NPC items.
     *
     * Patterns wildcard the fragile bytes (rel8 jump distances, struct
     * offsets) and append the trailing EB (jmp opcode of the OUTER
     * loop exit, NOT the patch target) to disambiguate from a
     * structurally identical loop at +0x70 in the same function.
     *
     * Branch-encoding constraint (aob-signatures.md section 8):
     *       Unlike every other AOB in this project, the 74 rel8 opcode
     *       in these patterns is intentionally literal and must NOT be
     *       wildcarded -- it is the exact byte the mod flips at
     *       runtime. A future compiler that emits this jz in its 6-byte
     *       0F 84 rel32 form would require a full re-RE of the patch
     *       strategy (not a pattern tweak); every candidate below would
     *       stop matching and the feature would fail-soft via the
     *       scanner's null return. Anchors on the outer 72 ?? EB loop
     *       structure share the same constraint for the same reason.
     */
    inline constexpr AddrCandidate k_charClassBypassCandidates[] = {
        // P1 -- mov r8,[rbx+??] + movzx r9d,[rax+??] + inner loop + jmp.
        // Patch byte at match + 0x11.
        {"CharClassBypass_P1_MovzxCmpLoop",
         "4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x11, 0},

        // P2 -- test edx,edx + jz + mov + movzx + cmp+jz + loop + jmp.
        // Patch byte at match + 0x15.
        {"CharClassBypass_P2_TestMovzxCmp",
         "85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x15, 0},

        // P3 -- xor ecx,ecx + test + jz + full inner loop + jmp. Deepest
        // anchor, most context. Patch byte at match + 0x17.
        {"CharClassBypass_P3_XorTestCmpLoop",
         "33 C9 85 D2 74 ?? 4C 8B 43 ?? 44 0F B7 88 ?? ?? 00 00 "
         "66 45 3B 0C 48 74 ?? FF C1 3B CA 72 ?? EB",
         ResolveMode::Direct, 0x17, 0},
    };

    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module function targets (audit 2026-05-08 part 2).
    //
    // Four function-target cascades migrated out of prefab_wrapper_swap.cpp
    // where they previously lived as single-anchor inline constexpr arrays.
    // Each now has a 3-anchor cascade per the ordering rule in
    // CrimsonDesertCore/external/DetourModKit/docs/misc/aob-signatures.md.
    //
    //   ApptResMgrInit  : sub_1408AF8F0 -- one-shot capture hook target;
    //                                      reads ResMgr/loader/container.
    //   ApptInnerLookup : sub_140350910 -- partprefab container hashtable
    //                                      lookup primitive. Pure read.
    //   ApptStringIntern: sub_1403016B0 -- StringInfo string-intern primitive
    //                                      callable as `handle_t(const char*)`.
    //   StructCopy      : sub_140352AA0 -- 0x40-byte struct-copy hot path
    //                                      that PrefabWrapperSwap inline-hooks
    //                                      to swap source wrapper-ptrs.
    //
    // Hit counts re-verified live via Cheat Engine + IDA on v1.05.01 .text
    // before authoring. Where a function had a sibling clone (linker-emitted
    // duplicate compiled from a templated header) and no global anchor was
    // unique, the cascade leads with a RipRelative call-site anchor that
    // walks an `E8 disp32` from a known caller (which IS unique) to the
    // canonical target.
    // -----------------------------------------------------------------------

    /**
     * @brief ApptResMgrInit (sub_1408AF8F0) -- one-shot capture hook target.
     *
     * Outer ResMgr-init function. PrefabWrapperSwap installs an inline
     * entry hook that runs the trampoline and then snapshots ResMgr at
     * a1[5] (a1+0x28), the loader at ResMgr+0x58, and the partprefab
     * container at loader+0x08. The hook is one-shot; subsequent calls
     * are pass-throughs.
     *
     * If these break: the function pushes all 8 callee-saved registers
     * (push rbp/rbx/rsi/rdi/r12-r15 = `40 55 53 56 57 41 54 41 55 41 56
     * 41 57`) and then sets up a 0xF8-byte stack frame. That 13-byte
     * push run + the chkstk-free `48 81 EC F8 00 00 00` direct alloc are
     * the two distinguishing prologue features. Re-anchor by combining
     * either with one of the early-body markers (TLS slot 0x58 read or
     * the constant `204h` payload size).
     */
    inline constexpr AddrCandidate k_apptResMgrInitCandidates[] = {
        // P1 -- full prologue (8 callee-saved push run + lea rbp + direct
        // 0xF8 alloc + arg shuffle). 1 hit on v1.05.01 .text. No RIP-rel
        // bytes inside the window so wildcards are not needed.
        {"PrefabWrapperSwap_ApptResMgrInit_P1_FullPrologue",
         "40 55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 E1 "
         "48 81 EC F8 00 00 00 "
         "4C 8B FA 4C 8B F1",
         ResolveMode::Direct, 0, 0},

        // P2 -- mid-prologue: lea rbp,[rsp-0x1F] + direct alloc + arg
        // shuffle + xor r13d,r13d. Walk-back -0x0D to function start.
        // 1 hit on v1.05.01 .text. Survives a future build that adds or
        // reorders early callee-save pushes (because the lea-rbp marker
        // pins frame setup, not which regs were pushed first).
        {"PrefabWrapperSwap_ApptResMgrInit_P2_PostPushAlloca",
         "48 8D 6C 24 E1 48 81 EC F8 00 00 00 "
         "4C 8B FA 4C 8B F1 45 33 ED",
         ResolveMode::Direct, -0x0D, 0},

        // P3 -- TLS-canary load body anchor. The unique `mov r12d, 204h ;
        // mov rax, gs:58h ; add r12, [rax]` sequence reads the TLS slot
        // into r12 to use as a per-thread scratch base (shape only this
        // function emits in v1.05.01 -- the 204h immediate is a
        // build-stable scratch-arena ID). Walk-back -0x2D to function
        // start. Anchors past the prologue entirely so a prologue-shape
        // shuffle does not sink P3.
        {"PrefabWrapperSwap_ApptResMgrInit_P3_TlsScratchSetup",
         "41 BC 04 02 00 00 65 48 8B 04 25 58 00 00 00 "
         "4C 03 20 41 8D 55 10 8D 4A 30",
         ResolveMode::Direct, -0x2D, 0},
    };

    /**
     * @brief ApptInnerLookup (sub_140350910) -- partprefab container
     *        hashtable lookup primitive. Pure read.
     *
     * Signature: `__int64(*)(table_struct*, key_wrapper_ptr_ptr*)`.
     * `table_struct` is `container + 0x70` -- the boot-loaded primary
     * hash table. Returns 0 on miss or `entry+0x10` on hit (a 24-byte
     * metadata payload pointer).
     *
     * IMPORTANT: this function has a byte-identical sibling clone at
     * `sub_1430C4880` (UI/render subsystem). Both implement the same
     * primitive but only `sub_140350910` is wired to the partprefab
     * `container+0x70` shape. P1 below leads with a RipRelative call-site
     * anchor (in `sub_140347BB0` which calls into the canonical clone)
     * because the function-prologue cascade alone matches BOTH copies.
     * Per feedback_aob_cascade_ordering: any P1 with hit count >=2
     * shadows unique fallbacks; the call-site anchor is unique and
     * dispatches the cascade past the clone problem entirely.
     *
     * If these break: re-find an xref to `sub_140350910` from
     * gameplay-side code (e.g. `sub_140347BB0` at +0x1A25 / +0x1B98 in
     * v1.05.01) and grab the 12-byte window before the `E8 disp32` for a
     * fresh RipRelative anchor.
     */
    inline constexpr AddrCandidate k_apptInnerLookupCandidates[] = {
        // P1 -- RipRelative resolve via call site at 0x1403495D5 inside
        // sub_140347BB0. Window: `mov ecx, [rax+disp32]` (the field-load
        // disp32 is wildcarded since it is a stable game-struct offset
        // but compiler-specific in encoding) + `add rcx, 0x70 ; mov rdx,
        // rbx ; call sub_140350910`. The `48 83 C1 70` is the literal
        // `+0x70` walk-offset that distinguishes the partprefab table from
        // sibling tables; it is a SEMANTIC constant kept literal. `8B 88`
        // = `mov ecx, [rax+disp32]`.
        {"PrefabWrapperSwap_ApptInnerLookup_P1_CallSiteRipRel",
         "8B 88 ?? ?? ?? ?? 48 83 C1 70 48 8B D3 E8 | ?? ?? ?? ??",
         ResolveMode::RipRelative, 14, 18},

        // P2 -- function prologue + early-return path. The prologue is
        // shared with one templated clone (`sub_1430C4880`). When P1
        // succeeds this is unused; when it fails this returns the FIRST
        // linear-scan match which is `sub_140350910` on v1.05.01 (the
        // canonical lookup primitive lives in the lower .text band). The
        // `48 83 EC 20` direct alloc + `83 79 04 00` field-zero compare
        // pin this function shape against unrelated functions.
        {"PrefabWrapperSwap_ApptInnerLookup_P2_FullPrologueEarlyExit",
         "4C 89 74 24 20 41 57 48 83 EC 20 "
         "83 79 04 00 4C 8B FA 4C 8B F1 75 0E "
         "33 C0 4C 8B 74 24 ?? 48 83 C4 20 41 5F C3",
         ResolveMode::Direct, 0, 0},

        // P3 -- prologue + post-early-exit field load. Walks one more
        // basic block past P2 into the `mov [rsp+arg_0], rbx ; mov rbx,
        // [rdx] ; mov [rsp+arg_10], rdi ; mov edi, [rbx+0xC]` shape. The
        // first `?? ` is the rsp disp8 of the saved-r14 spill (compiler-
        // owned). Same multi-hit caveat as P2 (clone shares this body).
        {"PrefabWrapperSwap_ApptInnerLookup_P3_PrologueBodyChain",
         "4C 89 74 24 20 41 57 48 83 EC 20 "
         "83 79 04 00 4C 8B FA 4C 8B F1 75 0E "
         "33 C0 4C 8B 74 24 ?? 48 83 C4 20 41 5F C3 "
         "48 89 5C 24 30 48 8B 1A 48 89 7C 24 40 8B 7B 0C",
         ResolveMode::Direct, 0, 0},
    };

    /**
     * @brief ApptStringIntern (sub_1403016B0) -- string-intern primitive.
     *
     * Signature: `handle_t(*)(const char* utf8)`. Lowercases nothing,
     * just returns the engine's interned-string handle expected by
     * `sub_141D38810` and `sub_140350910`. Returns 0 for null/empty
     * input.
     *
     * Like ApptInnerLookup, this function has a templated sibling clone
     * (linker-emitted from a header). The full prologue is unique in
     * v1.05.01 so P1 stays direct; P2 and P3 are body anchors that fall
     * back if the prologue shifts.
     *
     * If these break: re-anchor on the unique `48 C7 C3 FF FF FF FF`
     * (mov rbx, -1 = strlen-counter init) + the strncpy_s import-call
     * `FF 15 ?? ?? ?? ??` shape -- the import slot is a __ImageImpDir
     * entry whose location is build-stable.
     */
    inline constexpr AddrCandidate k_apptStringInternCandidates[] = {
        // P1 -- full prologue + null/empty short-circuit + strlen-loop
        // init. `48 C7 C3 FF FF FF FF` is `mov rbx, -1` -- strlen pre-
        // decrement counter. 1 hit on v1.05.01 .text.
        {"PrefabWrapperSwap_ApptStringIntern_P1_FullPrologue",
         "40 56 48 83 EC 20 "
         "48 8B F1 48 85 C9 74 52 "
         "80 39 00 74 4D "
         "48 89 5C 24 30 "
         "48 C7 C3 FF FF FF FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- mid-body anchor on the strlen-loop interior + the back-
        // jump (`75 F7` = jne -9 to walk to next byte while [rcx+rbx]
        // != 0). 1 hit on v1.05.01 .text. Walk-back -0x13 to function
        // start. Survives a prologue-shuffle that drops the `74 52` /
        // `74 4D` short-jumps in favour of `0F 84 rel32` (only the body
        // shape is anchored).
        {"PrefabWrapperSwap_ApptStringIntern_P2_StrlenLoopBody",
         "48 89 5C 24 30 48 C7 C3 FF FF FF FF "
         "48 89 7C 24 38 48 FF C3 80 3C 19 00 75 F7",
         ResolveMode::Direct, -0x13, 0},

        // P3 -- truncated prologue (no `mov rbx, -1`). Same head as P1
        // but stops one step earlier; survives a build that re-orders
        // the `mov [rsp+arg_0], rbx` / `mov rbx, -1` pair. Still unique
        // on v1.05.01 because the `74 52 ... 74 4D ... 48 89 5C 24 30`
        // null-empty-skip-then-spill sequence is function-specific.
        {"PrefabWrapperSwap_ApptStringIntern_P3_HeadShortPair",
         "40 56 48 83 EC 20 "
         "48 8B F1 48 85 C9 74 52 "
         "80 39 00 74 4D "
         "48 89 5C 24 30",
         ResolveMode::Direct, 0, 0},
    };

    /**
     * @brief StructCopy (sub_140352AA0) -- 0x40-byte struct-copy hotpath.
     *
     * Signature: `__int64(*)(dst, src)`. The function copies a partprefab
     * wrapper-related struct field-by-field. PrefabWrapperSwap installs
     * an inline hook here and (when LT-active) substitutes Kliff source
     * wrappers with target wrappers for the duration of the copy.
     *
     * Function reads the engine's StringInfo vtable sentinel
     * `0x145BC4638` via a `lea rax, [rip+disp32]` early in the body;
     * that single RIP-rel byte is wildcarded. All other bytes in the
     * patterns below are stable.
     *
     * If these break: the function's signature is `dst,src ->
     * mov [dst], 0 ; copy src->dst ; lea rax, [vtable] ; mov [src],
     * rax ; movzx-byte transfers from [src+8..src+0xA] into [dst+8..]`.
     * Re-anchor on the byte-transfer block (P3 below) -- it is the most
     * function-specific shape and the least likely to shuffle.
     */
    inline constexpr AddrCandidate k_structCopyCandidates[] = {
        // P1 -- full prologue + first qword copy + vtable load. Single
        // RIP-rel `lea rax, [rip+disp32]` wildcarded (loads
        // `0x145BC4638`). 1 hit on v1.05.01 .text.
        {"PrefabWrapperSwap_StructCopy_P1_FullPrologueWithVtable",
         "48 89 5C 24 18 48 89 6C 24 20 48 89 4C 24 08 "
         "56 57 41 56 48 83 EC 20 "
         "4C 8B F2 48 8B F1 33 ED 48 89 29 "
         "48 8B 02 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 02",
         ResolveMode::Direct, 0, 0},

        // P2 -- truncated prologue (no RIP-rel). Stops at `48 89 29`
        // (the `mov [rcx], rbp` zeroing of dst+0). Survives a future
        // build that moves the vtable lea later in the function. WARNING:
        // pattern fragment also matches `sub_140355210` (sibling). Hit
        // count is 1 on the canonical target only when the cascade
        // proceeds past P1 first; otherwise relies on
        // `sanity_check_function_prologue` post-resolve.
        {"PrefabWrapperSwap_StructCopy_P2_PrologueNoVtable",
         "48 89 5C 24 18 48 89 6C 24 20 48 89 4C 24 08 "
         "56 57 41 56 48 83 EC 20 "
         "4C 8B F2 48 8B F1 33 ED 48 89 29",
         ResolveMode::Direct, 0, 0},

        // P3 -- byte-transfer body anchor. The unique 4-byte payload
        // copy (`movzx eax, byte ptr [rdx+8/9/A] ; mov [rcx+8/9/A], al`
        // x3) plus the `mov eax, [rdx+0xC] ; mov [rcx+0xC], eax` dword
        // tail and the trailing `mov [rcx+0x10], rbp` zero-store. 1 hit
        // on v1.05.01 .text. Walk-back -0x2F to function start.
        // Patch-survival: the byte-by-byte transfer shape is what the
        // compiler emits when struct alignment is 1 (packed); it is a
        // strong tell of this exact function and is unlikely to shuffle.
        {"PrefabWrapperSwap_StructCopy_P3_ByteTransferBlock",
         "48 89 02 0F B6 42 08 88 41 08 "
         "0F B6 42 09 88 41 09 "
         "0F B6 42 0A 88 41 0A "
         "8B 42 0C 89 41 0C 48 89 69 10",
         ResolveMode::Direct, -0x2F, 0},
    };

    // -----------------------------------------------------------------------
    // ItemNameTable bounded-window anchor patterns (audit 2026-05-08 part 3).
    //
    // These are NOT cascades: they are pattern strings handed to
    // `DMK::Scanner::find_pattern` for a 0x40--0x80-byte LOCAL scan inside
    // a function whose start has already been resolved (via
    // `k_subTranslatorCandidates`). They live here to keep all byte-pattern
    // string literals in one place per the audit policy.
    //
    // The `|` glyph marks the point where `parse_aob` should compute its
    // pattern.offset for downstream `match + offset` arithmetic (DMK v3.0.2+
    // applies offset internally during find_pattern).
    //
    // Consumed by `ItemNameTable::resolve_chain` in item_name_table.cpp.
    // -----------------------------------------------------------------------

    /**
     * @brief Step-1 anchor inside SubTranslator (v1.05.00 encoding).
     *
     * Locates `call sub_141D45270` inside `sub_14076D950`. The second
     * `lea` encodes rsp-relative (`48 8D 4C 24 ??`, 4 bytes) instead of
     * v1.04's rbp-relative (`48 8D 4D ??`, 3 bytes). The disp8 slots are
     * wildcarded so a future stack-frame shift inside the same function
     * does not require another anchor variant.
     *
     * Used as the FIRST pattern in a 0x80-byte scan window. Anchor offset
     * `|` lands on the byte immediately after the `E8` opcode = start of
     * the call's disp32.
     */
    inline constexpr const char *k_nametableSubTxV105Anchor =
        "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8 | ?? ?? ?? ??";

    /**
     * @brief Step-1 anchor inside SubTranslator (v1.04.00 fallback).
     *
     * Older encoding where the second `lea` is rbp-relative. Tried after
     * the v1.05 anchor inside the same 0x80-byte window.
     */
    inline constexpr const char *k_nametableSubTxV104Anchor =
        "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8 | ?? ?? ?? ??";

    /**
     * @brief Step-3 anchor inside ItemAccessor (sub_1402D75D0).
     *
     * Locates `mov rbx, cs:qword_145CEF370` inside the 4th hop of the
     * name-table chain. The 6-byte prologue-tail anchor (push r14 + sub
     * rsp,0x40 + movzx edi,word ptr [rcx]) pins the call site inside a
     * bounded 0x40-byte scan of the function -- global uniqueness is not
     * required because the scan is locally bounded.
     *
     * Anchor offset `|` lands on the start of the `48 8B 1D disp32`
     * instruction. The disp32 is read by the consumer with `read_i32_safe`
     * at `match + 3` for RIP-relative resolution to qword_145CEF370.
     */
    inline constexpr const char *k_nametableItemAccessorAnchor =
        "41 56 48 83 EC 40 0F B7 39 | 48 8B 1D ?? ?? ?? ??";

    // -----------------------------------------------------------------------
    // DyeRecordInject function targets.
    //
    // The dye-injection module installs an inline detour on
    // `sub_141E019E0` (DyeCopier) that, post-trampoline, calls
    // `sub_140CADEF0` (DyeCopy) directly to APPEND 16 fabricated ARMOR_MOD
    // records to dst+120.
    //
    //   DyeCopier  : sub_141E019E0 -- inline detour. Post-trampoline appends
    //                                 16 dye records via DyeCopy primitive.
    //   DyeCopy    : sub_140CADEF0 -- 16-byte ARMOR_MOD record copy primitive.
    //                                 Resolved as a function pointer and
    //                                 called directly from the detour.
    //
    // Per feedback_aob_cascade_ordering: each P1 below is verified unique
    // (n=1) on v1.05.01 .text. The DyeCopy prologue alone matches 40 sites
    // (it is the engine's universal grow-and-emplace template); P1 there is
    // a body-shape anchor that locks onto the unique 16-byte record-copy
    // emitter (shl rcx,4 + add rcx,[rbx] + the field-by-field byte transfer
    // sequence). All other prologues are function-distinctive.
    //
    // If these break: each candidate's comment names the anchor offset
    // backed up to function start. Re-find the function in IDA, capture the
    // 24--40 byte window, wildcard volatile rel32 targets, and re-verify
    // uniqueness via mcp__ida-pro-mcp__find_bytes.
    // -----------------------------------------------------------------------

    /**
     * @brief DyeCopier (sub_141E019E0) -- per-slot dye-record copy driver.
     *
     * RVA 0x01E019E0 on v1.05.01. Signature
     * `__int64(*)(dst_iteminfo, src_iteminfo)` -- copies primary fields then
     * appends the 12-record dye vector at src+120 into dst+120 via
     * sub_140CADEF0. DyeRecordInject installs an inline detour here to
     * append 16 fabricated dye records post-trampoline (see
     * `dye_copier_inline_detour` in dye_record_inject.cpp).
     *
     * Prologue is highly distinctive: a 5-register save run
     * (rbp/rsi/rdi/r14/r15) followed by the field-by-field copy through the
     * first 0x60 bytes of the iteminfo struct. No RIP-relative bytes inside
     * the chosen anchor windows -- wildcards are not needed.
     */
    inline constexpr AddrCandidate k_dyeCopierCandidates[] = {
        // P1 -- full prologue + first three field copies. The
        // `48 8B F2 4C 8B F1` (mov rsi,rdx ; mov r14,rcx) arg-shuffle
        // followed by the qword/word/word field copies through [rdx+0..0xA]
        // is unique to this iteminfo-copy function. 1 hit on v1.05.01.
        {"DyeCopier_P1_FullPrologue",
         "48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 56 41 57 "
         "48 83 EC 20 48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-shuffle anchor on the field-copy chain. Walk-back
        // -0x15 to function start. Survives a future build that drops or
        // reorders the early callee-save pushes -- the field-copy shape is
        // the function-defining behaviour.
        {"DyeCopier_P2_FieldCopyChain",
         "48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08 66 89 41 08 "
         "0F B7 42 0A 66 89 41 0A 48 8B 42 10 48 89 41 10",
         ResolveMode::Direct, -0x15, 0},

        // P3 -- mid-body AVX xmm copy. The
        // `vmovups xmm0, [rdx+28h] ; vmovups [rcx+28h], xmm0` pair followed
        // by the `vmovsd` qword move and continued field copies is a unique
        // SSE/AVX shape this function emits at offset +0x49. Walk-back
        // -0x49 to function start. Anchors entirely past the prologue, so
        // a prologue-shape shuffle does not sink P3.
        {"DyeCopier_P3_AvxFieldCopy",
         "C5 F8 10 42 28 C5 F8 11 41 28 C5 FB 10 4A 38 C5 FB 11 49 38 "
         "0F B7 42 40 66 89 41 40 48 8B 42 48 48 89 41 48",
         ResolveMode::Direct, -0x49, 0},
    };

    /**
     * @brief DyeCopy (sub_140CADEF0) -- 16-byte ARMOR_MOD record-copy primitive.
     *
     * RVA 0x00CADEF0 on v1.05.01. Signature
     * `__int64(*)(vector_t* dst, const ArmorMod16* src)` -- grows dst's
     * 16-byte-stride array if needed, then writes one record by reading
     * fields from `[rdx+0..0xC]`. The breakthrough detour calls this
     * directly post-trampoline to append fabricated dye records.
     *
     * The function prologue is the engine's universal grow-and-emplace
     * template (40 byte-identical instances on v1.05.01). P1 is therefore a
     * body anchor that locks onto the unique 16-byte record-copy emitter
     * (shl rcx,4 ; add rcx,[rbx] ; the byte-by-byte transfer of channel /
     * R / G / B / 0xFF / repair_byte from [rdi+6..0xB] into [rcx+6..0xB]).
     * That shape is what makes this primitive the "ARMOR_MOD writer" rather
     * than a generic vector grow.
     *
     * If these break: re-anchor on the
     * `48 C1 E1 04 ; 48 03 0B ; 89 01` triplet (shift-by-4-stride +
     * add-base-pointer + write-hash-u32). That sequence is the function's
     * signature behaviour; a future build is unlikely to alter it without
     * also redesigning the ARMOR_MOD record layout.
     */
    inline constexpr AddrCandidate k_dyeCopyCandidates[] = {
        // P1 -- mid-prologue + capacity-check + grow-call chain. The
        // `8B 49 0C 8B 43 08 3B C8 77 ?? 8D 14 4D 01 00 00 00 03 D1 B9 01
        //  00 00 00 D1 EA 3B D1 0F 42 D1 48 8B CB 3B C2 0F 47 D0 E8` chain
        // anchors on the count/capacity load + the grow-size formula
        // `1 + count*2`, the lower-bound clamp via `cmovb`, the upper-bound
        // clamp via `cmova`, and the call to the underlying grow primitive.
        // The `8B 49 0C` is `mov ecx, [rcx+0xC]` reading the count field,
        // then the chain-into-grow reaches the unique 16-byte record copy.
        // The `77 ??` rel8 is wildcarded (jump distance compiler-owned).
        // 1 hit on v1.05.01 .text. Walk-back -0x10 to function start.
        {"DyeCopy_P1_GrowChainMidProlog",
         "8B 49 0C 8B 43 08 3B C8 77 ?? 8D 14 4D 01 00 00 00 03 D1 "
         "B9 01 00 00 00 D1 EA 3B D1 0F 42 D1 48 8B CB 3B C2 0F 47 D0 E8 "
         "?? ?? ?? ?? 8B 43 08 8B C8 8B 07 48 C1 E1 04 48 03 0B 89 01",
         ResolveMode::Direct, -0x10, 0},

        // P2 -- 16-byte record-copy emitter body. The `shl rcx,4 ;
        // add rcx,[rbx] ; mov [rcx],eax` triplet computes the next-record
        // byte address (count<<4 = 16-byte stride), then the byte-by-byte
        // transfers fan out: word `[rdi+4..5]` -> `[rcx+4..5]`, then
        // singles for channel (`+6`), R (`+7`), G (`+8`), B (`+9`). This
        // shape is what makes the function the ARMOR_MOD writer. 1 hit on
        // v1.05.01. Walk-back -0x43 to function start.
        {"DyeCopy_P2_ArmorModRecordCopy",
         "48 C1 E1 04 48 03 0B 89 01 0F B7 47 04 66 89 41 04 "
         "0F B6 47 06 88 41 06 0F B6 47 07 88 41 07 "
         "0F B6 47 08 88 41 08 0F B6 47 09 88 41 09",
         ResolveMode::Direct, -0x43, 0},

        // P3 -- tail of the byte-by-byte copy + post-write count++ + ret.
        // The trailing field transfers (`[rdi+0xB]` -> `[rcx+0xB]`,
        // `[rdi+0xC]` -> `[rcx+0xC]`) followed by `inc dword [rbx+8]`
        // (count++) and the standard `pop rdi ; ret` epilogue are unique
        // to this exact function shape. 1 hit on v1.05.01. Walk-back
        // -0x77 to function start. The `0F B6 47 0B 88 41 0B 0F B6 47 0C
        // 88 41 0C` is the last byte-pair of the record copy; `FF 43 08`
        // is the count increment that proves the dst is a vector with a
        // count field at +8.
        {"DyeCopy_P3_TailCountInc",
         "0F B6 47 0B 88 41 0B 0F B6 47 0C 88 41 0C "
         "FF 43 08 48 8B 5C 24 30 48 83 C4 20 5F C3",
         ResolveMode::Direct, -0x77, 0},
    };

    /**
     * @brief ColorPublisher (sub_142F59370) -- per-(dst, src) matInst
     *        publisher invoked from the matInst-list copy loop. ColorOverride
     *        installs a MidHook here so every dst matInst exposed during
     *        a transmog apply gets its content_hash and slot cached into
     *        MatInstOwner / CarrierSet for the setter substitute to query.
     *
     * The prologue saves seven callee-saved registers (rbp, rbx, rsi,
     * rdi, r12-r15) and frames with `lea rbp,[rsp-0x1F]; sub rsp,
     * 0xF8`. The wide saved-reg window plus the `lea rax, [rip+disp32]`
     * to a vtable constant make the prologue distinctive enough that
     * a 7-instruction window matches uniquely.
     */
    inline constexpr AddrCandidate k_colorPublisherCandidates[] = {
        // P1 -- full prologue. The stack-alloc imm32 (`F8 00 00 00`)
        // is wildcarded because the frame size can shift if locals
        // are added or removed in a patch. The trailing `48 8D 05`
        // (lea rax, [rip+disp32]) literal leads into a vtable RIP-
        // relative that we deliberately stop one byte before -- the
        // disp32 itself is volatile so we don't include it. 1 unique
        // hit on v1.06.
        {"ColorPublisher_P1_FullPrologue",
         "4C 89 44 24 18 48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 "
         "41 57 48 8D 6C 24 E1 48 81 EC ?? ?? ?? ?? 49 8B F8 4C 8B EA "
         "4C 8B F9 48 8D 05",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-saves frame anchor. Picks up at the `lea
        // rbp,[rsp-0x1F]; sub rsp, ???` pair followed by the arg-
        // reload triple (mov rdi,r8 / mov r13,rdx / mov r15,rcx).
        // Walk-back -0x16 (22 bytes) to function start.
        {"ColorPublisher_P2_PostSavesFrame",
         "48 8D 6C 24 E1 48 81 EC ?? ?? ?? ?? 49 8B F8 4C 8B EA 4C 8B F9 "
         "48 8D 05",
         ResolveMode::Direct, -0x16, 0},

        // P3 -- mid-body permutations-token canary XOR. The engine
        // computes `(prev_canary ^ al) & 1 ^ prev_canary` and stores
        // it back, then ORs 2 into the result. This bit-twiddle has
        // a unique fingerprint:
        //   movzx r10d,[rbp+X]   ; load prev canary
        //   xor   r10b, al        ; xor with new lsb
        //   and   r10b, 1         ; mask
        //   xor   r10b, [rbp+X]   ; flip prev
        //   mov   [rbp+X], r10b   ; store
        //   movzx eax, r10b       ; reload
        //   or    al, 2            ; mark "seen"
        //   mov   [rbp+X], al     ; store again
        // The disp8 frame slot reused throughout is a single byte
        // and is kept literal because the function reuses the same
        // local across all writes. 1 unique hit on v1.06. Walk-back
        // -0x43 to function start.
        {"ColorPublisher_P3_PermutCanaryXor",
         "44 0F B6 55 AF 44 32 D0 41 80 E2 01 44 32 55 AF "
         "44 88 55 AF 41 0F B6 C2 0C 02 88 45 AF",
         ResolveMode::Direct, -0x43, 0},
    };

    /**
     * @brief HostScope OwnerVfunc1 (sub_14204FD40) -- per-host owner-
     *        container vtable slot that invokes the matInst-list copy
     *        loop (sub_141026640) which in turn dispatches the publisher.
     *        Mid-hooked by ColorOverride::HostScope to capture rcx (the
     *        live owner container) for the player-vs-NPC election.
     *
     * The function is one of three byte-identical sibling thunks
     * (the other two at 0x1404F1710 and 0x1428EC250 share the entire
     * body except the inner `call rel32` disp32, which is unique per
     * thunk and therefore unusable as a stable anchor). The only
     * patch-stable way to single this thunk out is to anchor on the
     * preceding function's tail epilogue + alignment padding, then
     * walk forward into the prologue.
     */
    inline constexpr AddrCandidate k_hostScopeVfunc1Candidates[] = {
        // P1 -- preceding-function epilogue + 3-byte CC alignment +
        // full thunk prologue head. The preceding fn ends with the
        // semantic vector pop-back sequence:
        //   lea  eax,[rcx-1]
        //   mov  rbp,[rsp+0x48]
        //   mov  [rsi+8],eax        ; vec.count = vec.count - 1
        //   add  rsp, 0x20
        //   pop  rsi
        //   ret
        // followed by 3 bytes of `CC CC CC` padding and then the
        // thunk's prologue. 1 unique hit on v1.06. Walk forward
        // +0x14 (20 bytes) to thunk entry. Brittle if the preceding
        // function or its padding shifts under a patch; P2 / P3 give
        // closer-in fallbacks.
        {"HostScopeVfunc1_P1_PrevTailPadStart",
         "8D 41 FF 48 8B 6C 24 48 89 46 08 48 83 C4 20 5E C3 "
         "CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9",
         ResolveMode::Direct, +0x14, 0},

        // P2 -- shorter preceding-tail anchor starting at the
        // `mov [rsi+8],eax` store. Drops the `lea eax,[rcx-1]` head
        // so a patch that reuses an equivalent post-decrement pattern
        // still matches. 1 unique hit on v1.06. Walk forward +0xC
        // (12 bytes) to thunk entry.
        {"HostScopeVfunc1_P2_PrevPopCountPadStart",
         "89 46 08 48 83 C4 20 5E C3 "
         "CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9",
         ResolveMode::Direct, +0x0C, 0},

        // P3 -- minimal preceding-tail anchor starting at the `pop
        // rsi; ret`. Keeps just the 3-byte padding + the deep thunk
        // prologue (including the `test r9, r9` arg-null guard) for
        // disambiguation. 1 unique hit on v1.06. Walk forward +0x5
        // (5 bytes) to thunk entry.
        {"HostScopeVfunc1_P3_PrevRetPadStart",
         "5E C3 CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9 49 8B E8 48 8B DA 48 8B F1 4D 85 C9",
         ResolveMode::Direct, +0x05, 0},
    };

    /**
     * @brief HostScope OwnerVfunc2 (sub_142050690) -- sibling per-host
     *        owner-container vtable slot. Same role as Vfunc1 (capture
     *        rcx as the live owner container) but with a distinct
     *        prologue, so it admits a direct-prologue anchor without
     *        relying on the preceding function.
     */
    inline constexpr AddrCandidate k_hostScopeVfunc2Candidates[] = {
        // P1 -- full prologue. Spills 4 args (rbx, rbp, rsi, rdi),
        // pushes r14, allocates 0x60 of stack, then loads rbx <- rdx
        // and rdi <- rcx, zeros r14d, and tests r9b (the inline-call
        // optimization flag arg). The combination of 4 spilled args
        // + `41 56` push r14 + `48 83 EC 60` is what makes this
        // prologue distinctive vs siblings; 1 unique hit on v1.06.
        {"HostScopeVfunc2_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "41 56 48 83 EC 60 48 8B DA 48 8B F9 45 33 F6 45 84 C9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill + frame setup + arg flag test. Anchors
        // past the four 5-byte arg-home stores; the `74 6D` short jz
        // is the early-return when the flag arg is zero. Walk-back
        // -0x14 (20 bytes) to function start.
        {"HostScopeVfunc2_P2_PostArgSpillFlagTest",
         "41 56 48 83 EC 60 48 8B DA 48 8B F9 45 33 F6 45 84 C9 "
         "74 6D 48 8D 4C 24 38 E8 ?? ?? ?? ?? 90 48 8B 74 24 38",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- post first-call NOP + vtable-call into the inner
        // copier. The `90` is a single-byte alignment NOP that the
        // compiler emits between the `call rel32` and the next
        // instruction; `mov rsi,[rsp+0x38]` reloads the saved
        // argument. The `test rsi, rsi; jz` rejects null input. Walk-
        // back -0x32 (50 bytes) to function start.
        {"HostScopeVfunc2_P3_PostFirstCallNop",
         "90 48 8B 74 24 38 48 85 F6 74 ?? 48 8B 07 48 8D 53 08 "
         "48 85 DB 49 0F 44 D6 48 8B CF FF 90 20 03 00 00",
         ResolveMode::Direct, -0x32, 0},
    };

    /**
     * @brief PropertyByteSetter (sub_140A03810) -- 4-byte property
     *        descriptor's BYTE-variant write path. Mid-hooked by
     *        ColorOverride::SetterSubstitute so the engine's per-property
     *        color writes can be redirected to user-chosen RGB values.
     *
     * The function tests the descriptor's callback at `[rcx+0x78]`,
     * falls through to a 4-byte equality test when present, and tail-
     * jumps to a downstream writer on mismatch. A sibling at sub_14091CC90
     * shares the entire byte-compare body but reads property bytes as
     * DWORD (`41 8B 01`) instead of byte-per-byte (`41 0F B6 01`); that
     * single 4-byte opcode difference at offset +0x23 is the unique
     * discriminator and is kept literal in every candidate below.
     */
    inline constexpr AddrCandidate k_setterByteCandidates[] = {
        // P1 -- full prologue + first byte load. The `74 ??` rel8
        // jumps are kept literal because their distances (`66` and
        // `40`) form part of the unique fingerprint -- if a patch
        // changes them the next candidate covers the case. 1 unique
        // hit on v1.06.
        {"SetterByte_P1_FullPrologue",
         "48 8B 41 78 4D 8B C8 48 85 C0 74 66 45 33 C0 4C 8D 52 F8 "
         "48 85 D2 48 63 51 70 4D 0F 44 D0 85 D2 74 40 41 0F B6 01",
         ResolveMode::Direct, 0, 0},

        // P2 -- prologue with both rel8 jz distances wildcarded.
        // Keeps the `41 0F B6 01` byte-load discriminator literal.
        // Survives compiler reflows that shift the jz targets without
        // changing the body. 1 unique hit on v1.06.
        {"SetterByte_P2_WildcardedJumpDist",
         "48 8B 41 78 4D 8B C8 48 85 C0 74 ?? 45 33 C0 4C 8D 52 F8 "
         "48 85 D2 48 63 51 70 4D 0F 44 D0 85 D2 74 ?? 41 0F B6 01",
         ResolveMode::Direct, 0, 0},

        // P3 -- preceding function's epilogue (movzx return-byte +
        // 0x1060 stack restore + pop rbx + ret) + 2-byte CC padding
        // + setter prologue head. Anchors when both P1 and P2 lose
        // their `41 0F B6 01` discriminator (e.g., the engine merges
        // the byte and dword setters in a future patch). Walk forward
        // +0xE (14 bytes) to setter entry.
        {"SetterByte_P3_PrevTailPadStart",
         "0F B6 C3 48 81 C4 60 10 00 00 5B C3 "
         "CC CC "
         "48 8B 41 78 4D 8B C8 48 85 C0 74 66 45 33 C0 4C 8D 52",
         ResolveMode::Direct, +0x0E, 0},
    };

    /**
     * @brief ColorTokenInterner (sub_140F46680) -- shader-property name
     *        interner. Maps an ASCII property name (e.g. "_tintColorR")
     *        to a stable u32 token id used downstream by the dye/
     *        material setter pipeline. Called once per property by the
     *        TLS-guarded registrars sub_14274A3C0 and sub_142749F10.
     *
     * The function lives in the `.tls` section. Body is large (0x86E
     * bytes) and includes a once-only `lock cmpxchg` guarded init path
     * that allocates the hash table, sets the bucket-prime count
     * (0x8E = 142) and sentinel cap (0x2FFFF), and publishes the state
     * pointer to qword_145E15620. Subsequent calls take the table
     * lock, look up the name, and return either the existing token or
     * a freshly minted one.
     *
     * Resolution lets ColorOverride::InternerHook walk the body to
     * locate the `qword_145E15620 = v10` store and reach the
     * entries-array without scanning E8 trampolines through a
     * registrar call site.
     */
    inline constexpr AddrCandidate k_colorTokenInternerCandidates[] = {
        // P1 -- full Microsoft __fastcall prologue. The 4 shadow-store
        // saves (`mov [rsp+disp8], rbx/r8d/rdx/rcx`) wildcard their
        // disp8 home-area offsets because the prototype's argument
        // layout is the only thing that pins them. The 7-register
        // push run `55 56 57 41 54 41 55 41 56 41 57`
        // (rbp/rsi/rdi/r12/r13/r14/r15) is the distinctive head:
        // very few functions save all 7 callee-saved regs. The
        // `lea rbp,[rsp-disp8]` frame setup and `sub rsp,imm32`
        // stack allocation both wildcard compiler-owned sizes. The
        // trailing `41 8B F9` (mov edi, r9d) captures the sentinel-
        // cap argument into a saved scratch register and pins this
        // function against any other 7-push function.
        {"ColorTokenInterner_P1_FullPrologue",
         "48 89 5C 24 ?? 44 89 44 24 ?? 48 89 54 24 ?? 48 89 4C 24 ?? "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 41 8B F9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca early-exit anchor. Walks back -0x24 to
        // reach the function start. The chain (sub rsp / mov edi,r9d
        // / mov r12,rdx / xor ebx,ebx / mov [rcx],ebx / test rdx,rdx)
        // is the function's argument-validation preamble: it
        // captures the sentinel cap, mirrors the name pointer into
        // r12, zeroes the output token (`*a1 = 0`), then tests the
        // name pointer for null. The literal sequence `4C 8B E2 33
        // DB 89 19 48 85 D2` (mov r12,rdx; xor ebx,ebx; mov [rcx],
        // ebx; test rdx,rdx) is unique to this function's entry
        // contract. Survives prologue reflows that affect the register
        // save layout but keep the argument plumbing identical.
        {"ColorTokenInterner_P2_PostAllocaEarlyExit",
         "48 81 EC ?? ?? ?? ?? 41 8B F9 4C 8B E2 33 DB 89 19 48 85 D2",
         ResolveMode::Direct, -0x24, 0},

        // P3 -- deep-body cap-init magic-write anchor. Walks back
        // -0x126 from the matched site to reach the function start.
        // After the once-only `lock cmpxchg` init guard succeeds,
        // the function writes a four-constant fingerprint:
        //   C7 46 50 8E 00 00 00   mov [rsi+0x50], 0x8E   ; bucket prime
        //   C7 46 54 FF FF 02 00   mov [rsi+0x54], 0x2FFFF; sentinel cap
        //   BA F8 FF 2F 00         mov edx, 0x2FFFF8      ; alloc size
        //   41 B8 10 00 00 00      mov r8d, 0x10          ; entry stride
        // This semantic fingerprint survives wholesale prologue
        // rewrites (e.g., a future patch swapping the fastcall ABI
        // for a different register save list) because the constants
        // are dictated by the interner's data-structure contract, not
        // by compiler layout.
        {"ColorTokenInterner_P3_CapInitMagicWrite",
         "48 8B F3 C7 46 50 8E 00 00 00 C7 46 54 FF FF 02 00 "
         "BA F8 FF 2F 00 41 B8 10 00 00 00",
         ResolveMode::Direct, -0x126, 0},
    };

    /**
     * @brief Property-registration call-site walk patterns.
     *        Anchor the opcode run the compiler emits before every
     *        call into the ColorTokenInterner from the TLS-guarded
     *        registrar functions (sub_14274A3C0 for dye-mask
     *        properties, sub_142749F10 for tint and detail
     *        properties). Each pattern is INTENTIONALLY multi-match
     *        (one hit per property registration); the discovery
     *        walker enumerates every hit, decodes the `lea rdx`
     *        displacement to read the property name, and accepts
     *        only the entries whose strings appear in its known-
     *        property allow-list.
     *
     * The shared head is the 17-byte run:
     *
     *   41 B9 FF FF 02 00      mov  r9d, 0x2FFFF       ; sentinel cap
     *   ?? 8D ?? 01            lea  r8d, [reg+1]       ; REX+ModR/M
     *                                                  ; wild; reg is
     *                                                  ; r13 (registrar
     *                                                  ; A) or rdi
     *                                                  ; (registrar B)
     *   48 8D 15 ?? ?? ?? ??   lea  rdx, [rip+name]    ; property name
     *
     * Followed within a few bytes by an rcx-load and an `E8 disp32`
     * call to the interner. The three patterns below anchor on
     * progressively wider windows of the call site; together they
     * provide resilience against compiler reflows of any single
     * one. The walker scans with each pattern in turn and merges
     * the hits (dedup by decoded slot address), so a single
     * pattern losing its shape on a future patch is tolerated as
     * long as at least one survives.
     *
     * Verified hit counts on v1.06:
     *   P1 (head only):                       2235 module-wide
     *   P2 (head + lea-rcx-slot + call):      1512 module-wide
     *   P3 (head + mov-rcx-reg + call):          4 module-wide
     *
     * P1 is the canonical superset and matches every registration
     * site across the binary. P2 misses the first-call-per-
     * registrar entries (which load rcx from a preloaded table-
     * base register via `mov rcx, reg`); P3 covers exactly those
     * first-call entries. Walking all three lets the discoverer
     * stay correct even if P1 ever loses its shape, because the
     * union of P2 and P3 covers the same site set as P1.
     */
    inline constexpr std::array<std::string_view, 3>
        k_colorTokenRegistrarCallAobs = {{
            // P1 -- 17-byte literal head. Anchors on the run from
            // `mov r9d, 0x2FFFF` through `lea rdx, [name]`. The
            // REX+ModR/M of the `lea r8d, [reg+1]` is wildcarded
            // (the counter register differs by registrar; r13d for
            // sub_14274A3C0, rdi for sub_142749F10).
            "41 B9 FF FF 02 00 "
            "?? 8D ?? 01 "
            "48 8D 15 ?? ?? ?? ??",

            // P2 -- head + lea-rcx-slot + call tail. Captures
            // calls 2..N within each registrar (these load rcx
            // via `lea rcx, [rip+slot]` to the current property's
            // backing storage). Tighter than P1 and survives a
            // future compiler reflow that changes the head shape
            // as long as the rcx-load + call tail is preserved.
            "41 B9 FF FF 02 00 "
            "?? 8D ?? 01 "
            "48 8D 15 ?? ?? ?? ?? "
            "48 8D 0D ?? ?? ?? ?? E8",

            // P3 -- head + mov-rcx-reg + call tail. Captures the
            // first registration call per registrar function (it
            // loads rcx from a preloaded table-base register via
            // `mov rcx, rsi` / `mov rcx, rbx`). Only 4 module-wide
            // hits on v1.06 -- the two registrars plus two
            // unrelated callers that happen to share the shape
            // (filtered by the name allow-list).
            "41 B9 FF FF 02 00 "
            "?? 8D ?? 01 "
            "48 8D 15 ?? ?? ?? ?? "
            "48 8B ?? E8",
        }};

    /// Byte width of the literal head shared by all
    /// k_colorTokenRegistrarCallAobs candidates. The walker uses
    /// this to step the cursor past a matched anchor before
    /// scanning for the next hit.
    inline constexpr std::size_t k_colorTokenRegistrarCallAobHeadLen = 17;

    /// Number of walk-pattern variants in k_colorTokenRegistrarCallAobs.
    /// Exposed as a standalone constant so dependent compile-time
    /// expressions (std::array sizing, unrolled loops) do not have to
    /// rebind the array through a reference before reading its extent
    /// (MSVC declines to treat `.size()` on a `const auto&` alias as a
    /// constant expression even when the underlying global is
    /// `inline constexpr`).
    inline constexpr std::size_t k_colorTokenRegistrarCallAobCount =
        k_colorTokenRegistrarCallAobs.size();

} // namespace Transmog
