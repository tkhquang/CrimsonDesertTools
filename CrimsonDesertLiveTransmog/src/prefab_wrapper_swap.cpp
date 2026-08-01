#include "prefab_wrapper_swap.hpp"
#include "aob_resolver.hpp"
#include "carrier_defaults.hpp"
#include "dye_record_inject.hpp"
#include "item_name_table.hpp"
#include "itemmesh_dumper.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Transmog::PrefabWrapperSwap
{
    // --- Constants ---
    //
    // StringInfo registry (pa::StringInfoManager, a pa::StaticInfoManager2<> subclass):
    //   +0x08 count u32, +0x58 array_ptr (QWORD entry-ptrs)
    //
    // The array_ptr offset moves whenever the pa::StaticInfoManager2 base changes width. The count at +0x08 and the
    // per-entry layout below sit ahead of the growth point, so they stay put. A stale array_ptr offset fails SILENTLY:
    // the neighboring offset also dereferences to a valid heap pointer (a filename string blob), so the
    // `arrayPtr < 0x10000` guard below does not trip and the walk emits garbage instead of bailing. Verify this offset
    // against live memory on patch day.
    // Do NOT blanket-apply a base-width change to every registry. Sibling registries move their container fields
    // independently, and one can shrink while this one grows.
    // Per-entry layout:
    //   +0x00 hash, +0x08 vtable (the StringInfo sentinel resolved into s_stringInfoVtable),
    //   +0x18 wrapper-ptr, +0x20 inline name (NUL-terminated)
    //
    // Wrapper layout:
    //   +0x00 string ptr, +0x10 refcount i32,
    //   +0x14 mode flag, +0x15 tombstone

    // Layout / sanity constants (compile-time only, not patchable).
    constexpr std::size_t k_stringInfoCountOff = 0x08; // u32 entry count
    constexpr std::size_t k_stringInfoArrayOff = 0x58; // qword base of the entry-ptr array
    constexpr std::size_t k_inlineNameOff = 0x20;
    constexpr std::size_t k_wrapperPtrOff = 0x18;
    constexpr std::size_t k_extNameMax = 256;
    constexpr std::uint32_t k_minPlausibleCount = 100;
    constexpr std::uint32_t k_maxPlausibleCount = 200000;

    // --- Runtime-resolved data globals ---
    //
    // Resolved by AOB cascades at init() (see aob_resolver.hpp::k_stringInfoRegistryCandidates et al). Atomic because
    // init runs on the main thread while the hot path (walk_string_info, enumerate_loader_registry_into_catalog) can
    // run on the background population thread. Zero means "not resolved yet". Every consumer must verify a non-zero
    // value before it dereferences.
    static std::atomic<std::uintptr_t> s_stringInfoRegistry{0};
    static std::atomic<std::uintptr_t> s_stringInfoVtable{0};
    static std::atomic<std::uintptr_t> s_loaderRegistrySingleton{0};
    static std::atomic<std::uintptr_t> s_apptContainerVtable{0};

    // The four cascades consumed below are defined in aob_resolver.hpp:
    //   k_apptResMgrInitCandidates    -- ResMgr init entry, the AppearanceTableLoader capture hook site
    //   k_apptInnerLookupCandidates   -- PartPrefab container lookup primitive
    //   k_apptStringInternCandidates  -- StringInfo string-intern primitive
    //   k_structCopyCandidates        -- wrapper struct-copy hot path
    // Each is a 3-anchor cascade per the AOB ordering rules in docs/aob-signatures.md (most-specific candidate first).

    // Offset within the container where the boot-loaded hash table struct begins. The boot loader populates
    // `container + 0x70` through the container insert primitive. The lookup primitive resolved by
    // k_apptInnerLookupCandidates takes the SAME table-struct pointer.
    static constexpr std::size_t k_apptHashTableOff = 0x70;

    // Module is "active" when at least one slot has a resolved swap pair installed in any s_swapMapPerChar bucket.
    // Selection is overlay-driven. There are no INI keys for this feature.
    static std::atomic<bool> s_active{false};

    // True once any swap pair is bound in any bucket. Cleared only on world-reload/reset. Lets the natpipe UNLINK hook
    // keep cleaning up installed targets even when the CURRENTLY-active character has no swap (s_active==false) -- e.g.
    // switching away from a swapped Damiane to a plain Oongka must still unlink Damiane's target. The empty-list fast
    // path keeps the hot cost negligible for the (common) zero-length unlink calls.
    static std::atomic<bool> s_mapsRetained{false};

    // Per-character resolved wrapper address maps. Each character's apply rebuilds ONLY its own bucket so a Damiane
    // apply does not wipe Kliff's substitutions while Kliff's body still has tgt wrappers physically attached. Without
    // per-char buckets, a chained Kliff-then-Damiane sequence leaves the next Kliff teardown with an empty map. The
    // natpipe hook then finds no substitution, the engine searches for the original src wrapper that the body no
    // longer holds, and the unlink fails silently (helm-stuck bug). The hook dispatches by s_activeCharIdx so
    // cross-character src collisions (Kliff+Oongka sharing cd_phm_00_ub_00_0054) still resolve to the correct target
    // for the body currently under assembly.
    //   bucket = s_activeCharIdx - 1 ([0]=Kliff, [1]=Damiane, [2]=Oongka)
    static std::mutex s_mapMtx;
    static std::unordered_map<std::uintptr_t, std::uintptr_t> s_swapMapPerChar[3];

    // Body-pointer (the natpipe hook's `a1`) -> character bucket. Learned in the natpipe hook whenever the ACTIVE
    // bucket owns a wrapper for that body (i.e. during the body's own assembly). Consulted when the engine later
    // unlinks that body while a DIFFERENT character is active, so we resolve the OUTGOING body's own bucket and unlink
    // its swap targets -- instead of missing on the active bucket and orphaning them (the fake-mask-persists bug).
    // Keying by the body, not s_activeCharIdx, is cross-talk-free even when characters share a carrier/source wrapper.
    // Only protagonist bodies with an active swap are ever recorded, so the map stays tiny.
    static std::mutex s_bodyMapMtx;
    static std::unordered_map<std::uintptr_t, int> s_bodyToChar;

    using StructCopyFn = std::int64_t(__fastcall *)(std::int64_t, std::int64_t);
    static StructCopyFn s_orig = nullptr;

    // Per-character target wrapper sets (parallel to s_swapMapPerChar). Used by the secondary-bind hook to detect "is
    // this record one of our substituted ones?" by comparing entry's wrapper-ptr at +0 against the union of all three
    // buckets.
    static std::unordered_set<std::uintptr_t> s_targetWrappersPerChar[3];

    // Destination tracking: every record we substituted, with its original Kliff wrapper. On deactivate, we walk this
    // vector and write the original wrapper back into the dest slot. This reverses our wrapper-substitution at the
    // engine-state level, so LT's tear_down (which walks the auth-table) finds records with ORIGINAL wrappers (the ones
    // it knows about) and can tear them down cleanly. Without this, our substitutions create scene-graph entries LT
    // cannot reach via its auth-table-driven tear-down, leading to stale renders (the helm leak being most visible).
    struct SubstRecord
    {
        std::uintptr_t destAddr;    // dest record's wrapper-ptr slot (= a1 + 0)
        std::uintptr_t origWrapper; // Kliff wrapper that was at *a2 before substitute
    };
    static std::mutex s_substLogMtx;
    static std::vector<SubstRecord> s_substLog;
    static constexpr std::size_t k_maxSubstLog = 256;

    // Natural-pipeline unlink. Called by safeTearDown and other unmount paths with a list of asset wrappers to unlink
    // from parent+88 records. The function is content-keyed: it walks parent+88 looking for records whose wrapper
    // field == one of the wrappers in the input list.
    //
    // Why we hook here: when LT's struct-copy hook substitutes a Kliff source wrapper with a target wrapper in
    // parent+88, the engine's tear-down still looks up the original Kliff wrapper at unmount time, fails to find it,
    // and leaves the substituted record alive (visible as a ghosted helm/cloak). At natural-pipeline entry we walk the
    // unlink list and replace each Kliff src with the corresponding target so the engine's content-keyed search hits.
    // Originals are restored on the way out so the caller's refcount-release loop decrements the same wrappers it
    // incremented.
    //
    // Resolved via k_naturalPipelineCandidates in aob_resolver.hpp.
    using NaturalPipelineFn = std::int64_t(__fastcall *)(std::int64_t a1, std::uint64_t *a2, std::uint64_t *a3);
    static NaturalPipelineFn s_origNaturalPipeline = nullptr;
    static std::atomic<std::uint64_t> s_natpipeHitCount{0};
    static std::atomic<std::uint64_t> s_natpipeSubstCount{0};
    static std::atomic<std::uint64_t> s_natpipeListEntries{0};

    // Name-based prefab lookup primitive, resolved through the k_apptNameLookupCandidates cascade in aob_resolver.hpp.
    // Signature: `__int64 fn(__int64 unused_a1, const char* name)`. It lowercases the name, interns it, then queries
    // the engine's name->wrapper registry at loaderRegistrySingleton+0x50. It returns entry+8 on a hit (the value
    // field of the registry entry, typically a wrapper-ptr or metadata-ptr) or 0 on a miss. Pure read-only.
    //
    // This primitive replaces the AppearanceTableLoader container chain, which queries different sub-tables that do
    // not hold partprefab names. The name->wrapper registry carries character body-mesh prefabs, so it is the correct
    // table for this use case.

    // Auto-deactivate-on-preset-switch state. Once swap activates and the user applies a body-mesh preset, we record
    // those itemIds. The next apply with DIFFERENT itemIds is treated as a switch-away and triggers
    // deactivate_for_clear before its substitutions can re-bind target wrappers to the new gear.
    static std::mutex s_lastApplyMtx;
    static std::uint16_t s_lastApplyItems[5] = {0, 0, 0, 0, 0};
    static bool s_lastApplyValid = false;

    // --- AppearanceTableLoader integration state ---
    //
    // Captured by the one-shot hook on the ResMgr init entry. After capture, s_apptContainer is the heap object whose
    // vtable (read-only check) matches s_apptContainerVtable below.
    //
    // Loader chain:
    //   ResMgrInit(a1)       -- entry, we snapshot a1 (the outer struct)
    //     ResMgr             -- read out of a1+0x40
    //     inner allocator    -- allocates the loader and stores it at ResMgr+0x88
    //
    // After the trampoline returns, ResMgr is at *(QWORD*)(a1+0x40) and the loader is at *(QWORD*)(ResMgr+0x88). The
    // PartPrefab container is at *(QWORD*)(loader+0x08).
    //
    // The hook installs once, snapshots, and disarms its capture flag. Subsequent calls (which DO occur during world
    // reload) pass through. We do NOT re-capture, because every PartPrefab consumer uses the original boot singleton.
    // The PartPrefab container vtable resolves at init() through reverse RTTI, or through k_apptLoaderCtorCandidates
    // plus an inline walk-forward scan over the ctor body. It is stored in s_apptContainerVtable. The
    // AppearanceTableLoader constructor allocates two containers and assigns final vtables:
    //   a1[0] (_appearanceContainer)       -- intermediate vtable
    //   a1[1] (_partPrefabDataContainer)   -- the one we want.
    // The walk-forward scan picks the SECOND `lea rax,[rip+disp32] ; mov [rdi],rax` pair inside the ctor.
    static constexpr std::size_t k_apptResMgrOff = 0x40;
    static constexpr std::size_t k_apptLoaderOff = 0x88;
    static constexpr std::size_t k_apptContainerOff = 0x08;

    static std::atomic<std::uintptr_t> s_apptContainer{0};
    static std::atomic<bool> s_apptCaptureDone{false};
    // Only the container snapshot is consumed by lookup_prefab_metadata.

    // Container-chain lookup primitives, resolved by AOB at init. They resolve in init() before the capture hook is
    // installed, so they are callable as soon as the snapshot lands. lookup_prefab_metadata no longer routes through
    // them -- it uses s_apptNameLookup below.
    using ApptStringInternFn = std::uintptr_t(__fastcall *)(const char *);
    using ApptLookupFn = std::int64_t(__fastcall *)(std::uintptr_t table_struct, std::uintptr_t *key_wrapper_ptr);
    // Name-based wrapper lookup. Pass any name string. It returns the value field (entry+8) of the matching registry
    // entry, or 0 on a miss. Internally it lowercases the name and interns it before it queries the name->wrapper
    // registry at loaderRegistrySingleton+0x50. AOB-resolved at init.
    using ApptNameLookupFn = std::int64_t(__fastcall *)(std::int64_t unused_a1, const char *name);
    static ApptStringInternFn s_apptStringIntern = nullptr;
    static ApptLookupFn s_apptLookup = nullptr;
    static ApptNameLookupFn s_apptNameLookup = nullptr;

    // Trampoline for the ResMgr init entry hook.
    using ApptResMgrInitFn = char(__fastcall *)(std::int64_t a1, void *a2);
    static ApptResMgrInitFn s_apptResMgrInitOrig = nullptr;

    // Wrapper +0x40 slot inside the scene-graph struct.
    static constexpr std::size_t k_sceneGraphWrapperOff = 0x40;
    // Slot-id u32 lives at struct+0x48 (the factory writes *a3 there).
    static constexpr std::size_t k_sceneGraphSlotIdOff = 0x48;
    // Helm slot ID -- the only slot that needs a scene-graph reverse-write, because helm is the only pair with a
    // suffix mismatch (`_d` -> `_c`) that routes through a separate scene-graph branch. The engine's tear-down cannot
    // reach that branch through runtime-resource-pointer equality. Other pairs preserve their suffix and unlink
    // naturally on the next apply, so a revert of their +0x40 only confuses rendering (visible as chest/cloak clipping
    // on preset-switch).
    static constexpr std::uint32_t k_helmSlotId = 0xAA9A;

    static std::atomic<std::uint64_t> s_callCount{0};
    static std::atomic<std::uint64_t> s_substCount{0};

    // --- SEH-isolated read/write helpers ---

    static bool write_qword_seh(void *p, std::uint64_t value) noexcept
    {
        bool ok = false;
        [&]() __declspec(noinline)
        {
            __try
            {
                *static_cast<volatile std::uint64_t *>(p) = value;
                ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                ok = false;
            }
        }();
        return ok;
    }

    static std::size_t read_cstr_seh(const void *p, char *out, std::size_t cap) noexcept
    {
        if (!p || cap == 0)
            return SIZE_MAX;
        std::size_t len = SIZE_MAX;
        [&]() __declspec(noinline)
        {
            __try
            {
                const auto *src = static_cast<const volatile char *>(p);
                for (std::size_t i = 0; i < cap; ++i)
                {
                    const char c = src[i];
                    out[i] = c;
                    if (c == 0)
                    {
                        len = i;
                        return;
                    }
                }
                out[cap - 1] = 0;
                len = SIZE_MAX;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                len = SIZE_MAX;
            }
        }();
        return len;
    }

    // Bumps a wrapper's refcount via Interlocked, gated on the same condition the engine's own retain path uses
    // (refcount field >= 0). No-op if the wrapper is the sentinel or the refcount is already negative ("static, do not
    // refcount").
    static void increment_wrapper_refcount(std::uintptr_t wrapper) noexcept
    {
        const auto vtableSentinel = s_stringInfoVtable.load(std::memory_order_acquire);
        if (wrapper == vtableSentinel || wrapper < 0x10000ULL)
            return;
        [&]() __declspec(noinline)
        {
            __try
            {
                auto *rc = reinterpret_cast<volatile LONG *>(wrapper + 16);
                if (*rc >= 0)
                    InterlockedIncrement(rc);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }();
    }

    // Balances increment_wrapper_refcount: releases one reference on a wrapper we previously bumped at install. Gated
    // on refcount > 0 so we never drive a live wrapper negative (a 0/negative field means "static, do not refcount").
    // Used by deactivate_for_clear's reverse-write to undo the install-time bump when we detach an orphaned target.
    static void decrement_wrapper_refcount(std::uintptr_t wrapper) noexcept
    {
        const auto vtableSentinel = s_stringInfoVtable.load(std::memory_order_acquire);
        if (wrapper == vtableSentinel || wrapper < 0x10000ULL)
            return;
        [&]() __declspec(noinline)
        {
            __try
            {
                auto *rc = reinterpret_cast<volatile LONG *>(wrapper + 16);
                if (*rc > 0)
                    InterlockedDecrement(rc);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }();
    }

    // Reverse-write one substituted record IFF its slot still holds one of our target wrappers: release the install
    // refcount bump, then restore the original source wrapper. Returns true when it reverted. Split out of
    // deactivate_for_clear because that function holds C++ objects (vector/set/scoped_lock) whose unwinding is illegal
    // in the same frame as `__try` under /EHsc (C2712) -- this helper keeps only POD locals so the SEH guard is legal.
    static bool revert_one_subst(std::uintptr_t destAddr, std::uintptr_t origWrapper,
                                 const std::unordered_set<std::uintptr_t> &ourTargets) noexcept
    {
        if (destAddr < 0x10000)
            return false;
        __try
        {
            const auto cur = *reinterpret_cast<volatile std::uintptr_t *>(destAddr);
            if (cur < 0x10000 || ourTargets.find(cur) == ourTargets.end())
                return false; // slot no longer holds one of our targets (freed / reused / re-substituted) -- skip
            decrement_wrapper_refcount(cur);                                    // balance the install-time bump
            *reinterpret_cast<volatile std::uintptr_t *>(destAddr) = origWrapper; // restore original source
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // --- AppearanceTableLoader hook callback ---
    //
    // SafetyHook entry hook on the ResMgr init entry. We invoke the trampoline first (so the engine's allocation path
    // runs unmodified), then snapshot ResMgr / loader / container by walking the documented chain:
    //
    //   ResMgr     = *(QWORD*)(a1 + 0x40)
    //   loader     = *(QWORD*)(ResMgr + 0x88)
    //   container  = *(QWORD*)(loader + 0x08)
    //
    // After capture we validate the container's vtable against s_apptContainerVtable (the PartPrefabDataContainer
    // vtable) before we publish the pointers atomically. A single capture is enough -- subsequent calls pass through.
    static char __fastcall on_appt_resmgr_init(std::int64_t a1, void *a2) noexcept
    {
        // Run the engine's path first. The capture must read fields that the inner allocator populates.
        char rv = 0;
        if (s_apptResMgrInitOrig)
        {
            [&]() __declspec(noinline)
            {
                __try
                {
                    rv = s_apptResMgrInitOrig(a1, a2);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    rv = 0;
                }
            }();
        }

        // Already captured? Pass-through.
        if (s_apptCaptureDone.load(std::memory_order_acquire))
            return rv;

        auto &logger = DMK::Logger::get_instance();

        std::uintptr_t resMgr = 0;
        std::uintptr_t loader = 0;
        std::uintptr_t container = 0;
        std::uintptr_t vtable = 0;
        [&]() __declspec(noinline)
        {
            __try
            {
                resMgr =
                    *reinterpret_cast<volatile std::uintptr_t *>(static_cast<std::uintptr_t>(a1) + k_apptResMgrOff);
                if (resMgr < 0x10000ULL)
                    return;
                loader = *reinterpret_cast<volatile std::uintptr_t *>(resMgr + k_apptLoaderOff);
                if (loader < 0x10000ULL)
                    return;
                container = *reinterpret_cast<volatile std::uintptr_t *>(loader + k_apptContainerOff);
                if (container < 0x10000ULL)
                    return;
                vtable = *reinterpret_cast<volatile std::uintptr_t *>(container);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                resMgr = 0;
                loader = 0;
                container = 0;
                vtable = 0;
            }
        }();

        // Validate. If any field is bogus, log and let the next call try again. This is safe: pass-through behavior is
        // preserved.
        const auto expectedVtable = s_apptContainerVtable.load(std::memory_order_acquire);
        if (container == 0 || vtable != expectedVtable)
        {
            logger.debug("[prefab-swap] AppearanceTableLoader capture "
                         "deferred: a1=0x{:X} resMgr=0x{:X} loader=0x{:X} "
                         "container=0x{:X} vt=0x{:X} (expected 0x{:X})",
                         static_cast<std::uintptr_t>(a1), resMgr, loader, container, vtable, expectedVtable);
            return rv;
        }

        s_apptContainer.store(container, std::memory_order_release);
        s_apptCaptureDone.store(true, std::memory_order_release);

        logger.info("[prefab-swap] AppearanceTableLoader captured: "
                    "resMgr=0x{:X} loader=0x{:X} container=0x{:X} "
                    "(vtable 0x{:X} verified)",
                    resMgr, loader, container, vtable);
        return rv;
    }

    // --- AppearanceTableLoader public API ---

    bool is_loader_ready() noexcept
    {
        // The AOB-resolved name lookup is self-contained: it queries the name->wrapper registry at
        // loaderRegistrySingleton+0x50 directly, so the container capture and its scan machinery are not needed here.
        // Verify only that the function pointer is wired.
        return s_apptNameLookup != nullptr;
    }

    std::uintptr_t lookup_prefab_metadata(const char *name) noexcept
    {
        if (!name || !*name)
            return 0;
        if (!s_apptNameLookup)
            return 0;

        // The primitive handles the full lookup chain internally: lowercase -> intern -> query the global
        // name->wrapper registry at loaderRegistrySingleton+0x50. It returns entry+8 on a hit (the value field, so a
        // non-zero result means the prefab is registered and has a wrapper alive somewhere in the engine), or 0 on a
        // miss. It is pure read-only and safe from any thread.
        std::uintptr_t result = 0;
        [&]() __declspec(noinline)
        {
            __try
            {
                result = static_cast<std::uintptr_t>(s_apptNameLookup(0, name));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                result = 0;
            }
        }();
        return result;
    }

    void for_each_loader_prefab_name(const std::function<void(std::string_view)> &cb) noexcept
    {
        // Mirror of `enumerate_loader_registry_into_catalog` minus the slot-tag filter and pending-merge bookkeeping.
        // Walks the table struct (singleton+0x50) entry-by-entry, reads the inline key name from each wrapper, emits
        // via callback. Skips the StringInfo-vtable sentinel rows that hold metadata-only (non-name-bearing) entries.
        if (!cb)
            return;
        const auto singletonAbs = s_loaderRegistrySingleton.load(std::memory_order_acquire);
        if (singletonAbs < 0x10000ULL)
            return;
        const auto singletonPtr = DMKMemory::seh_read<std::uint64_t>(singletonAbs).value_or(0);
        if (singletonPtr < 0x10000ULL)
            return;
        // singleton + 0x50 = table struct (matches internal k_loaderRegistryTableOff defined later in this TU).
        const std::uintptr_t tableStruct = singletonPtr + 0x50;
        const auto count = DMKMemory::seh_read<std::uint32_t>(tableStruct + 0x04).value_or(0);
        const auto dataArrayPtr = DMKMemory::seh_read<std::uint64_t>(tableStruct + 0x18).value_or(0);
        if (count == 0 || count > 100000 || dataArrayPtr < 0x10000ULL)
            return;

        std::vector<std::uintptr_t> entryPtrs;
        entryPtrs.resize(count);
        const bool bulkOk = DMKMemory::seh_read_bytes(dataArrayPtr, entryPtrs.data(), count * sizeof(std::uintptr_t));

        const auto vtableSentinel = s_stringInfoVtable.load(std::memory_order_acquire);
        char nameBuf[k_extNameMax + 1] = {0};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entry =
                bulkOk ? entryPtrs[i] : DMKMemory::seh_read<std::uint64_t>(dataArrayPtr + 8ULL * i).value_or(0);
            if (entry < 0x10000ULL)
                continue;
            const auto wrapper = DMKMemory::seh_read<std::uint64_t>(entry + 0x08).value_or(0);
            if (wrapper < 0x10000ULL)
                continue;
            if (wrapper == vtableSentinel)
                continue;
            constexpr std::size_t k_loaderNameCap = 96;
            const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(wrapper + 0x18), nameBuf, k_loaderNameCap);
            if (rlen == SIZE_MAX || rlen == 0)
                continue;
            cb(std::string_view(nameBuf, rlen));
        }
    }

    // --- Per-slot catalog state ---
    //
    // Built lazily on first overlay open via populate_slot_catalogs(). Each catalog holds the prefabs whose inline name
    // starts with the slot-specific sub-prefix (e.g. "cd_phm_00_hel_00_"). Selection indices index into the catalog
    // vector. -1 means "unset".
    //
    // Catalog mutex is separate from s_mapMtx so the UI thread can refresh without serializing with the active hot
    // path. The hot path never reads catalogs. It reads s_swapMapPerChar, which each apply rebuilds from the catalog
    // selections of one character.
    static std::mutex s_catalogMtx;
    static std::array<std::vector<PrefabEntry>, static_cast<std::size_t>(Transmog::TransmogSlot::Count)> s_slotCatalogs;
    // Per-slot picker selection state. Sentinel -1 means "no selection". Any non-negative value indexes into the
    // slot's catalog. The arrays are sized to TransmogSlot::Count, so a fixed-length brace-init list drifts whenever a
    // new slot is added. A helper returns an array filled with -1 instead. The count then follows the enum size
    // automatically.
    static constexpr auto k_initialSelectionIndices = []()
    {
        constexpr auto N = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        std::array<int, N> a{};
        for (std::size_t i = 0; i < N; ++i)
            a[i] = -1;
        return a;
    }();
    static std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)> s_selSrcIdx =
        k_initialSelectionIndices;
    static std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)> s_selTgtIdx =
        k_initialSelectionIndices;
    static std::atomic<bool> s_catalogPopulated{false};

    // Per-character buffered copies of the selection arrays. UI writes through set_selection mirror into
    // s_selSrcIdxPerChar[active-1] / s_selTgtIdxPerChar[active-1]. The globals above stay as the "active editing view"
    // the UI reads back. `apply_selections_to_swap_map` rebuilds the active character's bucket
    // (s_swapMapPerChar[active-1]) from that character's row each apply. It leaves the other characters' buckets
    // intact, so a substitution picked on Damiane stays live in the engine's wrapper-substitution path even while the
    // user edits another character. Without these rows the picker writes a single global, which the next dropdown
    // switch overwrites. That silently drops the outgoing character's variant.
    //
    // s_activeCharIdx is 0 until PresetManager::apply_to_state binds a character via set_active_char_idx(). While idx
    // is 0, set_selection writes only to the globals (boot-time defaults).
    static std::atomic<std::uint32_t> s_activeCharIdx{0};
    static std::array<std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)>, 3> s_selSrcIdxPerChar =
        {k_initialSelectionIndices, k_initialSelectionIndices, k_initialSelectionIndices};
    static std::array<std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)>, 3> s_selTgtIdxPerChar =
        {k_initialSelectionIndices, k_initialSelectionIndices, k_initialSelectionIndices};

    // Slot prefix tables come from slot_metadata.hpp's SlotMetadata table (slot_meta(slot).prefabPrefixMale /
    // .prefabPrefixFemale). The engine uses `cd_phm_*` for male body prefabs and `cd_phw_*` for female. Both variants
    // live in the same StringInfo registry and can be transmogged interchangeably.
    //
    //   Helm   -> "cd_ph[mw]_00_hel_00_"
    //   Chest  -> "cd_ph[mw]_00_ub_00_"
    //   Cloak  -> "cd_ph[mw]_00_cloak_00_"
    //   Gloves -> "cd_ph[mw]_00_hand_00_"
    //   Boots  -> "cd_ph[mw]_00_foot_00_"
    //
    // The body-mesh-swap module is armor-family ONLY. Accessory slots (Earring*, Necklace, Ring*, Lantern, Glasses,
    // Mask, Backpack, Bracelet) do not live under cd_phm_*/cd_phw_*, so an empty prefix excludes them. An empty prefix
    // means "this slot is not body-mesh-swappable" -- the picker dropdown stays empty and the slot routes through the
    // carrier-based transmog path.

    // --- Shared StringInfo walker (bulk-copy fast path) ---
    //
    // A shared helper so every caller reuses one bulk-copy + vtable filter + prefix gate, without a duplicate
    // SEH-isolated read setup. The visitor receives the entry pointer, decoded name, wrapper-ptr (entry+0x18) and hash
    // (entry+0x00) for each entry whose vtable matches the StringInfo sentinel and whose inline name starts with
    // `prefix`.
    //
    // Long-name entries (external string at +0x20 instead of inline) bypass the prefix gate and use Path B
    // (wrapper-chain decode), so any caller that passes a non-empty prefix still gets long names that match the prefix
    // in their decoded form.
    //
    // Returns the total entries walked. It logs walk timing at debug level.
    using EntryVisitor =
        std::function<void(std::uintptr_t entry, const char *name, std::uintptr_t wrapper, std::uint32_t hash)>;
    static std::uint32_t walk_string_info(const char *prefix, EntryVisitor visitor) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const std::size_t prefixLen = prefix ? std::strlen(prefix) : 0;

        const auto regAbs = s_stringInfoRegistry.load(std::memory_order_acquire);
        if (regAbs < 0x10000ULL)
        {
            logger.warning("[prefab-swap] walk_string_info: registry not "
                           "resolved (s_stringInfoRegistry=0). Returning 0 "
                           "entries; the picker dropdown will be empty until "
                           "init() succeeds.");
            return 0;
        }
        const auto regAddr = reinterpret_cast<const void *>(regAbs);
        const auto registryPtr =
            DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(regAddr)).value_or(0);
        if (registryPtr < 0x10000ULL)
            return 0;
        const auto count = DMKMemory::seh_read<std::uint32_t>(registryPtr + k_stringInfoCountOff).value_or(0);
        const auto arrayPtr = DMKMemory::seh_read<std::uint64_t>(registryPtr + k_stringInfoArrayOff).value_or(0);
        if (count < k_minPlausibleCount || count > k_maxPlausibleCount || arrayPtr < 0x10000ULL)
            return 0;

        // Bulk-copy entry-pointer array so the inner loop reads from process memory without a per-element SEH frame.
        // Falls back to per-entry SEH reads if the bulk copy faults.
        std::vector<std::uintptr_t> entryPtrs;
        const std::size_t arrayBytes = static_cast<std::size_t>(count) * sizeof(std::uintptr_t);
        bool bulkOk = false;
        if (arrayBytes > 0)
        {
            entryPtrs.resize(count);
            bulkOk = DMKMemory::seh_read_bytes(arrayPtr, entryPtrs.data(), arrayBytes);
        }

        const auto walkStart = std::chrono::steady_clock::now();
        std::uint32_t scanned = 0;
        std::uint32_t vtMatched = 0;
        std::uint32_t prefMatched = 0;

        // Same 128B header copy: covers vtable @ +8, wrapper-ptr @ +0x18, and the inline-name region @ +0x20.
        constexpr std::size_t k_headerBytes = 0x80;
        alignas(8) std::uint8_t header[k_headerBytes];

        // Path-B fallback decode for long names whose +0x20 holds an external string ptr instead of an inline
        // NUL-terminated name.
        auto decode_long_name = [](std::uintptr_t entry, char *buf, std::size_t cap) -> bool
        {
            const auto wrapperPtr = DMKMemory::seh_read<std::uint64_t>(entry + k_wrapperPtrOff).value_or(0);
            if (wrapperPtr < 0x10000ULL)
                return false;
            const auto strPtr = DMKMemory::seh_read<std::uint64_t>(wrapperPtr).value_or(0);
            if (strPtr < 0x10000ULL)
                return false;
            const auto extLen = read_cstr_seh(reinterpret_cast<const void *>(strPtr), buf, cap);
            return extLen != SIZE_MAX && extLen > 0;
        };

        // Snapshot the resolved sentinel ONCE per walk -- this avoids an atomic load per entry across the whole
        // registry walk.
        const auto vtableSentinel = s_stringInfoVtable.load(std::memory_order_acquire);
        if (vtableSentinel < 0x10000ULL)
        {
            logger.warning("[prefab-swap] walk_string_info: StringInfo vtable "
                           "sentinel not resolved -- aborting walk");
            return 0;
        }

        char buf[k_extNameMax + 1] = {0};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entryPtr =
                bulkOk ? entryPtrs[i] : DMKMemory::seh_read<std::uint64_t>(arrayPtr + 8ULL * i).value_or(0);
            if (entryPtr < 0x10000ULL)
                continue;
            ++scanned;

            if (!DMKMemory::seh_read_bytes(entryPtr, header, k_headerBytes))
                continue;

            const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t *>(header + 8);
            if (vtable != vtableSentinel)
                continue;
            ++vtMatched;

            // Prefix gate (only for inline-name entries -- skipped for long-name entries that route through Path B
            // below).
            const unsigned char first = header[k_inlineNameOff];
            const bool printableLead = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                                       (first >= '0' && first <= '9') || first == '_' || first == '/' || first == '.';
            if (prefixLen > 0 && printableLead && std::memcmp(header + k_inlineNameOff, prefix, prefixLen) != 0)
                continue;
            ++prefMatched;

            // Decode name from local header (Path A) or fall back to wrapper-chain (Path B) for long external strings.
            buf[0] = 0;
            bool decoded = false;
            {
                const char *src = reinterpret_cast<const char *>(header + k_inlineNameOff);
                const std::size_t maxLen = k_headerBytes - k_inlineNameOff;
                std::size_t L = 0;
                while (L < maxLen && src[L] != 0)
                    ++L;
                if (L > 0 && L < maxLen)
                {
                    bool printable = true;
                    for (std::size_t k = 0; k < L; ++k)
                    {
                        const unsigned char c = static_cast<unsigned char>(src[k]);
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                              c == '/' || c == '.' || c == '-'))
                        {
                            printable = false;
                            break;
                        }
                    }
                    if (printable)
                    {
                        std::memcpy(buf, src, L);
                        buf[L] = 0;
                        decoded = true;
                    }
                }
            }
            if (!decoded)
            {
                if (!decode_long_name(entryPtr, buf, k_extNameMax))
                    continue;
                // Re-apply prefix filter to long-name entries that dodged the inline-prefix gate above.
                if (prefixLen > 0 && std::strncmp(buf, prefix, prefixLen) != 0)
                    continue;
            }

            const std::uintptr_t entryWrapper = *reinterpret_cast<const std::uintptr_t *>(header + k_wrapperPtrOff);
            const std::uint32_t entryHash = *reinterpret_cast<const std::uint32_t *>(header);

            visitor(entryPtr, buf, entryWrapper, entryHash);
        }

        const auto walkMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walkStart).count();
        logger.debug("[prefab-swap] StringInfo walk: count={} scanned={} "
                     "vtable-pass={} prefix-pass={} prefix=\"{}\" ({}ms)",
                     count, scanned, vtMatched, prefMatched, (prefix && prefix[0]) ? prefix : "(none)", walkMs);

        return scanned;
    }

    // Heap-walk for partprefabdyeslot-style wrappers, which live in a parallel allocation pool that StringInfo does
    // not index. Used as the Pass-2 fallback when StringInfo does not carry an entry for a configured name. It is its
    // own helper so populate_slot_catalogs and apply_selections_to_swap_map both reuse it.
    //
    // For each name in `srcNames`, it appends matching wrapper addresses to `outSrcByIdx` (ALL matches per name). For
    // each name in `tgtNames`, it writes the FIRST matching wrapper to `outTgtByIdx` (single-substitution target). The
    // caller passes parallel vectors keyed by index.
    static void heap_walk_partprefab_for_names(const std::vector<std::string> &srcNames,
                                               const std::vector<std::string> &tgtNames,
                                               std::vector<std::vector<std::uintptr_t>> &outSrcByIdx,
                                               std::vector<std::uintptr_t> &outTgtByIdx) noexcept
    {
        if (srcNames.size() != outSrcByIdx.size() || tgtNames.size() != outTgtByIdx.size())
            return;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const std::uintptr_t addrEnd = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
        while (addr < addrEnd)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
                break;
            const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto regionSize = mbi.RegionSize;
            const bool committed = (mbi.State == MEM_COMMIT);
            const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;
            const bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
            if (committed && writable && !guarded && regionSize >= 0x40 && regionSize < 0x40000000ULL)
            {
                const auto end = regionBase + regionSize - 0x40;
                for (std::uintptr_t p = regionBase; p < end; p += 8)
                {
                    const auto v = DMKMemory::seh_read<std::uint64_t>(p).value_or(0);
                    if (v != p + 0x18)
                        continue;
                    const auto len = DMKMemory::seh_read<std::uint32_t>(p + 8).value_or(0);
                    if (len == 0 || len >= k_extNameMax)
                        continue;
                    char nameBuf[k_extNameMax + 1] = {0};
                    const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(p + 0x18), nameBuf, k_extNameMax);
                    if (rlen == SIZE_MAX || rlen != len)
                        continue;

                    for (std::size_t k = 0; k < srcNames.size(); ++k)
                    {
                        if (!srcNames[k].empty() && std::strcmp(nameBuf, srcNames[k].c_str()) == 0)
                            outSrcByIdx[k].push_back(p);
                    }
                    for (std::size_t k = 0; k < tgtNames.size(); ++k)
                    {
                        if (outTgtByIdx[k] == 0 && !tgtNames[k].empty() &&
                            std::strcmp(nameBuf, tgtNames[k].c_str()) == 0)
                            outTgtByIdx[k] = p;
                    }
                }
            }
            addr = regionBase + regionSize;
            if (regionSize == 0)
                break;
        }
    }

    // Body-rig-stripped stem of a mesh prefab name. A body-mesh prefab is shaped `cd_<rig>_<NN>_<stem>` where
    // `<rig>` is a body-rig token (phm=male, phw=female, pom=orc, pgm/pfw/... = other rigs, and all start with 'p') and
    // `<NN>` is the rig-index digits. The `<stem>` (e.g. "mask_00_0271_a") identifies the item's mesh independent of
    // which body renders it -- the SAME logical item is `cd_phm_00_mask_00_0271_a` on Kliff, `cd_phw_01_mask_00_0271_a`
    // on Damiane, `cd_pom_01_mask_00_0271_a` on Oongka (note the rig-index also differs: 00 vs 01). Returns "" when the
    // name is not a body-rig prefab (e.g. `cd_t0000_lantern_0003`), so callers fall back to exact-name matching.
    //
    // Used to register EVERY rig sibling of a carrier as a swap source, so the source set covers whichever rig the
    // wearer's REAL body emits. This tolerates body-swap mods that render a different rig than the character's
    // name-derived default assumes.
    static std::string rig_stripped_stem(const std::string &name) noexcept
    {
        constexpr std::string_view kPfx = "cd_";
        if (name.size() < kPfx.size() || name.compare(0, kPfx.size(), kPfx) != 0)
            return {};
        const std::size_t rigStart = kPfx.size();
        if (name[rigStart] != 'p') // body-rig tokens all start with 'p' (phm/phw/pom/pgm/pfw/...)
            return {};
        const std::size_t rigEnd = name.find('_', rigStart);
        if (rigEnd == std::string::npos)
            return {};
        const std::size_t idxStart = rigEnd + 1;
        const std::size_t idxEnd = name.find('_', idxStart);
        if (idxEnd == std::string::npos || idxEnd == idxStart)
            return {};
        for (std::size_t k = idxStart; k < idxEnd; ++k) // rig-index must be all digits
            if (name[k] < '0' || name[k] > '9')
                return {};
        return name.substr(idxEnd + 1);
    }

    // Runtime source-seed for a slot's body-mesh swap. Derives the carrier item's rig mesh name from its itemId at
    // runtime (variant_meshes_for_item -> desc+0x3E0 variant list), in place of a hardcoded prefab column in
    // carrier_defaults. Returns the first rig-shaped mesh -- which rig comes first is irrelevant, since the
    // rig-stripped stem is what the sibling registration keys on, and every rig of the item shares that stem. Empty
    // when the carrier itemName is unset/unresolved or the registries are not ready yet, in which case callers fall
    // back to the per-char selection seed.
    static std::string carrier_source_seed(Transmog::CarrierChar cc, Transmog::TransmogSlot slot) noexcept
    {
        const char *itemNm = Transmog::carrier_for(cc, slot).itemName;
        if (!itemNm || itemNm[0] == '\0')
            return {};
        const auto idOpt = Transmog::ItemNameTable::instance().id_of(itemNm);
        if (!idOpt)
            return {};
        // Prefer a body-rig mesh -- its rig-stripped stem drives the sibling (per-body) registration. But fall back to
        // the FIRST mesh for non-body-rig carriers: a Lantern emits the cd_t0000_ prop family (e.g.
        // cd_t0000_lantern_0003), which has no rig prefix so its stem is empty, yet the mesh name is still a valid,
        // resolvable source (it registers as its own source, with no rig siblings). Without this fallback such carriers
        // resolve to no source at all.
        std::string firstMesh;
        for (const auto &mesh : Transmog::variant_meshes_for_item(*idOpt))
        {
            if (firstMesh.empty())
                firstMesh = mesh;
            if (!rig_stripped_stem(mesh).empty())
                return mesh;
        }
        return firstMesh;
    }

    // Seed the per-character default SOURCE selections (s_selSrcIdxPerChar / s_selSrcIdx) from each carrier's runtime
    // variant meshes. Idempotent and cheap once done. GATED on ItemNameTable readiness: carrier_source_seed resolves
    // the carrier itemName -> itemId -> variant list, and the name table finishes building on a deferred worker AFTER
    // populate_slot_catalogs runs. A seed inside populate therefore silently no-ops. That leaves the picker with no
    // default source, so has_any_selection() stays false and the swap can never activate.
    // Instead this runs from the activation gate and from the picker read, so it lands the moment the table is ready.
    // It never overwrites a slot already seeded or user-picked, and it runs at most one full pass.
    static void ensure_default_sources_seeded() noexcept
    {
        static std::atomic<bool> s_sourcesSeeded{false};
        if (s_sourcesSeeded.load(std::memory_order_acquire))
            return;
        if (!s_catalogPopulated.load(std::memory_order_acquire) || !Transmog::ItemNameTable::instance().ready())
            return; // retry on a later call once BOTH the catalog and the name table exist

        constexpr std::size_t k_slotN = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        auto &logger = DMK::Logger::get_instance();
        const auto &activeChar = Transmog::PresetManager::instance().active_character();
        const auto seedChar = Transmog::carrier_char_from_name(activeChar).value_or(Transmog::CarrierChar::Kliff);
        std::size_t seededCount = 0;
        {
            std::scoped_lock lk(s_catalogMtx);
            if (s_sourcesSeeded.load(std::memory_order_relaxed))
                return; // another caller won the race and already seeded -- avoid a duplicate pass + log line
            // Seed every protagonist's row from their OWN carrier so per-body prefab families (cd_phm vs cd_phw) do
            // not cross-talk. A later set_active_char_idx hydrates the globals from a meaningful row.
            for (std::size_t ci = 0; ci < Transmog::k_carrierCharCount; ++ci)
            {
                const auto cc = static_cast<Transmog::CarrierChar>(ci);
                for (std::size_t i = 0; i < k_slotN; ++i)
                {
                    if (s_selSrcIdxPerChar[ci][i] >= 0)
                        continue; // already chosen (seeded earlier or user-picked)
                    const std::string src = carrier_source_seed(cc, static_cast<Transmog::TransmogSlot>(i));
                    if (src.empty())
                        continue; // no carrier itemName, or item/registries not resolvable -- leave for a later pass
                    const auto &cat = s_slotCatalogs[i];
                    int foundIdx = -1;
                    for (std::size_t k = 0; k < cat.size(); ++k)
                        if (cat[k].name == src)
                        {
                            foundIdx = static_cast<int>(k);
                            break;
                        }
                    // Body-swap resilience: if the derived rig prefab is not resident, seed from any resident rig
                    // sibling sharing the same mesh stem so the picker still shows a meaningful default source.
                    if (foundIdx < 0)
                    {
                        const std::string stem = rig_stripped_stem(src);
                        if (!stem.empty())
                            for (std::size_t k = 0; k < cat.size(); ++k)
                                if (rig_stripped_stem(cat[k].name) == stem)
                                {
                                    foundIdx = static_cast<int>(k);
                                    logger.trace("[prefab-swap] seed char[{}] slot[{}] carrier mesh \"{}\" not "
                                                 "resident -- seeding rig sibling \"{}\" (stem \"{}\")",
                                                 ci, i, src, cat[k].name, stem);
                                    break;
                                }
                    }
                    if (foundIdx >= 0)
                    {
                        s_selSrcIdxPerChar[ci][i] = foundIdx;
                        ++seededCount;
                    }
                }
            }
            // Mirror the active character's row into the globals the UI reads.
            const auto seedBucket = static_cast<std::size_t>(seedChar);
            for (std::size_t i = 0; i < k_slotN; ++i)
                if (s_selSrcIdx[i] < 0)
                    s_selSrcIdx[i] = s_selSrcIdxPerChar[seedBucket][i];
            s_sourcesSeeded.store(true, std::memory_order_release);
        }
        logger.info("[prefab-swap] seeded {} default source selection(s) from runtime carrier meshes across {} "
                    "character(s), active='{}' (ItemNameTable ready)",
                    seededCount, Transmog::k_carrierCharCount, activeChar);
    }

    // --- Per-slot catalog API ---

    const char *slot_prefix_str(Transmog::TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= Transmog::k_slotCount)
            return "";
        return Transmog::k_slotMetadata[idx].prefabPrefixMale;
    }

    bool is_catalog_populated() noexcept
    {
        return s_catalogPopulated.load(std::memory_order_acquire);
    }

    const std::vector<PrefabEntry> &slot_catalog(Transmog::TransmogSlot slot) noexcept
    {
        // Static empty fallback so the reference return is always valid, even before the catalog is populated.
        static const std::vector<PrefabEntry> s_empty;
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_slotCatalogs.size())
            return s_empty;
        return s_slotCatalogs[idx];
    }

    int selection_src_index(Transmog::TransmogSlot slot) noexcept
    {
        ensure_default_sources_seeded(); // land source defaults so the picker shows them once the name table is ready
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_selSrcIdx.size())
            return -1;
        return s_selSrcIdx[idx];
    }

    int selection_tgt_index(Transmog::TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_selTgtIdx.size())
            return -1;
        return s_selTgtIdx[idx];
    }

    void set_selection(Transmog::TransmogSlot slot, int srcIdx, int tgtIdx) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_selSrcIdx.size())
            return;
        // Clamp to catalog bounds. -1 is explicitly allowed for "unset".
        const auto catSize = static_cast<int>(s_slotCatalogs[idx].size());
        if (srcIdx < -1 || srcIdx >= catSize)
            srcIdx = -1;
        if (tgtIdx < -1 || tgtIdx >= catSize)
            tgtIdx = -1;
        s_selSrcIdx[idx] = srcIdx;
        s_selTgtIdx[idx] = tgtIdx;
        // Mirror the write into the active character's per-char row so `apply_selections_to_swap_map` retains it when
        // the user switches the editing character. Idx 0 (no character bound yet) is a no-op -- the globals carry the
        // boot-time defaults until PresetManager::apply_to_state runs and binds a row.
        const auto charIdx = s_activeCharIdx.load(std::memory_order_acquire);
        if (charIdx >= 1 && charIdx <= 3)
        {
            const auto bucket = static_cast<std::size_t>(charIdx - 1);
            s_selSrcIdxPerChar[bucket][idx] = srcIdx;
            s_selTgtIdxPerChar[bucket][idx] = tgtIdx;
        }
    }

    void set_active_char_idx(std::uint32_t idx) noexcept
    {
        if (idx > 3)
            idx = 0;
        const auto prev = s_activeCharIdx.exchange(idx, std::memory_order_acq_rel);
        if (idx == prev || idx == 0)
            return;
        // Hydrate the globals (the "active editing view") from the newly-bound character's row so subsequent UI reads
        // (selection_src_index / selection_tgt_index) reflect that character's selections rather than the previous
        // one's.
        ensure_default_sources_seeded(); // seed the per-char source rows before hydrating the globals from one
        const auto bucket = static_cast<std::size_t>(idx - 1);
        s_selSrcIdx = s_selSrcIdxPerChar[bucket];
        s_selTgtIdx = s_selTgtIdxPerChar[bucket];
    }

    void reset_per_char_state() noexcept
    {
        // Clear ONLY the stale-after-arena-flip state:
        //   - s_activeCharIdx so the next set_active_char_idx forces a fresh hydrate.
        //   - The active editing-view globals so the next apply_to_state's set_selection loop reads sane curSrc values
        //     after re-hydration.
        //   - s_swapMapPerChar and s_targetWrappersPerChar because their wrapper addresses point into the previous
        //     arena. If the natpipe hook fires against them, they either no-op (best case) or crash on dereference.
        //
        // The per-char `s_selSrcIdxPerChar` / `s_selTgtIdxPerChar` rows are CATALOG INDICES, not wrapper pointers.
        // Catalog re-population (populate_slot_catalogs) preserves entry names, so the indices stay meaningful. If you
        // wipe them here, you un-seed the per-character src defaults. The next post-save-load apply then sees an empty
        // src column, has_any_selection() returns false, and the swap map never reactivates. The user reloads into
        // "carrier visual only" instead of the picked prefab.
        s_activeCharIdx.store(0, std::memory_order_release);
        s_selSrcIdx = k_initialSelectionIndices;
        s_selTgtIdx = k_initialSelectionIndices;
        std::scoped_lock lk(s_mapMtx);
        for (auto &m : s_swapMapPerChar)
            m.clear();
        for (auto &s : s_targetWrappersPerChar)
            s.clear();
        {
            std::scoped_lock lk2(s_bodyMapMtx);
            s_bodyToChar.clear(); // body pointers point into the previous arena -- drop the learned a1->bucket map
        }
        s_mapsRetained.store(false, std::memory_order_release);
    }

    int adopt_into_slot_and_select(Transmog::TransmogSlot intoSlot, Transmog::TransmogSlot fromSlot,
                                   int fromIdx) noexcept
    {
        const auto into = static_cast<std::size_t>(intoSlot);
        const auto from = static_cast<std::size_t>(fromSlot);
        if (into >= s_slotCatalogs.size())
            return -1;
        if (from >= s_slotCatalogs.size())
            return -1;
        if (fromIdx < 0)
            return -1;
        std::scoped_lock lk(s_catalogMtx);
        if (static_cast<std::size_t>(fromIdx) >= s_slotCatalogs[from].size())
            return -1;
        const auto entry = s_slotCatalogs[from][fromIdx]; // copy
        // Dedup by name in intoSlot's catalog.
        auto &dst = s_slotCatalogs[into];
        int existing = -1;
        for (std::size_t i = 0; i < dst.size(); ++i)
        {
            if (dst[i].name == entry.name)
            {
                existing = static_cast<int>(i);
                break;
            }
        }
        const int newIdx = (existing >= 0) ? existing : (dst.push_back(entry), static_cast<int>(dst.size() - 1));
        s_selTgtIdx[into] = newIdx;
        return newIdx;
    }

    bool has_any_selection() noexcept
    {
        ensure_default_sources_seeded(); // land the source defaults if the name table became ready since populate
        for (std::size_t i = 0; i < s_selSrcIdx.size(); ++i)
        {
            if (s_selSrcIdx[i] >= 0 && s_selTgtIdx[i] >= 0)
                return true;
        }
        return false;
    }

    // --- Loader-registry enumeration (NPC body-mesh pickup) ---
    //
    // The StringInfo registry (s_stringInfoRegistry) holds the prefab wrappers that are *currently resident* in the
    // player-character pipeline (typically just the player's loaded set). Body-mesh prefabs for NPCs (cd_nh*) and
    // unloaded player variants live in a SECOND registry: the AppearanceTableLoader's own name table at
    // s_loaderRegistrySingleton + 0x50 (the singleton is dereferenced once at boot).
    //
    // Layout:
    //   table_struct = *(QWORD*)s_loaderRegistrySingleton + 0x50
    //     +0x00 bucket_count u32
    //     +0x04 count        u32
    //     +0x08 capacity     u32
    //     +0x10 bucket_array
    //     +0x18 data_array_ptr -> pointer[count]
    //
    //   data_array[i] -> entry_struct (24 bytes typical)
    //     +0x00 hash u32 + region u32
    //     +0x08 key_wrapper_ptr (interned-name wrapper)
    //     +0x10 value_wrapper_ptr (the partprefabdyeslot wrapper our
    //                              hook substitutes -- this is the
    //                              one we want to add to the catalog)
    //
    //   wrapper_ptr (+0x10 in entry):
    //     +0x00 ptr-to-self+0x18 (string interner self-pointer)
    //     +0x08 length u32
    //     +0x0C hash u32
    //     +0x18 inline NUL-terminated name
    //
    // Naming convention difference vs StringInfo:
    //   File path : cd_nhw_00_no_ub_00_20027  (with _00_ markers)
    //   Reg key   : cd_nhw_no_ub_20027        (no _00_ markers)
    // Slot classification needs to handle BOTH forms -- we look for the bare slot tag (`_hel_`, `_ub_`, `_cloak_`,
    // `_hand_`, `_foot_`) rather than the `_<tag>_00_` form used by StringInfo entries.
    //
    // Wrappers from this registry merge into existing catalog entries by name (deduped + sorted within each
    // PrefabEntry's wrappers vector). Names not yet in the catalog are inserted as fresh entries with the registry
    // wrapper as the sole instance. The singleton itself resolves into s_loaderRegistrySingleton at init(). The +0x50
    // offset walks into the table struct: a stable game-ABI offset, kept literal.
    constexpr std::size_t k_loaderRegistryTableOff = 0x50;

    static std::size_t enumerate_loader_registry_into_catalog() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const auto walkStart = std::chrono::steady_clock::now();
        constexpr std::size_t k_slotN = static_cast<std::size_t>(Transmog::TransmogSlot::Count);

        // Slot tag substrings (BARE form -- the registry uses no `_00_` markers). Each row is a null-terminated list
        // of substring patterns. An entry joins the slot if ANY pattern matches. The classifier loop OR's across
        // patterns AND across slots (a 1H sword name lands in MainHand AND OffHand AND SubWeapon's catalog, a ring
        // lands in both Ring1 and Ring2). Single-tag rows still work. Trailing nullptrs terminate the list.
        //
        // Gendered weapon slots accept BOTH `_phm_*` (male model) and `_phw_*` (female model) prefixes so Damiane's
        // `cd_phw_01_*` weapons land in the same catalogs Kliff's `cd_phm_01_*` counterparts do. Without this the
        // secondary-paired and female-character weapon catalogs end up empty and src seeding fails (which hides the
        // Prefabs picker checkbox). Ranged covers bows (_04_), pistols (_06_), and cannons (_13_) for the
        // cross-character ranged families captured in carrier_defaults.hpp.
        static constexpr std::size_t k_slotTagMax = 8;
        static constexpr const char *k_slotTagPatterns[k_slotN][k_slotTagMax] = {
            {"_hel_", nullptr},                                                                // Helm
            {"_ub_", nullptr},                                                                 // Chest
            {"_cloak_", nullptr},                                                              // Cloak
            {"_hand_", nullptr},                                                               // Gloves
            {"_foot_", nullptr},                                                               // Boots
            {"_earring_", nullptr},                                                            // Earring1
            {"_earring_", nullptr},                                                            // Earring2
            {"_necklace_", nullptr},                                                           // Necklace
            {"_ring_", nullptr},                                                               // Ring1 (paired)
            {"_ring_", nullptr},                                                               // Ring2
            {"_lantern_", nullptr},                                                            // Lantern
            {"_glasses_", nullptr},                                                            // Glasses
            {"_mask_00_", nullptr},                                                            // Mask
            {"_bag_0", nullptr},                                                               // Backpack
            {"_rinkband_", nullptr},                                                           // Bracelet
            {"_phm_01_", "_phw_01_", nullptr},                                                 // MainHand
            {"_phm_01_", "_phw_01_", "_03_shield_", nullptr},                                  // OffHand (1H + shields)
            {"_phm_04_", "_phw_04_", "_phm_06_", "_phw_06_", "_phm_13_", "_phw_13_", nullptr}, // Ranged
            {"_phm_01_dagger_", "_phw_01_dagger_", nullptr},                                   // SubWeapon
            {"_phm_02_", "_phw_02_", nullptr},                                                 // TwoHandWeapon
        };

        // Snapshot resolved sentinel (one atomic load per scan).
        const auto vtableSentinel = s_stringInfoVtable.load(std::memory_order_acquire);

        // Step 1: dereference the singleton, walk to the table struct.
        const auto singletonAbs = s_loaderRegistrySingleton.load(std::memory_order_acquire);
        if (singletonAbs < 0x10000ULL)
        {
            logger.warning("[prefab-swap] Loader registry singleton not "
                           "resolved -- skip enumeration");
            return 0;
        }
        const auto singletonPtr = DMKMemory::seh_read<std::uint64_t>(singletonAbs).value_or(0);
        if (singletonPtr < 0x10000ULL)
        {
            logger.warning("[prefab-swap] Loader registry singleton "
                           "@0x{:X} unreadable -- skip enumeration",
                           singletonAbs);
            return 0;
        }
        const std::uintptr_t tableStruct = singletonPtr + k_loaderRegistryTableOff;

        // Step 2: read count + data_array_ptr, sanity-check.
        const auto count = DMKMemory::seh_read<std::uint32_t>(tableStruct + 0x04).value_or(0);
        const auto dataArrayPtr = DMKMemory::seh_read<std::uint64_t>(tableStruct + 0x18).value_or(0);
        if (count == 0 || count > 100000 || dataArrayPtr < 0x10000ULL)
        {
            logger.warning("[prefab-swap] Loader registry sanity failed: "
                           "count={} dataArrayPtr=0x{:X} (table @0x{:X}) -- "
                           "skip enumeration",
                           count, dataArrayPtr, tableStruct);
            return 0;
        }

        // Step 3: bulk-copy the data-array pointer table (Phase-1 perf pattern -- one SEH frame for the whole array,
        // then per-entry reads against process memory).
        std::vector<std::uintptr_t> entryPtrs;
        const std::size_t arrayBytes = static_cast<std::size_t>(count) * sizeof(std::uintptr_t);
        bool bulkOk = false;
        if (arrayBytes > 0)
        {
            entryPtrs.resize(count);
            bulkOk = DMKMemory::seh_read_bytes(dataArrayPtr, entryPtrs.data(), arrayBytes);
        }

        // Step 4: walk entries, classify by slot tag, collect (name, wrapper) pairs into per-slot vectors. Deferred
        // merge into s_slotCatalogs after the walk so the catalog mutex is not held during the scan.
        struct Pending
        {
            std::string name;
            std::uintptr_t wrapper;
        };
        std::array<std::vector<Pending>, k_slotN> pending;
        for (auto &v : pending)
            v.reserve(512);

        std::uint32_t scanned = 0;
        std::uint32_t prefixMatch = 0;
        char nameBuf[k_extNameMax + 1] = {0};

        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entry =
                bulkOk ? entryPtrs[i] : DMKMemory::seh_read<std::uint64_t>(dataArrayPtr + 8ULL * i).value_or(0);
            if (entry < 0x10000ULL)
                continue;
            ++scanned;

            // Read KEY wrapper-ptr at entry+0x08. The KEY wrapper holds the inline prefab name in the standard
            // partprefabdyeslot format (+0x18 string buffer) -- the same format the body-mesh hook substitutes by
            // pointer equality. The +0x10 VALUE wrapper is a metadata struct (counts/IDs), not a name-bearing wrapper,
            // so reading +0x18 there gives junk and the prefix gate filters everything out.
            const auto wrapper = DMKMemory::seh_read<std::uint64_t>(entry + 0x08).value_or(0);
            if (wrapper < 0x10000ULL)
                continue;
            // Skip the StringInfo vtable sentinel (the registry can hold metadata-only entries that are not
            // partprefab wrappers).
            if (wrapper == vtableSentinel)
                continue;

            // Read inline name at wrapper+0x18 (max 96 chars per spec but we use k_extNameMax==256 for the buffer cap).
            constexpr std::size_t k_loaderNameCap = 96;
            const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(wrapper + 0x18), nameBuf, k_loaderNameCap);
            if (rlen == SIZE_MAX || rlen == 0)
                continue;

            // Broad `cd_` gate admits every character-prefab family (player, NPC, all races). The slot-tag substring
            // loop below (`_ub_`, `_hel_`, `_cloak_`, etc.) is the real classifier: entries that match no slot tag are
            // silently dropped, so non-armor families (monsters, misc) never enter any per-slot catalog. The "Exact"
            // picker toggle remains the user-facing per-slot filter.
            if (!(nameBuf[0] == 'c' && nameBuf[1] == 'd' && nameBuf[2] == '_'))
                continue;
            ++prefixMatch;

            // Slot classification by bare tag substring. NO break:
            // names matching multiple slots (e.g. `cd_phm_01_dagger_*` matches both MainHand's `_phm_01_` and
            // SubWeapon's `_phm_01_dagger_`) intentionally land in every matching catalog. This is what populates the
            // secondary paired slots (Ring2, OffHand) so their src can seed and the
            // Prefabs picker checkbox shows.
            for (std::size_t si = 0; si < k_slotN; ++si)
            {
                bool matched = false;
                for (std::size_t pi = 0; pi < k_slotTagMax; ++pi)
                {
                    const char *pat = k_slotTagPatterns[si][pi];
                    if (!pat)
                        break;
                    if (std::strstr(nameBuf, pat) != nullptr)
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    continue;
                Pending p;
                p.name = std::string(nameBuf, rlen);
                p.wrapper = wrapper;
                pending[si].push_back(std::move(p));
            }
        }

        // Step 5: merge into s_slotCatalogs. Two cases per pending:
        //   a) name already present -> append wrapper to existing
        //      PrefabEntry::wrappers (sorted + deduped).
        //   b) name absent -> insert a fresh PrefabEntry with the
        //      registry wrapper as the sole instance.
        std::array<std::size_t, k_slotN> addedCount{};
        std::array<std::size_t, k_slotN> mergedCount{};
        {
            std::scoped_lock lk(s_catalogMtx);
            for (std::size_t si = 0; si < k_slotN; ++si)
            {
                auto &cat = s_slotCatalogs[si];
                // Build an index for O(1) name->idx lookup. The catalogs are alphabetically sorted by name at this
                // point, so a lower_bound also works, but a hash map is simpler and the catalogs are small.
                std::unordered_map<std::string, std::size_t> idxByName;
                idxByName.reserve(cat.size() * 2);
                for (std::size_t ei = 0; ei < cat.size(); ++ei)
                    idxByName.emplace(cat[ei].name, ei);

                for (auto &p : pending[si])
                {
                    const auto it = idxByName.find(p.name);
                    if (it != idxByName.end())
                    {
                        // Merge: append + sort + unique.
                        auto &e = cat[it->second];
                        e.wrappers.push_back(p.wrapper);
                        std::sort(e.wrappers.begin(), e.wrappers.end());
                        e.wrappers.erase(std::unique(e.wrappers.begin(), e.wrappers.end()), e.wrappers.end());
                        ++mergedCount[si];
                    }
                    else
                    {
                        // Insert fresh entry.
                        PrefabEntry e;
                        e.name = p.name;
                        e.wrappers = {p.wrapper};
                        e.hash = 0;         // filled by enrichment
                        e.metadata = 0;     // filled by enrichment
                        e.is_loaded = true; // wrapper present
                        idxByName.emplace(e.name, cat.size());
                        cat.push_back(std::move(e));
                        ++addedCount[si];
                    }
                }

                // Re-sort the catalog alphabetically (insertions broke the invariant). Dedup by name as a defensive
                // measure -- idxByName guards inserts, so it does not fire, but it is cheap relative to the sort.
                std::sort(cat.begin(), cat.end(),
                          [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; });
                cat.erase(std::unique(cat.begin(), cat.end(),
                                      [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }),
                          cat.end());
            }
        }

        const auto walkMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walkStart).count();
        std::size_t totalAdded = 0;
        std::size_t totalMerged = 0;
        for (std::size_t i = 0; i < k_slotN; ++i)
        {
            totalAdded += addedCount[i];
            totalMerged += mergedCount[i];
        }
        logger.debug("[prefab-swap] Loader registry enumeration: "
                     "walked={} entries, scanned={} body-mesh-prefix={} "
                     "added {} new prefabs, merged {} into existing "
                     "(helm={} chest={} cloak={} gloves={} boots={}) ({}ms)",
                     count, scanned, prefixMatch, totalAdded, totalMerged, addedCount[0], addedCount[1], addedCount[2],
                     addedCount[3], addedCount[4], walkMs);

        return totalAdded;
    }

    std::size_t populate_slot_catalogs() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const auto walkStart = std::chrono::steady_clock::now();

        // Build a single shared catalog from one StringInfo walk, then copy it into every slot's local vector.
        // Per-slot tag filtering is intentionally NOT applied here: accessory slots (Earring/Necklace) carry no useful
        // filter tag, and body-specific prefixes leave several female-side dropdowns empty (e.g. Damiane's actual ring
        // carriers are cd_phm_00_ring_*, not cd_phw_*). The full body-mesh family in every slot lets the user pick
        // anything. The search box already filters by name.
        constexpr std::size_t k_slotN = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        std::array<std::vector<PrefabEntry>, k_slotN> local;

        // Broad prefix gates the walk to body-mesh entries. The 3-char gate "cd_" admits ALL character-prefab families:
        //   cd_phm_00_*       player human male
        //   cd_phw_00_*       player human female
        //   cd_nhm_*          NPC human male
        //   cd_nhw_*          NPC human female (incl. cd_nhw_00_no_*)
        //   cd_t0000_*        gender-shared accessory family (lanterns)
        //   cd_m0001_*        creature/monster mesh families
        //   cd_*              any future variant the engine ships
        // The StringInfo vtable sentinel filter inside walk_string_info drops the bulk of non-body StringInfo entries
        // before this visitor ever runs.
        constexpr const char *k_broadPrefix = "cd_";

        std::vector<PrefabEntry> shared;
        const auto total =
            walk_string_info(k_broadPrefix,
                             [&](std::uintptr_t /*entry*/, const char *name, std::uintptr_t wrapper, std::uint32_t hash)
                             {
                                 if (wrapper < 0x10000ULL)
                                     return;
                                 // Seed with the StringInfo wrapper as the canonical first instance. The boot-time
                                 // heap walk below merges parallel-pool wrappers into the same vector. metadata and
                                 // is_loaded are filled in below once the catalog is sorted.
                                 PrefabEntry e;
                                 e.name = std::string(name);
                                 e.wrappers = {wrapper};
                                 e.hash = hash;
                                 e.metadata = 0;     // filled post-walk
                                 e.is_loaded = true; // wrapper present
                                 shared.push_back(std::move(e));
                             });
        (void)total;

        // Sort + dedup once on the shared catalog, then copy to each slot. Sorting before copy means the slot vectors
        // are already sorted (the per-slot sort/dedup pass below becomes a no-op for them -- left in place to handle
        // any future per-slot additions, e.g. enumerate_loader_registry_into_catalog).
        std::sort(shared.begin(), shared.end(),
                  [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; });
        shared.erase(std::unique(shared.begin(), shared.end(),
                                 [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }),
                     shared.end());
        for (std::size_t i = 0; i < k_slotN; ++i)
            local[i] = shared;

        // Sort each slot's entries alphabetically (UX) and dedup by name. StringInfo can carry parallel allocations of
        // the same name, and the dropdown must show one row per logical prefab.
        std::array<std::size_t, k_slotN> counts{};
        for (std::size_t i = 0; i < k_slotN; ++i)
        {
            auto &v = local[i];
            std::sort(v.begin(), v.end(), [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; });
            v.erase(std::unique(v.begin(), v.end(),
                                [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }),
                    v.end());
            counts[i] = v.size();
        }

        {
            std::scoped_lock lk(s_catalogMtx);
            for (std::size_t i = 0; i < k_slotN; ++i)
                s_slotCatalogs[i] = std::move(local[i]);
            // Reset selections that point past the new catalog size. Without this, a refresh after the catalog shrinks
            // leaves stale indices that reference freed entries.
            for (std::size_t i = 0; i < k_slotN; ++i)
            {
                const auto sz = static_cast<int>(s_slotCatalogs[i].size());
                if (s_selSrcIdx[i] >= sz)
                    s_selSrcIdx[i] = -1;
                if (s_selTgtIdx[i] >= sz)
                    s_selTgtIdx[i] = -1;
            }

            // Auto-seed of source selections runs below, AFTER enumerate_loader_registry_into_catalog(). That call
            // adds many entries and re-sorts each slot's vector, which invalidates any index seeded here.
        }

        // --- Heap-walk merge: cache parallel-pool wrappers per name ---
        //
        // The StringInfo walk above seeded each PrefabEntry with the entry+0x18 wrapper. The engine also sources
        // wrappers from a parallel partprefabdyeslot pool that StringInfo does NOT index. We walk ONCE here at boot
        // for ALL cataloged names. The dominant cost is the heap traversal itself, so the single-pass cost for N names
        // is close to the cost for 1.
        //
        // Pass empty tgtNames so only the src side runs. We want all wrappers per name, regardless of src/tgt
        // classification (the catalog is symmetric -- any entry can take either role).
        const auto hwStart = std::chrono::steady_clock::now();
        std::vector<std::string> allNames;
        struct LocRef
        {
            std::size_t slot, idx;
        };
        std::vector<LocRef> allLocs;
        {
            std::scoped_lock lk(s_catalogMtx);
            std::size_t reserve = 0;
            for (std::size_t si = 0; si < k_slotN; ++si)
                reserve += s_slotCatalogs[si].size();
            allNames.reserve(reserve);
            allLocs.reserve(reserve);
            for (std::size_t si = 0; si < k_slotN; ++si)
            {
                for (std::size_t ei = 0; ei < s_slotCatalogs[si].size(); ++ei)
                {
                    allNames.push_back(s_slotCatalogs[si][ei].name);
                    allLocs.push_back({si, ei});
                }
            }
        }

        std::vector<std::vector<std::uintptr_t>> outBySrc(allNames.size());
        std::vector<std::uintptr_t> outTgt(allNames.size(), 0);
        std::size_t totalWrappers = 0;
        if (!allNames.empty())
        {
            heap_walk_partprefab_for_names(allNames, /*tgtNames=*/{}, outBySrc, outTgt);

            std::scoped_lock lk(s_catalogMtx);
            for (std::size_t i = 0; i < allLocs.size(); ++i)
            {
                const auto si = allLocs[i].slot;
                const auto ei = allLocs[i].idx;
                if (si >= s_slotCatalogs.size() || ei >= s_slotCatalogs[si].size())
                    continue;
                auto &e = s_slotCatalogs[si][ei];
                for (auto w : outBySrc[i])
                    e.wrappers.push_back(w);
                std::sort(e.wrappers.begin(), e.wrappers.end());
                e.wrappers.erase(std::unique(e.wrappers.begin(), e.wrappers.end()), e.wrappers.end());
                totalWrappers += e.wrappers.size();
            }
        }
        const auto hwMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hwStart).count();

        // --- Loader-registry enumeration ---
        //
        // Pulls in NPC and unloaded body-mesh prefabs from the AppearanceTableLoader's name registry at
        // loaderRegistrySingleton+0x50. The StringInfo walk above only sees prefabs currently bound into the player
        // pipeline. This fills in the rest, so the picker can target any body-mesh asset the engine knows about.
        //
        // Order matters: runs AFTER the StringInfo walk + heap walk (so existing entries get their parallel-pool
        // wrappers merged first), and BEFORE metadata enrichment (so newly added entries also get their metadata
        // cross-reference).
        enumerate_loader_registry_into_catalog();

        // Cross-slot union pass. enumerate_loader_registry adds NPC entries to a SINGLE slot per name (the one whose
        // tag pattern matches, e.g. `cd_nhw_no_ub_20027` lands only in Chest). The cross-slot prefab-mode tab in the
        // picker reads slot 0's catalog and expects a true union, so without this pass all chest-only / cloak-only /
        // boots-only / gloves-only NPC additions are invisible in any other slot's prefab dropdown. Merge by name
        // (preserve wrappers/metadata of the first row seen for that name) and copy the resulting union back into every
        // LT-managed slot.
        {
            std::scoped_lock lk(s_catalogMtx);
            std::unordered_map<std::string, PrefabEntry> unionByName;
            unionByName.reserve(20000);
            for (std::size_t si = 0; si < k_slotN; ++si)
            {
                for (const auto &e : s_slotCatalogs[si])
                {
                    auto [it, inserted] = unionByName.emplace(e.name, e);
                    if (!inserted)
                    {
                        // Same name already present -- merge wrapper pointers so the union row carries every pool
                        // variant (parallel-pool wrappers vary across slot-specific enumerate adds).
                        auto &dst = it->second.wrappers;
                        for (auto w : e.wrappers)
                            dst.push_back(w);
                        std::sort(dst.begin(), dst.end());
                        dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
                    }
                }
            }
            std::vector<PrefabEntry> unionVec;
            unionVec.reserve(unionByName.size());
            for (auto &kv : unionByName)
                unionVec.push_back(std::move(kv.second));
            std::sort(unionVec.begin(), unionVec.end(),
                      [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; });
            for (std::size_t si = 0; si < k_slotN; ++si)
                s_slotCatalogs[si] = unionVec;
            // Selection indices were valid against the pre-union catalogs. Re-clamp so any picks survive the re-sort.
            // NOTE: this assumes an earlier pass set the selections by name (preset_manager does this). Session-only
            // picks made before populate_slot_catalogs runs are already clamped to the post-enumerate sort, so the
            // extra re-sort here can shift their indices. The runtime cost is one bounds clamp, which is acceptable.
            for (std::size_t i = 0; i < k_slotN; ++i)
            {
                const auto sz = static_cast<int>(s_slotCatalogs[i].size());
                if (s_selSrcIdx[i] >= sz)
                    s_selSrcIdx[i] = -1;
                if (s_selTgtIdx[i] >= sz)
                    s_selTgtIdx[i] = -1;
            }
        }

        // --- AppearanceTableLoader metadata enrichment ---
        //
        // Cross-references each catalog entry against the engine's name->wrapper registry at
        // loaderRegistrySingleton+0x50, through the AOB-resolved name lookup primitive. One direct call per name, and
        // no heap scan.

        // For every catalog entry seeded above (StringInfo-resident), ask the loader whether it knows about the same
        // name. We record the metadata pointer so the picker / future force-load logic can verify the engine's catalog
        // membership. is_loaded stays true here -- the entry came from a StringInfo walk so a wrapper IS resident.
        //
        // A full iteration of the loader container to surface NOT-YET-loaded prefabs needs a walk of the hash-bucket
        // structure inside the container. The bucket shape is documented (+0x68/+0x70 hash buckets), but the
        // iteration semantics and the lock-acquisition discipline are not. The safer path is the on-demand lookup
        // already exposed via `lookup_prefab_metadata` -- the picker can call it when the user types a name not in the
        // catalog, then surface a synthetic "(unloaded)" entry on a hit.
        //
        // SEH-isolated: the lookup can touch engine memory if the loader captured during this populate cycle.
        std::size_t metaEnriched = 0;
        if (is_loader_ready())
        {
            std::scoped_lock lk(s_catalogMtx);
            for (auto &cat : s_slotCatalogs)
            {
                for (auto &e : cat)
                {
                    const auto m = lookup_prefab_metadata(e.name.c_str());
                    if (m)
                    {
                        e.metadata = m;
                        ++metaEnriched;
                    }
                }
            }
        }
        if (is_loader_ready())
        {
            logger.debug("[prefab-swap] Catalog metadata enrichment: "
                         "{} entries cross-referenced against the "
                         "AppearanceTableLoader catalog.",
                         metaEnriched);
        }
        else
        {
            logger.debug("[prefab-swap] Catalog metadata enrichment "
                         "skipped -- AppearanceTableLoader not yet captured.");
        }

        // Publish AFTER the heap-walk merge so apply paths waiting on the catalog see fully-resolved wrapper vectors
        // (no partial single-wrapper data leaking into the swap map).
        s_catalogPopulated.store(true, std::memory_order_release);

        // Seed the per-character default SOURCE selections. Deferred to ensure_default_sources_seeded() because it
        // needs ItemNameTable (to resolve carrier itemName -> itemId -> variant meshes), which is NOT ready this early
        // -- it builds on a worker that finishes after this catalog populate. The call here is a best-effort attempt
        // and is usually a no-op. The activation gate (has_any_selection) and the picker read (selection_src_index)
        // re-invoke it, so the defaults land the instant the name table is ready.
        ensure_default_sources_seeded();

        const auto walkMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walkStart).count();
        logger.info("[prefab-swap] Catalog populated: helm={} chest={} "
                    "cloak={} gloves={} boots={} ({}ms)",
                    counts[0], counts[1], counts[2], counts[3], counts[4], walkMs);
        logger.debug("[prefab-swap] Catalog wrappers cached: {} names, "
                     "{} total wrappers ({}ms heap walk)",
                     allNames.size(), totalWrappers, hwMs);

        std::size_t totalCount = 0;
        for (auto c : counts)
            totalCount += c;
        return totalCount;
    }

    std::size_t apply_selections_to_swap_map() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Snapshot per-slot plans entirely from the catalog. All wrappers (StringInfo entry+0x18 plus the
        // parallel-pool variants recovered by the boot heap walk) are pre-cached in PrefabEntry::wrappers, so this hot
        // path is O(N) over the configured slots, with no I/O and no heap walk.
        //
        // Per-character build (apply-windowed model). The swap map's job is install-time substitution: the natpipe
        // hook redirects src->tgt while the engine reads a wrapper for installation. After that the tgt is materially
        // installed and subsequent rendering reads it directly. Each apply covers exactly ONE body, so the map is
        // keyed by character. A rebuild of ONLY the active character's bucket preserves the other characters'
        // installed substitutions for their next teardown. Without per-char keying, a Damiane apply between two Kliff
        // applies wipes Kliff's bucket, the second Kliff apply's tear_down cannot unlink Kliff's still-attached tgt
        // wrappers, and they stay stuck on the body. The natpipe hook dispatches on s_activeCharIdx, which resolves
        // cross-character src collisions (Kliff+Oongka both default to `cd_phm_00_ub_00_0054` for Chest). Each body's
        // apply then sees only its own bucket and the right tgt fires. The per-char `s_selSrcIdxPerChar` /
        // `s_selTgtIdxPerChar` arrays preserve in-memory picks across editing-character switches. The per-char
        // swap-map buckets are the runtime materialization of those picks.
        constexpr std::size_t k_slotN = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        struct SlotPlan
        {
            std::string srcName;                     // primary/log source name (the resolved rig)
            std::vector<std::string> srcNames;       // ALL rig siblings registered as sources (body-swap robustness)
            std::string tgtName;
            std::vector<std::uintptr_t> srcWrappers; // from catalog (union across rig siblings)
            std::uintptr_t tgtWrapper{0};
        };
        std::array<SlotPlan, k_slotN> plans;

        // Resolve the apply target character. `s_activeCharIdx` is primed by `PresetManager::apply_to_state` whenever
        // the editing character changes, and tracks the character whose carrier+prefab WILL be installed by the
        // upcoming apply. Idx 0 means PresetManager did not bind a character yet (boot-time, before the first
        // apply_to_state). In that case bail out. The next apply_to_state retries with a real idx.
        const auto activeIdx = s_activeCharIdx.load(std::memory_order_acquire);
        if (activeIdx < 1 || activeIdx > 3)
        {
            // No bound character -- skip the rebuild entirely. Other characters' previously-installed buckets must stay
            // intact so the natpipe-hook can still find their substitutions during a later teardown.
            return 0;
        }
        const auto ci = static_cast<std::size_t>(activeIdx - 1);
        const auto cc = static_cast<Transmog::CarrierChar>(ci);
        {
            std::scoped_lock lk(s_catalogMtx);
            for (std::size_t i = 0; i < k_slotN; ++i)
            {
                const auto tgtIdx = s_selTgtIdxPerChar[ci][i];
                if (tgtIdx < 0)
                    continue;
                auto &cat = s_slotCatalogs[i];
                if (tgtIdx >= static_cast<int>(cat.size()))
                    continue;

                // src resolution priority:
                //   1. Active character's runtime carrier source mesh (carrier_source_seed derives it from the
                //      carrier itemId's variant list) matched by name in the slot's catalog -- the authoritative
                //      source identity for the currently-installing character.
                //   2. Per-char s_selSrcIdxPerChar fallback when the carrier lookup misses (no carrier source, or the
                //      derived mesh is not present in the slot's catalog).
                //   3. cat0 cross-slot adoption when the slot's own catalog never received the carrier prefab (paired
                //      slots whose tag patterns missed at boot, or prefabs absent from the loader registry).
                std::size_t resolvedSrcIdx = SIZE_MAX;
                const std::string expectedSrcStr = carrier_source_seed(cc, static_cast<Transmog::TransmogSlot>(i));
                const char *expectedSrc = expectedSrcStr.empty() ? nullptr : expectedSrcStr.c_str();
                if (expectedSrc && expectedSrc[0] != '\0')
                {
                    for (std::size_t k = 0; k < cat.size(); ++k)
                    {
                        if (cat[k].name == expectedSrc)
                        {
                            resolvedSrcIdx = k;
                            break;
                        }
                    }
                    if (resolvedSrcIdx == SIZE_MAX && !s_slotCatalogs.empty())
                    {
                        const auto &cat0 = s_slotCatalogs[0];
                        for (std::size_t k = 0; k < cat0.size(); ++k)
                        {
                            if (cat0[k].name == expectedSrc)
                            {
                                cat.push_back(cat0[k]);
                                resolvedSrcIdx = cat.size() - 1;
                                break;
                            }
                        }
                    }
                }
                if (resolvedSrcIdx == SIZE_MAX)
                {
                    const auto srcIdx = s_selSrcIdxPerChar[ci][i];
                    if (srcIdx >= 0 && srcIdx < static_cast<int>(cat.size()))
                        resolvedSrcIdx = static_cast<std::size_t>(srcIdx);
                }
                if (resolvedSrcIdx == SIZE_MAX)
                    continue;

                plans[i].srcName = cat[resolvedSrcIdx].name;
                plans[i].tgtName = cat[tgtIdx].name;
                if (!cat[tgtIdx].wrappers.empty())
                    plans[i].tgtWrapper = cat[tgtIdx].wrappers.front();

                // Register EVERY body-rig sibling of the resolved source, not just the rig matching the character's
                // name-derived body. The carrier item shares one mesh across rigs (e.g. Kliff_Mask ->
                // cd_phm_00_/cd_phw_01_/cd_pom_01_mask_00_0271_a). Which rig the engine emits depends on the
                // wearer's REAL body, which a body-swap mod can change out from under the name->body assumption in
                // carrier_defaults. The slot catalog tag (e.g. "_mask_00_") already holds every rig variant, so we
                // union the wrappers of all entries whose rig-stripped stem matches -- the natpipe hook then catches
                // whichever rig is emitted. Excludes the chosen target wrapper so we never redirect it onto itself.
                // Falls back to the single resolved entry when the name has no rig-stem shape (e.g. lanterns) or has no
                // siblings resident.
                const std::string srcStem = rig_stripped_stem(cat[resolvedSrcIdx].name);
                std::size_t siblingCount = 0;
                for (std::size_t k = 0; k < cat.size(); ++k)
                {
                    const bool isSibling =
                        srcStem.empty() ? (k == resolvedSrcIdx) : (rig_stripped_stem(cat[k].name) == srcStem);
                    if (!isSibling)
                        continue;
                    plans[i].srcNames.push_back(cat[k].name);
                    std::size_t added = 0;
                    for (auto w : cat[k].wrappers)
                        if (w != 0 && w != plans[i].tgtWrapper)
                        {
                            plans[i].srcWrappers.push_back(w);
                            ++added;
                        }
                    ++siblingCount;
                    logger.trace("[prefab-swap]   char[{}] slot[{}] src-sibling \"{}\" catalogWrappers={}{}", ci, i,
                                 cat[k].name, added, (k == resolvedSrcIdx) ? " (primary rig)" : "");
                }
                std::sort(plans[i].srcWrappers.begin(), plans[i].srcWrappers.end());
                plans[i].srcWrappers.erase(std::unique(plans[i].srcWrappers.begin(), plans[i].srcWrappers.end()),
                                           plans[i].srcWrappers.end());
                logger.trace("[prefab-swap]   char[{}] slot[{}] src-register primary=\"{}\" stem=\"{}\" "
                             "rigSiblings={} srcWrappers={} tgt=\"{}\" (0x{:X})",
                             ci, i, cat[resolvedSrcIdx].name, srcStem, siblingCount, plans[i].srcWrappers.size(),
                             cat[tgtIdx].name, plans[i].tgtWrapper);
            }
        }

        // Fresh heap walk for the source names of the selected slots. The cataloged `wrappers` field can be incomplete
        // for prefabs that have many parallel-pool instances: the batched walk in populate_slot_catalogs harvests only
        // the instances resident at scan time, while a direct heap walk for one name finds all of them. The body-mesh
        // hook substitutes by pointer equality, so missing wrappers = missing substitutions = no visible swap.
        //
        // Re-walk for the small set of currently-selected source names here. Triggered through
        // reactivate_with_selections (a picker pick, or notify_apply_starting at the start of an apply pass), not on a
        // per-frame path.
        std::vector<std::string> srcNamesForWalk;
        std::vector<std::size_t> srcSlotIdx;
        for (std::size_t i = 0; i < k_slotN; ++i)
        {
            // Walk every rig sibling registered as a source for this slot, not just the primary name, so the fresh
            // walk picks up parallel-pool wrappers of the non-default rigs too (the ones a body-swap mod emits).
            for (const auto &sn : plans[i].srcNames)
            {
                if (sn.empty())
                    continue;
                srcNamesForWalk.push_back(sn);
                srcSlotIdx.push_back(i);
            }
        }
        if (!srcNamesForWalk.empty())
        {
            std::vector<std::vector<std::uintptr_t>> outBySrc(srcNamesForWalk.size());
            std::vector<std::uintptr_t> outTgt(srcNamesForWalk.size(), 0);
            heap_walk_partprefab_for_names(srcNamesForWalk, /*tgtNames=*/{}, outBySrc, outTgt);
            for (std::size_t k = 0; k < srcNamesForWalk.size(); ++k)
            {
                auto &dst = plans[srcSlotIdx[k]].srcWrappers;
                const auto tgtW = plans[srcSlotIdx[k]].tgtWrapper;
                std::size_t walkAdded = 0;
                for (auto w : outBySrc[k])
                    if (w != 0 && w != tgtW) // never redirect the target wrapper onto itself
                    {
                        dst.push_back(w);
                        ++walkAdded;
                    }
                std::sort(dst.begin(), dst.end());
                dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
                logger.trace("[prefab-swap]   char[{}] slot[{}] heap-walk src \"{}\" found={} added={} "
                             "srcWrappers-now={}",
                             ci, srcSlotIdx[k], srcNamesForWalk[k], outBySrc[k].size(), walkAdded, dst.size());
            }
        }

        // Build s_swapMapPerChar[ci] atomically under s_mapMtx. The bucket covers exactly the active character's body
        // for this apply. The natpipe hook reads from it during install-time wrapper traversal and dispatches via
        // s_activeCharIdx, so other characters' buckets stay live for their own pending teardowns.
        // s_targetWrappersPerChar[ci] is this character's cleanup ledger. deactivate_for_clear drains the substitution
        // ledger, then merges all three buckets into one target set and validates every drained record against it.
        std::size_t resolved = 0;
        {
            std::scoped_lock lk(s_mapMtx);
            s_swapMapPerChar[ci].clear();
            s_targetWrappersPerChar[ci].clear();
            for (std::size_t i = 0; i < k_slotN; ++i)
            {
                auto &p = plans[i];
                if (p.srcName.empty() || p.tgtName.empty())
                    continue;
                if (p.srcWrappers.empty() || p.tgtWrapper == 0)
                {
                    logger.warning("[prefab-swap]   char[{}] slot[{}] "
                                   "UNRESOLVED \"{}\" -> \"{}\" "
                                   "(srcWrappers={} tgtWrapper=0x{:X})",
                                   ci, i, p.srcName, p.tgtName, p.srcWrappers.size(), p.tgtWrapper);
                    continue;
                }
                for (auto sw : p.srcWrappers)
                    s_swapMapPerChar[ci].emplace(sw, p.tgtWrapper);
                s_targetWrappersPerChar[ci].insert(p.tgtWrapper);
                ++resolved;
                logger.debug("[prefab-swap]   char[{}] slot[{}] RESOLVED "
                             "\"{}\" ({} src) -> \"{}\" (0x{:X})",
                             ci, i, p.srcName, p.srcWrappers.size(), p.tgtName, p.tgtWrapper);
            }
        }
        if (resolved > 0)
            s_mapsRetained.store(true, std::memory_order_release); // enable natpipe cleanup even after s_active drops
        return resolved;
    }

    // Arm the swap for a single-slot install. The header explains why this cannot route through
    // reactivate_with_selections. That path drains the substitution ledger for every slot, and a single-slot apply
    // re-installs only its own slot.
    //
    // apply_selections_to_swap_map is a pure recompute. It resolves the selected names to their live wrapper pointers
    // and rebuilds the per-character buckets. It reads no record and reverts nothing, so a slot that already holds a
    // live substitution keeps it.
    //
    // The rebuild is also what keeps this correct across a character switch. A retained map holds the previous
    // character's wrapper pointers, and a bind against those installs that character's mesh.
    std::size_t ensure_armed_for_slot_apply() noexcept
    {
        if (!s_orig)
            return 0; // hook not installed
        if (!has_any_selection())
            return 0; // nothing selected -- leave the current state alone
        const auto resolved = apply_selections_to_swap_map();
        if (resolved == 0)
        {
            DMK::Logger::get_instance().debug("[prefab-swap] slot-apply arm: no selections resolved -- "
                                              "leaving swap state unchanged");
            return 0;
        }
        if (!s_active.exchange(true, std::memory_order_acq_rel))
            DMK::Logger::get_instance().debug("[prefab-swap] slot-apply arm: ACTIVATED ({} slot(s) bound)", resolved);
        return resolved;
    }

    // Reactivate using the current per-slot dropdown selections. This is the auto-apply path triggered when the
    // slot-row body combos change in the overlay. It runs a deactivate-then-activate cycle, so the new selection
    // becomes visible without a hotkey press.
    std::size_t reactivate_with_selections() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        if (!s_orig)
            return 0; // hook not installed -- nothing to do
        if (s_active.load(std::memory_order_acquire))
        {
            // Cleanly tear down the prior substitution (scene-graph +0x40 reverts plus the staging-record
            // reverse-write) so the new map does not double-bind the previous targets.
            deactivate_for_clear();
        }
        if (!has_any_selection())
        {
            // Nothing left to bind -- stay deactivated.
            return 0;
        }
        const auto resolved = apply_selections_to_swap_map();
        if (resolved == 0)
        {
            logger.warning("[prefab-swap] reactivate_with_selections: no "
                           "slot selections resolved -- staying INACTIVE");
            return 0;
        }
        s_active.store(true, std::memory_order_release);
        s_callCount.store(0, std::memory_order_relaxed);
        s_substCount.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lk(s_lastApplyMtx);
            s_lastApplyValid = false;
            std::memset(s_lastApplyItems, 0, sizeof(s_lastApplyItems));
        }
        logger.info("[prefab-swap] reactivated via UI selections "
                    "({} slot(s) bound)",
                    resolved);
        return resolved;
    }

    // --- Hook callback ---

    static std::int64_t __fastcall on_struct_copy(std::int64_t a1, std::int64_t a2)
    {
        const auto trampoline = s_orig;
        if (!trampoline)
            return 0;

        // Cheap guards before any indirect read.
        if (!s_active.load(std::memory_order_acquire))
            return trampoline(a1, a2);
        if (!Transmog::in_transmog().load(std::memory_order_relaxed))
            return trampoline(a1, a2);
        if (a2 < 0x10000)
            return trampoline(a1, a2);

        s_callCount.fetch_add(1, std::memory_order_relaxed);

        const auto srcWrapper = DMKMemory::seh_read<std::uint64_t>(a2).value_or(0);
        // Filter the StringInfo vtable sentinel: a2 sometimes points at the entry's +0x08 vtable slot rather than its
        // wrapper-ptr slot, and srcWrapper then equals the sentinel address. The sentinel resolves through
        // k_stringInfoVtableCandidates. A zero here means the cascade missed, so the equality test never matches and
        // the swap-map lookup proceeds unchanged.
        const auto siVtable = s_stringInfoVtable.load(std::memory_order_acquire);
        if (srcWrapper < 0x10000ULL || srcWrapper == siVtable)
            return trampoline(a1, a2);

        std::uintptr_t tgtWrapper = 0;
        {
            const auto activeIdx = s_activeCharIdx.load(std::memory_order_acquire);
            if (activeIdx < 1 || activeIdx > 3)
                return trampoline(a1, a2);
            const auto bucket = static_cast<std::size_t>(activeIdx - 1);
            std::scoped_lock lk(s_mapMtx);
            auto &m = s_swapMapPerChar[bucket];
            const auto it = m.find(srcWrapper);
            if (it == m.end())
                return trampoline(a1, a2);
            tgtWrapper = it->second;
        }

        // Bump target's refcount BEFORE substitution so that the destination's eventual decrement-on-destruct stays
        // balanced.
        increment_wrapper_refcount(tgtWrapper);

        // Substitute: the caller's source struct (a2) now points at our target wrapper. The struct-copy trampoline
        // will move the wrapper-ptr to dest+0 and write the sentinel back to source+0. The caller's cleanup then sees
        // a sentinel and skips the decrement of the now-unreferenced original wrapper -- a small +1 leak we tolerate.
        if (!write_qword_seh(reinterpret_cast<void *>(a2), tgtWrapper))
        {
            // Substitute failed -- pass through. This leaks a refcount bump on tgtWrapper. Rare path, accept it.
            return trampoline(a1, a2);
        }

        const auto sc = s_substCount.fetch_add(1, std::memory_order_relaxed);
        if (sc < 50)
        {
            // Diagnostic for first N substitutions.
            DMK::Logger::get_instance().info("[prefab-swap] SWAP src=0x{:X} -> tgt=0x{:X} "
                                             "(subst #{})",
                                             srcWrapper, tgtWrapper, sc + 1);
        }

        // Run the trampoline -- it MOVEs *a2 (our target wrapper) to *a1 (dest+0) and sentinels *a2.
        const auto rc = trampoline(a1, a2);

        // Track the dest so deactivate_for_clear can reverse-write the original Kliff wrapper, restoring engine state
        // to a form LT's auth-table-driven tear_down can walk cleanly.
        {
            std::scoped_lock lk(s_substLogMtx);
            if (s_substLog.size() < k_maxSubstLog)
            {
                s_substLog.push_back({static_cast<std::uintptr_t>(a1), srcWrapper});
            }
        }

        return rc;
    }

    // Natural-pipeline unlink hook.
    //
    // Walks RDX's wrapper list at hook entry. For each entry whose wrapper matches the active character's src in
    // s_swapMapPerChar[s_activeCharIdx-1], substitutes to the corresponding target. Calls trampoline. Restores the
    // originals afterwards so the caller's refcount-release loop on the list operates on the same wrappers it
    // incremented.
    //
    // Verbose logging:
    //   - HOOK ENTRY: hit#, a1 (body), list ptr, count, return address.
    //   - PER ENTRY[i]: orig wrapper, swap-map decision (SUBST or PASSTHROUGH), tgt if substituted.
    //   - POST-CALL:   substitutions performed, list count.
    //   - RESTORE:     each restoration, final list state.
    static std::int64_t __fastcall on_natural_pipeline(std::int64_t a1, std::uint64_t *a2, std::uint64_t *a3)
    {
        const auto trampoline = s_origNaturalPipeline;
        if (!trampoline)
            return 0;

        // Passthrough only when the feature has NEVER bound a swap this session. We deliberately do NOT gate on
        // s_active alone: an installed target must still be unlinked when its body is torn down even if the
        // now-active character has no swap (s_active==false) -- the cross-character orphan path. The empty-list fast
        // path below keeps the cost negligible for the common zero-length unlink calls.
        if (!s_active.load(std::memory_order_acquire) && !s_mapsRetained.load(std::memory_order_acquire))
            return trampoline(a1, a2, a3);

        auto &logger = DMK::Logger::get_instance();
        const auto hitSeq = s_natpipeHitCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto callerRa = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

        // Read the list shape: a2 -> { data: u64*, count: u32, ... }
        // Stride 16 bytes per entry: (wrapper_qword, byte_flag, padding).
        std::uint64_t *listData = nullptr;
        std::uint32_t listCount = 0;
        if (a2)
        {
            const auto raw = DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a2)).value_or(0);
            listData = reinterpret_cast<std::uint64_t *>(raw);
            listCount = DMKMemory::seh_read<std::uint32_t>(
                            reinterpret_cast<std::uintptr_t>(reinterpret_cast<const char *>(a2) + 8))
                            .value_or(0);
        }

        s_natpipeListEntries.fetch_add(listCount, std::memory_order_relaxed);

        // Empty-list fast path: the engine fires this function from many call sites (the render/animation tick among
        // them) with listCount=0. There is nothing for us to do, and a log line per call floods the trace stream. Skip
        // the hook body entirely and just call the trampoline.
        constexpr std::uint32_t k_maxEntries = 64;
        std::uint64_t saved[k_maxEntries] = {};
        std::uint32_t substCount = 0;
        const auto cnt = (listCount < k_maxEntries) ? listCount : k_maxEntries;

        if (!listData || cnt == 0)
            return trampoline(a1, a2, a3);

        // Resolve which character's bucket this teardown/install path applies to. s_activeCharIdx is set by
        // PresetManager::apply_to_state BEFORE the engine drives any wrapper traversal, so by the time the hook fires
        // it already points at the body being assembled or torn down. With no active character bound we pass through
        // unchanged.
        const auto activeIdx = s_activeCharIdx.load(std::memory_order_acquire);
        if (activeIdx < 1 || activeIdx > 3)
            return trampoline(a1, a2, a3);
        const auto bucket = static_cast<std::size_t>(activeIdx - 1);

        // Walk the list under SEH and substitute matching src wrappers. PASSTHROUGH entries (wrapper not in this
        // character's bucket, or a low address) intentionally do not log. The engine queries many unrelated wrappers,
        // and the noise drowns out the rare SUBST events that matter for the body-mesh cleanup path.
        for (std::uint32_t i = 0; i < cnt; ++i)
        {
            const auto orig =
                DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&listData[i * 2])).value_or(0);
            saved[i] = orig;
            if (orig < 0x10000ULL)
                continue;

            // Lookup in the active character's bucket. Read-only on the hot path, and the bucket is immutable while
            // s_active is true, per LT design.
            std::uintptr_t tgt = 0;
            {
                std::scoped_lock lk(s_mapMtx);
                auto &m = s_swapMapPerChar[bucket];
                auto it = m.find(static_cast<std::uintptr_t>(orig));
                if (it != m.end())
                    tgt = it->second;
            }

            if (tgt != 0)
            {
                // Active bucket owns this wrapper => a1 is the active character's body (this fires during its
                // assembly). Learn a1 -> bucket so a LATER teardown of this same body, while a DIFFERENT character
                // is active, resolves the correct bucket instead of orphaning the target.
                std::scoped_lock lk(s_bodyMapMtx);
                s_bodyToChar[static_cast<std::uintptr_t>(a1)] = static_cast<int>(bucket);
            }
            else
            {
                // Active bucket missed: the engine is unlinking a body OTHER than the active character (e.g.
                // Damiane's body torn down during a switch to Oongka -- the orphan path). Find which OTHER bucket(s)
                // own this wrapper. Exactly ONE owner is UNAMBIGUOUS, so the substitution is correct with zero
                // cross-talk (there is only one possible target). Only MULTIPLE owners are ambiguous -- the same
                // shared carrier swapped to DIFFERENT targets on more than one character. In that case disambiguate
                // by a1 (the body under process, learned during its own assembly). If a1 is unknown, SKIP rather than
                // risk an unlink of the wrong body's mesh. This is why keying by the wrapper's owning bucket, not by
                // s_activeCharIdx, is cross-talk-free: a shared wrapper is the ONLY case that can collide, and it is
                // handled explicitly.
                std::size_t matchBucket = 3;
                std::size_t matchCount = 0;
                {
                    std::scoped_lock lk(s_mapMtx);
                    for (std::size_t b = 0; b < 3; ++b)
                    {
                        if (b == bucket)
                            continue;
                        const auto it2 = s_swapMapPerChar[b].find(static_cast<std::uintptr_t>(orig));
                        if (it2 != s_swapMapPerChar[b].end())
                        {
                            ++matchCount;
                            matchBucket = b;
                            tgt = it2->second;
                        }
                    }
                }
                if (matchCount == 0)
                    continue; // not one of our sources in any bucket
                if (matchCount > 1)
                {
                    // Shared source in multiple buckets -> resolve by the body, or skip to avoid cross-talk.
                    int bodyBucket = -1;
                    {
                        std::scoped_lock lk(s_bodyMapMtx);
                        const auto bit = s_bodyToChar.find(static_cast<std::uintptr_t>(a1));
                        if (bit != s_bodyToChar.end())
                            bodyBucket = bit->second;
                    }
                    tgt = 0;
                    if (bodyBucket >= 0 && static_cast<std::size_t>(bodyBucket) != bucket)
                    {
                        std::scoped_lock lk(s_mapMtx);
                        auto &bm = s_swapMapPerChar[static_cast<std::size_t>(bodyBucket)];
                        const auto it2 = bm.find(static_cast<std::uintptr_t>(orig));
                        if (it2 != bm.end())
                        {
                            tgt = it2->second;
                            matchBucket = static_cast<std::size_t>(bodyBucket);
                        }
                    }
                    if (tgt == 0)
                    {
                        logger.trace("[natpipe-hook] hit#{} entry[{}] src 0x{:X} owned by {} buckets, a1=0x{:X} "
                                     "unresolved -- SKIP (avoid cross-talk)",
                                     hitSeq, i, orig, matchCount, static_cast<std::uint64_t>(a1));
                        continue;
                    }
                }
                static constexpr const char *k_charName[3] = {"Kliff", "Damiane", "Oongka"};
                logger.info("[prefab-swap] cleanup: unlinked orphaned body-mesh swap on {}'s body as it was torn "
                            "down (target 0x{:X} <- src 0x{:X}, {} owner) -- fixes the persistent fake-part after a "
                            "drop / character switch",
                            (matchBucket < 3 ? k_charName[matchBucket] : "?"), tgt, orig, matchCount);
            }

            if (write_qword_seh(&listData[i * 2], static_cast<std::uint64_t>(tgt)))
            {
                ++substCount;
                logger.trace("[natpipe-hook] hit#{} entry[{}] SUBST 0x{:X} -> "
                             "0x{:X} (src -> tgt) caller_ra=0x{:X}",
                             hitSeq, i, orig, tgt, callerRa);
            }
            else
            {
                logger.warning("[natpipe-hook] hit#{} entry[{}] write FAULTED -- "
                               "skipping",
                               hitSeq, i);
            }
        }

        s_natpipeSubstCount.fetch_add(substCount, std::memory_order_relaxed);

        // Run the natural pipeline. Engine walks parent+88 looking for our target wrappers, finds them, unlinks them.
        const auto result = trampoline(a1, a2, a3);

        // No substitutions -> no restore needed and no log output. Fall through and return without further work.
        if (substCount == 0)
            return result;

        // Restore originals so the caller's refcount-release loop on the list decrements the same wrappers it
        // incremented.
        std::uint32_t restored = 0;
        for (std::uint32_t i = 0; i < cnt; ++i)
        {
            if (saved[i] == 0)
                continue;
            const auto cur =
                DMKMemory::seh_read<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&listData[i * 2])).value_or(0);
            if (cur == saved[i])
                continue; // not substituted
            if (write_qword_seh(&listData[i * 2], saved[i]))
                ++restored;
        }
        logger.trace("[natpipe-hook] hit#{} done: substituted {} restored {} "
                     "result=0x{:X}",
                     hitSeq, substCount, restored, static_cast<std::uintptr_t>(result));
        return result;
    }

    // --- Init / shutdown ---

    void register_config()
    {
        // Body-mesh swap has no INI keys. The hook installs at boot. Source defaults derive at runtime from each
        // character's carrier item in carrier_defaults.hpp::k_carriers. Target selection is overlay-driven.
    }

    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        // --- RipRelative singleton cascades ---
        //
        // Four data-pointer cascades resolve StringInfoRegistry, StringInfoVtable, LoaderRegistry and
        // ApptContainerVtable. They store into atomic globals consumed by walk_string_info, the AppearanceTableLoader
        // enumerator, and the container-vtable filter. Cascade misses are non-fatal: each consumer treats a zero
        // atomic as a soft bypass (the catalog walk returns empty, the vtable filter accepts all entries) so the rest
        // of init() still completes.
        {
            const auto siReg = resolve_address(k_stringInfoRegistryCandidates, "StringInfoRegistry");
            if (siReg)
            {
                s_stringInfoRegistry.store(siReg, std::memory_order_release);
                logger.debug("[prefab-swap] StringInfoRegistry resolved at "
                             "0x{:X}",
                             siReg);
            }
            else
            {
                logger.warning("[prefab-swap] StringInfoRegistry cascade FAILED "
                               "-- catalog walk will return 0 entries.");
            }

            const auto siVt = resolve_address(k_stringInfoVtableCandidates, "StringInfoVtable");
            if (siVt)
            {
                s_stringInfoVtable.store(siVt, std::memory_order_release);
                logger.debug("[prefab-swap] StringInfoVtable resolved at "
                             "0x{:X}",
                             siVt);
            }
            else
            {
                logger.warning("[prefab-swap] StringInfoVtable cascade FAILED "
                               "-- StringInfo entry filter degraded.");
            }

            const auto loaderReg = resolve_address(k_loaderRegistryCandidates, "LoaderRegistry");
            if (loaderReg)
            {
                s_loaderRegistrySingleton.store(loaderReg, std::memory_order_release);
                logger.debug("[prefab-swap] LoaderRegistry resolved at "
                             "0x{:X}",
                             loaderReg);
            }
            else
            {
                logger.warning("[prefab-swap] LoaderRegistry cascade FAILED "
                               "-- AppearanceTableLoader enumeration disabled.");
            }

            // Resolve the part-prefab data container vtable by its RTTI name first: the class name is patch-stable
            // while the vtable address and the ctor's lea layout move between builds. Reverse RTTI therefore
            // self-heals where the byte scan below drifts. The lea-walk fallback runs only when reverse RTTI is
            // unavailable.
            if (const auto rttiVt =
                    DetourModKit::Rtti::vtable_for_type(
                        ".?AV?$ThreadSafeRefCountedContainerBase@VstaticstringA@pa@@VAppearanceTableData@2@UDefaultUserData@2@@pa@@")
                        .value_or(0))
            {
                s_apptContainerVtable.store(rttiVt, std::memory_order_release);
                logger.debug("[prefab-swap] ApptContainerVtable via RTTI: 0x{:X}", rttiVt);
            }
            // ApptContainerVtable: the cascade locates the AppearanceTableLoader ctor. Walk forward inside its first
            // 0x400 bytes for the SECOND `48 8D 05 ?? ?? ?? ?? 48 89 07` pair. The FIRST pair assigns the
            // intermediate `_appearanceContainer` vtable. The SECOND assigns the `_partPrefabDataContainer` vtable we
            // want. The scan is bounded. On a miss the container-vtable filter is skipped, which is safe: the check
            // is read-only, so its loss removes a sentinel, not a correctness guarantee.
            const auto ctorAbs = resolve_address(k_apptLoaderCtorCandidates, "ApptLoaderCtor");
            if (ctorAbs && s_apptContainerVtable.load(std::memory_order_acquire) == 0)
            {
                const auto *body = reinterpret_cast<const std::uint8_t *>(ctorAbs);
                constexpr std::size_t k_scanLen = 0x400;
                std::uintptr_t resolved = 0;
                std::size_t leaPairs = 0;
                for (std::size_t i = 0; i + 10 <= k_scanLen; ++i)
                {
                    if (body[i] == 0x48 && body[i + 1] == 0x8D && body[i + 2] == 0x05 && body[i + 7] == 0x48 &&
                        body[i + 8] == 0x89 && body[i + 9] == 0x07)
                    {
                        ++leaPairs;
                        if (leaPairs == 2)
                        {
                            std::int32_t disp = 0;
                            std::memcpy(&disp, body + i + 3, sizeof(disp));
                            const std::uintptr_t instrEnd = ctorAbs + i + 7;
                            resolved = instrEnd + static_cast<std::intptr_t>(disp);
                            break;
                        }
                    }
                }
                if (resolved)
                {
                    s_apptContainerVtable.store(resolved, std::memory_order_release);
                    logger.debug("[prefab-swap] ApptContainerVtable resolved "
                                 "at 0x{:X} (ctor 0x{:X}, lea#2)",
                                 resolved, ctorAbs);
                }
                else
                {
                    logger.warning("[prefab-swap] ApptContainerVtable: ctor "
                                   "0x{:X} contained <2 `lea+mov [rdi]` pairs "
                                   "in 0x{:X} bytes -- vtable filter disabled.",
                                   ctorAbs, k_scanLen);
                }
            }
            else if (s_apptContainerVtable.load(std::memory_order_acquire) == 0)
            {
                logger.warning("[prefab-swap] ApptContainerVtable cascade "
                               "FAILED -- container vtable filter disabled.");
            }
        }

        const auto addr =
            resolve_address(k_structCopyCandidates, std::size(k_structCopyCandidates), "PrefabWrapperSwap_StructCopy");
        if (!addr)
        {
            logger.warning("[prefab-swap] AOB scan failed -- feature disabled");
            return false;
        }
        if (!DMK::Scanner::is_likely_function_prologue(addr))
        {
            logger.warning("[prefab-swap] resolved 0x{:X} but prologue check "
                           "failed -- feature disabled",
                           addr);
            return false;
        }

        StructCopyFn trampoline = nullptr;
        auto &hookMgr = DMK::HookManager::get_instance();
        auto result =
            hookMgr.create_inline_hook("PrefabWrapperSwap_StructCopy", addr, reinterpret_cast<void *>(on_struct_copy),
                                       reinterpret_cast<void **>(&trampoline));
        if (!result.has_value())
        {
            logger.warning("[prefab-swap] hook install failed: {}",
                           DetourModKit::Hook::error_to_string(result.error()));
            return false;
        }

        s_orig = trampoline;
        s_active.store(false, std::memory_order_release);

        // Hook gates on Transmog::in_transmog() so real-item flow is untouched -- semantic invariant, not session
        // state.
        logger.info("[prefab-swap] installed at 0x{:X} (INACTIVE -- "
                    "press the toggle hotkey to resolve pairs and activate).",
                    addr);

        // Natural-pipeline unlink hook. Substitutes src wrappers with target wrappers in the input list before the
        // engine walks parent+88 looking for matches. Resolved through the k_naturalPipelineCandidates cascade (3
        // anchors, see aob_resolver.hpp). On cascade failure the helm/cloak leak persists, but the rest of the mod
        // still loads.
        {
            const auto natpipeAbs = resolve_address(k_naturalPipelineCandidates, std::size(k_naturalPipelineCandidates),
                                                    "PrefabWrapperSwap_NaturalPipeline");
            if (!natpipeAbs)
            {
                logger.warning("[prefab-swap] NaturalPipeline AOB resolve "
                               "FAILED -- helm/cloak leak will persist. Other "
                               "swap features remain active.");
            }
            else if (!DMK::Scanner::is_likely_function_prologue(natpipeAbs))
            {
                // Prologue sanity gate (DMK::Scanner::is_likely_function_prologue). Same contract as the StructCopy
                // install -- guards against a cascade that picked up a fragment of an unrelated function after a future
                // patch reshuffles bytes.
                logger.warning("[prefab-swap] NaturalPipeline resolved at "
                               "0x{:X} but prologue check failed -- skipping "
                               "install. Helm/cloak leak will persist.",
                               natpipeAbs);
            }
            else
            {
                // The target is the pre-unlink wrapper-list walker. The hook substitutes src -> tgt at entry.
                logger.debug("[prefab-swap] NaturalPipeline resolved at 0x{:X}", natpipeAbs);
                NaturalPipelineFn natpipeTrampoline = nullptr;
                auto natpipeResult = hookMgr.create_inline_hook("PrefabWrapperSwap_NaturalPipeline", natpipeAbs,
                                                                reinterpret_cast<void *>(on_natural_pipeline),
                                                                reinterpret_cast<void **>(&natpipeTrampoline));
                if (!natpipeResult.has_value())
                {
                    logger.warning("[prefab-swap] NaturalPipeline hook "
                                   "install FAILED: {} -- helm/cloak leak will "
                                   "persist.",
                                   DetourModKit::Hook::error_to_string(natpipeResult.error()));
                }
                else
                {
                    s_origNaturalPipeline = natpipeTrampoline;
                    // The detour is a no-op when LT swap is OFF. While active, it substitutes src wrappers with target
                    // wrappers in the engine's natural unlink list.
                }
            }
        }

        // --- AppearanceTableLoader hooks ---
        //
        // Resolve the lookup primitives FIRST so by the time the capture hook fires, lookups are immediately callable.
        // AOB failure is non-fatal -- lookup_prefab_metadata returns 0 and the picker falls back to StringInfo-only
        // behavior.
        //
        // The PRIMARY lookup is a self-contained name->wrapper primitive that operates on
        // loaderRegistrySingleton+0x50. Resolved through the k_apptNameLookupCandidates cascade (3 anchors, see
        // aob_resolver.hpp). On cascade failure lookup_prefab_metadata returns 0 and the picker falls back to
        // StringInfo-only behavior.
        {
            const auto fnAbs = resolve_address(k_apptNameLookupCandidates, std::size(k_apptNameLookupCandidates),
                                               "PrefabWrapperSwap_ApptNameLookup");
            if (!fnAbs)
            {
                logger.warning("[prefab-swap] ApptNameLookup AOB resolve "
                               "FAILED -- lookup_prefab_metadata will return 0; "
                               "picker falls back to StringInfo-only behavior.");
            }
            else if (!DMK::Scanner::is_likely_function_prologue(fnAbs))
            {
                logger.warning("[prefab-swap] ApptNameLookup resolved at 0x{:X} "
                               "but prologue check failed -- skipping. "
                               "lookup_prefab_metadata will return 0.",
                               fnAbs);
            }
            else
            {
                s_apptNameLookup = reinterpret_cast<ApptNameLookupFn>(fnAbs);
                // The target is the name->wrapper lookup against loaderRegistrySingleton+0x50.
                logger.debug("[prefab-swap] ApptNameLookup resolved at 0x{:X}", fnAbs);
            }
        }

        const auto stringInternAddr =
            resolve_address(k_apptStringInternCandidates, std::size(k_apptStringInternCandidates),
                            "PrefabWrapperSwap_ApptStringIntern");
        if (stringInternAddr)
        {
            s_apptStringIntern = reinterpret_cast<ApptStringInternFn>(stringInternAddr);
            // The target is the StringInfo intern primitive for AppearanceTableLoader lookups.
            logger.debug("[prefab-swap] ApptStringIntern resolved at 0x{:X}", stringInternAddr);
        }
        else
        {
            logger.warning("[prefab-swap] ApptStringIntern AOB scan failed "
                           "-- AppearanceTableLoader lookups disabled.");
        }

        const auto apptLookupAddr = resolve_address(k_apptInnerLookupCandidates, std::size(k_apptInnerLookupCandidates),
                                                    "PrefabWrapperSwap_ApptInnerLookup");
        if (apptLookupAddr)
        {
            s_apptLookup = reinterpret_cast<ApptLookupFn>(apptLookupAddr);
            // The target is the PartPrefab container lookup. It is pure read-only and returns 24B metadata, or 0 on a
            // miss.
            logger.debug("[prefab-swap] ApptLookup resolved at 0x{:X}", apptLookupAddr);
        }
        else
        {
            logger.warning("[prefab-swap] ApptLookup AOB scan failed "
                           "-- AppearanceTableLoader lookups disabled.");
        }

        // Capture hook on the ResMgr init entry. We run the trampoline first, then read the populated chain. The
        // capture is a one-shot. Subsequent calls pass through.
        const auto apptInitAddr = resolve_address(k_apptResMgrInitCandidates, std::size(k_apptResMgrInitCandidates),
                                                  "PrefabWrapperSwap_ApptResMgrInit");
        if (apptInitAddr)
        {
            ApptResMgrInitFn apptInitTrampoline = nullptr;
            auto apptResult = hookMgr.create_inline_hook("PrefabWrapperSwap_ApptResMgrInit", apptInitAddr,
                                                         reinterpret_cast<void *>(on_appt_resmgr_init),
                                                         reinterpret_cast<void **>(&apptInitTrampoline));
            if (!apptResult.has_value())
            {
                logger.warning("[prefab-swap] ApptResMgrInit hook install "
                               "FAILED: {} -- AppearanceTableLoader capture "
                               "disabled; picker will only see StringInfo-resident "
                               "prefabs.",
                               DetourModKit::Hook::error_to_string(apptResult.error()));
            }
            else
            {
                s_apptResMgrInitOrig = apptInitTrampoline;
                // On the first fire the entry hook snapshots ResMgr/loader/container. Every subsequent call passes
                // through (one-shot capture).
            }
        }
        else
        {
            logger.warning("[prefab-swap] ApptResMgrInit AOB scan failed "
                           "-- AppearanceTableLoader capture disabled; picker "
                           "will only see StringInfo-resident prefabs.");
        }

        // Boot-time auto-scan: kick off a detached thread that waits for the world to be ready, then walks StringInfo
        // to populate the per-slot catalog. That also triggers the heap-walk merge for parallel-pool wrappers, and it
        // attempts the per-character source seed derived from each carrier's runtime meshes. The catalog is the single
        // source of truth for the picker UI and for the apply-time swap-map rebuild.
        std::thread(
            []()
            {
                auto &log = DMK::Logger::get_instance();
                // Wait for world ready -- poll forever (detached thread, a few KB of sleeping stack, and the OS reaps
                // it on process exit). No cap is needed: the save-load auto-refresh in the worker handles
                // steady-state catalog rotation, and the user never has to re-trigger this walk by hand.
                while (!Transmog::is_world_ready())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                log.info("[prefab-swap] boot-scan: world ready, populating "
                         "per-slot catalog...");
                populate_slot_catalogs();

                // Re-sync the active preset's body-mesh selections now that the catalog is populated. Presets loaded
                // before the heap walk finished have unresolved prefabName values, and this retroactive apply lands
                // them. Then trigger the apply pipeline so the swap activates. This mirrors the load-time auto-apply
                // path the item-name table uses when its deferred catalog scan completes.
                Transmog::PresetManager::instance().apply_to_state();
                Transmog::manual_apply();
                log.info("[prefab-swap] boot-scan: preset prefabs "
                         "re-synced and apply scheduled.");
            })
            .detach();
        return true;
    }

    void shutdown()
    {
        s_active.store(false, std::memory_order_release);
        s_orig = nullptr;
        {
            std::scoped_lock lk(s_substLogMtx);
            s_substLog.clear();
        }
        {
            std::scoped_lock lk(s_catalogMtx);
            for (auto &v : s_slotCatalogs)
                v.clear();
            s_selSrcIdx.fill(-1);
            s_selTgtIdx.fill(-1);
        }
        s_catalogPopulated.store(false, std::memory_order_release);
        std::scoped_lock lk(s_mapMtx);
        for (auto &m : s_swapMapPerChar)
            m.clear();
        for (auto &s : s_targetWrappersPerChar)
            s.clear();
        s_callCount.store(0, std::memory_order_relaxed);
        s_substCount.store(0, std::memory_order_relaxed);

        // Reset AppearanceTableLoader capture state. Do NOT null the lookup function pointers -- they are
        // trampoline-resolved addresses and HookManager owns the trampoline lifetime. The next init() re-resolves
        // them.
        s_apptContainer.store(0, std::memory_order_relaxed);
        s_apptCaptureDone.store(false, std::memory_order_release);
        s_apptResMgrInitOrig = nullptr;
        s_apptStringIntern = nullptr;
        s_apptLookup = nullptr;
        s_apptNameLookup = nullptr;
    }

    void notify_apply_starting(const std::uint16_t (&itemIds)[5])
    {
        // Apply-only activation lifecycle. Mirrors the carrier hybrid pattern: picker mutations only update
        // s_selSrcIdx/s_selTgtIdx (pending state). The actual swap-map rebuild and activation happen here, at the
        // start of each apply pass. If the user cleared all selections, this deactivates cleanly.
        if (!s_orig)
            return; // hook not installed -- nothing to do

        // Decide between "this apply has fakes to install" and "this is a cleanup-only pass" based on `itemIds`, NOT on
        // has_any_selection().
        //
        // Why: has_any_selection() reads the picker's s_selSrcIdx/s_selTgtIdx state, which only tracks the most recent
        // dropdown choice. It is decoupled from the user's Enabled toggle and from the per-slot mapping.active flags.
        // A cleanup-only pass (Enabled off, or every slot unticked -- both arrive here with itemIds = {0, 0, 0, 0, 0})
        // therefore still reports a selection and re-arms the swap map.
        //
        // A re-arm during cleanup is the failure mode that leaks the original fake mesh on toggle-off. The engine's
        // tear_down_by_item_id calls fire the natural-pipeline unlink with the *real* wrapper of the just-unequipped
        // item. With the swap map armed, the natpipe hook rewrites that to a target wrapper that is not present in
        // parent+88, the engine's unlink misses, and the real-item mesh stays painted on the actor.
        bool any_active_fake = false;
        for (auto id : itemIds)
        {
            if (id != 0)
            {
                any_active_fake = true;
                break;
            }
        }
        if (any_active_fake)
        {
            // At least one fake will be installed in this pass, so run the regular rebuild + activate cycle.
            reactivate_with_selections();
        }
        else
        {
            // Cleanup-only pass. Force the swap map off so the following tear_down calls run with the natpipe hook in
            // passthrough. Engine teardown then operates on the real wrappers in parent+88 directly, with no spurious
            // src -> tgt substitution. The swap map and the target-wrapper set are preserved, so a later real apply
            // can re-arm without another heap walk.
            if (s_active.load(std::memory_order_acquire))
                deactivate_for_clear();
        }

        // Record itemIds so notify_apply_finished can stash them for diagnostics (preset-switch detection is no longer
        // needed -- every apply rebuilds the swap map fresh from selections).
        std::scoped_lock lk(s_lastApplyMtx);
        std::memcpy(s_lastApplyItems, itemIds, sizeof(s_lastApplyItems));
        s_lastApplyValid = true;
    }

    void notify_apply_finished(const std::uint16_t (&itemIds)[5])
    {
        if (!s_active.load(std::memory_order_acquire))
            return;
        std::scoped_lock lk(s_lastApplyMtx);
        std::memcpy(s_lastApplyItems, itemIds, sizeof(s_lastApplyItems));
        s_lastApplyValid = true;
    }

    void deactivate_for_clear()
    {
        if (!s_active.load(std::memory_order_acquire))
            return;
        s_active.store(false, std::memory_order_release);

        // Flush dye-injector counters before the natpipe hook tears down the swap. The injector itself is stateless
        // across teardown (per-slot state is thread-local and consumed once per slotpop), but the counter dump helps
        // post-mortem when diagnosing missing dye records.
        Transmog::DyeRecordInject::restore_all();

        auto &logger = DMK::Logger::get_instance();

        // Reverse-write every record we substituted: restore its ORIGINAL source wrapper and release the refcount bump
        // the install did on the target. This is the cleanup the SubstRecord ledger exists for (see its doc-block).
        // The natural-pipeline hook only unlinks a target during ACTIVE re-assembly of that slot on the active
        // character, so a DROPPED swap (preset -> none) or a CROSS-CHARACTER teardown never routes through it. The
        // target wrapper then orphans in the scene graph and leaks its refcount, which climbs with every repeated
        // apply. A drain of the ledger here detaches those orphans.
        //
        // Per-record safety, which is why this is immune to the cross-talk a broader unlink hook causes: each
        // SubstRecord carries its OWN destAddr and origWrapper, so the restore is always correct even when characters
        // share a carrier/source. It is self-validating. A record reverts ONLY when its slot STILL holds one of our
        // target wrappers, so a freed / reused / re-substituted record fails that test and is skipped. Every raw
        // access is SEH-guarded.
        std::vector<SubstRecord> drainedRecords;
        {
            std::scoped_lock lk(s_substLogMtx);
            drainedRecords.swap(s_substLog);
        }
        std::unordered_set<std::uintptr_t> ourTargets;
        {
            std::scoped_lock lk(s_mapMtx);
            for (const auto &s : s_targetWrappersPerChar)
                ourTargets.insert(s.begin(), s.end());
        }
        std::size_t reverted = 0;
        for (const auto &r : drainedRecords)
            if (revert_one_subst(r.destAddr, r.origWrapper, ourTargets))
                ++reverted;

        // Swap map and target-wrapper sets are PRESERVED for instant re-activation. Only the per-install substitution
        // ledger (drained above) is consumed. A re-arm substitutes fresh records via on_struct_copy.
        logger.info("[prefab-swap] DEACTIVATED -- reverted {} substitution(s); swap map RETAINED for next activation.",
                    reverted);
    }

} // namespace Transmog::PrefabWrapperSwap
