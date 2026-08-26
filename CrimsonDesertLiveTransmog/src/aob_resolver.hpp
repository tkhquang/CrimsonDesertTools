#pragma once

// Mod-local AOB candidate definitions plus the shared CDCore::Anchors aliases. DetourModKit's cascading scanner does
// the resolution itself. resolve_address() flattens the std::expected return into the uintptr_t-or-zero shape that the
// call sites use.
//
// Naming convention (unified across both mods):
//   <RoleName>_P<N>_<AnchorDescriptor>
// See cdcore/anchors.hpp for the full convention and authoring rules.

#include <cdcore/anchors.hpp>

#include <DetourModKit.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Transmog
{
    using AddrCandidate = DetourModKit::Scanner::AddrCandidate;
    using ResolveMode = DetourModKit::Scanner::ResolveMode;

    // Shared candidate tables (aliased by reference from CDCore so the single source of truth lives in
    // cdcore/anchors.hpp).
    inline constexpr auto &k_worldSystemCandidates = CDCore::Anchors::k_worldSystemCandidates;
    inline constexpr auto &k_mapLookupCandidates = CDCore::Anchors::k_mapLookupCandidates;
    inline constexpr auto &k_partAddShowCandidates = CDCore::Anchors::k_partAddShowCandidates;
    inline constexpr auto &k_visualEquipChangeCandidates = CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_vecCandidates = CDCore::Anchors::k_visualEquipChangeCandidates;
    inline constexpr auto &k_batchEquipCandidates = CDCore::Anchors::k_batchEquipCandidates;

    namespace detail
    {
        /**
         * @brief Resolves the first matching candidate from a cascade and returns the absolute address, or 0 on
         *        failure.
         * @details The underlying cascade already logs the success line. On failure this helper emits a single Warning
         *          so caller code can stay focused on conditional feature wiring. For call sites that need the precise
         *          ResolveError, call DetourModKit::Scanner::resolve_cascade directly.
         */
        [[nodiscard]] inline std::uintptr_t resolve_cascade_or_zero(std::span<const AddrCandidate> candidates,
                                                                    std::string_view label)
        {
            // Host-EXE scope: every target resolves inside CrimsonDesert.exe. Bounding the scan and the
            // require_unique count to Memory::host_module_range() stops a generic-shaped candidate from
            // first-matching inside a sibling mod or overlay elsewhere in the process image. This cascade keeps the
            // prologue-fallback variant. That variant re-matches a sibling-stomped prologue, and its rebuilt jump
            // destination stays unbounded, so it still recovers a trampoline outside the EXE.
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

    // --- LiveTransmog-only candidate tables ------------------------------

    /**
     * @brief SafeTearDown -- scene-graph tear-down that retires a matched part without mutating the authoritative
     *        equip table at *(a1+0x78). Used by the two-phase transmog apply in real_part_tear_down.
     *
     * P1 extends 3 bytes past the prologue to disambiguate from a sibling function. The 33-byte prologue alone is not
     * unique.
     */
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        // P1 -- full prologue. Stack-save disp32 (50 FF FF FF) and stack-alloc size (B0 01 00 00) wildcarded per
        // signing rules. The 41 0F B7 F8 (movzx edi, r8w) tail is function-specific.
        {"SafeTearDown_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor. Stack-alloc imm32 wildcarded.
        {"SafeTearDown_P2_PostAlloca", "48 81 EC ?? ?? ?? ?? 41 0F B7 F8 48 8B F1", ResolveMode::Direct, -0x1A, 0},

        // P3 -- entry-block chain walk, past the whole prologue. Shape: capture the this-pointer, load the component at
        // [this+0x08], take its sub-object at +0x68, take that object's list head at +0x40, and reject an empty list.
        // Every displacement here is a game-struct field, so this row does not depend on any compiler-owned value
        // except the branch target and the frame slot, which are both wildcarded. Anchors at function start + 0x25.
        {"SafeTearDown_P3_ComponentChainWalk",
         "48 8B F1 48 8B 41 08 48 8B 58 68 4C 8B 73 40 4D 85 F6 0F 84 ?? ?? ?? ?? 89 95 ?? ?? ?? ??",
         ResolveMode::Direct, -0x25, 0},
    };

    /**
     * @brief SubTranslator -- SlotPopulator item-id translator. Not hooked. It is the first hop of the chain that
     *        walks to the iteminfo global, which the mod uses to build the stable item-name table at init. See
     *        item_name_table.cpp for the full 4-step chain.
     */
    inline constexpr AddrCandidate k_subTranslatorCandidates[] = {
        // The second scratch-buffer lea flips encodings across builds: rsp-relative 5-byte (`48 8D 4C 24 ??`,
        // lea rcx,[rsp+X]) against rbp-relative 4-byte (`48 8D 4D ??`, lea rcx,[rbp-Y]). Everything else in the entry
        // block is stable in shape. P1 to P3 pin the current encoding for a precise match, and P4 anchors entirely
        // past that lea so one more flip cannot take the whole cascade down. If this cascade breaks, look at that
        // ModRM byte first.

        // P1 -- full prologue through the scratch-buffer preparation. Frame and stack-allocation sizes are wildcarded.
        {"SubTranslator_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
         "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor. Same body shape as P1 without the head sentinels. The anchor sits at function
        // start + 0x19, so the walk-back is -0x19.
        {"SubTranslator_P2_PostAlloca", "48 8B F9 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8",
         ResolveMode::Direct, -0x19, 0},

        // P3 -- deeper anchor: the argument-count load and lea pair, then the post-call tail. Anchors at function
        // start + 0x1C, so the walk-back is -0x1C.
        {"SubTranslator_P3_ScratchBufPrep",
         "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? "
         "90 48 8B D0 48 8B CF E8",
         ResolveMode::Direct, -0x1C, 0},

        // P4 -- post-call tail. This row sits fully past the scratch-buffer preparation block, so it survives another
        // flip of the lea addressing mode. Shape: the two chained calls, the `movzx ebx,ax` result capture, and the
        // `cmp bx,0xFFFF` sentinel test. Anchors at function start + 0x31, so the walk-back is -0x31.
        {"SubTranslator_P4_PostCallTail",
         "48 8B D0 48 8B CF E8 ?? ?? ?? ?? 0F B7 D8 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 66 83 FB FF",
         ResolveMode::Direct, -0x31, 0},
    };

    /**
     * @brief InitSwapEntry -- initializes the swap-entry structure to default sentinel values (-1 / 0). Called by the
     *        mod immediately before each SlotPopulator invocation. Find it through the SlotPopulator call sites:
     *        `call InitSwapEntry` immediately precedes `call SlotPopulator`.
     *
     * Signature (x64 __fastcall):
     *   __int64 InitSwapEntry(__int64 dest)
     *
     * The cascade resolves it by AOB rather than by a hardcoded RVA, so it survives code drift in earlier .text
     * sections across game patches. The compiler can fold `mov rax,-1 ; mov [rcx],rax` (7+3 bytes) into a single
     * `mov qword [rcx],-1` (`48 C7 01 FF FF FF FF`) and reshuffle the sentinel-store offsets. That fold breaks every
     * candidate that pins the unfolded form.
     */
    inline constexpr AddrCandidate k_initSwapEntryCandidates[] = {
        // P1 -- true prologue: mov [rsp+8],rcx / push rbx / sub rsp,0x20 / mov rbx,rcx / mov qword [rcx],-1 /
        // mov ecx,0xFFFF / mov [rbx+8],cx.
        {"InitSwapEntry_P1_FullPrologue",
         "48 89 4C 24 08 53 48 83 EC 20 48 8B D9 "
         "48 C7 01 FF FF FF FF B9 FF FF 00 00 66 89 4B 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- init-body anchor with no prologue head: mov qword [rcx],-1 / mov ecx,0xFFFF / mov [rbx+8],cx. Anchors
        // at function start + 0x0D. Survives a prologue reshuffle that leaves the sentinel writes intact.
        {"InitSwapEntry_P2_SentinelBody", "48 C7 01 FF FF FF FF B9 FF FF 00 00 66 89 4B 08", ResolveMode::Direct, -0xD,
         0},

        // P3 -- second half of the sentinel run, past both the prologue and the first sentinel writes. Shape: the
        // 64-bit -1 store at +0x30, then the zero stores at +0x38, +0x40, +0x48, +0x50 and +0x58, then the lea of the
        // inline sub-object at +0x60. Every value here is a struct field offset or a fixed sentinel, so this row holds
        // no compiler-owned bytes at all. Anchors at function start + 0x2B.
        {"InitSwapEntry_P3_SentinelRunTail",
         "48 C7 43 30 FF FF FF FF 48 89 53 38 66 89 53 40 "
         "48 89 53 48 48 89 53 50 66 89 53 58 48 8D 43 60",
         ResolveMode::Direct, -0x2B, 0},
    };

    /**
     * @brief SlotPopulator -- populates the slot array (a1+440) with item visual data then calls VisualEquipChange.
     *        This is the function the server equip handler invokes to trigger a full visual equip with mesh loading.
     *
     * Signature (x64 __fastcall):
     *   __int64 SlotPopulator(
     *       __int64 a1, unsigned __int16* a2_itemData, __int64 a3_swapEntry)
     *
     * a2 is a 16-byte structure:
     *   +0: uint16 item ID
     *   +2:  byte   flag (2 = normal equip)
     *   +4:  int32  (-1)
     *   +12: uint16 secondary slot (0xFFFF to skip)
     */
    // ClaimAssembler -- f(node, a2_source, a3_out, a4) publishes claims into the node's claim vector
    // (data node+0x58, count node+0x60, capacity node+0x64, stride 16). Appends one claim per part and per sub-part.
    //
    // Retained for future claim work. It fires for fresh-node assembly, NOT for player equipment changes, so it is
    // not a route to a slot the equip path refuses.
    inline constexpr AddrCandidate k_claimAssemblerCandidates[] = {
        // P1 -- the call sequence that defines this function: the part-list merge, then the post-merge consumer,
        // with both rel32 displacements wildcarded. Scan-verified unique. Offset -0x92 backs up to the entry.
        //
        // The prologue alone is NOT usable here: the three-spill + seven-push + large-frame shape matches six
        // functions, and 74 share the push sequence. A bare prologue never survives require_unique in this binary.
        {"ClaimAssembler_P1_MergeCallSeq",
         "4C 8D 45 D0 E8 ?? ?? ?? ?? 89 44 24 58 48 8B 45 D0 48 89 45 A8 8B 45 D8 89 45 B0 "
         "4C 8D 8D E0 1B 00 00 4C 8D 45 A8 48 8D 54 24 58 49 8B CC E8 ?? ?? ?? ?? 4C 8B C3",
         ResolveMode::Direct, -0x92, 0},

        // P2 -- prologue pinned by the exact frame size (0x5D00) and lea displacement, which is what makes it
        // unique. More patch-fragile than P1 since a frame-size change breaks it, hence second.
        {"ClaimAssembler_P2_PrologueFrameSize",
         "48 89 5C 24 10 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 00 A4 FF FF B8 00 5D 00 00",
         ResolveMode::Direct, 0, 0},
    };

    // ItemToSlotResolve -- f(a1, itemId) -> slot handle, or 0xFFFF when the item cannot be placed.
    //
    // This is the FIRST thing SlotPopulator does, and a 0xFFFF here makes it bail before equipping anything. It tries
    // an actor-side lookup, then falls back to a per-character item -> slot table. Armor carriers survive it with the
    // slot empty; earring carriers do not, and that difference is the whole reason accessory transmog needs something
    // already worn. Anchored so LT can ASK the engine which carriers resolve instead of inferring it from failures.
    inline constexpr AddrCandidate k_itemToSlotResolveCandidates[] = {
        // P1 -- full prologue: rbx spill, the WORD argument spill (`mov [rsp+10h], dx`), three pushes, the 0x110
        // frame, then `mov rdi, rcx` and the `mov r8d, 1` that seeds the key builder.
        {"ItemToSlotResolve_P1_FullPrologue",
         "48 89 5C 24 08 66 89 54 24 10 55 56 57 48 8D 6C 24 ?? 48 81 EC ?? ?? 00 00 "
         "48 8B F9 41 B8 01 00 00 00",
         ResolveMode::Direct, 0, 0},
    };

    // SlotTagToHandle -- f(a1, out_u16, slotTag, flag). Walks the 208-byte part records at a1+128, matches
    // `record+200 == slotTag`, and writes `record+8` (the slot HANDLE) to *out. 0xFFFF when the tag is not present.
    //
    // PartSlotRefresh takes its two slot arguments in DIFFERENT namespaces: the first is a tag (matched against
    // bucket keys and record+200), the second is a handle (dereferenced through a lookup). Passing a tag for the
    // second faults. This is how the handle is obtained.
    inline constexpr AddrCandidate k_slotTagToHandleCandidates[] = {
        // P1 -- full prologue: three spills, push rdi, the 0x20 frame, then the `mov rax,[rcx+80h]` that loads the
        // part-record container.
        {"SlotTagToHandle_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
         "48 8B 81 80 00 00 00 48 8B FA 48 8B E9",
         ResolveMode::Direct, 0, 0},

        // P2 -- container load plus the argument shuffle and the base/count reads. Offset backs up to entry.
        {"SlotTagToHandle_P2_ContainerLoad", "48 8B 81 80 00 00 00 48 8B FA 48 8B E9 48 8B 58 08 8B 40 10",
         ResolveMode::Direct, -0x14, 0},
    };

    // PartSlotRefresh -- the per-slot rebuild SlotPopulator calls last, as f(a1, slotA, slotB, swapEntry).
    //
    // Every record it builds is keyed by its SECOND argument, which SlotPopulator fills with the slot DERIVED FROM
    // THE ITEM. For a paired slot that derivation is identical for both halves (both rings are typeCode 0x000a, both
    // earrings 0x0008), so an apply to the second half filed its entry under the right slot and then rebuilt the
    // first one. Calling this directly with the intended slot in both argument positions is what refreshes the half
    // the engine would otherwise skip.
    inline constexpr AddrCandidate k_partSlotRefreshCandidates[] = {
        // P1 -- full prologue: the two stack spills, the distinctive `mov [rsp+18h], r8w` (a WORD-sized argument
        // spill, rare on its own), the five pushes, then the large-frame alloca setup.
        {"PartSlotRefresh_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 20 66 44 89 44 24 18 "
         "55 57 41 54 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 49 8B F1",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca shuffle: rsi=r9 (swapEntry), then the two WORD argument extractions and rcx=a1. Offset
        // backs up to the function start.
        {"PartSlotRefresh_P2_PostAlloca", "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1", ResolveMode::Direct, -0x2D, 0},
    };

    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        // P1 -- full prologue through the register shuffle (mov r12,rdx; mov r13,rcx; xor edi,edi).
        {"SlotPopulator_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? 00 00 00 "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor: register shuffle + the mov-edi-to-stack + mov r14d, -1 sentinel +
        // movzx eax, r14w. Offset -0x22 backs up to function start.
        {"SlotPopulator_P2_PostAlloca", "4C 8B E2 4C 8B E9 33 FF 89 7D ?? 41 BE FF FF FF FF 41 0F B7 C6",
         ResolveMode::Direct, -0x22, 0},

        // P3 -- deeper anchor on the mov r14d,-1 sentinel + the inline mov [rbp+6Fh],ax ; mov ebx,0FFFFh follow-up.
        // Skips the register shuffle entirely and pins the post-init block. Offset -0x2D backs up to function start.
        {"SlotPopulator_P3_SentinelInit", "41 BE FF FF FF FF 41 0F B7 C6 66 89 45 6F BB FF FF 00 00",
         ResolveMode::Direct, -0x2D, 0},
    };

    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module data globals.
    //
    // Each of the six PrefabWrapperSwap data globals resolves through a 3-candidate cascade rather than through a
    // hardcoded absolute address, so a patch that shifts the image layout self-corrects:
    //
    //   StringInfoRegistry  : registry struct.
    //   StringInfoVtable    : vtable sentinel filter.
    //   LoaderRegistry      : partprefab name->wrapper registry.
    //   ApptContainerVtable : partPrefabDataContainer vtable. Used by lookup gating in lookup_prefab_metadata.
    //   NaturalPipeline     : engine unlink pipeline. Hooked.
    //   ApptNameLookup      : name->wrapper primitive. Called direct.
    //
    // For data globals (registry/vtable) the cascade resolves through a RIP-relative mov/lea instruction in a
    // non-template caller. The disp32 is wildcarded, and the cascade returns the absolute target through
    // ResolveMode::RipRelative.
    //
    // For the two function targets the cascade uses Direct mode against the function prologue. Three independent
    // anchors per function let a future compiler shuffle the prologue without breaking resolution.
    //
    // Every P1 below must match exactly once module-wide. P2 and P3 must also be unique so the cascade can recover
    // when a future patch breaks P1.
    //
    // If these break, each candidate comment describes its anchor site structurally. Re-find the site in the
    // disassembler, locate the load instruction, copy the surrounding 16--32 bytes, wildcard the disp32, and verify
    // uniqueness with a module-wide byte scan.
    // -----------------------------------------------------------------------

    /**
     * @brief StringInfoRegistry global -- the StringInfo registry struct.
     *
     * +0x08 holds the count u32, and +0x58 holds the QWORD entry-array pointer. PrefabWrapperSwap walks this registry
     * to resolve prefab NAMES to entry wrapper-ptrs.
     *
     * The entry-array offset and the sub-object offset these candidates key on both track the width of the
     * pa::StaticInfoManager2 base. When that base changes width, the entry-array pointer moves (+0x50 against +0x58)
     * and the sub-object moves (+0x60 against +0x68). The count at +0x08 and the per-entry layout (+0x08 vtable,
     * +0x18 wrapper, +0x20 inline name) do NOT move with it. To re-derive the live sub-object displacement, count the
     * module-wide occurrences of `mov rcx,[rip+X]; add rcx,0x60` against `add rcx,0x68`. The live form has hundreds of
     * occurrences and the dead form has zero. Do NOT blanket-apply the width change to other registries. The sibling
     * LoaderRegistry container field moves the other way.
     *
     * The backing registry class is pa::StringInfoManager (StaticInfoManager2<...> family). The resolver re-finds the
     * live holder each launch, so no absolute address is recorded here.
     *
     * All three candidates anchor on a `mov reg, [rip+disp32]` that loads this address. The disp32 is wildcarded, and
     * the rest of the 16+ byte window is unique text module-wide.
     */
    inline constexpr AddrCandidate k_stringInfoRegistryCandidates[] = {
        // Every row wildcards the sub-object displacement (`48 83 C1 ??`). That single byte tracks the
        // pa::StaticInfoManager2 width, and a pinned form of it breaks P1 and P3 together. With it wildcarded each row
        // matches both encodings, so no build-pinned twin is needed.
        //
        // P1 and P3 key on two different call sites but on the same instruction shape, so a change to the
        // `add rcx, imm8` idiom takes both down together. P2 is the answer to that: it anchors on a call site
        // where the compiler interleaved an unrelated store between the load and the add, which gives the cascade one
        // row of a genuinely different shape.

        // P1 -- distinctive frame shape: load the char-table value, store it at [rbp+0x58], load the registry into
        // rcx, walk to the sub-object, lea rdx,[rbp+0x58], then call. The local-variable disp8 is kept literal to
        // disambiguate, and the trailing `E8` call opcode is pinned for the same reason.
        {"StringInfoRegistry_P1_LoadAddCallSite", "8B 45 B0 89 45 58 48 8B 0D ?? ?? ?? ?? 48 83 C1 ?? 48 8D 55 58 E8",
         ResolveMode::RipRelative, 9, 13},

        // P2 -- interleaved call site. Here the compiler scheduled the outbound `lea rdx,[rsp+X]` and a `mov
        // [rbx+0xD8],ax` field store BETWEEN the registry load and the `add rcx`, so the canonical three-instruction
        // window does not appear at all. The two struct displacements (0xD8, 0xDC) and the source field (+0x38) are
        // game-owned and carry the uniqueness budget; the stack slot and the sub-object displacement are wildcarded.
        {"StringInfoRegistry_P2_InterleavedFieldStore",
         "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 66 89 83 D8 00 00 00 "
         "48 83 C1 ?? 8B 47 38 89 83 DC 00 00 00",
         ResolveMode::RipRelative, 3, 7},

        // P3 -- conditional load and store shape: load r9d, test, jump, store a dword to [rbp+0x70], then load the
        // registry. The `8B 4F 74` (mov ecx,[rdi+0x74]) is a stable game-struct field offset. The rel8 jump distance
        // is wildcarded because it is a compiler-owned value.
        {"StringInfoRegistry_P3_CondLoadStore",
         "8B 4F 74 85 C9 74 ?? 89 4D 70 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 ?? 48 8D 55 70",
         ResolveMode::RipRelative, 13, 17},
    };

    /**
     * @brief StringInfoVtable sentinel (resolved at runtime; no hardcoded address -- an absolute vtable value goes
     *        stale on every game build).
     *
     * Vtable pointer used as the +0x08 sentinel of every StringInfo entry. PrefabWrapperSwap reads it to filter out
     * non-StringInfo heap rows during walk_string_info.
     *
     * The engine references this vtable from thousands of sites, so a row must carry enough caller-specific context to
     * stay unique. Each row below sits in a different function, so a single-function recompile cannot break all three.
     * Every row wildcards the RIP displacement and keeps only opcodes, ModRM bytes and game-owned field offsets.
     */
    inline constexpr AddrCandidate k_stringInfoVtableCandidates[] = {
        // P1 -- assign site inside the large StringInfo-assign method:
        // `mov rcx,[rbp+0xE8] ; call rel32 ; lea r15,[rip+vtable] ; mov [rbp+0xE8],r15 ; test bl,bl`.
        // The window opens one byte into the `48 8B 8D` load, after the REX prefix, because the frame slot and the
        // load width both change across builds. The trailing `84 DB` separates this site from a byte-identical
        // 0xE8-slot site that assigns a different vtable.
        {"StringInfoVtable_P1_LargeMethodAssign",
         "8B 8D E8 00 00 00 E8 ?? ?? ?? ?? 4C 8D 3D ?? ?? ?? ?? "
         "4C 89 BD E8 00 00 00 84 DB",
         ResolveMode::RipRelative, 14, 18},

        // P2 -- init block: `mov [rbp-0x50],eax ; xor r12d,r12d ; mov [rbp-0x48],r12 ; lea r13,[rip+vtable] ; nop`.
        // The trailing `90` is alignment padding and is kept literal. If a future build drops it, P3 covers the site.
        {"StringInfoVtable_P2_R13InitBlock",
         "89 45 B0 45 33 E4 4C 89 65 B8 4C 8D 2D ?? ?? ?? ?? 90 "
         "48 8B 45 C8",
         ResolveMode::RipRelative, 13, 17},

        // P3 -- grow-and-emplace head of a StringInfo vector:
        // `mov rdi,rcx ; mov ecx,[rcx+0x0C] ; mov eax,[rdi+0x08] ; lea r13,[rip+vtable] ; cmp ecx,eax ; ja rel32`.
        // The capacity and count loads at +0x0C and +0x08 are game-owned field offsets, and the compare that follows
        // them binds the vtable load to this exact grow check. The window stops on the `0F 87` opcode and excludes its
        // rel32, so a change of branch distance cannot break the row. This row shares no function with P1 or P2.
        {"StringInfoVtable_P3_GrowCheckLea", "48 8B F9 8B 49 0C 8B 47 08 4C 8D 2D ?? ?? ?? ?? 3B C8 0F 87",
         ResolveMode::RipRelative, 12, 16},
    };

    /**
     * @brief IteminfoHolder global: the engine's per-process iteminfo registry pointer.
     *
     * `*holder` dereferences to the registry struct. `*holder + 0x08` holds the u32 entry count and
     * `*holder + 0x58` the QWORD entry-array pointer. The runtime item-to-prefab bridge (itemmesh_dumper) reads it to
     * enumerate every loaded item descriptor.
     *
     * That entry-array displacement tracks the pa::StaticInfoManager2 base and moves when that base changes width.
     * Re-verify it on every patch day. The adjacent +0x50 slot still holds a valid-looking heap pointer into a
     * DIFFERENT array, so a stale displacement reads the wrong array instead of faulting. The failure is SILENT.
     *
     * The backing registry class is pa::ItemInfoManager. The resolver re-finds the live holder each launch, so no
     * absolute address is recorded here.
     *
     * Each candidate anchors on a `mov reg, [rip+disp32]` that loads this address inside a lookup primitive. The
     * primitive walks to a sub-object, leas a stack frame slot for the probe key, then calls. The disp32 is
     * wildcarded. The caller-specific stack-frame offsets and the tail instructions after the call carry the
     * uniqueness budget.
     *
     * WARNING for a future generalization. A sibling manager emits the same lookup shape and its holder sits eight
     * bytes away from this one. Wildcarding the sub-object displacement on a row whose remaining context is not
     * caller-specific makes that row match both managers, and both matches resolve to the wrong global. Widen the
     * caller context instead of loosening the operands.
     */
    inline constexpr AddrCandidate k_iteminfoHolderCandidates[] = {
        // P1 -- caller with a 0x378 frame. The inbound `mov eax,[rbp+0x378]` and the trailing
        // `mov [rbp+0x350],r12d` store bracket the lookup and pin the row to this one caller. The sub-object
        // displacement is kept literal here on purpose: see the sibling-manager warning above. The `74 04 44 0F B7 20`
        // tail (jz rel8 / movzx r12d,[rax]) is the post-call success branch.
        {"IteminfoHolder_P1_RdiFrame0x378Lookup",
         "8B 85 78 03 00 00 89 45 C0 48 8B 3D ?? ?? ?? ?? "
         "48 8D 55 C0 48 8D 4F 68 E8 ?? ?? ?? ?? 48 85 C0 74 04 "
         "44 0F B7 20 44 89 A5 50 03 00 00",
         ResolveMode::RipRelative, 12, 16},

        // P2 -- distinctive dword-copy preamble `mov eax,[rbp+0xB0] ; mov [rbp+0xA8],eax` precedes the load. The
        // canonical lookup-call sequence follows, with the same 0xA8 frame displacement echoed in the outbound lea,
        // then a jz success tail. Those two frame offsets are what make the row caller-specific, so the sub-object
        // displacement can safely be wildcarded here. That displacement tracks the pa::StaticInfoManager2 layout and
        // changes across builds.
        {"IteminfoHolder_P2_FrameB0CopyLookup",
         "8B 85 B0 00 00 00 89 85 A8 00 00 00 48 8B 0D ?? ?? ?? ?? "
         "48 83 C1 ?? 48 8D 95 A8 00 00 00 E8 ?? ?? ?? ?? "
         "48 85 C0 74",
         ResolveMode::RipRelative, 15, 19},

        // P3 -- caller with an rsp-relative frame (`mov [rsp+0x50], eax`; `lea rdx, [rsp+0x50]`) rather than an
        // rbp-based one. Its post-call success branch lands on a jz rel32 (`0F 84 ?? ?? ?? ??`) followed by a u16
        // sentinel check `0F B7 10 66 44 3B F2` (movzx edx,[rax]; cmp r14w,dx). That sentinel byte sequence is the
        // canonical "no match" probe of this family of lookups.
        {"IteminfoHolder_P3_RspFrameSentinelProbe",
         "8B 07 89 44 24 50 48 8B 0D ?? ?? ?? ?? 48 83 C1 ?? "
         "48 8D 54 24 50 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? "
         "0F B7 10 66 44 3B F2",
         ResolveMode::RipRelative, 9, 13},
    };

    /**
     * @brief StringinfoHolder global -- the engine's per-process string-bag registry pointer.
     *
     * Sibling to IteminfoHolder, located 0x30 bytes higher in the engine's data section. It backs the string-bag
     * registry (icon-prefab names, asset paths). The runtime item-to-prefab bridge (itemmesh_dumper) uses it to
     * translate the u16 stringSlot stored in `iteminfo[id].desc+0x90` into a c-string wrapper.
     *
     * The generic lookup primitive is `mov reg, [rip+disp32]; add reg, 0x68; lea rdx, [frame]; call`. Its sub-object
     * displacement tracks the pa::StaticInfoManager2 width, so every row below wildcards it and matches both shapes.
     * That primitive appears in dozens of callers for both holders. Each candidate below extends the window with
     * caller-specific bytes (frame offsets, register selectors, sentinel writes), so the cascade cannot drift from
     * stringinfo to iteminfo on a future rebuild. P1 uses an r15-targeted load (REX.WR = 4C). P2 and P3 use the more
     * common rcx-targeted form.
     */
    inline constexpr AddrCandidate k_stringinfoHolderCandidates[] = {
        // P1 -- caller distinguished by an r15-targeted load (`mov r15, [rip+disp32]`, REX.WR = 4C) instead of the
        // usual rcx target, paired with a `lea rcx, [r15+off]` (`49 8D 4F ??`) outbound. The trailing
        // `74 05 0F B7 30 EB 05` (jz rel8 / movzx esi,[rax] / jmp rel8) is the success/fail two-arm join unique to
        // this caller. The sub-object displacement is wildcarded because a one-byte change there (0x60 against 0x68)
        // is enough on its own to sink this row.
        {"StringinfoHolder_P1_R15LookupTwoArm",
         "89 45 58 4C 8B 3D ?? ?? ?? ?? 48 8D 55 58 49 8D 4F ?? "
         "E8 ?? ?? ?? ?? 48 85 C0 74 05 0F B7 30 EB 05",
         ResolveMode::RipRelative, 6, 10},

        // P2 -- caller whose preamble is `mov eax, [rsi+4]; mov [rbp+0x40], eax`, followed by the load and the
        // canonical lookup-call. The post-call tail `45 33 C0 48 85 C0 0F 84 ?? ?? ?? ?? 44 0F B7 38 B8 FF FF 00 00`
        // is the no-match sentinel write of 0xFFFF into r15w and is unique text module-wide.
        {"StringinfoHolder_P2_EsiPlus4SentinelWrite",
         "8B 46 04 89 45 40 48 8B 0D ?? ?? ?? ?? 48 83 C1 ?? "
         "48 8D 55 40 E8 ?? ?? ?? ?? 45 33 C0 48 85 C0 "
         "0F 84 ?? ?? ?? ?? 44 0F B7 38 B8 FF FF 00 00",
         ResolveMode::RipRelative, 9, 13},

        // P3 -- reordered call site. The compiler emitted the outbound `lea rdx,[rsp+X]` BEFORE the `add rcx`, then
        // spilled the key into that same stack slot, then hoisted the `mov edx,0xFFFF` no-match sentinel above the
        // result test. That ordering does not occur at the P1 or P2 sites, so this row does not share their failure
        // mode. The stack slot and the sub-object displacement are wildcarded; the sentinel constant is literal.
        {"StringinfoHolder_P3_ReorderedSentinelLoad",
         "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 83 C1 ?? 89 5C 24 ?? "
         "E8 ?? ?? ?? ?? BA FF FF 00 00 48 85 C0",
         ResolveMode::RipRelative, 3, 7},
    };

    /**
     * @brief LoaderRegistry singleton -- the engine partprefab name->wrapper registry.
     *
     * The ApptNameLookup primitive (also AOB-resolved) dereferences this singleton and queries [+0x50].
     * PrefabWrapperSwap reads it on init for heap-walk enumeration of prefab wrappers.
     */
    inline constexpr AddrCandidate k_loaderRegistryCandidates[] = {
        // WARNING for P1. The loose add-0xD0 window matches several sites in the module. Two of them are P1 and P1B,
        // one loads the correct global through `lea rdx,[r14+0x20]`, and one is a decoy that loads a different global.
        // P1 is separated from the r14 twin only by the lea's base register (`49 8D 55` for r13 against `49 8D 56` for
        // r14). That is a one-nibble difference in the operand class that register allocation moves. P1 matches once
        // today and passes require_unique, but one register reallocation turns it into a two-match ambiguity. If P1
        // fails on a future build, look for the r14 twin before you assume the site moved. P1B, whose three-lea
        // argument setup is far more distinctive, and P3 are the durable rows.
        //
        // Note also that this registry's container field (compared as +0x40058 in P3) and the sibling
        // pa::StaticInfoManager2 family (see k_stringInfoRegistryCandidates) move in OPPOSITE directions across
        // builds. Never blanket-apply a layout shift from one registry to another.

        // P1 -- distinctive 64-bit add-immediate `48 81 C1 D0 00 00 00` (add rcx, 0xD0) after the registry load. That
        // is a stable game-struct walk offset. See the r14-twin warning above.
        {"LoaderRegistry_P1_AddD0CallSite", "48 8B 0D ?? ?? ?? ?? 48 81 C1 D0 00 00 00 49 8D 55 20 E8",
         ResolveMode::RipRelative, 3, 7},

        // P1B -- second site in the same function, the four-argument call variant. Its three-lea argument setup
        // (`lea r9,[rbp-1]; lea r8,[r13+0x20]; lea rdx,[rbp-0x19]`) is the most distinctive shape of any row here,
        // which makes this the row to trust when P1 goes ambiguous.
        {"LoaderRegistry_P1B_AddD0FourArgCallSite",
         "48 8B 0D ?? ?? ?? ?? 48 81 C1 D0 00 00 00 4C 8D 4D FF 4D 8D 45 20 48 8D 55 E7 E8",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- `mov [rbp+0x17], r15; mov rsi, [rip+disp32]; add rsi, 0x70`. Both the spill displacement and the load
        // destination register are compiler-owned and move across builds, so this is the weakest row of the four.
        {"LoaderRegistry_P2_RsiReadAfterStore", "4C 89 7D 17 48 8B 35 ?? ?? ?? ?? 48 83 C6 70",
         ResolveMode::RipRelative, 7, 11},

        // P3 -- a STORE (`mov [rip+disp32], rbx`) that initializes the singleton at engine-init time, not a load. The
        // disp32 still resolves to the singleton address. Distinctive context: an inline `EB 03` short jump and the
        // container-field compare.
        {"LoaderRegistry_P3_InitStoreSite",
         "48 89 03 48 89 1D ?? ?? ?? ?? EB 03 48 8B DF "
         "48 3B 9E 58 00 04 00",
         ResolveMode::RipRelative, 6, 10},
    };

    /**
     * @brief ApptContainerVtable -- the partPrefabDataContainer vtable.
     *
     * The AppearanceTableLoader constructor allocates two containers and assigns each one its final vtable:
     *   a1[0] (_appearanceContainer)     -> the appearance-container vtable
     *   a1[1] (_partPrefabDataContainer) -> the partPrefabDataContainer vtable   <-- the target
     *
     * The real class is pa::ThreadSafeRefCountedContainerBase<staticstringA, AppearanceTableData, DefaultUserData>,
     * the appearance-table cache keyed by staticstringA. The resolver walks to the live vtable each launch, so no
     * absolute address is recorded here.
     *
     * The vtable write has a single xref, inside that constructor. The vtable-write pattern is a code-generator
     * template emitted for about nine sibling container types, so an anchor on the lea+mov[rdi] sequence alone is not
     * unique.
     *
     * Resolution strategy: AOB-resolve the constructor prologue, which IS unique, then the C++ resolver walks forward
     * through the function body to find the SECOND `48 8D 05 ?? ?? ?? ?? 48 89 07` pair. The FIRST pair is the
     * intermediate `_appearanceContainer` vtable, and the SECOND is the target `_partPrefabDataContainer` vtable. The
     * walk-forward logic runs inline inside prefab_wrapper_swap.cpp's `init()` after this anchor resolves.
     *
     * If this breaks, re-AOB the constructor by its prologue, then find the second `lea rax, [rip+disp32]; mov
     * [rdi], rax` pair inside the function. The byte offset of that pair moves across builds. The walk is bounded to
     * the function's first 0x400 bytes, which stops it from running off into the next function.
     */
    inline constexpr AddrCandidate k_apptLoaderCtorCandidates[] = {
        // P1 -- full prologue (8 callee-saved regs + frame setup). The 41 54 41 55 41 56 41 57 (push r12-r15) is the
        // largest possible callee-save set, typical of a 540-byte function with many locals. The prologue starts
        // directly with `48 89 4C 24 08` (mov [rsp+8], rcx). The compiler can add or drop a leading `48 89 54 24 10`
        // (mov [rsp+0x10], rdx) shadow-store ahead of it, which sinks this row. P2 and P3 cover that case.
        {"ApptLoaderCtor_P1_FullPrologue",
         "48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 "
         "48 83 EC 38 48 8B F1 45 33 F6 4C 89 31 4C 89 71 08 4C 89 71 10 "
         "4C 89 71 18 44",
         ResolveMode::Direct, 0, 0},

        // P2 -- mid-prologue + first field-init. Skips the `mov rcx,rdx mov rax,rcx` boilerplate; anchors on the first
        // XOR + the sequence of zero-stores into [rcx+0..0x18].
        {"ApptLoaderCtor_P2_FieldInitChain",
         "45 33 F6 4C 89 31 4C 89 71 08 4C 89 71 10 4C 89 71 18 "
         "44 88 71 20 49 8B 00 48 89 41 10",
         ResolveMode::Direct, -0x1D, 0},

        // P3 -- mid-body anchor on the unique field-init shape that copies a 32-byte payload from a3 into
        // a1+0x10..0x20: the 49 8B 00 / 48 89 41 10 / 49 8B 40 08 / 48 89 41 18 / 41 0F B6 40 10 sequence is the inline
        // copy of {qword,qword, byte} from *a3. Unique to this loader constructor. Walk-back -0x2F lands on
        // function start.
        {"ApptLoaderCtor_P3_PayloadCopy",
         "44 88 71 20 49 8B 00 48 89 41 10 49 8B 40 08 48 89 41 18 "
         "41 0F B6 40 10",
         ResolveMode::Direct, -0x2F, 0},
    };

    /**
     * @brief NaturalPipeline -- engine unlink function.
     *
     * PrefabWrapperSwap installs a MidHook here to substitute Kliff src wrappers with target wrappers in the engine's
     * unlink list (helm/cloak ghost cleanup).
     *
     * The function is a 6k-byte unlink pipeline that pushes all 8 callee-saved registers, so the prologue is highly
     * distinctive.
     */
    inline constexpr AddrCandidate k_naturalPipelineCandidates[] = {
        // P1 -- full prologue + chkstk preamble + post-alloca arg shuffle. The stack reservation size changes across
        // patches, which sinks any row that pins the chkstk B8 immediate or the lea displacement. Both are therefore
        // wildcarded, and the frame-independent `48 2B E0 4D 8B E8 48 8B DA 4C 8B F9 41 83 78` body shuffle
        // (a3->r13, a2->rbx, a1->rdi, cmp [a3+8]) carries the uniqueness.
        {"NaturalPipeline_P1_FullPrologueChkstk",
         "48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF "
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? "
         "48 2B E0 4D 8B E8 48 8B DA 4C 8B F9 41 83 78",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill prologue. Anchors past the first arg-home store onto the (wildcarded) lea rbp + chkstk
        // pair and the body shuffle. Walk-back -5 = past `48 89 5C 24 10` to start.
        {"NaturalPipeline_P2_PostArgSpill",
         "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF "
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? "
         "48 2B E0 4D 8B E8 48 8B DA 4C 8B F9 41 83 78",
         ResolveMode::Direct, -0x05, 0},

        // P3 -- chkstk size (wildcarded) + stack adjustment + arg-shuffle. The 48 2B E0 (sub rsp, rax) is the
        // conventional __chkstk post-call. Walk-back -0x27 to function start.
        {"NaturalPipeline_P3_PostChkstkArgShuffle",
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 4D 8B E8 48 8B DA "
         "4C 8B F9 41 83 78",
         ResolveMode::Direct, -0x27, 0},
    };

    /**
     * @brief ApptNameLookup -- name->wrapper primitive.
     *
     * PrefabWrapperSwap calls this directly (not hooked) to resolve partprefab names to wrapper-ptrs. It lowercases
     * the name, interns it, queries the LoaderRegistry singleton at +0x50, and returns entry+8 on hit or 0 on miss.
     */
    inline constexpr AddrCandidate k_apptNameLookupCandidates[] = {
        // P1 -- full prologue + frame setup + first registry load. The 48 8D 6C 24 A0 (lea rbp,[rsp-0x60]) and 48 81 EC
        // 60 01 (sub rsp, 0x160) form a unique 0x160-byte stack frame. The immediately-following `mov rsi,
        // [rip+disp32]` loads the LoaderRegistry singleton (also resolved separately).
        {"ApptNameLookup_P1_FullPrologue",
         "48 89 5C 24 10 48 89 4C 24 08 55 56 57 "
         "48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF 48 89 7D 28",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill, before frame setup. Walk-back -10 to function start.
        {"ApptNameLookup_P2_PostArgSpill",
         "55 56 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF",
         ResolveMode::Direct, -0x0A, 0},

        // P3 -- frame setup + xor edi,edi + the local-var inits. The 48 C7 45 30 05 01 00 00 (mov [rbp+0x30], 0x105) is
        // a semantic constant (initial buffer capacity) -- stable. Walk-back -13 to function start.
        {"ApptNameLookup_P3_LocalInitConstant",
         "48 8D 6C 24 A0 48 81 EC 60 01 00 00 "
         "48 8B 35 ?? ?? ?? ?? 33 FF 48 89 7D 28 "
         "48 C7 45 30 05 01 00 00",
         ResolveMode::Direct, -0x0D, 0},
    };




    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module function targets.
    //
    // Four PrefabWrapperSwap function-target cascades, each with a 3-anchor cascade per the ordering rule in
    // CrimsonDesertCore/external/DetourModKit/docs/misc/aob-signatures.md.
    //
    //   ApptResMgrInit  : one-shot capture hook target. Reads ResMgr/loader/container.
    //   ApptInnerLookup : partprefab container hashtable lookup primitive. Pure read.
    //   ApptStringIntern: StringInfo string-intern primitive callable as `handle_t(const char*)`.
    //   StructCopy      : 0x40-byte struct-copy hot path that PrefabWrapperSwap inline-hooks to swap source
    //                     wrapper-ptrs.
    //
    // Verify every row's hit count against the live module before you ship a change. Where a function has a sibling
    // clone (a linker-emitted duplicate compiled from a templated header) and no global anchor is unique, the cascade
    // leads with a RipRelative call-site anchor. That anchor walks an `E8 disp32` from a known caller, which IS
    // unique, to the canonical target.
    // -----------------------------------------------------------------------

    /**
     * @brief ApptResMgrInit -- one-shot capture hook target.
     *
     * Outer ResMgr-init function. PrefabWrapperSwap installs an inline entry hook that runs the trampoline and then
     * snapshots ResMgr at a1[5] (a1+0x28), the loader at ResMgr+0x58, and the partprefab container at loader+0x08. The
     * hook is one-shot, and subsequent calls are pass-throughs.
     *
     * This is a lean re-initializer, not a big-prologue function. The prologue is `48 89 5C 24 10 / 48 89 74 24 18 /
     * 48 89 4C 24 08 / 57 / 48 83 EC 20 / 48 8B D9 (mov rbx,a1) / 33 FF`, followed by three `48 89 3D` RIP-relative
     * global zero-stores and a `mov rcx,[a1+0x80]; test; jz` teardown walk of the member chain a1+0x80..a1+0x28
     * (a1+0x28 is the ResMgr the hook snapshots). The per-thread scratch id (mov r12d,<id>) is not build-stable, so
     * this cascade avoids a scratch/TLS body anchor. Re-anchor on the prologue + global-zero run (P1/P2) or on the
     * member-teardown offset cascade 0x80->0x78->0x70 (P3).
     */
    inline constexpr AddrCandidate k_apptResMgrInitCandidates[] = {
        // P1 -- full prologue (3 arg-home stores + push rdi + 0x20 frame + mov rbx,a1 + xor edi,edi) extending into
        // the three RIP-rel global zero-stores and the first member-teardown test. The RIP-rel disp32s are
        // wildcarded. The `48 8B 89 80 00 00 00 / 48 85 C9 / 74 0C` (mov rcx,[a1+0x80]; test; jz) tail pins
        // uniqueness.
        {"PrefabWrapperSwap_ApptResMgrInit_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 48 89 4C 24 08 57 "
         "48 83 EC 20 48 8B D9 33 FF "
         "48 89 3D ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? "
         "48 8B 89 80 00 00 00 48 85 C9 74 0C",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill variant: drops the leading rbx/rsi home stores so a future reorder of the spill block
        // does not sink it. Walk-back -0x0A = past the first two home stores to start.
        {"PrefabWrapperSwap_ApptResMgrInit_P2_PostArgSpill",
         "48 89 4C 24 08 57 48 83 EC 20 48 8B D9 33 FF "
         "48 89 3D ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? 48 89 3D ?? ?? ?? ?? "
         "48 8B 89 80 00 00 00 48 85 C9 74 0C",
         ResolveMode::Direct, -0x0A, 0},

        // P3 -- body anchor on the member-teardown offset cascade 0x80 -> 0x78 -> 0x70 (mov rcx,[a1+0x80]; cond-dtor;
        // zero [a1+0x80]; lea [a1+0x78]; dtor; mov rcx,[a1+0x70]). This object layout is function-specific. The
        // alternative scratch/TLS idiom is NOT unique here -- it matches many generic sites module-wide -- so this row
        // anchors on the member layout instead. Call disp32s wildcarded. Walk-back -0x2E to start.
        {"PrefabWrapperSwap_ApptResMgrInit_P3_MemberTeardown",
         "48 8B 89 80 00 00 00 48 85 C9 74 0C E8 ?? ?? ?? ?? "
         "48 89 BB 80 00 00 00 48 8D 4B 78 E8 ?? ?? ?? ?? 90 48 8B 4B 70",
         ResolveMode::Direct, -0x2E, 0},
    };

    /**
     * @brief ApptInnerLookup -- partprefab container hashtable lookup primitive. Pure read.
     *
     * Signature: `__int64(*)(table_struct*, key_wrapper_ptr_ptr*)`. `table_struct` is `container + 0x70` -- the
     * boot-loaded primary hash table. Returns 0 on miss or `entry+0x10` on hit (a 24-byte metadata payload pointer).
     *
     * IMPORTANT: this function has a byte-identical sibling clone in the UI and render subsystem. Both implement the
     * same primitive, but only one of them is wired to the partprefab container. A prologue or body anchor therefore
     * matches BOTH copies and can never pass require_unique. Every row below is instead a RIP-relative CALL-SITE
     * anchor: it signs the caller's argument setup, which differs per caller, and resolves the call target. That is
     * what keeps each row unique and keeps the clone out of the result.
     *
     * If these break, find any call to the function from gameplay-side code, take the ten to twenty byte window that
     * ends on the `E8 disp32`, wildcard the frame displacements, and verify one match module-wide.
     */
    inline constexpr AddrCandidate k_apptInnerLookupCandidates[] = {
        // P1 -- RipRelative resolve through a gameplay-side call site. Window: `mov ecx, [rax+disp32]` (the field-load
        // disp32 is wildcarded since it is a stable game-struct offset but compiler-specific in encoding) +
        // `add rcx, 0x70 ; mov rdx, rbx ; call ApptInnerLookup`. The `48 83 C1 70` is the literal `+0x70` walk-offset
        // that distinguishes the partprefab table from sibling tables. It is a SEMANTIC constant and stays literal.
        // `8B 88` = `mov ecx, [rax+disp32]`.
        {"PrefabWrapperSwap_ApptInnerLookup_P1_CallSiteRipRel",
         "8B 88 ?? ?? ?? ?? 48 83 C1 70 48 8B D3 E8 | ?? ?? ?? ??", ResolveMode::RipRelative, 14, 18},

        // P2 -- second call site in the same caller. Window: `mov rcx,[rax+0xA0] ; add rcx,0x50 ; lea rdx,[rbp+X] ;
        // call`. It queries a different table of the same container, so its walk offset is 0x50 and not 0x70. That
        // does not matter for resolution: the cascade needs the CALL TARGET, and both sites call the same primitive.
        // The frame displacement is wildcarded; the two struct offsets carry the uniqueness budget.
        {"PrefabWrapperSwap_ApptInnerLookup_P2_SecondCallSite",
         "48 8B 88 A0 00 00 00 48 83 C1 50 48 8D 95 ?? ?? ?? ?? E8 | ?? ?? ?? ??", ResolveMode::RipRelative, 19, 23},

        // P3 -- call site in an unrelated caller, so a rewrite of the caller that P1 and P2 share cannot take all
        // three rows down at once. Window: the zeroed third argument, both argument leas, the call, the result
        // capture into rbx, and the follow-on lea and call. The two frame displacements are wildcarded; the shape of
        // the pair of chained calls is what makes it unique.
        {"PrefabWrapperSwap_ApptInnerLookup_P3_ForeignCallerCallSite",
         "45 33 C0 48 8D 55 ?? 48 8D 8D ?? ?? ?? ?? E8 | ?? ?? ?? ?? "
         "48 8B D8 48 8D 8D ?? ?? ?? ?? E8",
         ResolveMode::RipRelative, 15, 19},
    };

    /**
     * @brief ApptStringIntern -- string-intern primitive.
     *
     * Signature: `handle_t(*)(const char* utf8)`. It lowercases nothing. It returns the engine's interned-string
     * handle that ApptNameLookup and ApptInnerLookup expect. Returns 0 for null/empty input.
     *
     * Like ApptInnerLookup, this function has a templated sibling clone (linker-emitted from a header). The full
     * prologue is unique, so P1 stays direct. P2 and P3 are body anchors that take over if the prologue shifts.
     *
     * If these break, re-anchor on the unique `48 C7 C3 FF FF FF FF` (mov rbx, -1 = strlen-counter init) + the
     * strncpy_s import-call `FF 15 ?? ?? ?? ??` shape. The import slot is a __ImageImpDir entry whose location is
     * build-stable.
     */
    inline constexpr AddrCandidate k_apptStringInternCandidates[] = {
        // P1 -- full prologue + null/empty short-circuit + strlen-loop init. `48 C7 C3 FF FF FF FF` is `mov rbx, -1` --
        // strlen pre-decrement counter. One match module-wide.
        {"PrefabWrapperSwap_ApptStringIntern_P1_FullPrologue",
         "40 56 48 83 EC 20 "
         "48 8B F1 48 85 C9 74 52 "
         "80 39 00 74 4D "
         "48 89 5C 24 30 "
         "48 C7 C3 FF FF FF FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- mid-body anchor on the strlen-loop interior + the back-jump (`75 F7` = jne -9 to walk to next byte
        // while [rcx+rbx] != 0). One match module-wide. Walk-back -0x13 to function start. Survives a
        // prologue-shuffle that drops the `74 52` / `74 4D` short-jumps in favor of `0F 84 rel32`, because this row
        // anchors on the body shape only.
        {"PrefabWrapperSwap_ApptStringIntern_P2_StrlenLoopBody",
         "48 89 5C 24 30 48 C7 C3 FF FF FF FF "
         "48 89 7C 24 38 48 FF C3 80 3C 19 00 75 F7",
         ResolveMode::Direct, -0x13, 0},

        // P3 -- truncated prologue (no `mov rbx, -1`). Same head as P1 but stops one step earlier; survives a build
        // that re-orders the `mov [rsp+arg_0], rbx` / `mov rbx, -1` pair. Still unique because the `74 52
        // ... 74 4D ... 48 89 5C 24 30` null-empty-skip-then-spill sequence is function-specific.
        {"PrefabWrapperSwap_ApptStringIntern_P3_HeadShortPair",
         "40 56 48 83 EC 20 "
         "48 8B F1 48 85 C9 74 52 "
         "80 39 00 74 4D "
         "48 89 5C 24 30",
         ResolveMode::Direct, 0, 0},
    };

    /**
     * @brief StructCopy -- 0x40-byte struct-copy hotpath.
     *
     * Signature: `__int64(*)(dst, src)`. The function copies a partprefab wrapper-related struct field-by-field.
     * PrefabWrapperSwap installs an inline hook here and (when LT-active) substitutes Kliff source wrappers with target
     * wrappers for the duration of the copy.
     *
     * The function reads the engine's StringInfo vtable sentinel through a `lea rax, [rip+disp32]` early in the body.
     * That single RIP-rel displacement is wildcarded. All other bytes in the patterns below are stable.
     *
     * If these break, note that the function's shape is `dst,src -> mov [dst], 0 ; copy src->dst ; lea rax, [vtable] ;
     * mov [src], rax ; movzx-byte transfers from [src+8..src+0xA] into [dst+8..]`. Re-anchor on the byte-transfer
     * block (P3 below). It is the most function-specific shape and the least likely to shuffle.
     */
    /**
     * @brief Part-list assembly function -- merges an actor's part lists into its render container.
     *
     * Signature `f(a1 = assembly node, a2 = container, a3 = destination container)`.
     *
     * LT hooks this purely to learn WHICH actor the following struct-copy calls belong to. The node at `a1` carries
     * its appearance asset path at `+0x18` (a StringInfo wrapper), which
     * `CDCore::classify_appearance_by_path` maps to a protagonist index. The struct-copy chokepoint itself receives
     * only a staging-vector slot as its first argument, so it has no actor identity of its own.
     *
     * The `lea rbp, [rsp-disp32]` frame size, the chkstk immediate and its call displacement are wildcarded so a
     * frame-size change does not invalidate the anchor.
     */
    inline constexpr AddrCandidate k_partListMergeCandidates[] = {
        // P1 -- prologue through the argument shuffle. The register-save block ALONE is not unique (the 7-push plus
        // large-frame-lea shape matches double digits of functions module-wide), so the pattern deliberately runs on
        // through chkstk into `mov r14,r8 / mov r12,rdx / mov r13,rcx`, which is what makes it a single hit.
        {"PartListMerge_P1_PrologueThroughArgShuffle",
         "48 89 5C 24 20 48 89 54 24 10 "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 "
         "4D 8B F0 4C 8B E2 4C 8B E9",
         ResolveMode::Direct, 0, 0},

        // P2 -- argument shuffle plus the three-way count sum that sizes the destination reserve
        // (`list2count + list1count + a2count`). Unique on its own and independent of the prologue, so it survives a
        // register-save reshuffle that P1 would miss. It matches INSIDE the function, so disp_offset walks back to the
        // entry; re-measure that delta on patch day, and note the caller's prologue sanity check is what catches it
        // when the delta drifts.
        {"PartListMerge_P2_ArgShuffleCountSum",
         "4D 8B F0 4C 8B E2 4C 8B E9 "
         "8B 51 60 03 51 48 41 03 54 24 08",
         ResolveMode::Direct, -0x2A, 0},
    };

    /**
     * @brief UnlinkByWrapper -- direct unlink-a-single-wrapper-from-a-body primitive.
     *
     * Signature `__int64 __fastcall(parent, _QWORD **wrapper, a3, a4)`. **`a3` / `a4` are optional out-vectors and
     * are safe to pass 0.** Returns the number of records unlinked.
     *
     * Walks the body's attached-record vector (`parent+0x58` data, `parent+0x60` count; each entry's `+0x08` leads to
     * a record whose `+0x40` holds the identity wrapper), exact-matches against `**a2`, and swap-and-pop unlinks every
     * match.
     *
     * NaturalPipeline calls this per input wrapper after its router step. Calling it DIRECTLY is what lets LT evict a
     * specific stale visual without synthesizing a NaturalPipeline call (which would need two simultaneously-valid
     * input lists) and without driving `SafeTearDown`.
     */
    inline constexpr AddrCandidate k_unlinkByWrapperCandidates[] = {
        // P1 -- prologue through the argument shuffle. The two argument spills (`rbx` to `rsp+0x10`, `r9` to
        // `rsp+0x20`) plus the 7 pushes and the `mov r15,r8 / mov rdi,rdx` tail make this a single hit. Verified
        // count == 1 against the live image; the bare prologue alone is NOT unique in this binary.
        {"UnlinkByWrapper_P1_PrologueThroughArgShuffle",
         "48 89 5C 24 10 4C 89 4C 24 20 "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 83 EC 70 4D 8B F8 48 8B FA",
         ResolveMode::Direct, 0, 0},

        // P2 -- the record-vector scan head, which encodes the structure rather than the frame: load `a1+0x58`, load
        // `a1+0x60`, scale by 16, form the end pointer, and bail when empty. Survives a prologue reshuffle. Matches
        // INSIDE the function, so disp_offset walks back to the entry; re-measure that delta on patch day, and note
        // the caller's prologue check is what catches it when it drifts.
        {"UnlinkByWrapper_P2_RecordVectorScanHead",
         "4C 8B 51 58 44 8B 59 60 49 C1 E3 04 4D 03 DA",
         ResolveMode::Direct, -0x22, 0},
    };

    inline constexpr AddrCandidate k_structCopyCandidates[] = {
        // P1 -- full prologue + first qword copy + vtable load. The single RIP-rel `lea rax, [rip+disp32]` that loads
        // the StringInfo vtable sentinel is wildcarded. One match module-wide.
        {"PrefabWrapperSwap_StructCopy_P1_FullPrologueWithVtable",
         "48 89 5C 24 18 48 89 6C 24 20 48 89 4C 24 08 "
         "56 57 41 56 48 83 EC 20 "
         "4C 8B F2 48 8B F1 33 ED 48 89 29 "
         "48 8B 02 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 02",
         ResolveMode::Direct, 0, 0},

        // A truncated-prologue tier is not possible here. A prologue anchor without the vtable lea matches a sibling
        // copier and several byte-identical prologue copies in system DLL .text sections, so any short-prologue window
        // returns more than one hit. P2 therefore anchors deeper in the body instead of higher in the prologue.

        // P2 -- pointer-move block that follows the packed byte transfer. Shape: read the +0x10 pointer out of the
        // source, write it to the destination, null the source slot, null the destination +0x18 slot, then move the
        // +0x18 pointer across. This ownership-transfer idiom (copy across, then clear the source) is what makes the
        // window unique, and it holds no compiler-owned bytes at all. Anchors at function start + 0x51.
        {"PrefabWrapperSwap_StructCopy_P2_PointerMoveBlock",
         "48 8B 42 10 48 89 41 10 48 89 6A 10 "
         "48 89 69 18 48 8B 42 18 48 89 41 18",
         ResolveMode::Direct, -0x51, 0},

        // P3 -- byte-transfer body anchor. The unique 4-byte payload copy (`movzx eax, byte ptr [rdx+8/9/A] ; mov
        // [rcx+8/9/A], al` x3) plus the `mov eax, [rdx+0xC] ; mov [rcx+0xC], eax` dword tail and the trailing `mov
        // [rcx+0x10], rbp` zero-store. One match module-wide. Walk-back -0x2F to function start.
        // Patch-survival: the byte-by-byte transfer shape is what the compiler emits when struct alignment is 1
        // (packed). It is a strong tell of this exact function and is unlikely to shuffle.
        {"PrefabWrapperSwap_StructCopy_P3_ByteTransferBlock",
         "48 89 02 0F B6 42 08 88 41 08 "
         "0F B6 42 09 88 41 09 "
         "0F B6 42 0A 88 41 0A "
         "8B 42 0C 89 41 0C 48 89 69 10",
         ResolveMode::Direct, -0x2F, 0},
    };

    // -----------------------------------------------------------------------
    // ItemNameTable bounded-window anchor patterns.
    //
    // These are NOT cascades: they are pattern strings handed to `DMK::Scanner::find_pattern` for a 0x40--0x80-byte
    // LOCAL scan inside a function whose start the cascade already resolved (via `k_subTranslatorCandidates`). They
    // live here to keep all byte-pattern string literals in one place per the audit policy.
    //
    // The `|` glyph marks the point where `parse_aob` must compute its pattern.offset for downstream
    // `match + offset` arithmetic (DMK v3.0.2+ applies the offset internally during find_pattern).
    //
    // Consumed by `ItemNameTable::resolve_chain` in item_name_table.cpp.
    // -----------------------------------------------------------------------

    /**
     * @brief Step-1 anchor inside SubTranslator (current encoding).
     *
     * Locates the scratch-buffer call inside SubTranslator. The second `lea` encodes rsp-relative (`48 8D 4C 24 ??`,
     * 4 bytes) instead of the older rbp-relative (`48 8D 4D ??`, 3 bytes). The disp8 slots are wildcarded, so a future
     * stack-frame shift inside the same function does not require another anchor variant.
     *
     * Used as the FIRST pattern in a 0x80-byte scan window. The anchor offset `|` lands on the byte immediately after
     * the `E8` opcode, which is the start of the call's disp32.
     */
    inline constexpr const char *k_nametableSubTxV105Anchor =
        "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ?? E8 | ?? ?? ?? ??";

    /**
     * @brief Step-1 anchor inside SubTranslator (older encoding fallback).
     *
     * Older encoding where the second `lea` is rbp-relative. The scan tries it after the current-encoding anchor,
     * inside the same 0x80-byte window.
     */
    inline constexpr const char *k_nametableSubTxV104Anchor =
        "41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ?? E8 | ?? ?? ?? ??";

    /**
     * @brief Step-3 anchor inside ItemAccessor.
     *
     * Locates the `mov rbx, [rip+disp32]` that loads the iteminfo global inside the 4th hop of the name-table chain.
     * The 6-byte prologue-tail anchor (push r14 + sub rsp,imm8 + movzx edi,word ptr [rcx]) pins the call site inside a
     * bounded 0x40-byte scan of the function. Global uniqueness is not required because the scan is locally bounded.
     *
     * The anchor offset `|` lands on the start of the `48 8B 1D disp32` instruction. The consumer reads the disp32
     * with `read_i32_safe` at `match + 3` for RIP-relative resolution to the iteminfo global.
     *
     * The stack-alloc imm8 is wildcarded because the frame size changes across builds. A row that pins that imm8
     * makes resolve_chain() fail at Step 3 with "[nametable] mov-rbx anchor not found" even when SubTranslator
     * resolves fine. Wildcarding is safe here because the scan is bounded to 0x40 bytes of an already-located
     * function, so global uniqueness is not required.
     */
    inline constexpr const char *k_nametableItemAccessorAnchor = "41 56 48 83 EC ?? 0F B7 39 | 48 8B 1D ?? ?? ?? ??";

    // -----------------------------------------------------------------------
    // DyeRecordInject function targets.
    //
    // The dye-injection module installs an inline detour on DyeCopier that, post-trampoline, calls DyeCopy directly to
    // APPEND 16 fabricated ARMOR_MOD records to dst+120.
    //
    //   DyeCopier  : inline detour. Post-trampoline it appends 16 dye records through the DyeCopy primitive.
    //   DyeCopy    : 16-byte ARMOR_MOD record copy primitive. Resolved as a function pointer and called directly from
    //                the detour.
    //
    // Every P1 below must match exactly once module-wide. The DyeCopy prologue alone matches dozens of sites because
    // it is the engine's universal grow-and-emplace template. P1 there is therefore a body-shape anchor that locks
    // onto the unique 16-byte record-copy emitter (shl rcx,4 + add rcx,[rbx] + the field-by-field byte transfer
    // sequence). All other prologues are function-distinctive.
    //
    // If these break, each candidate comment names the anchor offset backed up to function start. Re-find the
    // function in the disassembler, capture the 24--40 byte window, wildcard volatile rel32 targets, and verify
    // uniqueness with a module-wide byte scan.
    // -----------------------------------------------------------------------

    /**
     * @brief DyeCopier -- per-slot dye-record copy driver.
     *
     * Signature `__int64(*)(dst_iteminfo, src_iteminfo)` -- copies primary fields then appends the 12-record dye
     * vector at src+120 into dst+120 through the DyeCopy primitive. DyeRecordInject installs an inline detour here to
     * append 16 fabricated dye records post-trampoline (see `dye_copier_inline_detour` in dye_record_inject.cpp).
     *
     * Prologue is highly distinctive: a 5-register save run (rbp/rsi/rdi/r14/r15) followed by the field-by-field copy
     * through the first 0x60 bytes of the iteminfo struct. No RIP-relative bytes inside the chosen anchor windows --
     * wildcards are not needed.
     */
    inline constexpr AddrCandidate k_dyeCopierCandidates[] = {
        // P1 -- full prologue + first three field copies. The `48 8B F2 4C 8B F1` (mov rsi,rdx ; mov r14,rcx)
        // arg-shuffle followed by the qword/word/word field copies through [rdx+0..0xA] is unique to this iteminfo-copy
        // function. One match module-wide.
        {"DyeCopier_P1_FullPrologue",
         "48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 56 41 57 "
         "48 83 EC 20 48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-shuffle anchor on the field-copy chain. Walk-back -0x15 to function start. Survives a future build
        // that drops or reorders the early callee-save pushes, because the field-copy shape is the function-defining
        // behavior.
        {"DyeCopier_P2_FieldCopyChain",
         "48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08 66 89 41 08 "
         "0F B7 42 0A 66 89 41 0A 48 8B 42 10 48 89 41 10",
         ResolveMode::Direct, -0x15, 0},

        // P3 -- mid-body AVX xmm copy. The `vmovups xmm0, [rdx+28h] ; vmovups [rcx+28h], xmm0` pair followed by the
        // `vmovsd` qword move and continued field copies is a unique SSE/AVX shape this function emits at offset +0x49.
        // Walk-back -0x49 to function start. Anchors entirely past the prologue, so a prologue-shape shuffle does not
        // sink P3.
        {"DyeCopier_P3_AvxFieldCopy",
         "C5 F8 10 42 28 C5 F8 11 41 28 C5 FB 10 4A 38 C5 FB 11 49 38 "
         "0F B7 42 40 66 89 41 40 48 8B 42 48 48 89 41 48",
         ResolveMode::Direct, -0x49, 0},
    };

    /**
     * @brief DyeCopy -- 16-byte ARMOR_MOD record-copy primitive.
     *
     * Signature `__int64(*)(vector_t* dst, const ArmorMod16* src)` -- grows dst's 16-byte-stride array if needed, then
     * writes one record by reading fields from `[rdx+0..0xC]`. The dye detour calls this directly post-trampoline to
     * append fabricated dye records.
     *
     * The function prologue is the engine's universal grow-and-emplace template, with dozens of byte-identical
     * instances module-wide. P1 is a body anchor that locks onto the unique 16-byte record-copy emitter (shl rcx,4 ;
     * add rcx,[rbx] ; the byte-by-byte transfer of channel / R / G / B / 0xFF / repair_byte from [rdi+6..0xB] into
     * [rcx+6..0xB]). That shape is what makes this primitive the "ARMOR_MOD writer" rather than a generic vector grow.
     *
     * If these break, re-anchor on the `48 C1 E1 04 ; 48 03 0B ; 89 01` triplet (shift-by-4-stride + add-base-pointer
     * + write-hash-u32). That sequence is the function's signature behavior. A future build is unlikely to alter it
     * without also redesigning the ARMOR_MOD record layout.
     */
    inline constexpr AddrCandidate k_dyeCopyCandidates[] = {
        // P1 -- mid-prologue + capacity-check + grow-call chain. The `8B 49 0C 8B 43 08 3B C8 77 ?? 8D 14 4D 01 00 00
        // 00 03 D1 B9 01
        //  00 00 00 D1 EA 3B D1 0F 42 D1 48 8B CB 3B C2 0F 47 D0 E8` chain
        // anchors on the count/capacity load + the grow-size formula `1 + count*2`, the lower-bound clamp via `cmovb`,
        // the upper-bound clamp via `cmova`, and the call to the underlying grow primitive. The `8B 49 0C` is `mov ecx,
        // [rcx+0xC]` reading the count field, then the chain-into-grow reaches the unique 16-byte record copy. The `77
        // ??` rel8 is wildcarded (jump distance compiler-owned). One match module-wide. Walk-back -0x10 to function
        // start.
        {"DyeCopy_P1_GrowChainMidProlog",
         "8B 49 0C 8B 43 08 3B C8 77 ?? 8D 14 4D 01 00 00 00 03 D1 "
         "B9 01 00 00 00 D1 EA 3B D1 0F 42 D1 48 8B CB 3B C2 0F 47 D0 E8 "
         "?? ?? ?? ?? 8B 43 08 8B C8 8B 07 48 C1 E1 04 48 03 0B 89 01",
         ResolveMode::Direct, -0x10, 0},

        // P2 -- 16-byte record-copy emitter body. The `shl rcx,4 ; add rcx,[rbx] ; mov [rcx],eax` triplet computes the
        // next-record byte address (count<<4 = 16-byte stride), then the byte-by-byte transfers fan out: word
        // `[rdi+4..5]` -> `[rcx+4..5]`, then singles for channel (`+6`), R (`+7`), G (`+8`), B (`+9`). This shape is
        // what makes the function the ARMOR_MOD writer. One match module-wide. Walk-back -0x43 to function start.
        {"DyeCopy_P2_ArmorModRecordCopy",
         "48 C1 E1 04 48 03 0B 89 01 0F B7 47 04 66 89 41 04 "
         "0F B6 47 06 88 41 06 0F B6 47 07 88 41 07 "
         "0F B6 47 08 88 41 08 0F B6 47 09 88 41 09",
         ResolveMode::Direct, -0x43, 0},

        // P3 -- tail of the byte-by-byte copy + post-write count++ + ret. The trailing field transfers
        // (`[rdi+0xB]` -> `[rcx+0xB]`, `[rdi+0xC]` -> `[rcx+0xC]`) followed by `inc dword [rbx+8]` (count++) and the
        // standard `pop rdi ; ret` epilogue are unique to this exact function shape. One match module-wide. Walk-back
        // -0x77 to function start. The `0F B6 47 0B 88 41 0B 0F B6 47 0C 88 41 0C` is the last byte-pair of the record
        // copy. The `FF 43 08` count increment proves the dst is a vector with a count field at +8.
        {"DyeCopy_P3_TailCountInc",
         "0F B6 47 0B 88 41 0B 0F B6 47 0C 88 41 0C "
         "FF 43 08 48 8B 5C 24 30 48 83 C4 20 5F C3",
         ResolveMode::Direct, -0x77, 0},
    };

    /**
     * @brief ColorPublisher -- per-(dst, src) matInst publisher invoked from the matInst-list copy loop. ColorOverride
     *        installs a MidHook here so every dst matInst exposed during a transmog apply gets its content_hash and
     *        slot cached into MatInstOwner / CarrierSet for the setter substitute to query.
     *
     * "ColorPublisher" is a mod-internal label. The engine writer family is
     * pa::ClientFrameEventChange{,Global}MaterialParameter. The cascade below resolves the target by AOB, so no
     * absolute address is recorded here.
     *
     * The prologue saves seven callee-saved registers (rbp, rbx, rsi, rdi, r12-r15) and frames with `lea
     * rbp,[rsp-0x1F]; sub rsp, 0xF8`. The wide saved-reg window plus the `lea rax, [rip+disp32]` to a vtable constant
     * make the prologue distinctive enough that a 7-instruction window matches uniquely.
     */
    inline constexpr AddrCandidate k_colorPublisherCandidates[] = {
        // P1 -- full prologue. The stack-alloc imm32 (`F8 00 00 00`) is wildcarded because the frame size can shift if
        // a patch adds or removes locals. The trailing `48 8D 05` (lea rax, [rip+disp32]) literal leads into a vtable
        // RIP-relative. The window stops one byte before that disp32 because the disp32 itself is volatile. One match
        // module-wide.
        {"ColorPublisher_P1_FullPrologue",
         "4C 89 44 24 18 48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 "
         "41 57 48 8D 6C 24 E1 48 81 EC ?? ?? ?? ?? 49 8B F8 4C 8B EA "
         "4C 8B F9 48 8D 05",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-saves frame anchor. Picks up at the `lea rbp,[rsp-0x1F]; sub rsp, ???` pair followed by the
        // arg-reload triple (mov rdi,r8 / mov r13,rdx / mov r15,rcx). Walk-back -0x16 (22 bytes) to function start.
        {"ColorPublisher_P2_PostSavesFrame",
         "48 8D 6C 24 E1 48 81 EC ?? ?? ?? ?? 49 8B F8 4C 8B EA 4C 8B F9 "
         "48 8D 05",
         ResolveMode::Direct, -0x16, 0},

        // P3 -- mid-body permutations-token canary XOR. The engine computes `(prev_canary ^ al) & 1 ^ prev_canary` and
        // stores it back, then ORs 2 into the result. This bit-twiddle has a unique fingerprint:
        //   movzx r10d,[rbp+X]   ; load prev canary
        //   xor   r10b, al        ; xor with new lsb
        //   and   r10b, 1         ; mask
        //   xor   r10b, [rbp+X]   ; flip prev
        //   mov   [rbp+X], r10b   ; store
        //   movzx eax, r10b       ; reload
        //   or    al, 2            ; mark "seen"
        //   mov   [rbp+X], al     ; store again
        // The disp8 frame slot reused throughout is a single byte and is kept literal because the function reuses the
        // same local across all writes. One match module-wide. Walk-back -0x43 to function start.
        {"ColorPublisher_P3_PermutCanaryXor",
         "44 0F B6 55 AF 44 32 D0 41 80 E2 01 44 32 55 AF "
         "44 88 55 AF 41 0F B6 C2 0C 02 88 45 AF",
         ResolveMode::Direct, -0x43, 0},
    };

    /**
     * @brief HostScope OwnerVfunc1 -- per-host owner-container vtable slot that invokes the matInst-list copy loop,
     *        which in turn dispatches the publisher. Mid-hooked by ColorOverride::HostScope to capture rcx (the live
     *        owner container) for the player-vs-NPC election.
     *
     * The function is one of three byte-identical sibling thunks. The other two share the entire body except the inner
     * `call rel32` disp32, which is unique per thunk and therefore unusable as a stable anchor. The only patch-stable
     * way to single this thunk out is to anchor on the preceding function's tail epilogue + alignment padding, then
     * walk forward into the prologue.
     */
    inline constexpr AddrCandidate k_hostScopeVfunc1Candidates[] = {
        // P1 -- preceding-function epilogue + 3-byte CC alignment + full thunk prologue head. The preceding fn ends
        // with the semantic vector pop-back sequence:
        //   lea  eax,[rcx-1]
        //   mov  rbp,[rsp+0x48]
        //   mov  [rsi+8],eax        ; vec.count = vec.count - 1
        //   add  rsp, 0x20
        //   pop  rsi
        //   ret
        // followed by 3 bytes of `CC CC CC` padding and then the thunk's prologue. One match module-wide. Walk forward
        // +0x14 (20 bytes) to thunk entry. This row is brittle if the preceding function or its padding shifts under a
        // patch. P2 and P3 give closer-in fallbacks.
        {"HostScopeVfunc1_P1_PrevTailPadStart",
         "8D 41 FF 48 8B 6C 24 48 89 46 08 48 83 C4 20 5E C3 "
         "CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9",
         ResolveMode::Direct, +0x14, 0},

        // P2 -- shorter preceding-tail anchor starting at the `mov [rsi+8],eax` store. Drops the `lea eax,[rcx-1]` head
        // so a patch that reuses an equivalent post-decrement pattern still matches. One match module-wide. Walk
        // forward +0xC (12 bytes) to thunk entry.
        {"HostScopeVfunc1_P2_PrevPopCountPadStart",
         "89 46 08 48 83 C4 20 5E C3 "
         "CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9",
         ResolveMode::Direct, +0x0C, 0},

        // P3 -- minimal preceding-tail anchor starting at the `pop rsi; ret`. Keeps just the 3-byte padding + the deep
        // thunk prologue (including the `test r9, r9` arg-null guard) for disambiguation. One match module-wide. Walk
        // forward +0x5 (5 bytes) to thunk entry.
        {"HostScopeVfunc1_P3_PrevRetPadStart",
         "5E C3 CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9 49 8B E8 48 8B DA 48 8B F1 4D 85 C9",
         ResolveMode::Direct, +0x05, 0},
    };

    /**
     * @brief HostScope OwnerVfunc2 -- sibling per-host owner-container vtable slot. Same role as Vfunc1 (capture rcx
     *        as the live owner container) but with a distinct prologue, so it admits a direct-prologue anchor without
     *        relying on the preceding function.
     */
    inline constexpr AddrCandidate k_hostScopeVfunc2Candidates[] = {
        // P1 -- full prologue. Spills 4 args (rbx, rbp, rsi, rdi), pushes r14, allocates 0x60 of stack, then loads rbx
        // <- rdx and rdi <- rcx, zeros r14d, and tests r9b (the inline-call optimization flag arg). The combination of
        // 4 spilled args + `41 56` push r14 + `48 83 EC 60` is what makes this prologue distinctive against the
        // siblings. One match module-wide.
        {"HostScopeVfunc2_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "41 56 48 83 EC 60 48 8B DA 48 8B F9 45 33 F6 45 84 C9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill + frame setup + arg flag test. Anchors past the four 5-byte arg-home stores; the `74 6D`
        // short jz is the early-return when the flag arg is zero. Walk-back -0x14 (20 bytes) to function start.
        {"HostScopeVfunc2_P2_PostArgSpillFlagTest",
         "41 56 48 83 EC 60 48 8B DA 48 8B F9 45 33 F6 45 84 C9 "
         "74 6D 48 8D 4C 24 38 E8 ?? ?? ?? ?? 90 48 8B 74 24 38",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- post first-call NOP + vtable-call into the inner copier. The `90` is a single-byte alignment NOP that
        // the compiler emits between the `call rel32` and the next instruction; `mov rsi,[rsp+0x38]` reloads the saved
        // argument. The `test rsi, rsi; jz` rejects null input. Walk-back -0x32 (50 bytes) to function start.
        {"HostScopeVfunc2_P3_PostFirstCallNop",
         "90 48 8B 74 24 38 48 85 F6 74 ?? 48 8B 07 48 8D 53 08 "
         "48 85 DB 49 0F 44 D6 48 8B CF FF 90 20 03 00 00",
         ResolveMode::Direct, -0x32, 0},
    };

    /**
     * @brief PropertyByteSetter -- 4-byte property descriptor's BYTE-variant write path. Mid-hooked by
     *        ColorOverride::SetterSubstitute so the engine's per-property color writes can be redirected to user-chosen
     *        RGB values.
     *
     * The color descriptors written here are pa::PartPrefabMaterialParemeterSetEventData / ...FloatEventData (the
     * engine's "Paremeter" misspelling is intentional). "SetterSubstitute" is a mod-internal label.
     *
     * The function tests the descriptor's callback at `[rcx+0x78]`, falls through to a 4-byte equality test when
     * present, and tail-jumps to a downstream writer on mismatch. A sibling function shares the entire byte-compare
     * body but reads property bytes as DWORD (`41 8B 01`) instead of byte-per-byte (`41 0F B6 01`). That single 4-byte
     * opcode difference at offset +0x23 is the unique discriminator, and every candidate below keeps it literal.
     */
    inline constexpr AddrCandidate k_setterByteCandidates[] = {
        // BEWARE the near-clone that sits directly after this function. It shares the same byte-compare chain and
        // differs only in its head (`mov rax,[rcx+0x98]` against `[rcx+0x78]`) and its second gate (`cmp [rcx+0xD8]`
        // against `[rcx+0xC8]`). That is why P3 anchors on the function TAIL and not on the shared body.
        //
        // The entry block is volatile across builds. A patch can invert the first test polarity (`74` je against `75`
        // jne) or insert another gate, which breaks every row that pins the old shape. P1 and P2 therefore form a
        // specific-then-general pair over the same window, and P3 is the independent row that does not touch the entry
        // block at all.

        // P1 -- full entry block. The three rel8 jump distances are kept literal because they are part of the unique
        // fingerprint. P2 covers the case where a patch reflows them.
        {"SetterByte_P1_FullPrologue",
         "48 8B 41 78 4D 8B C8 4C 8B D1 48 85 C0 75 09 48 39 81 C8 00 00 00 74 74 "
         "33 C9 4C 8D 5A F8 48 85 D2 4C 0F 44 D9 49 63 4A 70 85 C9 74 40 41 0F B6 01",
         ResolveMode::Direct, 0, 0},

        // P2 -- same block with all three rel8 distances wildcarded. Survives a branch reflow.
        {"SetterByte_P2_WildcardedJumpDist",
         "48 8B 41 78 4D 8B C8 4C 8B D1 48 85 C0 75 ?? 48 39 81 C8 00 00 00 74 ?? "
         "33 C9 4C 8D 5A F8 48 85 D2 4C 0F 44 D9 49 63 4A 70 85 C9 74 ?? 41 0F B6 01",
         ResolveMode::Direct, 0, 0},

        // P3 -- body-tail dispatch anchor, entirely past the entry block, so it survives another re-emission of the
        // prologue gates. Shape: the property store, the tail-call jump table (`movsxd rcx,[r10+0x80]; add rcx,r11;
        // jmp rax`) and the vtable tail-call (`jmp [r10+0xC8]`). Anchors at function start + 0x5B, so the walk-back is
        // -0x5B.
        {"SetterByte_P3_BodyTailDispatch",
         "41 8B 01 49 8D 52 18 49 8B CB 41 89 00 E9 ?? ?? ?? ?? 49 8B D1 48 85 C0 74 ?? "
         "49 63 8A 80 00 00 00 49 03 CB 48 FF E0 49 8B CB 49 FF A2 C8 00 00 00 C3",
         ResolveMode::Direct, -0x5B, 0},
    };

    /**
     * @brief ColorTokenInterner -- shader-property name interner. Maps an ASCII property name (e.g. "_tintColorR") to
     *        a stable u32 token id used downstream by the dye/material setter pipeline. Called once per property by
     *        the two TLS-guarded registrars (one for dye-mask properties, one for tint and detail properties).
     *
     * The function lives in the `.tls` section. The body is large (0x86E bytes) and includes a once-only
     * `lock cmpxchg` guarded init path. That path allocates the hash table, sets the bucket-prime count (0x8E = 142)
     * and the sentinel cap (0x2FFFF), and publishes the state pointer to a module global. The InternerHook computes
     * that global from the publish store's RIP displacement and never hardcodes it. Subsequent calls take the table
     * lock, look up the name, and return either the existing token or a freshly minted one.
     *
     * Resolution lets ColorOverride::InternerHook walk the body to locate the `qword = state` publish store and reach
     * the entries-array without scanning E8 trampolines through a registrar call site.
     */
    inline constexpr AddrCandidate k_colorTokenInternerCandidates[] = {
        // P1 -- full Microsoft __fastcall prologue. The 4 shadow-store saves (`mov [rsp+disp8], rbx/r8d/rdx/rcx`)
        // wildcard their disp8 home-area offsets because the prototype's argument layout is the only thing that pins
        // them. The 7-register push run `55 56 57 41 54 41 55 41 56 41 57` (rbp/rsi/rdi/r12/r13/r14/r15) is the
        // distinctive head, because very few functions save all 7 callee-saved regs. The `lea rbp,[rsp-disp8]` frame
        // setup and the `sub rsp,imm32` stack allocation both wildcard compiler-owned sizes. The trailing `41 8B F9`
        // (mov edi, r9d) captures the sentinel-cap argument into a saved scratch register and pins this function
        // against any other 7-push function.
        {"ColorTokenInterner_P1_FullPrologue",
         "48 89 5C 24 ?? 44 89 44 24 ?? 48 89 54 24 ?? 48 89 4C 24 ?? "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 41 8B F9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca early-exit anchor. Walks back -0x24 to reach the function start. The chain (sub rsp / mov
        // edi,r9d / mov r12,rdx / xor ebx,ebx / mov [rcx],ebx / test rdx,rdx) is the function's argument-validation
        // preamble: it captures the sentinel cap, mirrors the name pointer into r12, zeroes the output token (`*a1 =
        // 0`), then tests the name pointer for null. The literal sequence `4C 8B E2 33 DB 89 19 48 85 D2` (mov r12,rdx;
        // xor ebx,ebx; mov [rcx], ebx; test rdx,rdx) is unique to this function's entry contract. Survives prologue
        // reflows that affect the register save layout but keep the argument plumbing identical.
        {"ColorTokenInterner_P2_PostAllocaEarlyExit", "48 81 EC ?? ?? ?? ?? 41 8B F9 4C 8B E2 33 DB 89 19 48 85 D2",
         ResolveMode::Direct, -0x24, 0},

        // P3 -- deep-body cap-init magic-write anchor. Walks back -0x126 from the matched site to reach the function
        // start. After the once-only `lock cmpxchg` init guard succeeds, the function writes a four-constant
        // fingerprint:
        //   C7 46 50 8E 00 00 00   mov [rsi+0x50], 0x8E   ; bucket prime
        //   C7 46 54 FF FF 02 00   mov [rsi+0x54], 0x2FFFF; sentinel cap
        //   BA F8 FF 2F 00         mov edx, 0x2FFFF8      ; alloc size
        //   41 B8 10 00 00 00      mov r8d, 0x10          ; entry stride
        // This semantic fingerprint survives wholesale prologue rewrites (e.g., a future patch swapping the fastcall
        // ABI for a different register save list) because the constants are dictated by the interner's data-structure
        // contract, not by compiler layout.
        {"ColorTokenInterner_P3_CapInitMagicWrite",
         "48 8B F3 C7 46 50 8E 00 00 00 C7 46 54 FF FF 02 00 "
         "BA F8 FF 2F 00 41 B8 10 00 00 00",
         ResolveMode::Direct, -0x126, 0},
    };

    /**
     * @brief Property-registration call-site walk patterns. Anchor the opcode run the compiler emits before every call
     *        into the ColorTokenInterner from the two TLS-guarded registrar functions (one for dye-mask properties,
     *        one for tint and detail properties). Each pattern is INTENTIONALLY multi-match, with one hit per property
     *        registration. The discovery walker enumerates every hit, decodes the `lea rdx` displacement to read the
     *        property name, and accepts only the entries whose strings appear in its known-property allow-list.
     *
     * The shared head is the 17-byte run:
     *
     *   41 B9 FF FF 02 00      mov  r9d, 0x2FFFF       ; sentinel cap
     *   ?? 8D ?? 01            lea  r8d, [reg+1]       ; REX and ModR/M wildcarded; reg is r13 in the dye-mask
     *                                                  ; registrar and rdi in the tint-and-detail registrar
     *   48 8D 15 ?? ?? ?? ??   lea  rdx, [rip+name]    ; property name
     *
     * An rcx-load and an `E8 disp32` call to the interner follow within a few bytes. The three patterns below anchor
     * on progressively wider windows of the call site. Together they survive a compiler reflow of any single one. The
     * walker scans with each pattern in turn and merges the hits (dedup by decoded slot address), so one pattern
     * losing its shape on a future patch is tolerated while at least one survives.
     *
     * P1 is the canonical superset and matches every registration site across the binary. P2 misses the
     * first-call-per-registrar entries, which load rcx from a preloaded table-base register through `mov rcx, reg`.
     * P3 covers exactly those first-call entries. Walking all three keeps the discoverer correct even if P1 loses its
     * shape, because the union of P2 and P3 covers the same site set as P1.
     */
    inline constexpr std::array<std::string_view, 3> k_colorTokenRegistrarCallAobs = {{
        // P1 -- 17-byte literal head. Anchors on the run from `mov r9d, 0x2FFFF` through `lea rdx, [name]`. The
        // REX+ModR/M of the `lea r8d, [reg+1]` is wildcarded because the counter register differs by registrar: r13d
        // in the dye-mask registrar, rdi in the tint-and-detail registrar.
        "41 B9 FF FF 02 00 "
        "?? 8D ?? 01 "
        "48 8D 15 ?? ?? ?? ??",

        // P2 -- head + lea-rcx-slot + call tail. Captures calls 2..N within each registrar (these load rcx via `lea
        // rcx, [rip+slot]` to the current property's backing storage). Tighter than P1 and survives a future compiler
        // reflow that changes the head shape as long as the rcx-load + call tail is preserved.
        "41 B9 FF FF 02 00 "
        "?? 8D ?? 01 "
        "48 8D 15 ?? ?? ?? ?? "
        "48 8D 0D ?? ?? ?? ?? E8",

        // P3 -- head + mov-rcx-reg + call tail. Captures the first registration call per registrar function (it loads
        // rcx from a preloaded table-base register through `mov rcx, rsi` or `mov rcx, rbx`). It matches only a
        // handful of sites: the two registrars, plus unrelated callers that share the shape and that the name
        // allow-list filters out.
        "41 B9 FF FF 02 00 "
        "?? 8D ?? 01 "
        "48 8D 15 ?? ?? ?? ?? "
        "48 8B ?? E8",
    }};

    /**
     * Byte width of the literal head shared by all k_colorTokenRegistrarCallAobs candidates. The walker uses this to
     * step the cursor past a matched anchor before scanning for the next hit.
     */
    inline constexpr std::size_t k_colorTokenRegistrarCallAobHeadLen = 17;

    /**
     * Number of walk-pattern variants in k_colorTokenRegistrarCallAobs. Exposed as a standalone constant so dependent
     * compile-time expressions (std::array sizing, unrolled loops) do not have to rebind the array through a reference
     * before reading its extent (MSVC declines to treat `.size()` on a `const auto&` alias as a constant expression
     * even when the underlying global is `inline constexpr`).
     */
    inline constexpr std::size_t k_colorTokenRegistrarCallAobCount = k_colorTokenRegistrarCallAobs.size();

    /*
     * HelmAudioRegistrar -- per-tag passive-skill registrar. Invoked once per `{u16 tag, u32 lvl}` audio-classifier
     * entry on the equipped item (iteminfo desc+0x100 vector). Reads the tag from `*r8`, the level from `r9`, and the
     * character skill manager from `rcx`. The helm-audio filter hooks the function entry inline and short-circuits
     * the call (zero the status int, return) when the call matches the audio-classifier code path (a7==0, 8-byte {u16
     * tag, u16 0, u16 lvl, u16 0} buffer at `a3`) AND the resolved skill's first per-level entry classifies as
     * `pa::GameAudioEffectBuffData` AND the host actor classifies as a configured protagonist. The gate derives
     * muffle-class membership from the engine's own RTTI rather than from a hardcoded tag-id set, so it admits any
     * future tag backed by the same class automatically. See helm_audio_filter.{cpp,hpp} for the gate rationale and
     * the bypass-safety analysis on the single virtual call SUPPRESS bypasses.
     *
     * Prologue:
     *   44 89 4C 24 20             mov  [rsp+20h], r9d   ; home a3
     *   48 89 54 24 10             mov  [rsp+10h], rdx   ; home a1
     *   55 53 56 57                push rbp/rbx/rsi/rdi
     *   41 54 41 56 41 57          push r12/r14/r15
     *   48 8D AC 24 B0 FE FF FF    lea  rbp, [rsp-150h]
     *   48 81 EC 60 02 00 00       sub  rsp, 260h
     *
     * The 7-register save list together with the specific lea / sub immediates (-0x150, +0x260) pin this prologue
     * uniquely, with one match in the whole module. The two leading argument-home stores anchor the entry. The 7
     * pushes give a compact callee-save fingerprint. The lea/sub pair encodes the function's specific 0x4B0-byte
     * frame.
     */

    /**
     * @brief Signature input for the skill-tag resolver scan (`resolve_skill_tag_resolver` in helm_audio_filter.cpp).
     *
     * This is the only resolution path for that target, and it is deliberately NOT unique.
     *
     * The engine emits one u16-tag resolver per pa::*InfoManager, and they are byte-identical for more than forty
     * bytes: same prologue, same tag load, same bound check, same entry fetch. The single difference is which manager
     * global each one loads, and that lives in a RIP-relative displacement. A unique byte signature must therefore
     * bake that displacement, which stops matching on the next build that relocates the global. This AOB instead
     * signs only the opcode and ModRM shape, with every movable operand wildcarded: the manager disp32 and the
     * forward-jump rel32. It matches every member of the family. The consumer enumerates the hits and picks the right
     * one by reading each resolver's manager pointer and comparing that manager's MSVC RTTI class name against
     * @ref k_skillInfoManagerRttiName. A class name survives relocation. A displacement does not.
     *
     * The body anchors at function entry + 0x12, past the first five bytes, so a sibling mod that inline-hooks the
     * resolver and overwrites its prologue does not stop the scan from matching.
     *
     *   0F B7 39                 movzx edi, word ptr [rcx]  ; *tag
     *   48 8B 1D ?? ?? ?? ??     mov   rbx, [rip+disp32]    ; manager global, wildcarded
     *   3B 7B 08                 cmp   edi, [rbx+8]         ; bound check
     *   0F 83 ?? ?? ?? ??        jae   <out of range>       ; rel32, wildcarded
     *   4C 8D 34 FD 00 00 00 00  lea   r14, [rdi*8+0]       ; index scale
     *   48 8B 43 58              mov   rax, [rbx+0x58]      ; entry array
     *   49 8B 04 06              mov   rax, [r14+rax]       ; entry load
     *
     * The entry-array displacement (`48 8B 43 58` here) tracks the pa::StaticInfoManager2 layout and moves when that
     * base changes width. A stale value produces zero matches, and the scan reports that at trace level only. Verify
     * this signature against live memory on every patch day, because a dead scan looks like a clean log.
     */
    inline constexpr std::string_view k_skillTagResolverBodyAob = "0F B7 39 "
                                                                  "48 8B 1D ?? ?? ?? ?? "
                                                                  "3B 7B 08 "
                                                                  "0F 83 ?? ?? ?? ?? "
                                                                  "4C 8D 34 FD 00 00 00 00 "
                                                                  "48 8B 43 58 "
                                                                  "49 8B 04 06";

    /**
     * @brief Decorated MSVC RTTI name of the manager whose resolver we want. Compared byte-exact
     *        (DMKRtti::vtable_is_type rejects substrings) so the sibling SkillTree / SkillTreeGroup / SkillGroup
     *        managers cannot be mistaken for the SkillInfo one. A class rename in a future patch is the only edit this
     *        primary path ever needs.
     */
    inline constexpr std::string_view k_skillInfoManagerRttiName = ".?AVSkillInfoManager@pa@@";

    /// Body offset of the disp32 inside the `mov rbx,[rip+disp32]`.
    inline constexpr std::ptrdiff_t k_skillTagResolverDispOffset = 6;
    /// End of the `mov rbx,[rip+disp32]`, i.e. the RIP base for its disp32.
    inline constexpr std::ptrdiff_t k_skillTagResolverInstrEnd = 10;
    /// Distance from the body anchor (entry+0x12) back to the entry.
    inline constexpr std::ptrdiff_t k_skillTagResolverEntryBackoff = 0x12;

    /**
     * @brief `pa::GameAudioEffectBuffData` vtable -- the class marker we compare buff-instance vtable pointers against
     *        to identify muffle-class skills.
     *
     * Each `pa::GameAudioEffectBuffData` instance stores the address of vfunc[0] (= vtable + 8 bytes past the RTTI
     * metadata ptr) in its first qword. The chain walk in helm_audio_filter.cpp resolves a tag's skill record ->
     * per-level entry array -> first entry, then reads `*entry` and compares against the value resolved here.
     *
     * Resolution strategy: AOB on the class's CONSTRUCTOR's final vtable assignment (which lives in `.text`), then use
     * RipRelative mode to read the constructor's RIP-rel disp32 to compute the absolute vtable address. Direct AOB scan
     * on the vtable bytes themselves cannot work because DetourModKit's `scan_executable_regions` filters by
     * READABLE_EXEC_FLAGS only (scanner.cpp:669) and `.rdata` (PAGE_READONLY) is skipped.
     *
     * Constructor tail:
     *   48 89 91 88 00 00 00         mov [rcx+88h], rdx        ; parent ctor fill
     *   48 8D 05 ?? ?? ?? ??         lea rax, [rip+disp32]     ; load vtable addr
     *   48 89 01                     mov [rcx], rax            ; store at obj[0]
     *   C6 81 90 00 00 00 03         mov byte ptr [rcx+90h], 3 ; init audio_class byte
     *   48 8B C1                     mov rax, rcx              ; this-return
     *   C3                           ret
     *
     * The 28-byte signature is unique module-wide. The disp32 is wildcarded for build-portability.
     * RipRelative decode:
     *   - disp_offset    = 10  (offset of the disp32 from match start. The LEA is at match+7, the opcode
     *                          `48 8D 05` is 3 bytes, so the disp32 starts at match+10.)
     *   - instr_end_offset = 14 (the next instruction begins 7 bytes after the LEA start. The LEA is 7 bytes, so
     *                          match+7+7 = match+14.)
     * Resolved address = match + 14 + sign_extend(disp32). The 0x90-byte audio_class init right after the vtable store
     * anchors the pattern specifically to GameAudioEffectBuffData. Other ctors that do similar vtable assignments have
     * different post-vtable field initialization shapes.
     *
     * P2 extends the anchor one instruction earlier (the parent ctor's +0x84 state-init byte write) to add a stable
     * 7-byte discriminator upstream of the vtable LEA. The disp_offset shifts to 17 and instr_end_offset to 21 to
     * follow the LEA's new position within the window. P3 walks back another 17 bytes to also include the `[rcx+0x78]`
     * qword fill and the `[rcx+0x80]` dword fill that GameAudioEffectBuffData's parent ctor performs immediately
     * before the +0x84 byte. That gives the widest sub-frame-shaped fingerprint without dragging in calls that vary
     * across builds.
     */
    inline constexpr AddrCandidate k_gameAudioEffectVtableCandidates[] = {
        // Primary -- resolve by RTTI mangled name. Every pa::GameAudioEffectBuffData instance stores its primary
        // (COL.offset == 0) vtable base in its first qword, which is exactly what the byte ctor-LEA tiers below
        // recover. Resolving by the patch-stable mangled name self-heals across the vtable relocations that move those
        // byte anchors between builds. The backend is unique-only and fails closed, so an absent name falls through to
        // the byte tiers.
        {"GameAudioEffectVtable_RTTI", ".?AVGameAudioEffectBuffData@pa@@", ResolveMode::RttiVtable},

        // P1 -- constructor tail with RIP-rel LEA. One match module-wide. The resolved target is the vfunc[0] address,
        // which is the value objects store in their first qword.
        {"GameAudioEffectVtable_P1_CtorLea",
         "48 89 91 88 00 00 00 "
         "48 8D 05 ?? ?? ?? ?? "
         "48 89 01 "
         "C6 81 90 00 00 00 03 "
         "48 8B C1 "
         "C3",
         ResolveMode::RipRelative, 10, 14},

        // P2 -- extend P1 backwards by the parent ctor's +0x84 state-init byte:
        //   C6 81 84 00 00 00 03   mov  byte ptr [rcx+0x84], 3
        //   48 89 91 88 00 00 00   mov  [rcx+0x88], rdx
        //   48 8D 05 ?? ?? ?? ??   lea  rax, [rip+disp32]  ; vtable
        //   48 89 01               mov  [rcx], rax
        //   C6 81 90 00 00 00 03   mov  byte ptr [rcx+0x90], 3
        //   48 8B C1               mov  rax, rcx
        //   C3                     ret
        // The dual `byte ptr [..0x84]=3` + `byte ptr [..0x90]=3` state-byte pair flanks the LEA and pins this ctor
        // against sibling effect-buff ctors that init only one of the two bytes. LEA now sits 7 bytes deeper into the
        // pattern, so disp_offset = 10 + 7 = 17, instr_end_offset = 14 + 7 = 21.
        {"GameAudioEffectVtable_P2_CtorStatePair",
         "C6 81 84 00 00 00 03 "
         "48 89 91 88 00 00 00 "
         "48 8D 05 ?? ?? ?? ?? "
         "48 89 01 "
         "C6 81 90 00 00 00 03 "
         "48 8B C1 "
         "C3",
         ResolveMode::RipRelative, 17, 21},

        // P3 -- extend P2 backwards by the parent ctor's earlier payload writes at [rcx+0x78] (qword) and [rcx+0x80]
        // (dword):
        //   48 89 51 78            mov  [rcx+0x78], rdx
        //   89 91 80 00 00 00      mov  [rcx+0x80], edx
        //   ... (P2 body) ...
        // These two writes capture the parent ctor's signature packing of `rdx` into three sequential fields (qword at
        // 0x78, dword at 0x80, byte at 0x84) -- a layout shape specific to this effect-buff family. LEA shifts a
        // further 10 bytes (the P2 backward extension is 7, and P3 adds 10), so disp_offset = 17 + 10 = 27 and
        // instr_end_offset = 21 + 10 = 31.
        {"GameAudioEffectVtable_P3_CtorFullPayload",
         "48 89 51 78 "
         "89 91 80 00 00 00 "
         "C6 81 84 00 00 00 03 "
         "48 89 91 88 00 00 00 "
         "48 8D 05 ?? ?? ?? ?? "
         "48 89 01 "
         "C6 81 90 00 00 00 03 "
         "48 8B C1 "
         "C3",
         ResolveMode::RipRelative, 27, 31},
    };

    // -----------------------------------------------------------------------
    // PlayerStatic -- engine global whose chain reaches the currently-controlled protagonist's
    // pa::ServerChildOnlyInGameActor.
    //
    // Used by helm_audio_filter.cpp for the Kliff init-race fallback. When the actor's CharacterAssets vector is not
    // yet wired up (the first muffle event after world load), the asset-string scan returns Unknown. If the chain leaf
    // here equals the host under classification, the filter attributes that host to Kliff. Kliff is always the
    // first-spawned protagonist and the controlled actor at world load.
    //
    // Walk (runtime):
    //   *(static)         -> root container
    //   *(root  + 0x18)   -> pa::NwVirtualAsyncSession
    //   *(nwSes + 0xA0)   -> pa::ServerUserActor
    //   *(srvUA + 0xD0)   -> pa::ServerChildOnlyInGameActor (controlled)
    //
    // P1 anchors on the writer site. P2 extends that same writer forward through the TLS-guarded null-check (jz +
    // gs:0x58 TIB load + compare against `[r12+rdx]`), so the cascade still resolves when a future compiler reshuffles
    // the scratch-id immediate within the same writer. P3 anchors on an unrelated reader site on the SafeLaunch /
    // pre-character-spawn path. That is a completely different call graph, and it loads PlayerStatic as the first
    // argument to an actor-query primitive. P3 keeps the cascade alive even when a future patch reshapes the whole
    // writer function that P1 and P2 sit in.
    // -----------------------------------------------------------------------
    inline constexpr AddrCandidate k_playerStaticCandidates[] = {
        // P1 -- unique writer site. It carries the distinctive `mov r12d, <scratch-id>` init tag immediately after the
        // writer, plus a follow-on load from [rdi+0xF8]. The scratch-id is NOT build-stable across patches, so the
        // imm32 is wildcarded. The +0xF8 ABI offset and the writer+test shape keep the site unique. The rel8 of the
        // trailing jcc is wildcarded.
        {"PlayerStatic_P1_WriterSite",
         "4C 89 3D ?? ?? ?? ?? 48 8B 8F F8 00 00 00 "
         "41 BC ?? ?? 00 00 48 85 C9 ??",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- writer site extended through the TLS-guard tail. Same store as P1 (`mov [rip+disp32], r15`) followed by
        // the [rdi+0xF8] field load and the (wildcarded) scratch-id tag, then continues past the `74 ??` short-jz into
        // the TIB load and the per-thread flag-byte compare:
        //   74 ??                          jz   short <skip>          ; rel8 wildcarded
        //   65 48 8B 04 25 58 00 00 00     mov  rax, gs:0x58          ; TIB
        //   48 8B 10                       mov  rdx, [rax]            ; TLS block
        //   45 38 3C 14                    cmp  [r12+rdx], r15b       ; flag test
        // The rel8 jz byte is wildcarded for encoding-flip safety. The trailing TLS+compare shape is unique text and
        // pins the writer. The scratch-id imm32 is wildcarded because it shifts across patches. The RipRelative
        // offsets are unchanged from P1 (disp_offset = 3, instr_end_offset = 7) because the store is still the first
        // instruction in the window.
        {"PlayerStatic_P2_WriterSiteTlsTail",
         "4C 89 3D ?? ?? ?? ?? 48 8B 8F F8 00 00 00 "
         "41 BC ?? ?? 00 00 48 85 C9 74 ?? "
         "65 48 8B 04 25 58 00 00 00 "
         "48 8B 10 "
         "45 38 3C 14",
         ResolveMode::RipRelative, 3, 7},

        // P3 -- reader site on an orthogonal call graph (SafeLaunch / pre-character-spawn path). It loads PlayerStatic
        // into rcx as the first argument to an actor-query primitive. The remaining args are two xor-zeroed dwords and
        // an `lea rdx, [rsp+0x64]` out-pointer.
        //   45 33 C9                   xor  r9d, r9d              ; arg4 = 0
        //   45 33 C0                   xor  r8d, r8d              ; arg3 = 0
        //   48 8D 54 24 64             lea  rdx, [rsp+0x64]       ; out int*
        //   48 8B 0D ?? ?? ?? ??       mov  rcx, [rip+PlayerStatic]
        //   E8 ?? ?? ?? ??             call <actor-query primitive>
        // `mov rcx, [rip+disp32]` is 7 bytes (`48 8B 0D` + disp32). The load starts at pattern offset 11, so the
        // disp32 starts at offset 14 and the next instruction begins at offset 18. The RIP-rel target is
        // match + 18 + sign_extend(disp32). This row is genuinely independent from P1 and P2, so a patch that shuffles
        // only the writer function leaves it intact.
        {"PlayerStatic_P3_SafeLaunchReader",
         "45 33 C9 45 33 C0 "
         "48 8D 54 24 64 "
         "48 8B 0D ?? ?? ?? ?? "
         "E8 ?? ?? ?? ??",
         ResolveMode::RipRelative, 14, 18},
    };

    inline constexpr AddrCandidate k_helmAudioRegistrarCandidates[] = {
        // P1 -- full prologue, entry-anchored. dispOffset = 0 because the pattern starts exactly at the function entry.
        {"HelmAudioRegistrar_P1_FullPrologue",
         "44 89 4C 24 20 "
         "48 89 54 24 10 "
         "55 53 56 57 41 54 41 56 41 57 "
         "48 8D AC 24 B0 FE FF FF "
         "48 81 EC 60 02 00 00",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue lea+sub+arg-stash chain at entry+0x14, past SafetyHook's 5-byte JMP window so the
        // resolver still works after a sibling inline-hooks the entry. The `48 8D AC 24 B0 FE FF FF 48 81 EC 60 02 00
        // 00` lea/sub pair followed by `48 8B 41 08 4C 8B F9 48 89 45 90 48 8B FA` (mov rax,[rcx+8] / mov r15,rcx /
        // mov [rbp-0x90],rax / mov rdi,rdx) gives a 27-byte uniquely-pinned anchor. dispOffset = -0x14 walks back to
        // the entry.
        {"HelmAudioRegistrar_P2_PostPrologue",
         "48 8D AC 24 B0 FE FF FF "
         "48 81 EC 60 02 00 00 "
         "48 8B 41 08 4C 8B F9 "
         "48 89 45 90 48 8B FA",
         ResolveMode::Direct, -0x14, 0},

        // P3 -- deep TLS-gate anchor at entry+0x4D. Lands well past both the 5-byte SafetyHook JMP window AND the
        // post-prologue P2 anchor, so it still resolves even when a sibling mod installs a long mid-function detour
        // over the prologue tail.
        //
        // The body shape at this offset is the function's thread-local-flag preamble:
        //   65 48 8B 04 25 58 00 00 00   mov  rax, gs:58h         ; TIB->ThreadLocalStoragePointer
        //   33 F6                         xor  esi, esi
        //   48 8B 1D ?? ?? ?? ??         mov  rbx, cs:_someGlobal ; RIP-rel, disp32 wildcarded
        //   4C 8B 00                      mov  r8, [rax]           ; TLS block base
        //   B8 F2 01 00 00                mov  eax, 1F2h           ; TLS slot index 498
        //   42 38 34 00                   cmp  [rax+r8], sil       ; flag byte == 0?
        //   48 0F 45 DE                   cmovnz rbx, rsi          ; conditionally zero rbx
        //
        // The combination of a TIB load with the specific TLS slot 0x1F2 and a cmovnz on (rbx, rsi) is a
        // single-occurrence fingerprint module-wide. The TLS slot index is a build-stable per-binary constant. The
        // global RIP-disp32 and the SIB byte of the RIP-rel mov are wildcarded. dispOffset = -0x4D walks back to the
        // entry.
        {"HelmAudioRegistrar_P3_TlsGate",
         "65 48 8B 04 25 58 00 00 00 "
         "33 F6 "
         "48 8B 1D ?? ?? ?? ?? "
         "4C 8B 00 "
         "B8 F2 01 00 00 "
         "42 38 34 00 "
         "48 0F 45 DE",
         ResolveMode::Direct, -0x4D, 0},
    };

} // namespace Transmog
