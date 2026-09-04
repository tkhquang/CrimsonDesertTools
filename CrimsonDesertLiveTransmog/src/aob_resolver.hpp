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
     *        equip table at AuthTable::k_containerPtrOffset. Used by the two-phase transmog apply in
     *        real_part_tear_down.
     *
     * The prologue alone is not unique, so P1 runs past it into the body.
     */
    // EVERY row here MUST carry the component chain walk `[this+0x08] -> +0x68 -> +0x40`. Those three game-owned
    // field offsets are this function's identity; no unrelated function reproduces them.
    //
    // A row built only from opcodes and register moves -- a stack-alloc, a word-argument extract, a register move --
    // is not an identifier. The image contains unrelated vtable methods that open with exactly that shape, their
    // walk-backs land on their own real entries (so an entry-plausibility check cannot catch the mistake), and the
    // wrong function returns harmlessly without detaching anything. The symptom is purely visual and easy to blame
    // on mod logic: the real part keeps rendering underneath the transmogged one, and a slot going from "no
    // transmog" to "transmog" silently does nothing.
    //
    // Prefer losing the cascade to matching the wrong function.
    inline constexpr AddrCandidate k_safeTearDownCandidates[] = {
        // P1 -- full prologue through the word-argument extract. Both frame immediates are wildcarded. rdi is
        // SPILLED here rather than pushed, so the register-save run is five pushes and the spill block is three.
        {"SafeTearDown_P1_FullPrologue",
         "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 "
         "55 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 41 0F B7 F8",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor, extended through the component chain walk so it cannot match a function that
        // merely shares the stack-alloc and the word-argument extract. Anchors at function start + 0x20 (three
        // 5-byte spills, five pushes, an 8-byte frame lea and a 7-byte sub rsp).
        {"SafeTearDown_P2_PostAlloca",
         "48 81 EC ?? ?? ?? ?? 41 0F B7 F8 ?? 8B F1 48 8B 41 08 48 8B 58 68",
         ResolveMode::Direct, -0x20, 0},

        // P3 -- the chain walk alone, past the whole prologue: capture the this-pointer, load the component at
        // [this+0x08], take its sub-object at +0x68, take that object's list head at +0x40, and reject an empty list.
        // Every displacement is a game-struct field, so this row survives any prologue reshuffle. Anchors at function
        // start + 0x2B.
        //
        // The two moves are loosened differently. The first wildcards only its REX prefix, so it still pins the
        // destination's low three bits; the second wildcards its whole ModRM and is destination-agnostic. Tighten or
        // loosen either one deliberately -- they are not equivalent.
        {"SafeTearDown_P3_ComponentChainWalk",
         "?? 8B F1 48 8B 41 08 48 8B 58 68 4C 8B ?? 40 4D 85",
         ResolveMode::Direct, -0x2B, 0},
    };

    /**
     * @brief SubTranslator -- SlotPopulator's item -> slot resolver, `f(a1, itemId) -> slot handle` (0xFFFF when the
     *        item cannot be placed). Serves two callers in LT, which is why there is one cascade and not two.
     *
     * @details Called, not hooked. It is the FIRST thing SlotPopulator does, and a 0xFFFF makes SlotPopulator bail
     *          before equipping anything, so LT calls it directly to ask whether a carrier can be placed at all
     *          rather than inferring that from a failed apply. It tries an actor-side lookup, then falls back to a
     *          per-character item -> slot table.
     *
     *          Its entry block is also the first hop of the chain that walks to the iteminfo global, which the mod
     *          uses to build the stable item-name table at init. See item_name_table.cpp for the full 4-step chain.
     *
     *          Both roles resolve to the same address; do not add a second cascade for the resolver role, because
     *          two cascades onto one function drift apart and only one of them gets re-anchored on patch day.
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
        // Entry layout, with the byte lengths that produce each walk-back:
        //     +0x00  48 89 5C 24 18 / 48 89 4C 24 08   two arg-home spills
        //     +0x0A  55 56 57 41 56 41 57               five pushes
        //     +0x11  48 83 EC 20
        //     +0x15  48 8B D9                           mov rbx,rcx
        //     +0x18  48 C7 01 FF FF FF FF               <- P2 anchors here, so -0x18
        //     +0x1F  41 BF FF FF 00 00                  mov r15d,0FFFFh
        //     +0x25  66 44 89 79 08                     mov [rcx+8],r15w
        //     +0x2A  45 33 F6 / 4C 89 71 18
        //     +0x31  4C 89 71 20                        <- P3 anchors here, so -0x31

        // P1 -- true prologue through the first two sentinel writes. The register the engine parks the 0xFFFF
        // constant in is pinned here and in P2, so a reallocation of it costs both rows. P3 holds no register at
        // all and is the row that survives that.
        {"InitSwapEntry_P1_FullPrologue",
         "48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 56 41 57 48 83 EC 20 48 8B D9 "
         "48 C7 01 FF FF FF FF 41 BF FF FF 00 00 66 44 89 79 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- init-body anchor with no prologue head: mov qword [rcx],-1 / mov r15d,0xFFFF / mov [rcx+8],r15w.
        // Anchors at function start + 0x18. Survives a prologue reshuffle that leaves the sentinel writes intact.
        {"InitSwapEntry_P2_SentinelBody", "48 C7 01 FF FF FF FF 41 BF FF FF 00 00 66 44 89 79 08", ResolveMode::Direct,
         -0x18, 0},

        // P3 -- second half of the sentinel run, past both the prologue and the first sentinel writes. Shape: the
        // zero store at +0x20, the lea of the inline sub-object at +0x28, its own zero and -1 stores, then the u16
        // zero at +0x40. Every DISPLACEMENT here is a struct field offset or a fixed sentinel; the only
        // compiler-owned part is the register selection, and it is a different register from the one P1 and P2
        // depend on, so the three rows do not fail together. Anchors at function start + 0x31.
        {"InitSwapEntry_P3_SentinelRunTail",
         "4C 89 71 20 48 8D 79 28 4C 89 37 "
         "48 C7 47 08 FF FF FF FF 4C 89 77 10 66 44 89 71 40",
         ResolveMode::Direct, -0x31, 0},
    };

    // SlotTagToHandle -- f(a1, out_u16, slotTag, flag). Walks the part records in the container the engine reaches
    // through `mov rax,[a1+X]`, matches `record+0xC8 == slotTag`, and writes `record+8` (the slot HANDLE) to *out.
    // 0xFFFF when the tag is not present.
    //
    // PartSlotRefresh takes its two slot arguments in DIFFERENT namespaces: the first is a tag (matched against
    // bucket keys and record+0xC8), the second is a handle (dereferenced through a lookup). Passing a tag for the
    // second faults. This is how the handle is obtained.
    //
    // The container displacement X is the one operand here the engine renumbers, and it moves on its own while
    // every other byte of the function holds -- so it is wildcarded in the rows below rather than pinned. The same
    // displacement is mirrored by AuthTable::k_containerPtrOffset, whose header explains why a stale copy of it
    // fails silently rather than loudly. Update both together.
    //
    // The rest of the walk is stated literally and does not move with it: array base at container+0x08, live count
    // at container+0x10, entry stride 0xD0, slot tag at entry+0xC8, 0xFFFF written to *out when the tag is absent.
    inline constexpr AddrCandidate k_slotTagToHandleCandidates[] = {
        // P1 -- full prologue: three spills, push rdi, the frame, then the container load and the argument shuffle.
        {"SlotTagToHandle_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC ?? "
         "48 8B 81 ?? 00 00 00 48 8B FA 48 8B E9",
         ResolveMode::Direct, 0, 0},

        // P2 -- the record-walk setup, entirely past the prologue: base/count reads, the stride multiply and the
        // end-pointer form. Independent of the prologue AND of the container displacement, so it is the row that
        // survives the failure mode that killed the previous pair. Anchors at function start + 0x21.
        {"SlotTagToHandle_P2_RecordWalkSetup", "48 8B 58 08 8B 40 10 4C 69 D0 ?? ?? ?? ?? 4C 03 D3 49 3B DA",
         ResolveMode::Direct, -0x21, 0},

        // P3 -- the not-found tail: the 0xFFFF sentinel store into *out followed by the register restores and the
        // frame teardown. Branch-free, and past the whole search loop, so a rewrite of the loop body cannot reach it.
        // Anchors at function start + 0x56.
        {"SlotTagToHandle_P3_SentinelTail",
         "66 C7 07 FF FF 48 8B 5C 24 ?? 48 8B C7 48 8B 6C 24 ?? 48 8B 74 24 ?? 48 83 C4",
         ResolveMode::Direct, -0x56, 0},
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

        // P2 -- post-alloca shuffle: rsi=r9 (swapEntry), the two WORD argument extractions, rcx=a1, then the scratch
        // lea/call/nop and the part-record count load. The shuffle ALONE is not unique -- it also occurs in an
        // unrelated function -- so the row deliberately runs on through the call into `mov edx,[r14+<countOff>]`. The
        // lea and call displacements and the count field offset are wildcarded; only opcodes carry the match.
        {"PartSlotRefresh_P2_PostAllocaThroughCountLoad",
         "49 8B F1 41 0F B7 D8 0F B7 FA 4C 8B F1 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 41 8B 96 ?? ?? ?? ??",
         ResolveMode::Direct, -0x2D, 0},

        // P3 -- the part-record search loop, which encodes the structure rather than the frame: index to r8d, the
        // `lea rax,[rcx+rcx*2]` triple-scale, and the WORD compare of `[r9+rax*8]` against the wanted tag. Survives a
        // prologue reshuffle that would sink both rows above. Stops before the loop's `jz`/`jb`, because a short Jcc
        // flips encoding freely (aob-signatures.md section 9).
        {"PartSlotRefresh_P3_RecordSearchLoop", "41 8B C8 48 8D 04 49 66 41 39 3C C1", ResolveMode::Direct, -0x70, 0},
    };

    /**
     * @brief SlotPopulator -- populates the character's slot array with item visual data then calls
     *        VisualEquipChange. This is the function the server equip handler invokes to trigger a full visual equip
     *        with mesh loading.
     *
     * Signature (x64 __fastcall):
     *   __int64 SlotPopulator(
     *       __int64 a1, unsigned __int16* a2_itemData, __int64 a3_swapEntry)
     *
     * a2 is a 16-byte structure:
     *   +0:  uint16 item ID
     *   +2:  byte   flag (2 = normal equip)
     *   +4:  int32  (-1)
     *   +12: uint16 secondary slot (0xFFFF to skip)
     */
    // WARNING, and the reason every walk-back below is spelled out with the byte lengths that produce it.
    //
    // A Direct-mode walk-back is exactly as build-specific as the pattern bytes are, and nothing in the resolver
    // validates it. A prologue that changes length leaves the body bytes a row matches on untouched, so the row
    // still resolves -- to the wrong address. Landing short puts the target inside the PREVIOUS function's `pop`
    // chain, and calling that corrupts the stack. Re-measure the walk-back whenever the prologue moves, and treat
    // "the pattern still matches" as saying nothing about whether the offset is still right. This target is
    // inline-hooked, so CDCore::Glue::looks_like_function_entry is the runtime backstop for exactly this.
    //
    // Entry layout, with the byte lengths that produce the offsets:
    //     +0x00  48 89 5C 24 08              mov [rsp+8],rbx      (rbx is SPILLED here, not pushed)
    //     +0x05  4C 89 44 24 18              mov [rsp+18],r8
    //     +0x0A  55 56 57 41 54 41 55 41 56 41 57
    //     +0x15  48 8B EC                    mov rbp,rsp
    //     +0x18  48 83 EC ??                 sub rsp,imm8
    //     +0x1C  4C 8B E2 4C 8B E9 33 FF     <- P2 anchors here, so -0x1C
    //     +0x24  89 7D ??
    //     +0x27  41 BE FF FF FF FF           <- P3 anchors here, so -0x27
    inline constexpr AddrCandidate k_slotPopulatorCandidates[] = {
        // P1 -- full prologue through the register shuffle (mov r12,rdx; mov r13,rcx; xor edi,edi). The frame
        // immediate is wildcarded; the spill/push split and the `mov rbp,rsp` framing are what carry the match.
        {"SlotPopulator_P1_FullPrologue",
         "48 89 5C 24 08 4C 89 44 24 18 "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8B EC 48 83 EC ?? "
         "4C 8B E2 4C 8B E9 33 FF",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-alloca anchor: register shuffle + the mov-edi-to-stack + mov r14d, -1 sentinel +
        // movzx eax, r14w. Offset -0x1C backs up to function start.
        {"SlotPopulator_P2_PostAlloca", "4C 8B E2 4C 8B E9 33 FF 89 7D ?? 41 BE FF FF FF FF 41 0F B7 C6",
         ResolveMode::Direct, -0x1C, 0},

        // P3 -- deeper anchor on the mov r14d,-1 sentinel + the inline mov [rbp+X],ax ; mov ebx,0FFFFh follow-up.
        // Skips the register shuffle entirely and pins the post-init block. The frame slot the sentinel is written to
        // is wildcarded because it is a compiler-assigned local. Offset -0x27 backs up to function start.
        {"SlotPopulator_P3_SentinelInit", "41 BE FF FF FF FF 41 0F B7 C6 66 89 45 ?? BB FF FF 00 00",
         ResolveMode::Direct, -0x27, 0},
    };

    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module data globals.
    //
    // Each PrefabWrapperSwap data global resolves through a 3-candidate cascade rather than through a hardcoded
    // absolute address, so a patch that shifts the image layout self-corrects:
    //
    //   StringInfoRegistry  : registry struct.
    //   StringInfoVtable    : vtable sentinel filter.
    //   LoaderRegistry      : partprefab name->wrapper registry.
    //   NaturalPipeline     : engine unlink pipeline. Hooked.
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
     * The entry-array offset tracks the width of the pa::StaticInfoManager2 base and moves when that base changes
     * width (+0x50 against +0x58). The count at +0x08 and the per-entry layout (+0x08 vtable, +0x18 wrapper, +0x20
     * inline name) do NOT move with it. Do NOT blanket-apply a width change to other registries: the sibling
     * LoaderRegistry container field moves the other way.
     *
     * The backing registry class is pa::StringInfoManager (StaticInfoManager2<...> family). The resolver re-finds the
     * live holder each launch, so no absolute address is recorded here.
     *
     * All three candidates anchor on a `mov reg, [rip+disp32]` that loads this address. The disp32 is wildcarded, and
     * the rest of the 16+ byte window is unique text module-wide.
     */
    // Do NOT anchor a row on a manager-lookup primitive. The engine does not reach a manager through
    // `mov reg,[rip+holder]; add reg,<subobj>; lea rdx,[frame]; call`; that shape does not exist in the image, so an
    // `add reg, imm8` row cannot be built here however tempting the idiom looks.
    //
    // The accessor that replaces it is emitted as a byte-identical template clone per manager, differing ONLY in its
    // RIP displacement. Any pattern cut from a clone body therefore matches every manager at once and can never
    // satisfy require_unique. Each row below is anchored in a distinct CALLER instead, which is the only place
    // manager-specific context survives.
    inline constexpr AddrCandidate k_stringInfoRegistryCandidates[] = {
        // P1 -- caller that loads a `+0x828` field and null-checks it before the registry load. That field read is
        // the caller-specific part and carries the whole uniqueness budget; the bucket-probe tail after the load
        // (`cmp dword [reg+0x6C],0` / `mov r8d,[reg+0x68]` / `mov ecx,[rcx+0x18]`) confirms it is a registry access
        // and not an unrelated global.
        {"StringInfoRegistry_P1_Field828GuardedLoad",
         "48 8B BA 28 08 00 00 48 85 FF 74 ?? 4C 8B 15 ?? ?? ?? ?? "
         "41 83 7A 6C 00 74 ?? 45 8B 42 68 8B 49 18",
         ResolveMode::RipRelative, 15, 19},

        // P2 -- interleaved call site. The compiler schedules a `mov [rdi+0xD8],cx` field store and the 0xFFFF
        // sentinel seed around the registry load, so this window is built entirely from game-owned displacements:
        // the two struct fields (0xD8, 0xDC) and the source field (+0x38) carry the whole uniqueness budget. The
        // load destination and the field-store base are compiler-owned and have to be re-cut when they move.
        {"StringInfoRegistry_P2_InterleavedFieldStore",
         "48 8B 2D ?? ?? ?? ?? 45 33 C0 41 BE FF FF 00 00 0F B7 08 "
         "66 89 8F D8 00 00 00 8B 46 38 89 87 DC 00 00 00",
         ResolveMode::RipRelative, 3, 7},

        // P3 -- a third call site, in a caller neither P1 nor P2 touches, so a rewrite of one caller cannot take the
        // whole cascade down. Shape: read a count through rdi, branch out when it is zero, then load the registry and
        // run the same bucket probe. The branch distance is wildcarded and the row stops before the probe's own
        // conditional jumps.
        {"StringInfoRegistry_P3_CountGuardedLoad",
         "8B 7F 0C 45 85 FF 0F 84 ?? ?? ?? ?? 4C 8B 1D ?? ?? ?? ?? "
         "45 8B 63 6C 45 85 E4 74 ?? 45 8B 4B 68 45 85 C9",
         ResolveMode::RipRelative, 15, 19},
    };

    /**
     * @brief StringInfoVtable sentinel (resolved at runtime; no hardcoded address -- an absolute vtable value goes
     *        stale on every game build).
     *
     * Vtable pointer used as the +0x08 sentinel of every StringInfo entry. PrefabWrapperSwap reads it to filter out
     * non-StringInfo heap rows during walk_string_info.
     *
     * The engine references this vtable from many thousands of sites, most of them bulk static-initializer tables
     * that carry no context at all, so a row must carry enough caller-specific context to stay unique. Every row
     * wildcards the RIP displacement and keeps only opcodes, ModRM bytes and game-owned field offsets.
     */
    // All three rows anchor on the same semantic event -- an entry constructor writing this vtable into the entry's
    // `+0x08` slot -- but in three unrelated functions, so no single recompile takes the cascade down. The `+0x08`
    // store is the load-bearing part: it is the field the walk_string_info filter later reads, so a row that matches
    // is a row that proves the value is the entry sentinel and not some other constant.
    //
    // Do not shorten these rows toward the bare `lea`; the surrounding stores are the only thing separating a real
    // ctor from the static-initializer noise.
    inline constexpr AddrCandidate k_stringInfoVtableCandidates[] = {
        // P1 -- ctor that zeroes its header first: `xor r15d,r15d ; mov [rbp+8],r15 ; lea r?,[rip+vtable] ;
        // mov [rbp+8],r12 ; mov [rbp+0x10],r15d ; mov [rbp+0x18],r15 ; mov byte [rbp+0x20],0xFF`. The trailing
        // 0xFF byte-store is the entry's "unset" marker and is what makes the window unique.
        {"StringInfoVtable_P1_ZeroedHeaderCtor",
         "45 33 FF 4C 89 7D 08 4C 8D ?? ?? ?? ?? ?? "
         "4C 89 65 08 44 89 7D 10 4C 89 7D 18 C6 45 20 FF",
         ResolveMode::RipRelative, 10, 14},

        // P2 -- allocate-then-construct site: the indirect allocator call, the two-arm join, then the vtable store
        // into the fresh entry and the `mov rax,[rdi+0x18]` / `mov [rax+r13*8],rbx` publish into the owning vector
        // plus its `inc dword [rdi+4]` count bump.
        {"StringInfoVtable_P2_AllocCtorPublish",
         "48 8B 08 FF 15 ?? ?? ?? ?? EB 07 4C 8D ?? ?? ?? ?? ?? "
         "4C 89 73 08 48 8B 47 18 4A 89 1C E8 FF 47 04",
         ResolveMode::RipRelative, 14, 18},

        // P3 -- a third ctor sharing P2's allocator lead-in but a different tail: the vtable store is followed by a
        // `mov byte [rdi+0x10],1` flag write and a `mov [rdi+0x18],rbp` back-reference. That tail is what separates
        // it from P2; the two rows are in different functions.
        {"StringInfoVtable_P3_FlaggedCtor",
         "48 8B 08 FF 15 ?? ?? ?? ?? EB 07 4C 8D ?? ?? ?? ?? ?? "
         "4C 89 77 08 C6 47 10 01 48 89 6F 18",
         ResolveMode::RipRelative, 14, 18},
    };

    // IteminfoHolder and StringinfoHolder deliberately have NO AOB cascade. Do not add one.
    //
    // A cascade for either would have to anchor on the per-manager accessor, and those accessors are byte-identical
    // template clones differing ONLY in their RIP displacement: any window cut from one matches every manager in the
    // image at once and can never satisfy require_unique. For the iteminfo holder it is worse still -- the slot has
    // exactly ONE referencing instruction anywhere in the image, and it lives inside such a clone.
    //
    // How each is reached instead:
    //
    //   StringinfoHolder -- was never a distinct global. It is the same slot as StringInfoRegistry, so
    //                       itemmesh_dumper resolves it through k_stringInfoRegistryCandidates above.
    //
    //   IteminfoHolder   -- resolved by ItemNameTable's bounded call-graph walk, which reaches the correct clone
    //                       first and only then reads its displacement. Read it via
    //                       ItemNameTable::instance().iteminfo_holder_addr().
    //
    // Layout both consumers depend on, unchanged across this drift: the slot dereferences to the registry struct,
    // whose u32 entry count is at +0x08 and whose qword entry-pointer array is at +0x58. That +0x58 tracks the
    // pa::StaticInfoManager2 base; the adjacent +0x50 slot still holds a valid-looking heap pointer into a DIFFERENT
    // array, so a stale displacement reads the wrong array instead of faulting. The failure is SILENT.

    /**
     * @brief LoaderRegistry singleton -- the engine partprefab name->wrapper registry.
     *
     * The engine's own name lookup dereferences this singleton and queries [+0x50]. PrefabWrapperSwap reads it on
     * init to enumerate prefab wrappers, and indexes that walk instead of calling the engine primitive.
     */
    inline constexpr AddrCandidate k_loaderRegistryCandidates[] = {
        // WARNING for P1. The add-0xD0 window is SHARED: two sites in the module carry it, and only one of them
        // loads THIS registry. The other is a decoy on a different global, separated only by what follows the add --
        // the decoy calls immediately, this site passes an argument first. Keep whatever instruction sits between
        // the add and the call inside the window. Without it the row is a coin flip between two globals, and both
        // outcomes resolve cleanly, so nothing downstream will tell you which one you got.
        //
        // That argument instruction is also the volatile part of the window: it is a one-instruction argument setup
        // whose form the compiler is free to change. Wildcard its operand, never its position.
        //
        // Note also that this registry's container field (the compare in P3) and the sibling
        // pa::StaticInfoManager2 family (see k_stringInfoRegistryCandidates) move in OPPOSITE directions across
        // builds. Never blanket-apply a layout shift from one registry to another.

        // P1 -- distinctive 64-bit add-immediate `48 81 C1 D0 00 00 00` (add rcx, 0xD0) after the registry load.
        // That is a stable game-struct walk offset. See the decoy warning above for why the window runs past it.
        {"LoaderRegistry_P1_AddD0CallSite",
         "48 8B 0D ?? ?? ?? ?? 48 81 C1 D0 00 00 00 48 8B ?? E8 ?? ?? ?? ?? 48 85 C0 0F 85",
         ResolveMode::RipRelative, 3, 7},

        // P1B -- an independent site that does not touch the add-0xD0 window at all, so it cannot inherit P1's decoy
        // ambiguity. Loads the registry into rbx, spills a frame pointer, then dispatches through the registry's own
        // `+0xDC` member. Do not go looking for a four-argument call variant with a three-lea argument setup here;
        // that shape does not exist in the image.
        {"LoaderRegistry_P1B_MemberDispatchSite",
         "48 8B 1D ?? ?? ?? ?? 48 89 7D E7 FF 83 DC 00 00 00",
         ResolveMode::RipRelative, 3, 7},

        // P2 -- load the registry, walk `+0x70`, then read the count at `+0x04` of that sub-object. The load's
        // destination register is compiler-owned, and the two instructions after it name the same register, so the
        // trio moves as a unit and there is nothing useful to wildcard: pin it and let the row fail loudly into P3
        // if it rotates.
        {"LoaderRegistry_P2_RegistryWalkToCount", "4C 8B 3D ?? ?? ?? ?? 49 83 C7 70 45 8B 67 04",
         ResolveMode::RipRelative, 3, 7},

        // P3 -- a STORE (`mov [rip+disp32], rbx`) that initializes the singleton at engine-init time, not a load. The
        // disp32 still resolves to the singleton address. Distinctive context: an inline `EB 03` short jump and the
        // container-field compare.
        //
        // The container field displacement is wildcarded to its low two bytes. That field tracks the registry
        // container's layout and moves on its own while every other byte in the window stays put, so a pinned form
        // silently matches nothing. The `04 00` high half stays literal: a displacement in the 0x0004xxxx range is
        // what keeps the compare distinguishable from an ordinary small-offset one.
        {"LoaderRegistry_P3_InitStoreSite",
         "48 89 03 48 89 1D ?? ?? ?? ?? EB 03 48 8B DF "
         "48 3B 9E ?? ?? 04 00",
         ResolveMode::RipRelative, 6, 10},
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
        // wildcarded, and the frame-independent body shuffle (a3 and a2 and a1 parked in callee-saved registers,
        // then `cmp [a3+8]`) carries the uniqueness.
        //
        // The last two shuffle moves are wildcarded down to their opcode, and P3 wildcards all three. Each is a
        // 3-byte reg-to-reg `mov`, so the window length is fixed either way, while WHICH register each one lands in
        // is exactly what register allocation drifts -- one changed byte there is enough to retire every row that
        // pins it. The `48 2B E0` chkstk adjustment ahead of the shuffle and the `41 83 78 08 00` argument test
        // behind it are what carry the match.
        {"NaturalPipeline_P1_FullPrologueChkstk",
         "48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF "
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? "
         "48 2B E0 4D 8B E8 ?? 8B ?? ?? 8B ?? 41 83 78",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill prologue. Anchors past the first arg-home store onto the (wildcarded) lea rbp + chkstk
        // pair and the body shuffle. Walk-back -5 = past `48 89 5C 24 10` to start.
        {"NaturalPipeline_P2_PostArgSpill",
         "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 "
         "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF "
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? "
         "48 2B E0 4D 8B E8 ?? 8B ?? ?? 8B ?? 41 83 78",
         ResolveMode::Direct, -0x05, 0},

        // P3 -- chkstk size (wildcarded) + stack adjustment + arg-shuffle. The 48 2B E0 (sub rsp, rax) is the
        // conventional __chkstk post-call. Walk-back -0x27 to function start.
        {"NaturalPipeline_P3_PostChkstkArgShuffle",
         "B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 ?? 8B ?? ?? 8B ?? "
         "?? 8B ?? 41 83 78 08 00",
         ResolveMode::Direct, -0x27, 0},
    };

    // There is deliberately NO name->wrapper lookup primitive anchored here.
    //
    // The engine's own primitive would be the obvious way to turn a prefab name into a wrapper, but LT does not call
    // it: PrefabWrapperSwap already walks the same loader-registry name table for its catalog, so indexing that walk
    // in-process answers the same question with no anchor to maintain, no direct call into engine code with a raw
    // name pointer, and an O(1) hash hit per name instead of a per-name engine query.
    //
    // Do not re-add one. The gain would be nil and the cost is another cascade to re-verify every patch day.

    // -----------------------------------------------------------------------
    // PrefabWrapperSwap module function targets.
    //
    // Each carries its own cascade, ordered most-specific-first per
    // CrimsonDesertCore/external/DetourModKit/docs/misc/aob-signatures.md.
    //
    //   PartListMerge    : part-list assembly. Hooked to learn which actor the following struct-copies belong to.
    //   UnlinkByWrapper  : unlink one wrapper from a body. Called direct.
    //   PartDescriptorBuild : per-socket descriptor build. Hooked by SocketMeshOverride to rewrite the mesh.
    //   StructCopy       : 0x40-byte struct-copy hot path, inline-hooked to swap source wrapper-ptrs.
    //
    // Verify every row's hit count against the live module before you ship a change. Where a function has a sibling
    // clone (a linker-emitted duplicate compiled from a templated header) and no global anchor is unique, the cascade
    // leads with a RipRelative call-site anchor. That anchor walks an `E8 disp32` from a known caller, which IS
    // unique, to the canonical target.
    // -----------------------------------------------------------------------

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
        // P1 -- prologue through the argument shuffle and into the three-way count sum. The register-save block
        // ALONE is not unique (the push-run plus large-frame-lea shape matches double digits of functions
        // module-wide), so the pattern deliberately runs on through chkstk into the shuffle and the count reads,
        // which is what makes it a single hit.
        //
        // Whether a given callee-saved register is pushed or spilled to its home slot is compiler-owned, and so is
        // which register each shuffle move lands in. The three moves are wildcarded to their opcode for the same
        // reason as NaturalPipeline: they are fixed-length reg-to-reg moves, so wildcarding costs no window length,
        // and the `8B 51 60 03 51 48` count sum behind them is what carries the match.
        {"PartListMerge_P1_PrologueThroughArgShuffle",
         "48 89 54 24 10 "
         "55 53 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? ?? ?? "
         "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 "
         "?? 8B ?? ?? 8B ?? ?? 8B ?? 8B 51 60 03 51 48",
         ResolveMode::Direct, 0, 0},

        // P2 -- argument shuffle plus the three-way count sum that sizes the destination reserve
        // (`list2count + list1count + a2count`). Unique on its own and independent of the prologue, so it survives a
        // register-save reshuffle that P1 would miss. It matches INSIDE the function, so disp_offset walks back to the
        // entry; re-measure that delta on patch day, and note the caller's prologue sanity check is what catches it
        // when the delta drifts.
        {"PartListMerge_P2_ArgShuffleCountSum",
         "?? 8B ?? ?? 8B ?? ?? 8B ?? "
         "8B 51 60 03 51 48 41 03 ?? 08 41 39 50 0C",
         ResolveMode::Direct, -0x26, 0},
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

    /**
     * @brief PartDescriptorBuild -- builds the part descriptor for ONE socket and appends it to the rebuild request.
     *
     * `sub_14081DD40(a1, &partId, slotTag, a4, a5, record, outList)`. It expands the part to mesh ids
     * (`sub_142074920`), and for each one takes the canonical wrapper (`sub_1403120F0(meshId) + 0x18`) into the
     * descriptor's FIRST field before appending the 112-byte descriptor to `outList` through `sub_14037EC40`.
     *
     * That first field is the mesh that will be attached to the socket, which makes this the override point: the
     * slot tag is an argument here, whereas the append itself (already hooked as StructCopy) cannot tell which
     * socket it is serving.
     */
    inline constexpr AddrCandidate k_partDescriptorBuildCandidates[] = {
        // P1 -- full prologue: the `mov rax,rsp` frame plus four argument spills and 8 pushes. One match module-wide.
        {"PartDescriptorBuild_P1_FullPrologue",
         "48 8B C4 4C 89 48 20 66 44 89 40 18 48 89 50 10 48 89 48 08 "
         "55 53 56 57 41 54 41 55 41 56 41 57",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-frame argument shuffle plus the item-id sentinel test. Anchors past the prologue entirely, so a
        // spill reorder or a frame resize that sinks P1 leaves this row standing. Shape: the four argument moves
        // (r9->rbx, r8w->esi, rdx->r13, rcx->r14), the xor/spill of the loop counter, then `mov eax,0FFFFh` and the
        // WORD compare against `*partId` that decides whether there is anything to build. The frame displacement is
        // wildcarded; the 0xFFFF sentinel is semantic and stays literal. Anchors at function start + 0x33.
        {"PartDescriptorBuild_P2_ArgShuffleSentinelTest",
         "49 8B D9 41 0F B7 F0 4C 8B EA 4C 8B F1 33 FF 89 7C 24 ?? B8 FF FF 00 00 66 3B 02",
         ResolveMode::Direct, -0x33, 0},
    };

    /**
     * @brief Claim-walk deref site -- the shape ClaimWalkGuard patches. NOT an AddrCandidate cascade.
     *
     * Deliberately outside the cascade: `resolve_address` requires a UNIQUE match, and this pattern is expected to
     * hit MORE THAN ONE site (two, at time of writing). Feeding a knowingly non-unique signature through the cascade
     * would mean weakening `require_unique`, which is the invariant that catches a drifted signature on patch day.
     * It lives here anyway so the whole engine-address inventory stays in one file.
     *
     *   mov  rax, [rbx+8]        48 8B 43 08     entry's owning pointer (claim vector, `node+0x58`)
     *   mov  rcx, [rax+28h]      48 8B 48 28     faults when the owner is null
     *   test rcx, rcx            48 85 C9
     *   jz   <next entry>        74 ??           rel8 to the loop-continue label
     *
     * The signature stops BEFORE that `jz`, per the short-Jcc rule in aob-signatures.md section 9: a compiler is free
     * to emit the branch as `0F 84 rel32` instead, which would change the opcode byte and retire the row. The three
     * instructions that remain are already exactly as selective (verified: same two sites with and without the
     * branch). The guard still needs the branch, so it VALIDATES the opcode at @ref k_claimWalkJzOffset at install
     * time and skips any site that does not carry it -- which turns an encoding change into a logged skip instead of
     * a silent no-match.
     *
     * The claim erase nulls owner slots while they are still inside the count and only decrements the count once its
     * shift finishes, so a walk overlapping an erase reads a null owner here. Nothing locks: the engine is safe only
     * because its own erases and walks are scheduled as jobs that never overlap. LT drives erases from its apply
     * worker, so they can. See claim_walk_guard.hpp for the guard and the reasoning.
     *
     * Patch day: the guard logs the site count it found and warns when it is not the expected two. Only the
     * RBX-based encoding is listed -- a walker allocated to another register would need its site branch-checked by
     * hand before being added.
     */
    inline constexpr const char *k_claimWalkSiteAob = "48 8B 43 08 48 8B 48 28 48 85 C9";

    /// Sites @ref k_claimWalkSiteAob is known to occupy. A mismatch means the walk survey needs redoing.
    inline constexpr std::size_t k_claimWalkExpectedSites = 2;

    /// Offset from @ref k_claimWalkSiteAob to `mov rcx,[rax+28h]` -- the instruction the guard precedes.
    inline constexpr std::size_t k_claimWalkDerefOffset = 4;

    /// Offset from @ref k_claimWalkSiteAob to the loop-continue `jz rel8`, whose target the guard decodes.
    inline constexpr std::size_t k_claimWalkJzOffset = 11;

    /**
     * @brief StructCopy -- 0x40-byte struct-copy hotpath.
     *
     * Signature: `__int64(*)(dst, src)`. The function copies a partprefab wrapper-related struct field-by-field.
     * PrefabWrapperSwap installs an inline hook here and (when LT-active) substitutes carrier source wrappers with
     * target wrappers for the duration of the copy.
     *
     * The function reads the engine's StringInfo vtable sentinel through a `lea rax, [rip+disp32]` early in the body.
     * That single RIP-rel displacement is wildcarded. All other bytes in the patterns below are stable.
     *
     * If these break, note that the function's shape is `dst,src -> mov [dst], 0 ; copy src->dst ; lea rax, [vtable] ;
     * mov [src], rax ; movzx-byte transfers from [src+8..src+0xA] into [dst+8..]`. Re-anchor on the byte-transfer
     * block (P3 below). It is the most function-specific shape and the least likely to shuffle.
     */
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
     * The prologue spills rbx to its home slot, homes two register arguments, saves five callee-saved registers
     * (rsi/rdi/r12/r14/r15), then runs the field-by-field copy through the first 0x60 bytes of the iteminfo struct.
     * No RIP-relative bytes inside the chosen anchor windows -- wildcards are not needed for addressing.
     *
     * Entry layout, with the byte lengths that produce the walk-backs:
     *     +0x00  48 89 5C 24 18                     mov [rsp+18],rbx
     *     +0x05  two 5-byte arg-home spills         (wildcarded, see P1)
     *     +0x0F  56 57 41 54 41 56 41 57            five pushes
     *     +0x17  48 83 EC 20
     *     +0x1B  48 8B F2 ...                       <- P2 anchors here, so -0x1B
     *     +0x4F  C5 F8 10 42 28 ...                 <- P3 anchors here, so -0x4F
     */
    inline constexpr AddrCandidate k_dyeCopierCandidates[] = {
        // P1 -- full prologue + first three field copies. The `48 8B F2 4C 8B F1` (mov rsi,rdx ; mov r14,rcx)
        // arg-shuffle followed by the qword/word/word field copies through [rdx+0..0xA] is unique to this
        // iteminfo-copy function. One match module-wide.
        //
        // The ten bytes between the rbx spill and the push run are two arg-home spills. They are compiler-owned and
        // drift, so they are wildcarded rather than pinned; the field-copy chain at the end is what makes the row
        // unique. Their LENGTH is load-bearing even though their content is not -- it is what puts the push run at
        // +0x0F and both walk-backs below where they are.
        {"DyeCopier_P1_FullPrologue",
         "48 89 5C 24 18 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? "
         "56 57 41 54 41 56 41 57 "
         "48 83 EC 20 48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-shuffle anchor on the field-copy chain. Walk-back -0x1B to function start. Survives a future
        // build that drops or reorders the early callee-save pushes, because the field-copy shape is the
        // function-defining behavior.
        {"DyeCopier_P2_FieldCopyChain",
         "48 8B F2 4C 8B F1 48 8B 02 48 89 01 0F B7 42 08 66 89 41 08 "
         "0F B7 42 0A 66 89 41 0A 48 8B 42 10 48 89 41 10",
         ResolveMode::Direct, -0x1B, 0},

        // P3 -- mid-body AVX xmm copy. The `vmovups xmm0, [rdx+28h] ; vmovups [rcx+28h], xmm0` pair followed by the
        // `vmovsd` qword move and continued field copies is a unique SSE/AVX shape this function emits at offset
        // +0x4F. Walk-back -0x4F to function start. Anchors entirely past the prologue, so a prologue-shape shuffle
        // does not sink P3.
        {"DyeCopier_P3_AvxFieldCopy",
         "C5 F8 10 42 28 C5 F8 11 41 28 C5 FB 10 4A 38 C5 FB 11 49 38 "
         "0F B7 42 40 66 89 41 40 48 8B 42 48 48 89 41 48",
         ResolveMode::Direct, -0x4F, 0},
    };

    /**
     * @brief DyeCopy -- 16-byte ARMOR_MOD record-copy primitive.
     *
     * Signature `__int64(*)(vector_t* dst, const ArmorMod16* src)` -- grows dst's 16-byte-stride array if needed, then
     * writes one record by reading fields from `[rdx+0..0xC]`. The dye detour calls this directly post-trampoline to
     * append fabricated dye records.
     *
     * The function prologue is the engine's universal grow-and-emplace template, with dozens of byte-identical
     * instances module-wide, so no row may rest on the prologue alone. What singles this one out is the 16-byte
     * record-copy emitter (shl by 4 for the stride ; add the array base ; the byte-by-byte transfer of channel /
     * R / G / B / 0xFF / repair_byte from [rdi+6..0xB] into the record). That shape is what makes this primitive the
     * "ARMOR_MOD writer" rather than a generic vector grow.
     *
     * If these break, re-anchor on the shift-by-4-stride + add-base-pointer + write-hash-u32 triplet
     * (`48 C1 E? 04 ; 48 03 1? ; 89 0?` in whichever cursor register the build picked). It is the function's
     * signature behavior, and a future build is unlikely to alter it without also redesigning the ARMOR_MOD record
     * layout. The cursor register is compiler-owned, which is why P2 and P3 wildcard the store destinations: anchor
     * on the shape, never on the register.
     *
     * Entry layout, with the byte lengths that produce the walk-backs:
     *     +0x00  48 89 5C 24 08 / 57 / 48 83 EC 20 / 48 8B D9 / 48 8B FA    prologue and arg capture
     *     +0x10  8B 49 08 / 8B 43 0C / 3B C1 / 77 ??                        count vs capacity, skip the grow
     *     +0x1A  the grow-size arithmetic and the grow call
     *     +0x41  the stride shift and record write                          <- P2 anchors here, so -0x41
     *     +0x75  the last byte-pair, count++ and epilogue                   <- P3 anchors here, so -0x75
     */
    inline constexpr AddrCandidate k_dyeCopyCandidates[] = {
        // P1 -- true prologue through the capacity check and the grow call. Shape: spill rbx, push rdi, take the 0x20
        // frame, capture both arguments, then read the live count from `[rcx+0x08]` and the capacity from
        // `[rbx+0x0C]` and skip the grow while the capacity still has room. The grow size is the engine's 1.5x rule,
        // `(3 * capacity + 1) >> 1`, clamped below via `cmovb` and above via `cmova`. The `77 ??` rel8 is wildcarded
        // because the jump distance is compiler-owned. One match module-wide.
        //
        // Anchoring at the entry is deliberate: it gives the cascade one row that does not depend on the grow-size
        // arithmetic at all, which is the part a compiler is most free to re-associate.
        {"DyeCopy_P1_PrologueToGrowCheck",
         "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA "
         "8B 49 08 8B 43 0C 3B C1 77 ?? 8D 14 45 01 00 00 00 03 D0 "
         "B8 01 00 00 00 D1 EA 3B D0 0F 42 D0 3B CA 0F 47 D1 48 8B CB E8 "
         "?? ?? ?? ?? 8B 53 08 8B 07 48 C1 E2 04 48 03 13 89 02",
         ResolveMode::Direct, 0, 0},

        // P2 -- 16-byte record-copy emitter body. The shift-by-4 / add-base / store-hash triplet computes the
        // next-record byte address (count<<4 = 16-byte stride), then the byte-by-byte transfers fan out: word
        // `[rdi+4..5]`, then singles for channel (`+6`), R (`+7`), G (`+8`), B (`+9`). This shape is what makes the
        // function the ARMOR_MOD writer. The destination register of each store is wildcarded, because that is the
        // one part of this block register allocation moves. One match module-wide. Walk-back -0x41 to function
        // start.
        {"DyeCopy_P2_ArmorModRecordCopy",
         "48 C1 E2 04 48 03 13 89 02 0F B7 47 04 66 89 ?? 04 "
         "0F B6 47 06 88 ?? 06 0F B6 47 07 88 ?? 07 "
         "0F B6 47 08 88 ?? 08 0F B6 47 09 88 ?? 09",
         ResolveMode::Direct, -0x41, 0},

        // P3 -- tail of the byte-by-byte copy + post-write count++ + ret. The trailing field transfers from
        // `[rdi+0xB]` and `[rdi+0xC]`, followed by `inc dword [rbx+8]` (count++) and the standard `pop rdi ; ret`
        // epilogue, are unique to this exact function shape. One match module-wide. Walk-back -0x75 to function
        // start. The `FF 43 08` count increment proves the dst is a vector with a count field at +8.
        {"DyeCopy_P3_TailCountInc",
         "0F B6 47 0B 88 ?? 0B 0F B6 47 0C 88 ?? 0C "
         "FF 43 08 48 8B 5C 24 30 48 83 C4 20 5F C3",
         ResolveMode::Direct, -0x75, 0},
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
     * The prologue homes two register arguments, saves eight callee-saved registers (rbp, rbx, rsi, rdi, r12-r15),
     * then frames with `lea rbp,[rsp-disp8]; sub rsp, imm32`. Both frame immediates are compiler-owned and are
     * wildcarded in every row below. What identifies the function is the arg-home stores, the eight-push saved-reg
     * run, and the arg-reload quad (r12<-r9, rdi<-r8, r13<-rdx, r15<-rcx).
     */
    inline constexpr AddrCandidate k_colorPublisherCandidates[] = {
        // P1 -- full prologue through the arg-reload quad. Both frame immediates are wildcarded because the frame
        // size and the lea displacement shift whenever a patch adds or removes locals. The quad itself is what
        // identifies the function, and it is pinned. One match module-wide.
        {"ColorPublisher_P1_FullPrologue",
         "4C 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 "
         "41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4D 8B E1 49 8B F8 "
         "4C 8B EA 4C 8B F9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-saves frame anchor. Picks up at the `lea rbp,[rsp-disp8]; sub rsp, imm32` pair followed by the
        // arg-reload quad, and runs on into the zeroed stack slot after it so the window is not just the quad.
        // Walk-back -0x16 (10 bytes of arg homes, 12 of pushes) to function start.
        {"ColorPublisher_P2_PostSavesFrame",
         "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4D 8B E1 49 8B F8 4C 8B EA 4C 8B F9 "
         "33 C9 89 4C 24",
         ResolveMode::Direct, -0x16, 0},

        // P3 -- mid-body host-context load, entirely past the prologue and branch-free:
        //   mov rax,[rip+global]        ; the world/host singleton slot
        //   mov r10,[rax]
        //   mov rsi,[r10+0x0004xxxx]    ; the sub-object this publisher runs against
        //   mov [rbp-X],rsi ; mov [rbp+X],rcx ; lea rax,[rsp+X] ; mov [rbp+X],rax
        // The RIP displacement and every frame slot are wildcarded; the load chain and the 0x0004xxxx sub-object
        // displacement are what make the window unique. Walk-back -0x34 to function start.
        //
        // Do not try to anchor this row on the permutations-token canary XOR (`xor r10b,al ; and r10b,1 ; or al,2`).
        // That bit-twiddle does not exist in the image; the host-context load is what this function reliably emits.
        {"ColorPublisher_P3_HostContextLoad",
         "48 8B 05 ?? ?? ?? ?? 4C 8B 10 49 8B B2 ?? ?? 04 00 "
         "48 89 75 ?? 48 89 4D ?? 48 8D 44 24 ?? 48 89 45 ??",
         ResolveMode::Direct, -0x34, 0},
    };

    /**
     * @brief HostScope OwnerVfunc1 -- per-host owner-container vtable slot that invokes the matInst-list copy loop,
     *        which in turn dispatches the publisher. Mid-hooked by ColorOverride::HostScope to capture rcx (the live
     *        owner container) for the player-vs-NPC election.
     *
     * The function is one of three byte-identical sibling thunks, so nothing inside the thunk can single it out. The
     * only discriminator is the preceding function, which is why the single row below anchors on that function's
     * tail epilogue plus the alignment padding and then walks FORWARD into the prologue.
     */
    // There are THREE byte-identical clones of this thunk. They differ only in two `call rel32` displacements, and
    // those displacements resolve to the SAME two absolute targets, so the three functions are literally the same
    // code emitted three times. No window cut from the thunk body can ever tell them apart.
    //
    // The clone this row selects is the one the vast majority of vtables reference, which maximizes what an
    // observational mid-hook sees. Because the hook only reads RCX and never alters control flow, resolving to a
    // different clone would be harmless, but it would observe far fewer containers. Prefer the most-referenced one
    // when re-deriving.
    //
    // That leaves the preceding function as the only discriminator, so the row below crosses inter-function padding
    // -- normally forbidden, and the reason this cascade is one row rather than three. The padding bytes are kept
    // LITERAL for exactly that reason: if the linker rebalances them the row fails to match and the feature disables
    // itself, which is the correct outcome. It must never silently resolve to a neighboring function. The durable
    // replacement is an RttiVtable tier plus a slot index, which needs a resolver change, not another byte row.
    inline constexpr AddrCandidate k_hostScopeVfunc1Candidates[] = {
        // P1 -- preceding-function tail (a two-arm indirect-dispatch epilogue ending in `jmp rax`), then six bytes of
        // `CC` alignment, then the thunk prologue through its `xor r14d,r14d ; mov rdi,r9` head. Walk forward +0x1A
        // (20 bytes of tail + 6 of padding) to the thunk entry.
        {"HostScopeVfunc1_P1_PrevTailPadStart",
         "48 8B 41 78 49 8B C8 48 FF E0 48 63 49 68 49 03 C8 48 FF E0 "
         "CC CC CC CC CC CC "
         "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 "
         "45 33 F6 49 8B F9",
         ResolveMode::Direct, +0x1A, 0},
    };

    /**
     * @brief HostScope OwnerVfunc2 -- sibling per-host owner-container vtable slot. Same role as Vfunc1 (capture rcx
     *        as the live owner container) but with a distinct prologue, so it admits a direct-prologue anchor without
     *        relying on the preceding function.
     */
    inline constexpr AddrCandidate k_hostScopeVfunc2Candidates[] = {
        // P1 -- full prologue. Spills three args, pushes rdi/r14/r15, allocates 0x60 of stack, then loads rbx from
        // a2 and a1 into a register of the compiler's choosing, zeros r14d, and tests r9b (the inline-call
        // optimization flag arg). The 0x60 frame and the `xor r14d,r14d ; test r9b,r9b` flag test are what make this
        // prologue distinctive against the siblings; the a1 destination register is wildcarded because it is the
        // volatile part. One match module-wide.
        //
        // Whether an argument is homed to its stack slot or its register is pushed is compiler-owned, so re-measure
        // P2's walk-back whenever that split changes.
        {"HostScopeVfunc2_P1_FullPrologue",
         "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 "
         "57 41 56 41 57 48 83 EC 60 48 8B DA 48 8B ?? 45 33 F6 45 84 C9",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-arg-spill + frame setup + arg flag test. Anchors past the three 5-byte arg-home stores, so the
        // walk-back is -0x0F. Independent of the arg-home block, which is the part of the prologue that reshapes
        // most; the 0x60 frame and the `xor r14d,r14d ; test r9b,r9b` flag test carry the match.
        {"HostScopeVfunc2_P2_PostArgSpillFlagTest",
         "57 41 56 41 57 48 83 EC 60 48 8B DA 48 8B ?? 45 33 F6 45 84 C9",
         ResolveMode::Direct, -0x0F, 0},

        // P3 -- the owner-container vtable dispatch, entirely past the prologue. Shape: form `lea rdx,[rbx+8]`,
        // test the owner for null, `cmovz` to the fallback, load the vtable and call through `[vtbl+0x320]`.
        // Walk-back -0x3B to function start.
        //
        // The trailing `mov rbx,rax ; lea rdx,[rdi+8]` is load-bearing, not padding: the same
        // lea/test/cmovz/vtable-call shape repeats a second time later in this very function, and without the tail
        // the row matches both and fails require_unique.
        {"HostScopeVfunc2_P3_OwnerVtableDispatch",
         "48 8D 53 08 48 85 DB 49 0F 44 D6 48 8B 06 48 8B CE "
         "FF 90 20 03 00 00 48 8B D8 48 8D 57 08",
         ResolveMode::Direct, -0x3B, 0},
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
     * present, and tail-jumps to a downstream writer on mismatch. Sibling functions share the entire byte-compare
     * body but read the property bytes with wider loads instead of one at a time (`41 0F B6 00` at +0x29). No single
     * byte separates this function from all of them; see the discriminator note on the candidate table below for the
     * pair of struct offsets and the byte-by-byte chain that every row has to carry.
     */
    inline constexpr AddrCandidate k_setterByteCandidates[] = {
        // BEWARE the near-clone that sits directly after this function. It shares the entire byte-compare chain and
        // differs only in its head (`mov rax,[rcx+0x98]` against `[rcx+0x78]`) and its second gate (`cmp [rcx+0xD8]`
        // against `[rcx+0xC8]`), so both gates have to be inside every window.
        //
        // The entry block is volatile across builds. A patch can invert the first test polarity (`74` je against `75`
        // jne) or insert another gate, which breaks any row that pins the old shape. P1 and P2 are therefore a
        // specific-then-general pair over the same window, and P3 drops the first gate entirely so a rewrite of the
        // callback-present test cannot take all three down.

        // Why every row below spans a wildcarded rel8 branch, against the usual rule.
        //
        // This function and its near-clone differ in exactly TWO bytes across their entire bodies: the callback field
        // (`[rcx+0x78]` against `[rcx+0x98]`) at offset 0, and the second gate (`[rcx+0xC8]` against `[rcx+0xD8]`) at
        // offset 9. A short conditional jump sits between them, so no branch-free window can contain both, and
        // without both the row matches the clone too. There is no branch-free discriminator to prefer here.
        //
        // The family is larger than the one near-clone: five groups of two clones share this body. Only this group
        // compares the property bytes ONE AT A TIME (`movzx eax,byte [r8+n]` / `cmp [r9+n],al` for n = 0..3); the
        // other four use wider loads. That byte-by-byte chain is therefore mandatory in every row -- the entry gates
        // alone match four functions.
        //
        // Entry layout, with the deltas that produce the walk-backs:
        //     +0x00  48 8B 41 78 48 85 C0 75 ??        callback present?
        //     +0x09  48 39 81 C8 00 00 00 74 ??        <- P3 anchors here, so -9
        //     +0x12  45 33 C9 4C 8D 52 F8 48 85 D2 48 63 51 70 4D 0F 44 D1 83 FA FF 74 ??
        //     +0x29  41 0F B6 00 4D 8D 0C 12 41 38 01  first byte compare

        // P1 -- entry gates through the SECOND byte compare. Most specific row.
        {"SetterByte_P1_FullPrologue",
         "48 8B 41 78 48 85 C0 75 ?? 48 39 81 C8 00 00 00 74 ?? "
         "45 33 C9 4C 8D 52 F8 48 85 D2 48 63 51 70 4D 0F 44 D1 83 FA FF 74 ?? "
         "41 0F B6 00 4D 8D 0C 12 41 38 01 75 ?? 41 0F B6 40 01 41 38 41 01",
         ResolveMode::Direct, 0, 0},

        // P2 -- same window, stopping after the FIRST byte compare. Survives a reflow of the compare chain's tail.
        {"SetterByte_P2_WildcardedJumpDist",
         "48 8B 41 78 48 85 C0 75 ?? 48 39 81 C8 00 00 00 74 ?? "
         "45 33 C9 4C 8D 52 F8 48 85 D2 48 63 51 70 4D 0F 44 D1 83 FA FF 74 ?? "
         "41 0F B6 00 4D 8D 0C 12 41 38 01",
         ResolveMode::Direct, 0, 0},

        // P3 -- drops the first gate and opens on the second, which is the gate that carries the clone
        // discriminator (0xC8 against the clone's 0xD8). Survives a rewrite of the callback-present test.
        {"SetterByte_P3_SecondGateToByteCompare",
         "48 39 81 C8 00 00 00 74 ?? "
         "45 33 C9 4C 8D 52 F8 48 85 D2 48 63 51 70 4D 0F 44 D1 83 FA FF 74 ?? "
         "41 0F B6 00 4D 8D 0C 12 41 38 01",
         ResolveMode::Direct, -0x09, 0},
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
     * The shared head is the 19-byte run:
     *
     *   41 B9 FF FF 02 00      mov  r9d, 0x2FFFF       ; sentinel cap
     *   41 B8 01 00 00 00      mov  r8d, 1             ; index arg, materialized as a constant
     *   48 8D 15 ?? ?? ?? ??   lea  rdx, [rip+name]    ; property name
     *
     * The middle instruction is the volatile one: the compiler is free to derive that operand from a counter
     * register (`lea r8d, [reg+1]`, 4 bytes) instead of materializing it, which changes the head LENGTH. Keep
     * k_colorTokenRegistrarCallAobHeadLen and these rows in step, or the walker decodes names from the wrong
     * address on every hit and discovery silently reports zero slots.
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
        // P1 -- 19-byte literal head. Anchors on the run from `mov r9d, 0x2FFFF` through `lea rdx, [name]`.
        //
        // The middle instruction is the one the compiler is free to re-materialize: it can emit the operand as a
        // constant (`mov r8d, 1`, 6 bytes) or derive it from a counter register (`lea r8d, [reg+1]`, 4 bytes). Its
        // LENGTH is therefore part of the head, so k_colorTokenRegistrarCallAobHeadLen and the lea offset derived
        // from it must be re-measured together with these rows. A head length that disagrees with the pattern makes
        // the walker decode a name from the wrong address on every hit: every candidate then fails the allow-list,
        // discovery reports zero slots, and color override silently has nothing to bind.
        "41 B9 FF FF 02 00 "
        "41 B8 01 00 00 00 "
        "48 8D 15 ?? ?? ?? ??",

        // P2 -- head + lea-rcx-slot + call tail. Captures calls 2..N within each registrar (these load rcx via `lea
        // rcx, [rip+slot]` to the current property's backing storage). Tighter than P1 and survives a future compiler
        // reflow that changes the head shape as long as the rcx-load + call tail is preserved.
        "41 B9 FF FF 02 00 "
        "41 B8 01 00 00 00 "
        "48 8D 15 ?? ?? ?? ?? "
        "48 8D 0D ?? ?? ?? ?? E8",

        // P3 -- head + mov-rcx-reg + call tail. Captures the first registration call per registrar function (it loads
        // rcx from a preloaded table-base register through `mov rcx, rsi` or `mov rcx, rbx`). It matches only a
        // handful of sites: the two registrars, plus unrelated callers that share the shape and that the name
        // allow-list filters out.
        "41 B9 FF FF 02 00 "
        "41 B8 01 00 00 00 "
        "48 8D 15 ?? ?? ?? ?? "
        "48 8B ?? E8",
    }};

    /**
     * Byte width of the literal head shared by all k_colorTokenRegistrarCallAobs candidates. The walker uses this to
     * step the cursor past a matched anchor before scanning for the next hit.
     */
    inline constexpr std::size_t k_colorTokenRegistrarCallAobHeadLen = 19;

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
     * Entry layout, with the byte lengths that produce the walk-backs in the candidate table:
     *   +0x00  48 89 5C 24 18             mov  [rsp+18h], rbx   ; rbx is SPILLED here, not pushed
     *   +0x05  44 89 4C 24 20             mov  [rsp+20h], r9d   ; home a3
     *   +0x0A  48 89 54 24 10             mov  [rsp+10h], rdx   ; home a1
     *   +0x0F  55 56 57                   push rbp/rsi/rdi
     *   +0x12  41 54 41 55 41 56 41 57    push r12/r13/r14/r15
     *   +0x1A  48 8D AC 24 ?? ?? FF FF    lea  rbp, [rsp-disp32]
     *   +0x22  48 81 EC ?? ?? 00 00       sub  rsp, imm32
     *   +0x29  48 8B 41 08 ...            registry read and arg parking
     *
     * The register-save run alone is NOT unique -- it matches several functions -- so every row runs on into the
     * registry read. Both frame immediates are compiler-owned and are wildcarded in every row: pinning them buys no
     * uniqueness that the body read does not already provide, and costs the row on the next frame resize.
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
        // The push set swapped rbx for r13 and both frame immediates moved, so the prologue alone now matches five
        // functions; the `mov rax,[rcx+8]` body read is required to single this one out. Both frame immediates are
        // wildcarded, as is the register the a1 pointer is parked in.
        //
        // The leading `mov [rsp+18],rbx` is the real entry and MUST stay the first byte of this row. Opening on the
        // r9d arg-home store instead resolves to entry+5, which is inside the function: the inline hook's trampoline
        // then skips the rbx spill, and the epilogue restores rbx from stack the spill never wrote. Re-measure both
        // walk-backs below against this row whenever the prologue changes.
        {"HelmAudioRegistrar_P1_FullPrologue",
         "48 89 5C 24 18 "
         "44 89 4C 24 20 "
         "48 89 54 24 10 "
         "55 56 57 41 54 41 55 41 56 41 57 "
         "48 8D AC 24 ?? ?? FF FF "
         "48 81 EC ?? ?? 00 00 "
         "48 8B 41 08 4C 8B ?? 48 89 45 90 48 8B FA",
         ResolveMode::Direct, 0, 0},

        // P2 -- post-prologue lea+sub+arg-stash chain at entry+0x1A, past SafetyHook's 5-byte JMP window so the
        // resolver still works after a sibling inline-hooks the entry. The wildcarded lea/sub frame pair followed by
        // `mov rax,[rcx+8]` / `mov <a1reg>,rcx` / `mov [rbp-0x70],rax` / `mov rdi,rdx` is what makes it unique; the
        // frame pair alone is not. dispOffset = -0x1A walks back to the entry.
        {"HelmAudioRegistrar_P2_PostPrologue",
         "48 8D AC 24 ?? ?? FF FF "
         "48 81 EC ?? ?? 00 00 "
         "48 8B 41 08 4C 8B ?? "
         "48 89 45 90 48 8B FA",
         ResolveMode::Direct, -0x1A, 0},

        // P3 -- deep body anchor at entry+0x29. Lands well past the 5-byte SafetyHook JMP window, so it still
        // resolves when a sibling mod inline-hooks the entry, and it shares no bytes with the prologue that P1 and P2
        // both depend on.
        //
        // Shape: the skill-registry read and arg parking, then the walk into the registry's `+0x68` sub-object and
        // its `+0x1A0` field, which is what the registrar dispatches through:
        //   48 8B 41 08              mov rax,[rcx+8]      ; a1->registry
        //   4C 8B ??                 mov <a1reg>,rcx
        //   48 89 45 90              mov [rbp-0x70],rax
        //   48 8B FA                 mov rdi,rdx          ; a2
        //   33 D2 / 45 8B F1 / 4D 8B E8
        //   48 8B 40 68              mov rax,[rax+0x68]
        //   48 8B 88 ?? ?? 00 00     mov rcx,[rax+0x1A0]
        //
        // Do NOT re-anchor this row on the thread-local-flag preamble (`gs:58` + an indexed TLS slot + a cmovnz)
        // that also appears near here. That shape is generic, it occurs in unrelated functions, and its TLS slot
        // index is assigned per binary, so it retires itself on a rebuild while every other byte holds.
        //
        // Two operands in this window are wildcarded: the register a1 is parked in, and the low half of the `+0x1A0`
        // displacement, since a field offset inside that sub-object can shift without the surrounding shape
        // changing. The `+0x68` walk and the 13-byte arg-parking head are what carry the uniqueness; the `00 00`
        // high half keeps the second displacement pinned to a sub-0x10000 field offset.
        {"HelmAudioRegistrar_P3_RegistrySubObjectWalk",
         "48 8B 41 08 4C 8B ?? 48 89 45 90 48 8B FA "
         "33 D2 45 8B F1 4D 8B E8 "
         "48 8B 40 68 48 8B 88 ?? ?? 00 00",
         ResolveMode::Direct, -0x29, 0},
    };

} // namespace Transmog
