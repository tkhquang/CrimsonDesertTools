#include "prefab_wrapper_swap.hpp"
#include "aob_resolver.hpp"
#include "carrier_defaults.hpp"
#include "dye_record_inject.hpp"
#include "item_name_table.hpp"
#include "itemmesh_dumper.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>
#include <DetourModKit/region.hpp>

#include <Windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Transmog::prefab_wrapper_swap
{
    namespace
    {
        // Boot-scan worker. Owns the thread that waits for a world before populating the catalog. ~StoppableWorker
        // requests stop and joins, and its counted module reference keeps the worker visible in DMK's pin ledger
        // while it lives - which is what a raw detached thread never was.
        std::optional<DMK::StoppableWorker> s_boot_scan_worker;
    } // namespace

    // Constants
    //
    // StringInfo registry (pa::StringInfoManager, a pa::StaticInfoManager2<> subclass):
    //   +0x08 count u32, +0x58 array_ptr (QWORD entry-ptrs)
    //
    // The array_ptr offset moves whenever the pa::StaticInfoManager2 base changes width. The count at +0x08 and the
    // per-entry layout below sit ahead of the growth point, so they stay put. A stale array_ptr offset fails SILENTLY:
    // the neighboring offset also dereferences to a valid heap pointer (a filename string blob), so the plausibility
    // guard below does not trip and the walk emits garbage instead of a bail. Verify this offset against live memory
    // on patch day. Do NOT blanket-apply a base-width change to every registry. Sibling registries move their
    // container fields independently, and one can shrink while this one grows.
    // Per-entry layout:
    //   +0x00 hash, +0x08 vtable (the StringInfo sentinel resolved into s_string_info_vtable),
    //   +0x18 wrapper-ptr, +0x20 inline name (NUL-terminated)
    //
    // Wrapper layout:
    //   +0x00 string ptr, +0x10 refcount i32,
    //   +0x14 mode flag, +0x15 tombstone

    // Layout / sanity constants (compile-time only, not patchable).
    /// u32 entry count.
    constexpr std::size_t STRING_INFO_COUNT_OFF = 0x08;
    /// Qword base of the entry-ptr array.
    constexpr std::size_t STRING_INFO_ARRAY_OFF = 0x58;
    constexpr std::size_t INLINE_NAME_OFF = 0x20;
    constexpr std::size_t WRAPPER_PTR_OFF = 0x18;
    constexpr std::size_t EXT_NAME_MAX = 256;
    // Cap on a loader-resolved prefab name. Used by both name-resolution sites in this file.
    constexpr std::size_t LOADER_NAME_CAP = 96;
    constexpr std::uint32_t MIN_PLAUSIBLE_COUNT = 100;
    constexpr std::uint32_t MAX_PLAUSIBLE_COUNT = 200000;

    // Runtime-resolved data globals
    //
    // Resolved by AOB cascades at init() (see aob_resolver.hpp::string_info_registry() et al). Atomic because
    // init runs on the main thread while the hot path (walk_string_info, enumerate_loader_registry_into_catalog) can
    // run on the background population thread. Zero means "not resolved yet". Every consumer must verify a non-zero
    // value before it dereferences.
    static std::atomic<std::uintptr_t> s_string_info_registry{0};
    static std::atomic<std::uintptr_t> s_string_info_vtable{0};
    static std::atomic<std::uintptr_t> s_loader_registry_singleton{0};

    // The cascades consumed below are defined in aob_resolver.hpp: struct_copy() (the wrapper struct-copy hot
    // path), natural_pipeline(), part_list_merge() and unlink_by_wrapper(). Each is ordered most-specific-first per
    // the AOB rules in CrimsonDesertCore/external/DetourModKit/docs/misc/aob-signatures.md.

    // Module is "active" when at least one slot has a resolved swap pair installed in any s_swap_map_per_char bucket.
    // Selection is overlay-driven. There are no INI keys for this feature.
    static std::atomic<bool> s_active{false};

    // True once any swap pair is bound in any bucket. Cleared only on world-reload/reset. Lets the natpipe UNLINK hook
    // keep cleaning up installed targets even when the CURRENTLY-active character has no swap (s_active==false) - e.g.
    // switching away from a swapped Damiane to a plain Oongka must still unlink Damiane's target. The empty-list fast
    // path keeps the hot cost negligible for the (common) zero-length unlink calls.
    static std::atomic<bool> s_maps_retained{false};

    // Per-character resolved wrapper address maps. Each character's apply rebuilds ONLY its own bucket so a Damiane
    // apply does not wipe Kliff's substitutions while Kliff's body still has tgt wrappers physically attached. Without
    // per-char buckets, a chained Kliff-then-Damiane sequence leaves the next Kliff teardown with an empty map. The
    // natpipe hook then finds no substitution, the engine searches for the original src wrapper that the body no
    // longer holds, and the unlink fails silently (helm-stuck bug). The hook dispatches by s_active_char_idx so
    // cross-character src collisions (Kliff+Oongka sharing cd_phm_00_ub_00_0054) still resolve to the correct target
    // for the body currently under assembly.
    //   bucket = s_active_char_idx - 1 ([0]=Kliff, [1]=Damiane, [2]=Oongka)
    static std::mutex s_map_mtx;

    /// Seed the engine mixes into its prefab-name hash. Part of the hash's identity, not a tunable.
    static constexpr std::uint32_t PREFAB_HASH_SEED = 0xC5EDEu;
    /// lookup3 consumes 12 bytes per mixing round and folds whatever is left in the tail switch.
    static constexpr std::size_t LOOKUP3_BLOCK_BYTES = 12;

    /**
     * @brief Jenkins lookup3 `hashlittle`, seeded with @ref PREFAB_HASH_SEED.
     *
     * @details MEASURED: a prefab wrapper stores exactly this value at `+0x0C`, over its FULL suffixed name (`_l`,
     *          `_r`, `_in`, `_index01_r` included) and without a trailing NUL. Confirmed on 18 live wrappers across
     *          two independent paths - attached-record identities (`rec+0x40`) and the wrappers handed to the
     *          struct-copy chokepoint. It is also the same hash the game archive uses for an item's prefab list, so
     *          the two namespaces are one.
     *
     *          The swap map keys on this instead of on the wrapper pointer, which removes instance discovery:
     *          every instance of a name carries the same hash, so it no longer matters which one the engine passes.
     *          The catalog and the engine draw from different pools, so pointer keying can hold a correctly-named
     *          binding that never matches.
     * @warning The body below is the reference lookup3 mixing schedule transcribed literally. Do not "tidy" the
     *          rotations or reorder the tail switch: the value has to match the engine's byte for byte.
     */
    static constexpr std::uint32_t prefab_name_hash(std::string_view name) noexcept
    {
        const auto rot = [](std::uint32_t x, int k) constexpr { return (x << k) | (x >> (32 - k)); };

        std::uint32_t a = 0xdeadbeefu + static_cast<std::uint32_t>(name.size()) + PREFAB_HASH_SEED;
        std::uint32_t b = a;
        std::uint32_t c = a;

        std::size_t i = 0;
        std::size_t len = name.size();
        const auto u8 = [&](std::size_t idx) constexpr
        { return static_cast<std::uint32_t>(static_cast<unsigned char>(name[idx])); };
        const auto word = [&](std::size_t idx) constexpr
        { return u8(idx) | (u8(idx + 1) << 8) | (u8(idx + 2) << 16) | (u8(idx + 3) << 24); };

        // clang-format off
        while (len > LOOKUP3_BLOCK_BYTES)
        {
            a += word(i);
            b += word(i + 4);
            c += word(i + 8);
            a -= c; a ^= rot(c, 4);  c += b;
            b -= a; b ^= rot(a, 6);  a += c;
            c -= b; c ^= rot(b, 8);  b += a;
            a -= c; a ^= rot(c, 16); c += b;
            b -= a; b ^= rot(a, 19); a += c;
            c -= b; c ^= rot(b, 4);  b += a;
            i += LOOKUP3_BLOCK_BYTES;
            len -= LOOKUP3_BLOCK_BYTES;
        }
        // Tail: fold the remaining 1..12 bytes, then finalize. A zero-length name returns the seeded value.
        switch (len)
        {
        case 12: c += u8(i + 11) << 24; [[fallthrough]];
        case 11: c += u8(i + 10) << 16; [[fallthrough]];
        case 10: c += u8(i + 9) << 8;   [[fallthrough]];
        case 9:  c += u8(i + 8);        [[fallthrough]];
        case 8:  b += u8(i + 7) << 24;  [[fallthrough]];
        case 7:  b += u8(i + 6) << 16;  [[fallthrough]];
        case 6:  b += u8(i + 5) << 8;   [[fallthrough]];
        case 5:  b += u8(i + 4);        [[fallthrough]];
        case 4:  a += u8(i + 3) << 24;  [[fallthrough]];
        case 3:  a += u8(i + 2) << 16;  [[fallthrough]];
        case 2:  a += u8(i + 1) << 8;   [[fallthrough]];
        case 1:  a += u8(i);            break;
        default: return c; // len == 0
        }
        c ^= b; c -= rot(b, 14);
        a ^= c; a -= rot(c, 11);
        b ^= a; b -= rot(a, 25);
        c ^= b; c -= rot(b, 16);
        a ^= c; a -= rot(c, 4);
        b ^= a; b -= rot(a, 14);
        c ^= b; c -= rot(b, 24);
        // clang-format on
        return c;
    }

    /// The name hash a live wrapper carries, or 0 when it cannot be read.
    static std::uint32_t wrapper_name_hash(std::uintptr_t wrapper) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
            return 0;
        return DMK::memory::read<std::uint32_t>(DMK::Address{wrapper + 0x0C}).value_or(0);
    }

    // One swap binding. `src_name` is kept so a hit can be confirmed against the wrapper's own name - a 32-bit hash
    // over the whole prefab corpus is not collision-proof, and a wrong substitution renders the wrong mesh.
    struct SwapEntry
    {
        std::uintptr_t tgt_wrapper{0};
        std::string src_name;
    };
    static std::unordered_map<std::uint32_t, SwapEntry> s_swap_map_per_char[3];

    // Body-pointer (the natpipe hook's `a1`) -> character bucket. Learned in the natpipe hook whenever the ACTIVE
    // bucket owns a wrapper for that body (i.e. during the body's own assembly). Consulted when the engine later
    // unlinks that body while a DIFFERENT character is active, so we resolve the OUTGOING body's own bucket and unlink
    // its swap targets - instead of missing on the active bucket and orphaning them (the fake-mask-persists bug).
    // A key on the body, not on s_active_char_idx, is cross-talk-free even when characters share a source wrapper.
    // Only protagonist bodies with an active swap are ever recorded, so the map stays tiny.
    static std::mutex s_body_map_mtx;
    static std::unordered_map<std::uintptr_t, int> s_body_to_char;

    using StructCopyFn = std::int64_t(__fastcall *)(std::int64_t, std::int64_t);
    static StructCopyFn s_orig = nullptr;

    // Per-character target wrapper sets (parallel to s_swap_map_per_char). Used by the secondary-bind hook to detect
    // "is this record one of our substituted ones?" It compares the entry's wrapper-ptr at +0 against the union of all
    // three buckets.
    static std::unordered_set<std::uintptr_t> s_target_wrappers_per_char[3];

    // Validity stamp for the derived per-slot target table below: the world generation and character it was built
    // for. Zero means never built.
    //
    // The table is DERIVED state (preset -> selections -> targets), and the recurring failure was a read of it
    // while it still described a previous world: a reload dressed the new body from the last session's uncommitted
    // picks. A reset at each save-load site does not hold - there are three such branches, a pinned character takes
    // a different one than an unpinned character, and a future patch can add another.
    //
    // A stamp instead makes staleness impossible to READ. Any path that bumps the world generation is covered,
    // paths not yet found included, and a table built for one protagonist can never be served to another.
    static std::mutex s_target_table_stamp_mtx;
    static std::uint64_t s_target_table_world_gen = 0;
    static std::uint32_t s_target_table_char_idx = 0;

    // Target wrapper per slot, per character. s_swap_map_per_char is keyed by SOURCE name hash, so it names what a
    // mesh becomes, not what a SOCKET wears - and the socket is what the mesh-override hook knows. It rebuilds
    // alongside the swap map from the same plans, so the two cannot disagree.
    static std::uintptr_t s_slot_target_wrapper_per_char[3][Transmog::SLOT_COUNT]{};

    // Direct fakes: slots where the equipped item IS the target, so no substitution happens and nothing lands in
    // s_target_wrappers_per_char. Kept in their OWN set because apply_selections_to_swap_map rebuilds the target set
    // from swap plans alone, which wipes these on the next apply - including the clearing apply, which is exactly when
    // the sweep needs them in order to recognize them as orphans.
    static std::unordered_set<std::uintptr_t> s_direct_fakes_per_char[3];

    // Destination tracking: every record we substituted, with its original Kliff wrapper. On deactivate, we walk this
    // vector and write the original wrapper back into the dest slot. This reverses our wrapper-substitution at the
    // engine-state level, so LT's tear_down (which walks the auth-table) finds records with ORIGINAL wrappers (the ones
    // it knows about) and can tear them down cleanly. Without this, our substitutions create scene-graph entries LT
    // cannot reach via its auth-table-driven tear-down, leading to stale renders (the helm leak being most visible).
    struct SubstRecord
    {
        /// Dest record's wrapper-ptr slot, which is `a1 + 0`.
        std::uintptr_t dest_addr{0};
        /// Source wrapper that sat at `*a2` before the substitution.
        std::uintptr_t orig_wrapper{0};
    };
    static std::mutex s_subst_log_mtx;
    static std::vector<SubstRecord> s_subst_log;
    static constexpr std::size_t MAX_SUBST_LOG = 256;

    // Natural-pipeline unlink. Called by safe_tear_down and other unmount paths with a list of asset wrappers to unlink
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
    // Resolved via natural_pipeline() in aob_resolver.hpp.
    using NaturalPipelineFn = std::int64_t(__fastcall *)(std::int64_t a1, std::uint64_t *a2, std::uint64_t *a3);
    static NaturalPipelineFn s_orig_natural_pipeline = nullptr;
    static std::atomic<std::uint64_t> s_natpipe_hit_count{0};
    static std::atomic<std::uint64_t> s_natpipe_subst_count{0};
    static std::atomic<std::uint64_t> s_natpipe_list_entries{0};

    // Auto-deactivate-on-preset-switch state. Once swap activates and the user applies a body-mesh preset, we record
    // those itemIds. The next apply with DIFFERENT itemIds is treated as a switch-away and triggers
    // deactivate_for_clear before its substitutions can re-bind target wrappers to the new gear.
    static std::mutex s_last_apply_mtx;
    static std::uint16_t s_last_apply_items[5] = {
        0,
        0,
        0,
        0,
        0,
    };
    static bool s_last_apply_valid = false;

    // Wrapper +0x40 slot inside the scene-graph struct.
    static constexpr std::size_t SCENE_GRAPH_WRAPPER_OFF = 0x40;
    // Slot-id u32 lives at struct+0x48 (the factory writes *a3 there).
    static constexpr std::size_t SCENE_GRAPH_SLOT_ID_OFF = 0x48;
    // Helm slot ID - the only slot that needs a scene-graph reverse-write, because helm is the only pair with a
    // suffix mismatch (`_d` -> `_c`) that routes through a separate scene-graph branch. The engine's tear-down cannot
    // reach that branch through runtime-resource-pointer equality. Other pairs preserve their suffix and unlink
    // naturally on the next apply, so a revert of their +0x40 only confuses rendering (visible as chest/cloak clipping
    // on preset-switch).
    static constexpr std::uint32_t HELM_SLOT_ID = 0xAA9A;

    static std::atomic<std::uint64_t> s_call_count{0};
    static std::atomic<std::uint64_t> s_subst_count{0};

    // Substitution write-target guard
    //
    // The record-copy chokepoint is called as `StructCopy(dest, src)` where `src` is a record the caller built on
    // its OWN STACK - verified in the player-loadout site, which does `lea rdx,[rsp+20]` immediately before the call.
    // The substitution therefore writes to a stack temporary that the engine is about to copy into a staging vector.
    // It never reaches a live container, the equip authority table, or anything that serializes into a save.
    //
    // That invariant is the entire safety argument for this feature, so it is checked rather than assumed. A `src` that
    // is NOT on the calling thread's stack means the assumption no longer holds on that path - possibly a new call
    // site introduced by a patch - and the write is refused. A lost substitution is a cosmetic regression. A write
    // into a persistent structure is not, and the two are not worth trading.
    //
    // Range comes from the current thread's TIB (StackBase at gs:[0x08], StackLimit at gs:[0x10]) so it is exact for
    // whichever engine thread happens to be assembling, with no assumptions about which thread that is.
    [[nodiscard]] static bool is_on_current_thread_stack(std::uintptr_t p) noexcept
    {
        const auto stack_base = static_cast<std::uintptr_t>(__readgsqword(0x08));
        const auto stack_limit = static_cast<std::uintptr_t>(__readgsqword(0x10));
        if (stack_limit == 0 || stack_base <= stack_limit)
            return false; // unreadable TIB - fail closed
        return p >= stack_limit && p < stack_base;
    }

    static std::atomic<std::uint64_t> s_guard_rejects{0};

    // Per-apply census of this chokepoint. Several of the gate's exits are deliberately silent in normal play: a
    // non-protagonist assembly is refused without a word, and a wrapper that is not in the swap map is the
    // overwhelmingly common case, far too frequent to log. That silence leaves one question unanswerable from the
    // log alone - when a slot renders untransmogged, did the engine never emit the part through here at all, or did
    // it emit and the gate refuse? These counters, plus the list of wrapper hashes actually seen while the window is
    // open, separate the two.
    //
    // The window is armed by notify_apply_starting and stays armed until the NEXT one, because the substitutions
    // that matter arrive on the engine's async rebuild after notify_apply_finished has already returned. So the
    // steady-state cost is paid on every call, not only during an apply, and everything on that path is bounded
    // deliberately: the counters are relaxed atomics, and both list recorders test their cap BEFORE hashing or
    // locking, so once a list is full the detour is back to plain increments.
    static std::atomic<bool> s_census_armed{false};
    // Detour entries, counted BEFORE any filter.
    static std::atomic<std::uint32_t> s_census_raw{0};
    static std::atomic<std::uint32_t> s_census_calls{0};
    static std::atomic<std::uint32_t> s_census_scope_reject{0};
    static std::atomic<std::uint32_t> s_census_hash_fail{0};
    static std::atomic<std::uint32_t> s_census_miss{0};
    static std::atomic<std::uint32_t> s_census_hit{0};
    static std::mutex s_census_mtx;
    // Cap on the emitted-wrapper list. A single body emits far fewer than this many distinct parts, so the cap is
    // only ever reached by a wrong actor scope, which is itself the answer the census is looking for.
    static constexpr std::size_t CENSUS_SEEN_CAP = 64;
    // Cap on the scope-rejected list. Sized for a crowd of NPCs assembling around the player.
    static constexpr std::size_t CENSUS_REJ_CAP = 128;
    // Deduped and capped at CENSUS_SEEN_CAP.
    static std::vector<std::uint32_t> s_census_seen;
    // Map keys that actually substituted in the window. Bounded by the swap map, which holds one key per bound slot.
    static std::vector<std::uint32_t> s_census_hit_keys;
    // Wrapper hashes that reached the detour but were scope-rejected. Deduped and capped at CENSUS_REJ_CAP.
    static std::vector<std::uint32_t> s_census_rej_seen;
    // Lock-free mirrors of "the matching list is full". Read on the detour path so a saturated list costs neither a
    // name hash nor a mutex acquisition. They are written only under s_census_mtx, alongside the push that fills
    // the list.
    static std::atomic<bool> s_census_seen_full{false};
    static std::atomic<bool> s_census_rej_full{false};
    // 1..3: the character this window is applying, 0 when unknown.
    static std::atomic<std::uint32_t> s_census_bucket{0};

    /**
     * @brief Print one census line: what the struct-copy chokepoint saw since the window was armed.
     *
     * `mapKeys` is what substitution is waiting for, `seenHashes` is what the engine actually emitted for a
     * protagonist. Non-protagonist assemblies never reach the seen list - they are refused upstream and counted in
     * scope_reject - so the list stays short and on-topic. The two together answer the only question that matters
     * when a slot renders untransmogged: an empty seen list means the engine never emitted the part at all, while a
     * seen list that misses every map key means it emitted something the map was not built for.
     */
    static void log_census(std::string_view phase)
    {
        // Name every bound slot whose substitution did NOT happen in this window. A slot that fails to substitute
        // renders the real gear, and that failure is otherwise invisible in the log - the swap is silent about a
        // wrapper it never saw, which is exactly how a visual-only regression stays hidden until someone looks at
        // the character. Reported as the source PREFAB name, so the line points straight at the slot.
        //
        // Restricted to the bucket this window actually applied. The other characters' buckets stay bound while they
        // idle offscreen, and the engine emits nothing for a body it does not assemble, so a report on them marks
        // every idle companion as failed on every apply - noise that buries the one line that means something.
        //
        // Loaded outside the lock: it is an atomic, and the report branches below need it after the lock is
        // released.
        const auto bucket = s_census_bucket.load(std::memory_order_relaxed);
        std::string keys;
        std::string seen;
        std::string missed;
        {
            // ONE scoped_lock over both, never two nested ones. The detour holds s_map_mtx and takes s_census_mtx
            // inside it to record a hit. An acquisition here in the opposite order inverts the lock order.
            // The two-mutex form locks them as a unit and cannot deadlock against that nesting.
            std::scoped_lock lk(s_map_mtx, s_census_mtx);
            for (std::size_t ci = 0; ci < 3; ++ci)
            {
                for (const auto &kv : s_swap_map_per_char[ci])
                {
                    if (bucket != ci + 1 || std::find(s_census_hit_keys.begin(), s_census_hit_keys.end(), kv.first) !=
                                                s_census_hit_keys.end())
                        continue;
                    // Flag the ones that DID reach the chokepoint and were refused for scope. That is the signature
                    // of an install arriving outside the apply window rather than not arriving at all.
                    const bool out_of_scope = std::find(s_census_rej_seen.begin(), s_census_rej_seen.end(), kv.first) !=
                                              s_census_rej_seen.end();
                    missed += std::format(
                        "{}c{}:{}{}",
                        missed.empty() ? "" : " ",
                        ci + 1,
                        kv.second.src_name,
                        out_of_scope ? "(OUT-OF-SCOPE)" : ""
                    );
                }
            }
            // The key and hash dumps are only worth their size - and their formatting cost - when something
            // failed. On a clean pass the counters alone say everything, and printing every bound key plus every
            // wrapper the body emitted, twice per apply, drowns the log.
            if (!missed.empty())
            {
                for (std::size_t ci = 0; ci < 3; ++ci)
                    for (const auto &kv : s_swap_map_per_char[ci])
                        keys += std::format("{}c{}:0x{:08X}", keys.empty() ? "" : " ", ci + 1, kv.first);
                for (const auto h : s_census_seen)
                    seen += std::format("{}0x{:08X}", seen.empty() ? "" : " ", h);
            }
        }
        const auto raw = s_census_raw.load(std::memory_order_relaxed);
        const auto calls = s_census_calls.load(std::memory_order_relaxed);
        const auto scope_reject = s_census_scope_reject.load(std::memory_order_relaxed);
        const auto hash_fail = s_census_hash_fail.load(std::memory_order_relaxed);
        const auto miss = s_census_miss.load(std::memory_order_relaxed);
        const auto hit = s_census_hit.load(std::memory_order_relaxed);
        // Debug, not warning, on every branch. The "apply" line is emitted before the engine's async rebuild has
        // run, so an incomplete substitution list is the NORMAL state there rather than a fault, and a warning fires
        // on every healthy apply. The "tail" line is the one worth reading, and even it lists slots the engine had no
        // reason to rebuild this pass.
        auto &logger = DMK::log();
        if (bucket < 1 || bucket > 3)
        {
            // No bound character means no bucket was checked. Say so rather than falling through to the clean line:
            // a diagnostic that reports success without having looked is worse than one that says nothing.
            logger.debug(
                "[prefab-swap] census({}): raw={} calls={} scopeReject={} hashFail={} miss={} hit={} | "
                "bucket=unknown, substitution not checked",
                phase,
                raw,
                calls,
                scope_reject,
                hash_fail,
                miss,
                hit
            );
            return;
        }
        if (missed.empty())
        {
            logger.debug(
                "[prefab-swap] census({}): raw={} calls={} scopeReject={} hashFail={} miss={} hit={} | all "
                "bound slots substituted",
                phase,
                raw,
                calls,
                scope_reject,
                hash_fail,
                miss,
                hit
            );
            return;
        }
        logger.debug(
            "[prefab-swap] census({}): raw={} calls={} scopeReject={} hashFail={} miss={} hit={} | "
            "NOT-SUBSTITUTED=[{}] | mapKeys=[{}] seenHashes=[{}]",
            phase,
            raw,
            calls,
            scope_reject,
            hash_fail,
            miss,
            hit,
            missed,
            keys,
            seen
        );
    }

    // SEH-isolated C-string read. DMK has no bounded NUL-terminated read verb, so this one keeps its own guard.

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
        const auto vtable_sentinel = s_string_info_vtable.load(std::memory_order_acquire);
        if (wrapper == vtable_sentinel || !DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
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
        const auto vtable_sentinel = s_string_info_vtable.load(std::memory_order_acquire);
        if (wrapper == vtable_sentinel || !DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
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

    /**
     * @brief Reverse-write one substituted record when its slot still holds one of our target wrappers.
     *
     * @details Releases the install-time refcount bump, then restores the original source wrapper. A slot that was
     *          freed, reused or re-substituted fails the membership test and is skipped.
     * @return True when the record reverted.
     */
    static bool revert_one_subst(
        std::uintptr_t dest_addr,
        std::uintptr_t orig_wrapper,
        const std::unordered_set<std::uintptr_t> &our_targets
    ) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{dest_addr}))
            return false;
        const auto cur = DMK::memory::read<std::uintptr_t>(DMK::Address{dest_addr});
        if (!cur || !DMK::memory::is_plausible_ptr(DMK::Address{*cur}) || our_targets.find(*cur) == our_targets.end())
            return false;
        decrement_wrapper_refcount(*cur); // balance the install-time bump
        return DMK::memory::write_in_place<std::uintptr_t>(DMK::Address{dest_addr}, orig_wrapper).has_value();
    }

    // AppearanceTableLoader public API

    void for_each_loader_prefab_name(const std::function<void(std::string_view)> &cb) noexcept
    {
        // Mirror of `enumerate_loader_registry_into_catalog` minus the slot-tag filter and pending-merge bookkeeping.
        // Walks the table struct (singleton+0x50) entry-by-entry, reads the inline key name from each wrapper, emits
        // via callback. Skips the StringInfo-vtable sentinel rows that hold metadata-only (non-name-bearing) entries.
        if (!cb)
            return;
        const auto singleton_abs = s_loader_registry_singleton.load(std::memory_order_acquire);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{singleton_abs}))
            return;
        const auto singleton_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{singleton_abs}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{singleton_ptr}))
            return;
        // singleton + 0x50 = table struct (matches internal LOADER_REGISTRY_TABLE_OFF defined later in this TU).
        const std::uintptr_t table_struct = singleton_ptr + 0x50;
        const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{table_struct + 0x04}).value_or(0);
        const auto data_array_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{table_struct + 0x18}).value_or(0);
        if (count == 0 || count > 100000 || !DMK::memory::is_plausible_ptr(DMK::Address{data_array_ptr}))
            return;

        std::vector<std::uintptr_t> entry_ptrs;
        entry_ptrs.resize(count);
        const bool bulk_ok =
            DMK::memory::read_into(
                DMK::Address{data_array_ptr},
                std::span{reinterpret_cast<std::byte *>(entry_ptrs.data()), count * sizeof(std::uintptr_t)}
            )
                .has_value();

        const auto vtable_sentinel = s_string_info_vtable.load(std::memory_order_acquire);
        char nameBuf[EXT_NAME_MAX + 1] = {0};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entry =
                bulk_ok ? entry_ptrs[i]
                        : DMK::memory::read<std::uint64_t>(DMK::Address{data_array_ptr + 8ULL * i}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{entry}))
                continue;
            const auto wrapper = DMK::memory::read<std::uint64_t>(DMK::Address{entry + 0x08}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
                continue;
            if (wrapper == vtable_sentinel)
                continue;
            const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(wrapper + 0x18), nameBuf, LOADER_NAME_CAP);
            if (rlen == SIZE_MAX || rlen == 0)
                continue;
            cb(std::string_view(nameBuf, rlen));
        }
    }

    // Per-slot catalog state
    //
    // Built lazily on first overlay open via populate_slot_catalogs(). Each catalog holds the prefabs whose inline name
    // starts with the slot-specific sub-prefix (e.g. "cd_phm_00_hel_00_"). Selection indices index into the catalog
    // vector. -1 means "unset".
    //
    // Catalog mutex is separate from s_map_mtx so the UI thread can refresh without serializing with the active hot
    // path. The hot path never reads catalogs. It reads s_swap_map_per_char, which each apply rebuilds from the catalog
    // selections of one character.
    static std::mutex s_catalog_mtx;
    static std::array<std::vector<PrefabEntry>, static_cast<std::size_t>(Transmog::TransmogSlot::Count)>
        s_slot_catalogs;

    // Per-slot picker selection state. Sentinel -1 means "no selection". Any non-negative value indexes into the
    // slot's catalog. The arrays are sized to TransmogSlot::Count, so a fixed-length brace-init list drifts whenever a
    // new slot is added. A helper returns an array filled with -1 instead. The count then follows the enum size
    // automatically.
    static constexpr auto INITIAL_SELECTION_INDICES = []()
    {
        constexpr auto N = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        std::array<int, N> a{};
        for (std::size_t i = 0; i < N; ++i)
            a[i] = -1;
        return a;
    }();
    static std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)> s_sel_src_idx =
        INITIAL_SELECTION_INDICES;
    static std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)> s_sel_tgt_idx =
        INITIAL_SELECTION_INDICES;
    static std::atomic<bool> s_catalog_populated{false};

    // Per-character buffered copies of the selection arrays. UI writes through set_selection mirror into
    // s_sel_src_idx_per_char[active-1] / s_sel_tgt_idx_per_char[active-1]. The globals above stay as the "active
    // editing view" the UI reads back. `apply_selections_to_swap_map` rebuilds the active character's bucket
    // (s_swap_map_per_char[active-1]) from that character's row each apply. It leaves the other characters' buckets
    // intact, so a substitution picked on Damiane stays live in the engine's wrapper-substitution path even while the
    // user edits another character. Without these rows the picker writes a single global, which the next dropdown
    // switch overwrites. That silently drops the outgoing character's variant.
    //
    // s_active_char_idx is 0 until PresetManager::apply_to_state binds a character via set_active_char_idx(). While idx
    // is 0, set_selection writes only to the globals (boot-time defaults).
    static std::atomic<std::uint32_t> s_active_char_idx{0};
    static std::array<std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)>, 3>
        s_sel_src_idx_per_char = {INITIAL_SELECTION_INDICES, INITIAL_SELECTION_INDICES, INITIAL_SELECTION_INDICES};
    static std::array<std::array<int, static_cast<std::size_t>(Transmog::TransmogSlot::Count)>, 3>
        s_sel_tgt_idx_per_char = {INITIAL_SELECTION_INDICES, INITIAL_SELECTION_INDICES, INITIAL_SELECTION_INDICES};

    // Shared StringInfo walker (bulk-copy fast path)
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
    static std::uint32_t walk_string_info(std::string_view prefix, EntryVisitor visitor) noexcept
    {
        auto &logger = DMK::log();
        const std::size_t prefix_len = prefix.size();

        const auto reg_abs = s_string_info_registry.load(std::memory_order_acquire);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{reg_abs}))
        {
            logger.warning(
                "[prefab-swap] walk_string_info: registry not resolved (s_string_info_registry=0). Returning 0 "
                "entries; the picker dropdown will be empty until init() succeeds."
            );
            return 0;
        }
        const auto reg_addr = reinterpret_cast<const void *>(reg_abs);
        const auto registry_ptr =
            DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(reg_addr)}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{registry_ptr}))
            return 0;
        const auto count =
            DMK::memory::read<std::uint32_t>(DMK::Address{registry_ptr + STRING_INFO_COUNT_OFF}).value_or(0);
        const auto array_ptr =
            DMK::memory::read<std::uint64_t>(DMK::Address{registry_ptr + STRING_INFO_ARRAY_OFF}).value_or(0);
        if (count < MIN_PLAUSIBLE_COUNT || count > MAX_PLAUSIBLE_COUNT ||
            !DMK::memory::is_plausible_ptr(DMK::Address{array_ptr}))
            return 0;

        // Bulk-copy entry-pointer array so the inner loop reads from process memory without a per-element SEH frame.
        // Falls back to per-entry SEH reads if the bulk copy faults.
        std::vector<std::uintptr_t> entry_ptrs;
        const std::size_t array_bytes = static_cast<std::size_t>(count) * sizeof(std::uintptr_t);
        bool bulk_ok = false;
        if (array_bytes > 0)
        {
            entry_ptrs.resize(count);
            bulk_ok = DMK::memory::read_into(
                          DMK::Address{array_ptr},
                          std::span{reinterpret_cast<std::byte *>(entry_ptrs.data()), array_bytes}
            )
                          .has_value();
        }

        const auto walk_start = std::chrono::steady_clock::now();
        std::uint32_t scanned = 0;
        std::uint32_t vt_matched = 0;
        std::uint32_t pref_matched = 0;

        // Same 128B header copy: covers vtable @ +8, wrapper-ptr @ +0x18, and the inline-name region @ +0x20.
        constexpr std::size_t header_bytes = 0x80;
        alignas(8) std::uint8_t header[header_bytes];

        // Path-B fallback decode for long names whose +0x20 holds an external string ptr instead of an inline
        // NUL-terminated name.
        auto decode_long_name = [](std::uintptr_t entry, char *buf, std::size_t cap) -> bool
        {
            // entry -> +0x18 wrapper -> +0x00 string pointer.
            static constexpr std::ptrdiff_t name_chain[] = {
                static_cast<std::ptrdiff_t>(WRAPPER_PTR_OFF),
                0,
                0,
            };
            const auto str_ptr = DMK::memory::walk(DMK::Address{entry}, std::span<const std::ptrdiff_t>{name_chain});
            if (!str_ptr)
                return false;
            const auto ext_len = read_cstr_seh(reinterpret_cast<const void *>(str_ptr->raw()), buf, cap);
            return ext_len != SIZE_MAX && ext_len > 0;
        };

        // Snapshot the resolved sentinel ONCE per walk - this avoids an atomic load per entry across the whole
        // registry walk.
        const auto vtable_sentinel = s_string_info_vtable.load(std::memory_order_acquire);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{vtable_sentinel}))
        {
            logger.warning("[prefab-swap] walk_string_info: StringInfo vtable sentinel not resolved - aborting walk");
            return 0;
        }

        char buf[EXT_NAME_MAX + 1] = {0};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entry_ptr =
                bulk_ok ? entry_ptrs[i]
                        : DMK::memory::read<std::uint64_t>(DMK::Address{array_ptr + 8ULL * i}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{entry_ptr}))
                continue;
            ++scanned;

            if (!DMK::memory::read_into(
                     DMK::Address{entry_ptr},
                     std::span{reinterpret_cast<std::byte *>(header), header_bytes}
                )
                     .has_value())
                continue;

            const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t *>(header + 8);
            if (vtable != vtable_sentinel)
                continue;
            ++vt_matched;

            // Prefix gate (only for inline-name entries - skipped for long-name entries that route through Path B
            // below).
            const unsigned char first = header[INLINE_NAME_OFF];
            const bool printable_lead = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                                        (first >= '0' && first <= '9') || first == '_' || first == '/' || first == '.';
            if (prefix_len > 0 && printable_lead &&
                std::memcmp(header + INLINE_NAME_OFF, prefix.data(), prefix_len) != 0)
                continue;
            ++pref_matched;

            // Decode name from local header (Path A) or fall back to wrapper-chain (Path B) for long external strings.
            buf[0] = 0;
            bool decoded = false;
            {
                const char *src = reinterpret_cast<const char *>(header + INLINE_NAME_OFF);
                const std::size_t max_len = header_bytes - INLINE_NAME_OFF;
                std::size_t L = 0;
                while (L < max_len && src[L] != 0)
                    ++L;
                if (L > 0 && L < max_len)
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
                if (!decode_long_name(entry_ptr, buf, EXT_NAME_MAX))
                    continue;
                // Re-apply prefix filter to long-name entries that dodged the inline-prefix gate above.
                if (prefix_len > 0 && std::strncmp(buf, prefix.data(), prefix_len) != 0)
                    continue;
            }

            const std::uintptr_t entry_wrapper = *reinterpret_cast<const std::uintptr_t *>(header + WRAPPER_PTR_OFF);
            const std::uint32_t entry_hash = *reinterpret_cast<const std::uint32_t *>(header);

            visitor(entry_ptr, buf, entry_wrapper, entry_hash);
        }

        const auto walk_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walk_start)
                .count();
        logger.debug(
            "[prefab-swap] StringInfo walk: count={} scanned={} vtable-pass={} prefix-pass={} prefix=\"{}\" ({}ms)",
            count,
            scanned,
            vt_matched,
            pref_matched,
            prefix.empty() ? std::string_view{"(none)"} : prefix,
            walk_ms
        );

        return scanned;
    }

    // Heap-walk for partprefabdyeslot-style wrappers, which live in a parallel allocation pool that StringInfo does
    // not index. Used as the Pass-2 fallback when StringInfo does not carry an entry for a configured name. It is its
    // own helper so populate_slot_catalogs and apply_selections_to_swap_map both reuse it.
    //
    // For each name in `src_names`, it appends matching wrapper addresses to `out_src_by_idx` (ALL matches per name).
    // For each name in `tgt_names`, it writes the FIRST matching wrapper to `out_tgt_by_idx` (single-substitution
    // target). The caller passes parallel vectors keyed by index.
    static void heap_walk_partprefab_for_names(
        const std::vector<std::string> &src_names,
        const std::vector<std::string> &tgt_names,
        std::vector<std::vector<std::uintptr_t>> &out_src_by_idx,
        std::vector<std::uintptr_t> &out_tgt_by_idx
    ) noexcept
    {
        if (src_names.size() != out_src_by_idx.size() || tgt_names.size() != out_tgt_by_idx.size())
            return;
        const auto process_range = DMK::Region::whole_process();
        const std::uintptr_t addrEnd = process_range.end().raw();
        std::uintptr_t addr = process_range.base.raw();
        while (addr < addrEnd)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
                break;
            const auto region_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto region_size = mbi.RegionSize;
            const bool committed = (mbi.State == MEM_COMMIT);
            const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;
            const bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
            if (committed && writable && !guarded && region_size >= 0x40 && region_size < 0x40000000ULL)
            {
                const auto end = region_base + region_size - 0x40;
                for (std::uintptr_t p = region_base; p < end; p += 8)
                {
                    const auto v = DMK::memory::read<std::uint64_t>(DMK::Address{p}).value_or(0);
                    if (v != p + 0x18)
                        continue;
                    const auto len = DMK::memory::read<std::uint32_t>(DMK::Address{p + 8}).value_or(0);
                    if (len == 0 || len >= EXT_NAME_MAX)
                        continue;
                    char nameBuf[EXT_NAME_MAX + 1] = {0};
                    const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(p + 0x18), nameBuf, EXT_NAME_MAX);
                    if (rlen == SIZE_MAX || rlen != len)
                        continue;

                    for (std::size_t k = 0; k < src_names.size(); ++k)
                    {
                        if (!src_names[k].empty() && std::strcmp(nameBuf, src_names[k].c_str()) == 0)
                            out_src_by_idx[k].push_back(p);
                    }
                    for (std::size_t k = 0; k < tgt_names.size(); ++k)
                    {
                        if (out_tgt_by_idx[k] == 0 && !tgt_names[k].empty() &&
                            std::strcmp(nameBuf, tgt_names[k].c_str()) == 0)
                            out_tgt_by_idx[k] = p;
                    }
                }
            }
            addr = region_base + region_size;
            if (region_size == 0)
                break;
        }
    }

    /**
     * @brief True when `stem` is a RENDER VARIANT of `src_stem` - the same item, drawn differently.
     *
     * A helm does not render under its base prefab name. The engine picks an `_indexNN` variant at assembly time
     * (hair/head state drives the choice), so a registration of the base name alone leaves the wrapper that reaches
     * the chokepoint unmatched and the swap silently does nothing. Boots, gloves, chest and cloak do render under
     * their base name, which is why helm was the only slot that failed.
     *
     * The match must stay narrow. A bare prefix test also captures `hel_0122_01_index01_dd`, which is a DIFFERENT
     * item (Lardein) that merely shares the `hel_0122` prefix - an inserted `_NN` before `_index` marks a separate
     * item, not a variant. So only these forms count:
     *   <stem>                  the base itself
     *   <stem>_indexNN          render variant, optionally with a trailing _c / _d / _dd
     *   <stem>_c / <stem>_d     hair-covered / uncovered pair
     */
    [[nodiscard]] static bool is_render_variant_of(const std::string &stem, const std::string &src_stem) noexcept
    {
        if (src_stem.empty())
            return false;
        if (stem == src_stem)
            return true;
        if (stem.size() <= src_stem.size() || stem.compare(0, src_stem.size(), src_stem) != 0)
            return false;

        auto rest = std::string_view{stem}.substr(src_stem.size());
        if (rest == "_c" || rest == "_d" || rest == "_dd")
            return true;
        if (rest.rfind("_index", 0) != 0)
            return false;
        rest.remove_prefix(6);
        // At least one digit, then an optional _c / _d / _dd tail. Anything else is a different item.
        std::size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
            ++digits;
        if (digits == 0)
            return false;
        rest.remove_prefix(digits);
        return rest.empty() || rest == "_c" || rest == "_d" || rest == "_dd";
    }

    /**
     * @brief The `_l` / `_r` tail of a prefab name, or empty when it has none.
     *
     * The suffix belongs to the SOCKET, not the item: one item's mesh is placed two ways, and the descriptor only
     * ever names one side. A binding built from the descriptor name alone therefore misses the half the engine
     * installs under the other suffix.
     */
    static std::string_view side_suffix_of(std::string_view name) noexcept
    {
        if (name.size() < 2)
            return {};
        const auto tail = name.substr(name.size() - 2);
        if (tail == "_l" || tail == "_r")
            return tail;
        return {};
    }

    /// `name` with its side suffix replaced by `side`. Empty when `name` carries no side suffix.
    static std::string with_side_suffix(std::string_view name, std::string_view side) noexcept
    {
        const auto cur = side_suffix_of(name);
        if (cur.empty() || side.empty())
            return {};
        std::string out{name.substr(0, name.size() - cur.size())};
        out.append(side);
        return out;
    }

    /**
     * @brief Body-rig-stripped stem of a mesh prefab name, or "" when the name is not a body-rig prefab.
     *
     * @details A body-mesh prefab is shaped `cd_<rig>_<NN>_<stem>`, where `<rig>` is a body-rig token (phm=male,
     *          phw=female, pom=orc, pgm/pfw and the rest, all of which start with 'p') and `<NN>` is the rig-index
     *          digits. The `<stem>` (e.g. "mask_00_0271_a") identifies the item's mesh independent of which body
     *          renders it: the SAME logical item is `cd_phm_00_mask_00_0271_a` on Kliff, `cd_phw_01_mask_00_0271_a`
     *          on Damiane and `cd_pom_01_mask_00_0271_a` on Oongka. The rig-index also differs, 00 against 01. A
     *          name with no rig shape (e.g. `cd_t0000_lantern_0003`) returns "" and the caller falls back to an
     *          exact-name match.
     *
     *          Every rig sibling of a carrier registers as a swap source, so the source set covers whichever rig the
     *          wearer's REAL body emits. That tolerates a body-swap mod whose rig differs from the character's
     *          name-derived default.
     */
    static std::string rig_stripped_stem(const std::string &name) noexcept
    {
        constexpr std::string_view k_pfx = "cd_";
        if (name.size() < k_pfx.size() || name.compare(0, k_pfx.size(), k_pfx) != 0)
            return {};
        const std::size_t rig_start = k_pfx.size();
        if (name[rig_start] != 'p') // body-rig tokens all start with 'p' (phm/phw/pom/pgm/pfw/...)
            return {};
        const std::size_t rig_end = name.find('_', rig_start);
        if (rig_end == std::string::npos)
            return {};
        const std::size_t idx_start = rig_end + 1;
        const std::size_t idx_end = name.find('_', idx_start);
        if (idx_end == std::string::npos || idx_end == idx_start)
            return {};
        for (std::size_t k = idx_start; k < idx_end; ++k) // rig-index must be all digits
            if (name[k] < '0' || name[k] > '9')
                return {};
        return name.substr(idx_end + 1);
    }

    // Runtime source-seed for a slot's body-mesh swap. Derives the carrier item's rig mesh name from its item_id at
    // runtime (variant_meshes_for_item -> desc+0x3E0 variant list), in place of a hardcoded prefab column in
    // carrier_defaults. Returns the first rig-shaped mesh - which rig comes first is irrelevant, since the
    // rig-stripped stem is what the sibling registration keys on, and every rig of the item shares that stem. Empty
    // when the carrier item_name is unset/unresolved or the registries are not ready yet, in which case callers fall
    // back to the per-char selection seed.
    static std::string carrier_source_seed(Transmog::CarrierChar cc, Transmog::TransmogSlot slot) noexcept
    {
        const char *item_nm = Transmog::carrier_for(cc, slot).item_name;
        if (!item_nm || item_nm[0] == '\0')
            return {};
        const auto id_opt = Transmog::ItemNameTable::instance().id_of(item_nm);
        if (!id_opt)
            return {};
        // Prefer a body-rig mesh - its rig-stripped stem drives the sibling (per-body) registration. But fall back to
        // the FIRST mesh for non-body-rig carriers: a Lantern emits the cd_t0000_ prop family (e.g.
        // cd_t0000_lantern_0003), which has no rig prefix so its stem is empty, yet the mesh name is still a valid,
        // resolvable source (it registers as its own source, with no rig siblings). Without this fallback such carriers
        // resolve to no source at all.
        std::string first_mesh;
        for (const auto &mesh : Transmog::variant_meshes_for_item(*id_opt))
        {
            if (first_mesh.empty())
                first_mesh = mesh;
            if (!rig_stripped_stem(mesh).empty())
                return mesh;
        }
        return first_mesh;
    }

    // Seed the per-character default SOURCE selections (s_sel_src_idx_per_char / s_sel_src_idx) from each carrier's
    // runtime variant meshes. Idempotent and cheap once done. GATED on ItemNameTable readiness: carrier_source_seed
    // resolves the carrier item_name -> item_id -> variant list, and the name table finishes building on a deferred
    // worker AFTER populate_slot_catalogs runs. A seed inside populate therefore silently no-ops. That leaves the
    // picker with no default source, so has_any_selection() stays false and the swap can never activate. Instead this
    // runs from the activation gate and from the picker read, so it lands the moment the table is ready. It never
    // overwrites a slot already seeded or user-picked, and it runs at most one full pass.
    static void ensure_default_sources_seeded() noexcept
    {
        static std::atomic<bool> s_sources_seeded{false};
        if (s_sources_seeded.load(std::memory_order_acquire))
            return;
        if (!s_catalog_populated.load(std::memory_order_acquire) || !Transmog::ItemNameTable::instance().ready())
            return; // retry on a later call once BOTH the catalog and the name table exist

        constexpr std::size_t slot_n = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        auto &logger = DMK::log();
        const auto &active_char = Transmog::PresetManager::instance().active_character();
        const auto seed_char = Transmog::carrier_char_from_name(active_char).value_or(Transmog::CarrierChar::Kliff);
        std::size_t seeded_count = 0;
        {
            std::scoped_lock lk(s_catalog_mtx);
            if (s_sources_seeded.load(std::memory_order_relaxed))
                return; // another caller won the race and already seeded - avoid a duplicate pass + log line
            // Seed every protagonist's row from their OWN carrier so per-body prefab families (cd_phm vs cd_phw) do
            // not cross-talk. A later set_active_char_idx hydrates the globals from a meaningful row.
            for (std::size_t ci = 0; ci < Transmog::CARRIER_CHAR_COUNT; ++ci)
            {
                const auto cc = static_cast<Transmog::CarrierChar>(ci);
                for (std::size_t i = 0; i < slot_n; ++i)
                {
                    if (s_sel_src_idx_per_char[ci][i] >= 0)
                        continue; // already chosen (seeded earlier or user-picked)
                    const std::string src = carrier_source_seed(cc, static_cast<Transmog::TransmogSlot>(i));
                    if (src.empty())
                        continue; // no carrier item_name, or item/registries not resolvable - leave for a later pass
                    const auto &cat = s_slot_catalogs[i];
                    int found_idx = -1;
                    for (std::size_t k = 0; k < cat.size(); ++k)
                        if (cat[k].name == src)
                        {
                            found_idx = static_cast<int>(k);
                            break;
                        }
                    // Body-swap resilience: if the derived rig prefab is not resident, seed from any resident rig
                    // sibling sharing the same mesh stem so the picker still shows a meaningful default source.
                    if (found_idx < 0)
                    {
                        const std::string stem = rig_stripped_stem(src);
                        if (!stem.empty())
                            for (std::size_t k = 0; k < cat.size(); ++k)
                                if (rig_stripped_stem(cat[k].name) == stem)
                                {
                                    found_idx = static_cast<int>(k);
                                    logger.trace(
                                        "[prefab-swap] seed char[{}] slot[{}] carrier mesh \"{}\" not "
                                        "resident - seeding rig sibling \"{}\" (stem \"{}\")",
                                        ci,
                                        i,
                                        src,
                                        cat[k].name,
                                        stem
                                    );
                                    break;
                                }
                    }
                    if (found_idx >= 0)
                    {
                        s_sel_src_idx_per_char[ci][i] = found_idx;
                        ++seeded_count;
                    }
                }
            }
            // Mirror the active character's row into the globals the UI reads.
            const auto seed_bucket = static_cast<std::size_t>(seed_char);
            for (std::size_t i = 0; i < slot_n; ++i)
                if (s_sel_src_idx[i] < 0)
                    s_sel_src_idx[i] = s_sel_src_idx_per_char[seed_bucket][i];
            s_sources_seeded.store(true, std::memory_order_release);
        }
        logger.info(
            "[prefab-swap] seeded {} default source selection(s) from runtime carrier meshes across {} "
            "character(s), active='{}' (ItemNameTable ready)",
            seeded_count,
            Transmog::CARRIER_CHAR_COUNT,
            active_char
        );
    }

    // Per-slot catalog API

    bool is_catalog_populated() noexcept
    {
        return s_catalog_populated.load(std::memory_order_acquire);
    }

    const std::vector<PrefabEntry> &slot_catalog(Transmog::TransmogSlot slot) noexcept
    {
        // Static empty fallback so the reference return is always valid, even before the catalog is populated.
        static const std::vector<PrefabEntry> s_empty;
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_slot_catalogs.size())
            return s_empty;
        return s_slot_catalogs[idx];
    }

    int selection_src_index(Transmog::TransmogSlot slot) noexcept
    {
        ensure_default_sources_seeded(); // land source defaults so the picker shows them once the name table is ready
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_sel_src_idx.size())
            return -1;
        return s_sel_src_idx[idx];
    }

    int selection_tgt_index(Transmog::TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_sel_tgt_idx.size())
            return -1;
        return s_sel_tgt_idx[idx];
    }

    void set_selection(
        Transmog::TransmogSlot slot,
        int src_idx,
        int tgt_idx,
        std::string_view site,
        std::uint32_t char_idx_for
    ) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= s_sel_src_idx.size())
            return;
        // Clamp to catalog bounds. -1 is explicitly allowed for "unset".
        const auto cat_size = static_cast<int>(s_slot_catalogs[idx].size());
        if (src_idx < -1 || src_idx >= cat_size)
            src_idx = -1;
        if (tgt_idx < -1 || tgt_idx >= cat_size)
            tgt_idx = -1;
        s_sel_src_idx[idx] = src_idx;
        s_sel_tgt_idx[idx] = tgt_idx;
        // Mirror the write into the active character's per-char row so `apply_selections_to_swap_map` retains it when
        // the user switches the editing character. Idx 0 (no character bound yet) is a no-op - the globals carry the
        // boot-time defaults until PresetManager::apply_to_state runs and binds a row.
        // Prefer the bucket the CALLER named. A re-read of the bound character re-samples a global that another
        // thread mutates, so a per-slot restore loop can begin on one character's row and finish on another's -
        // which registers one character's prefab picks as another's targets.
        const auto char_idx = (char_idx_for != 0) ? char_idx_for : s_active_char_idx.load(std::memory_order_acquire);
        if (char_idx >= 1 && char_idx <= 3)
        {
            const auto bucket = static_cast<std::size_t>(char_idx - 1);
            s_sel_src_idx_per_char[bucket][idx] = src_idx;
            s_sel_tgt_idx_per_char[bucket][idx] = tgt_idx;

            // Record only REAL writes to a per-character target row, at TRACE, with the name the index resolves to
            // and the caller that asked for it. Those two together are what makes a cross-character write findable:
            // the row being poisoned, and who poisoned it. The CLEARED case is deliberately silent - a restore loop
            // clears every slot each time and buries the signal.
            if (tgt_idx >= 0 && tgt_idx < cat_size)
            {
                DMK::log().trace(
                    "[prefab-swap] sel-write bucket char[{}] slot[{}] tgt={} \"{}\" from={}",
                    bucket,
                    idx,
                    tgt_idx,
                    s_slot_catalogs[idx][tgt_idx].name,
                    site
                );
            }
        }
    }

    void set_active_char_idx(std::uint32_t idx) noexcept
    {
        if (idx > 3)
            idx = 0;
        const auto prev = s_active_char_idx.exchange(idx, std::memory_order_acq_rel);
        if (idx == prev || idx == 0)
            return;
        // Hydrate the globals (the "active editing view") from the newly-bound character's row so subsequent UI reads
        // (selection_src_index / selection_tgt_index) reflect that character's selections rather than the previous
        // one's.
        ensure_default_sources_seeded(); // seed the per-char source rows before hydrating the globals from one
        const auto bucket = static_cast<std::size_t>(idx - 1);
        s_sel_src_idx = s_sel_src_idx_per_char[bucket];
        s_sel_tgt_idx = s_sel_tgt_idx_per_char[bucket];
    }

    void reset_per_char_state() noexcept
    {
        // Clear ONLY the stale-after-arena-flip state:
        //   - s_active_char_idx so the next set_active_char_idx forces a fresh hydrate.
        //   - The active editing-view globals so the next apply_to_state's set_selection loop reads sane cur_src values
        //     after re-hydration.
        //   - s_swap_map_per_char and s_target_wrappers_per_char because their wrapper addresses point into
        //     the previous
        //     arena. If the natpipe hook fires against them, they either no-op (best case) or crash on dereference.
        //
        // The per-char `s_sel_src_idx_per_char` / `s_sel_tgt_idx_per_char` rows are CATALOG INDICES, not wrapper
        // pointers. Catalog re-population (populate_slot_catalogs) preserves entry names, so the indices stay
        // meaningful. If you wipe them here, you un-seed the per-character src defaults. The next post-save-load apply
        // then sees an empty src column, has_any_selection() returns false, and the swap map never reactivates. The
        // user reloads into "carrier visual only" instead of the picked prefab.
        s_active_char_idx.store(0, std::memory_order_release);
        s_sel_src_idx = INITIAL_SELECTION_INDICES;
        s_sel_tgt_idx = INITIAL_SELECTION_INDICES;
        std::scoped_lock lk(s_map_mtx);
        for (auto &m : s_swap_map_per_char)
            m.clear();
        for (auto &s : s_target_wrappers_per_char)
            s.clear();
        for (auto &s : s_direct_fakes_per_char)
            s.clear();
        {
            std::scoped_lock lk2(s_body_map_mtx);
            s_body_to_char.clear(); // body pointers point into the previous arena - drop the learned a1->bucket map
        }
        s_maps_retained.store(false, std::memory_order_release);
    }

    int adopt_into_slot_and_select(
        Transmog::TransmogSlot into_slot,
        Transmog::TransmogSlot from_slot,
        int from_idx
    ) noexcept
    {
        const auto into = static_cast<std::size_t>(into_slot);
        const auto from = static_cast<std::size_t>(from_slot);
        if (into >= s_slot_catalogs.size())
            return -1;
        if (from >= s_slot_catalogs.size())
            return -1;
        if (from_idx < 0)
            return -1;
        std::scoped_lock lk(s_catalog_mtx);
        if (static_cast<std::size_t>(from_idx) >= s_slot_catalogs[from].size())
            return -1;
        const auto entry = s_slot_catalogs[from][from_idx]; // copy
        // Dedup by name in into_slot's catalog.
        auto &dst = s_slot_catalogs[into];
        int existing = -1;
        for (std::size_t i = 0; i < dst.size(); ++i)
        {
            if (dst[i].name == entry.name)
            {
                existing = static_cast<int>(i);
                break;
            }
        }
        const int new_idx = (existing >= 0) ? existing : (dst.push_back(entry), static_cast<int>(dst.size() - 1));
        s_sel_tgt_idx[into] = new_idx;
        return new_idx;
    }

    bool has_any_selection() noexcept
    {
        ensure_default_sources_seeded(); // land the source defaults if the name table became ready since populate
        for (std::size_t i = 0; i < s_sel_src_idx.size(); ++i)
        {
            // Explicit pick: the user chose a prefab for this slot.
            if (s_sel_src_idx[i] >= 0 && s_sel_tgt_idx[i] >= 0)
                return true;

            // Derivable pick: the slot carries a target ITEM, whose prefab apply_selections_to_swap_map resolves.
            //
            // Without this the gate is unreachable for ordinary item transmog - an item-only slot has no target
            // INDEX, so the caller returns before the swap map is ever built and the derivation cannot run. The
            // source side is not required here: it is resolved later from the carrier at runtime, and a slot that
            // still fails to resolve is reported and skipped there.
            const auto &m = Transmog::slot_mappings()[i];
            if (m.active && m.target_item_id != 0)
                return true;
        }
        return false;
    }

    // Loader-registry enumeration (NPC body-mesh pickup)
    //
    // The StringInfo registry (s_string_info_registry) holds the prefab wrappers that are *currently resident* in the
    // player-character pipeline (typically the player's loaded set). Body-mesh prefabs for NPCs (cd_nh*) and
    // unloaded player variants live in a SECOND registry: the AppearanceTableLoader's own name table at
    // s_loader_registry_singleton + 0x50 (the singleton is dereferenced once at boot).
    //
    // Layout:
    //   table_struct = *(QWORD*)s_loader_registry_singleton + 0x50
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
    //                              hook substitutes - this is the
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
    // Slot classification needs to handle BOTH forms - we look for the bare slot tag (`_hel_`, `_ub_`, `_cloak_`,
    // `_hand_`, `_foot_`) rather than the `_<tag>_00_` form used by StringInfo entries.
    //
    // Wrappers from this registry merge into existing catalog entries by name (deduped + sorted within each
    // PrefabEntry's wrappers vector). Names not yet in the catalog are inserted as fresh entries with the registry
    // wrapper as the sole instance. The singleton itself resolves into s_loader_registry_singleton at init(). The +0x50
    // offset walks into the table struct: a stable game-ABI offset, kept literal.
    constexpr std::size_t LOADER_REGISTRY_TABLE_OFF = 0x50;

    static std::size_t enumerate_loader_registry_into_catalog() noexcept
    {
        auto &logger = DMK::log();
        const auto walk_start = std::chrono::steady_clock::now();
        constexpr std::size_t slot_n = static_cast<std::size_t>(Transmog::TransmogSlot::Count);

        // Slot tag substrings (BARE form - the registry uses no `_00_` markers). Each row is a null-terminated list
        // of substring patterns. An entry joins the slot if ANY pattern matches. The classifier loop OR's across
        // patterns AND across slots (a 1H sword name lands in MainHand AND OffHand AND SubWeapon's catalog, a ring
        // lands in both Ring1 and Ring2). Single-tag rows still work. Trailing nullptrs terminate the list.
        //
        // Gendered weapon slots accept BOTH `_phm_*` (male model) and `_phw_*` (female model) prefixes so Damiane's
        // `cd_phw_01_*` weapons land in the same catalogs Kliff's `cd_phm_01_*` counterparts do. Without this the
        // secondary-paired and female-character weapon catalogs end up empty and src seeding fails (which hides the
        // Prefabs picker checkbox). Ranged covers bows (_04_), pistols (_06_), and cannons (_13_) for the
        // cross-character ranged families captured in carrier_defaults.hpp.
        static constexpr std::size_t slot_tag_max = 8;
        static constexpr const char *slot_tag_patterns[slot_n][slot_tag_max] = {
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
            {nullptr},                                                                         // Tool (family unknown)
            {"_phm_01_", "_phw_01_", "_03_shield_", nullptr},                                  // OffHand2 (1H+shield)
            {"_phm_04_", "_phw_04_", "_phm_06_", "_phw_06_", "_phm_13_", "_phw_13_", nullptr}, // Ranged2
        };

        // Snapshot resolved sentinel (one atomic load per scan).
        const auto vtable_sentinel = s_string_info_vtable.load(std::memory_order_acquire);

        const auto singleton_abs = s_loader_registry_singleton.load(std::memory_order_acquire);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{singleton_abs}))
        {
            logger.warning("[prefab-swap] Loader registry singleton not resolved - skip enumeration");
            return 0;
        }
        const auto singleton_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{singleton_abs}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{singleton_ptr}))
        {
            logger.warning(
                "[prefab-swap] Loader registry singleton @0x{:X} unreadable - skip enumeration",
                singleton_abs
            );
            return 0;
        }
        const std::uintptr_t table_struct = singleton_ptr + LOADER_REGISTRY_TABLE_OFF;

        const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{table_struct + 0x04}).value_or(0);
        const auto data_array_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{table_struct + 0x18}).value_or(0);
        if (count == 0 || count > 100000 || !DMK::memory::is_plausible_ptr(DMK::Address{data_array_ptr}))
        {
            logger.warning(
                "[prefab-swap] Loader registry sanity failed: "
                "count={} dataArrayPtr=0x{:X} (table @0x{:X}) - skip enumeration",
                count,
                data_array_ptr,
                table_struct
            );
            return 0;
        }

        // One guarded frame for the whole array, then per-entry reads against process memory.
        std::vector<std::uintptr_t> entry_ptrs;
        const std::size_t array_bytes = static_cast<std::size_t>(count) * sizeof(std::uintptr_t);
        bool bulk_ok = false;
        if (array_bytes > 0)
        {
            entry_ptrs.resize(count);
            bulk_ok = DMK::memory::read_into(
                          DMK::Address{data_array_ptr},
                          std::span{reinterpret_cast<std::byte *>(entry_ptrs.data()), array_bytes}
            )
                          .has_value();
        }

        // The merge into s_slot_catalogs is deferred until after the walk, so the catalog mutex is free during it.
        struct Pending
        {
            std::string name;
            std::uintptr_t wrapper{0};
        };
        std::array<std::vector<Pending>, slot_n> pending;
        for (auto &v : pending)
            v.reserve(512);

        std::uint32_t scanned = 0;
        std::uint32_t prefix_match = 0;
        char nameBuf[EXT_NAME_MAX + 1] = {0};

        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uintptr_t entry =
                bulk_ok ? entry_ptrs[i]
                        : DMK::memory::read<std::uint64_t>(DMK::Address{data_array_ptr + 8ULL * i}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{entry}))
                continue;
            ++scanned;

            // Read KEY wrapper-ptr at entry+0x08. The KEY wrapper holds the inline prefab name in the standard
            // partprefabdyeslot format (+0x18 string buffer) - the same format the body-mesh hook substitutes by
            // pointer equality. The +0x10 VALUE wrapper is a metadata struct (counts/IDs), not a name-bearing wrapper,
            // so reading +0x18 there gives junk and the prefix gate filters everything out.
            const auto wrapper = DMK::memory::read<std::uint64_t>(DMK::Address{entry + 0x08}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
                continue;
            // Skip the StringInfo vtable sentinel (the registry can hold metadata-only entries that are not
            // partprefab wrappers).
            if (wrapper == vtable_sentinel)
                continue;

            // Read inline name at wrapper+0x18 (max 96 chars per spec but we use EXT_NAME_MAX==256 for the buffer cap).
            const auto rlen = read_cstr_seh(reinterpret_cast<const void *>(wrapper + 0x18), nameBuf, LOADER_NAME_CAP);
            if (rlen == SIZE_MAX || rlen == 0)
                continue;

            // Broad `cd_` gate admits every character-prefab family (player, NPC, all races). The slot-tag substring
            // loop below (`_ub_`, `_hel_`, `_cloak_`, etc.) is the real classifier: entries that match no slot tag are
            // silently dropped, so non-armor families (monsters, misc) never enter any per-slot catalog. The "Exact"
            // picker toggle remains the user-facing per-slot filter.
            if (!(nameBuf[0] == 'c' && nameBuf[1] == 'd' && nameBuf[2] == '_'))
                continue;
            ++prefix_match;

            // Slot classification by bare tag substring. NO break:
            // names matching multiple slots (e.g. `cd_phm_01_dagger_*` matches both MainHand's `_phm_01_` and
            // SubWeapon's `_phm_01_dagger_`) intentionally land in every matching catalog. This is what populates the
            // secondary paired slots (Ring2, OffHand) so their src can seed and the
            // Prefabs picker checkbox shows.
            for (std::size_t si = 0; si < slot_n; ++si)
            {
                bool matched = false;
                for (std::size_t pi = 0; pi < slot_tag_max; ++pi)
                {
                    const char *pat = slot_tag_patterns[si][pi];
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

        // A pending name already present appends its wrapper to the existing PrefabEntry::wrappers, sorted and
        // deduped. A name absent inserts a fresh PrefabEntry with the registry wrapper as the sole instance.
        std::array<std::size_t, slot_n> added_count{};
        std::array<std::size_t, slot_n> merged_count{};
        {
            std::scoped_lock lk(s_catalog_mtx);
            for (std::size_t si = 0; si < slot_n; ++si)
            {
                auto &cat = s_slot_catalogs[si];
                // Build an index for O(1) name->idx lookup. The catalogs are alphabetically sorted by name at this
                // point, so a lower_bound also works, but a hash map is simpler and the catalogs are small.
                std::unordered_map<std::string, std::size_t> idx_by_name;
                idx_by_name.reserve(cat.size() * 2);
                for (std::size_t ei = 0; ei < cat.size(); ++ei)
                    idx_by_name.emplace(cat[ei].name, ei);

                for (auto &p : pending[si])
                {
                    const auto it = idx_by_name.find(p.name);
                    if (it != idx_by_name.end())
                    {
                        // Merge: append + sort + unique.
                        auto &e = cat[it->second];
                        e.wrappers.push_back(p.wrapper);
                        std::sort(e.wrappers.begin(), e.wrappers.end());
                        e.wrappers.erase(std::unique(e.wrappers.begin(), e.wrappers.end()), e.wrappers.end());
                        ++merged_count[si];
                    }
                    else
                    {
                        // Insert fresh entry.
                        PrefabEntry e;
                        e.name = p.name;
                        e.wrappers = {p.wrapper};
                        e.hash = 0;
                        e.is_loaded = true; // wrapper present
                        idx_by_name.emplace(e.name, cat.size());
                        cat.push_back(std::move(e));
                        ++added_count[si];
                    }
                }

                // Re-sort the catalog alphabetically (insertions broke the invariant). Dedup by name as a defensive
                // measure - idx_by_name guards inserts, so it does not fire, but it is cheap relative to the sort.
                std::sort(
                    cat.begin(),
                    cat.end(),
                    [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; }
                );
                cat.erase(
                    std::unique(
                        cat.begin(),
                        cat.end(),
                        [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }
                    ),
                    cat.end()
                );
            }
        }

        const auto walk_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walk_start)
                .count();
        std::size_t total_added = 0;
        std::size_t total_merged = 0;
        for (std::size_t i = 0; i < slot_n; ++i)
        {
            total_added += added_count[i];
            total_merged += merged_count[i];
        }
        logger.debug(
            "[prefab-swap] Loader registry enumeration: walked={} entries, scanned={} body-mesh-prefix={} "
            "added {} new prefabs, merged {} into existing (helm={} chest={} cloak={} gloves={} boots={}) ({}ms)",
            count,
            scanned,
            prefix_match,
            total_added,
            total_merged,
            added_count[0],
            added_count[1],
            added_count[2],
            added_count[3],
            added_count[4],
            walk_ms
        );

        return total_added;
    }

    std::size_t populate_slot_catalogs() noexcept
    {
        auto &logger = DMK::log();
        const auto walk_start = std::chrono::steady_clock::now();

        // Build a single shared catalog from one StringInfo walk, then copy it into every slot's local vector.
        // Per-slot tag filtering is intentionally NOT applied here: accessory slots (Earring/Necklace) carry no useful
        // filter tag, and body-specific prefixes leave several female-side dropdowns empty (e.g. Damiane's actual ring
        // carriers are cd_phm_00_ring_*, not cd_phw_*). The full body-mesh family in every slot lets the user pick
        // anything. The search box already filters by name.
        constexpr std::size_t slot_n = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        std::array<std::vector<PrefabEntry>, slot_n> local;

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
        constexpr const char *broad_prefix = "cd_";

        std::vector<PrefabEntry> shared;
        walk_string_info(
            broad_prefix,
            [&](std::uintptr_t /*entry*/, const char *name, std::uintptr_t wrapper, std::uint32_t hash)
            {
                if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
                    return;
                // Seed with the StringInfo wrapper as the canonical first instance. The boot-time heap walk below
                // merges parallel-pool wrappers into the same vector. The metadata and is_loaded fields land below,
                // once the catalog is sorted.
                PrefabEntry e;
                e.name = std::string(name);
                e.wrappers = {wrapper};
                e.hash = hash;
                e.is_loaded = true; // wrapper present
                shared.push_back(std::move(e));
            }
        );

        // Sort + dedup once on the shared catalog, then copy to each slot. Sorting before copy means the slot vectors
        // are already sorted (the per-slot sort/dedup pass below becomes a no-op for them - left in place to handle
        // any future per-slot additions, e.g. enumerate_loader_registry_into_catalog).
        std::sort(
            shared.begin(),
            shared.end(),
            [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; }
        );
        shared.erase(
            std::unique(
                shared.begin(),
                shared.end(),
                [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }
            ),
            shared.end()
        );
        for (std::size_t i = 0; i < slot_n; ++i)
            local[i] = shared;

        // Sort each slot's entries alphabetically (UX) and dedup by name. StringInfo can carry parallel allocations of
        // the same name, and the dropdown must show one row per logical prefab.
        std::array<std::size_t, slot_n> counts{};
        for (std::size_t i = 0; i < slot_n; ++i)
        {
            auto &v = local[i];
            std::sort(v.begin(), v.end(), [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; });
            v.erase(
                std::unique(
                    v.begin(),
                    v.end(),
                    [](const PrefabEntry &a, const PrefabEntry &b) { return a.name == b.name; }
                ),
                v.end()
            );
            counts[i] = v.size();
        }

        {
            std::scoped_lock lk(s_catalog_mtx);
            for (std::size_t i = 0; i < slot_n; ++i)
                s_slot_catalogs[i] = std::move(local[i]);
            // Reset selections that point past the new catalog size. Without this, a refresh after the catalog shrinks
            // leaves stale indices that reference freed entries.
            for (std::size_t i = 0; i < slot_n; ++i)
            {
                const auto sz = static_cast<int>(s_slot_catalogs[i].size());
                if (s_sel_src_idx[i] >= sz)
                    s_sel_src_idx[i] = -1;
                if (s_sel_tgt_idx[i] >= sz)
                    s_sel_tgt_idx[i] = -1;
            }

            // Auto-seed of source selections runs below, AFTER enumerate_loader_registry_into_catalog(). That call
            // adds many entries and re-sorts each slot's vector, which invalidates any index seeded here.
        }

        // Heap-walk merge: cache parallel-pool wrappers per name
        //
        // The StringInfo walk above seeded each PrefabEntry with the entry+0x18 wrapper. The engine also sources
        // wrappers from a parallel partprefabdyeslot pool that StringInfo does NOT index. We walk ONCE here at boot
        // for ALL cataloged names. The dominant cost is the heap traversal itself, so the single-pass cost for N names
        // is close to the cost for 1.
        //
        // Pass empty tgt_names so only the src side runs. We want all wrappers per name, regardless of src/tgt
        // classification (the catalog is symmetric - any entry can take either role).
        const auto hw_start = std::chrono::steady_clock::now();
        std::vector<std::string> all_names;
        struct LocRef
        {
            std::size_t slot{};
            std::size_t entry_index{};
        };
        std::vector<LocRef> all_locs;
        {
            std::scoped_lock lk(s_catalog_mtx);
            std::size_t reserve = 0;
            for (std::size_t si = 0; si < slot_n; ++si)
                reserve += s_slot_catalogs[si].size();
            all_names.reserve(reserve);
            all_locs.reserve(reserve);
            for (std::size_t si = 0; si < slot_n; ++si)
            {
                for (std::size_t ei = 0; ei < s_slot_catalogs[si].size(); ++ei)
                {
                    all_names.push_back(s_slot_catalogs[si][ei].name);
                    all_locs.push_back({si, ei});
                }
            }
        }

        std::vector<std::vector<std::uintptr_t>> out_by_src(all_names.size());
        std::vector<std::uintptr_t> out_tgt(all_names.size(), 0);
        std::size_t total_wrappers = 0;
        if (!all_names.empty())
        {
            heap_walk_partprefab_for_names(all_names, /*tgt_names=*/{}, out_by_src, out_tgt);

            std::scoped_lock lk(s_catalog_mtx);
            for (std::size_t i = 0; i < all_locs.size(); ++i)
            {
                const auto si = all_locs[i].slot;
                const auto ei = all_locs[i].entry_index;
                if (si >= s_slot_catalogs.size() || ei >= s_slot_catalogs[si].size())
                    continue;
                auto &e = s_slot_catalogs[si][ei];
                for (auto w : out_by_src[i])
                    e.wrappers.push_back(w);
                std::sort(e.wrappers.begin(), e.wrappers.end());
                e.wrappers.erase(std::unique(e.wrappers.begin(), e.wrappers.end()), e.wrappers.end());
                total_wrappers += e.wrappers.size();
            }
        }
        const auto hw_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hw_start).count();

        // Loader-registry enumeration
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
            std::scoped_lock lk(s_catalog_mtx);
            std::unordered_map<std::string, PrefabEntry> union_by_name;
            union_by_name.reserve(20000);
            for (std::size_t si = 0; si < slot_n; ++si)
            {
                for (const auto &e : s_slot_catalogs[si])
                {
                    auto [it, inserted] = union_by_name.emplace(e.name, e);
                    if (!inserted)
                    {
                        // Same name already present - merge wrapper pointers so the union row carries every pool
                        // variant (parallel-pool wrappers vary across slot-specific enumerate adds).
                        auto &dst = it->second.wrappers;
                        for (auto w : e.wrappers)
                            dst.push_back(w);
                        std::sort(dst.begin(), dst.end());
                        dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
                    }
                }
            }
            std::vector<PrefabEntry> union_vec;
            union_vec.reserve(union_by_name.size());
            for (auto &kv : union_by_name)
                union_vec.push_back(std::move(kv.second));
            std::sort(
                union_vec.begin(),
                union_vec.end(),
                [](const PrefabEntry &a, const PrefabEntry &b) { return a.name < b.name; }
            );
            for (std::size_t si = 0; si < slot_n; ++si)
                s_slot_catalogs[si] = union_vec;
            // Selection indices were valid against the pre-union catalogs. Re-clamp so any picks survive the re-sort.
            // An earlier pass must set the selections by name (preset_manager does this). Session-only picks made
            // before populate_slot_catalogs runs are already clamped to the post-enumerate sort, so the extra re-sort
            // here can shift their indices. The runtime cost is one bounds clamp, which is acceptable.
            for (std::size_t i = 0; i < slot_n; ++i)
            {
                const auto sz = static_cast<int>(s_slot_catalogs[i].size());
                if (s_sel_src_idx[i] >= sz)
                    s_sel_src_idx[i] = -1;
                if (s_sel_tgt_idx[i] >= sz)
                    s_sel_tgt_idx[i] = -1;
            }
        }

        // Publish AFTER the heap-walk merge so apply paths waiting on the catalog see fully-resolved wrapper vectors
        // (no partial single-wrapper data leaking into the swap map).
        s_catalog_populated.store(true, std::memory_order_release);

        // Seed the per-character default SOURCE selections. Deferred to ensure_default_sources_seeded() because it
        // needs ItemNameTable (to resolve carrier item_name -> item_id -> variant meshes), which is NOT ready this
        // early
        // - it builds on a worker that finishes after this catalog populate. The call here is a best-effort attempt
        // and is usually a no-op. The activation gate (has_any_selection) and the picker read (selection_src_index)
        // re-invoke it, so the defaults land the instant the name table is ready.
        ensure_default_sources_seeded();

        const auto walk_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - walk_start)
                .count();
        logger.info(
            "[prefab-swap] Catalog populated: helm={} chest={} cloak={} gloves={} boots={} ({}ms)",
            counts[0],
            counts[1],
            counts[2],
            counts[3],
            counts[4],
            walk_ms
        );
        logger.debug(
            "[prefab-swap] Catalog wrappers cached: {} names, {} total wrappers ({}ms heap walk)",
            all_names.size(),
            total_wrappers,
            hw_ms
        );

        std::size_t total_count = 0;
        for (auto c : counts)
            total_count += c;
        return total_count;
    }

    /**
     * @brief Body this character's rig actually is, which honors the Body Type dropdown.
     *
     * The dropdown is the user's per-character override (stored by PresetManager, persisted to presets.json) and is
     * already what the picker's item-eligibility filter obeys. Reusing it here keeps the visual consistent with the
     * list the item was picked from, and makes a body-swap mod work by telling LT what the body now is instead of
     * inferring it.
     *
     * Empty or "Auto" defers to the character's hardcoded body. Anything unrecognized does the same.
     */
    static ItemNameTable::BodyKind effective_body_kind_for_char(Transmog::CarrierChar cc) noexcept
    {
        const auto name = std::string(Transmog::carrier_char_name(cc));
        if (name.empty())
            return ItemNameTable::BodyKind::Generic;

        const auto override_ = PresetManager::instance().body_kind_of(name);
        if (override_ == "Male")
            return ItemNameTable::BodyKind::Male;
        if (override_ == "Female")
            return ItemNameTable::BodyKind::Female;
        return ItemNameTable::body_kind_for_character(name);
    }

    std::size_t apply_selections_to_swap_map() noexcept
    {
        auto &logger = DMK::log();

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
        // wrappers, and they stay stuck on the body. The natpipe hook dispatches on s_active_char_idx, which resolves
        // cross-character src collisions (Kliff+Oongka both default to `cd_phm_00_ub_00_0054` for Chest). Each body's
        // apply then sees only its own bucket and the right tgt fires. The per-char `s_sel_src_idx_per_char` /
        // `s_sel_tgt_idx_per_char` arrays preserve in-memory picks across editing-character switches. The per-char
        // swap-map buckets are the runtime materialization of those picks.
        constexpr std::size_t slot_n = static_cast<std::size_t>(Transmog::TransmogSlot::Count);
        struct SlotPlan
        {
            /// Primary source name for the log, which is the resolved rig.
            std::string src_name;
            /// ALL rig siblings registered as sources, for body-swap robustness.
            std::vector<std::string> src_names;
            std::string tgt_name;
            std::uintptr_t tgt_wrapper{0};
            // Opposite socket of a paired slot. Its own target wrapper, because a `_r` source must reach the target's
            // `_r` mesh. The `_l` target in its place puts the wrong side's geometry in the socket.
            std::string side_tgt_name;
            std::uintptr_t side_tgt_wrapper{0};
        };
        std::array<SlotPlan, slot_n> plans;

        // Resolve the apply target character. `s_active_char_idx` is primed by `PresetManager::apply_to_state` whenever
        // the editing character changes, and tracks the character whose carrier+prefab WILL be installed by the
        // upcoming apply. Idx 0 means PresetManager did not bind a character yet (boot-time, before the first
        // apply_to_state). In that case bail out. The next apply_to_state retries with a real idx.
        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
        {
            // No bound character - skip the rebuild entirely. Other characters' previously-installed buckets must stay
            // intact so the natpipe-hook can still find their substitutions during a later teardown.
            return 0;
        }
        // The bucket being written and the SELECTIONS being read MUST come from the same character.
        //
        // `s_active_char_idx` picks the bucket. The per-character selection rows it indexes were filled by
        // PresetManager::apply_to_state from `active_preset()`, which reads the EDITING character. So the invariant
        // is: active_idx == idx(editing_character). Nothing enforced it, and when it broke the result was silent -
        // Kliff's bucket filled with Oongka's prefab picks and the natpipe hook installed them faithfully, because by
        // then they ARE Kliff's registered targets. No body-ownership check can see that: those verify whose BODY is
        // being dressed, and this is whose PICKS got written.
        //
        // The names are logged because `char[N]` alone never says which character N was.
        {
            auto &pm = PresetManager::instance();
            const auto editing_idx = CDCore::character_idx_from_name(pm.editing_character());
            if (editing_idx != 0 && editing_idx != active_idx)
            {
                logger.warning(
                    "[prefab-swap] swap-map build REFUSED: bucket char[{}] but the selections were loaded "
                    "for editing='{}' (char[{}]); controlled='{}' pinned={}. One character's prefab picks "
                    "would have been registered as another's targets.",
                    active_idx,
                    pm.editing_character(),
                    editing_idx,
                    pm.active_character(),
                    pm.editing_pinned() ? 1 : 0
                );
                return 0;
            }
            logger.debug(
                "[prefab-swap] swap-map build: bucket char[{}] editing='{}' controlled='{}' pinned={}",
                active_idx,
                pm.editing_character(),
                pm.active_character(),
                pm.editing_pinned() ? 1 : 0
            );
        }

        // The bucket being written and the mappings being read MUST describe the same character.
        //
        // This function writes bucket `active_idx` but sources every target from the ONE global slot_mappings(). Those
        // are kept in step by PresetManager::apply_to_state, and when they drift the result is silent and severe: the
        // bucket is filled with another character's targets and the natpipe hook then installs them faithfully,
        // because as far as every downstream check is concerned these ARE this character's targets. That is why the
        // body-ownership guards elsewhere cannot catch it - they verify WHOSE BODY, and this is WHOSE TARGETS.
        //
        // A refusal costs a rebuild: the bucket keeps its previous contents and the next apply_to_state retries with
        // a consistent pair. A pass costs the wrong character's armor, which is what a hot reload produces when it
        // binds one character while the mappings still hold another's slots.
        const auto mappings_owner = Transmog::slot_mappings_owner().load(std::memory_order_acquire);
        if (mappings_owner != 0 && mappings_owner != active_idx)
        {
            logger.warning(
                "[prefab-swap] swap-map build REFUSED: building char[{}]'s bucket but slot_mappings() "
                "belongs to char[{}]. One character's targets would have been installed on another. "
                "Skipping; the next apply_to_state rebuilds with a consistent pair.",
                active_idx,
                mappings_owner
            );
            return 0;
        }

        const auto ci = static_cast<std::size_t>(active_idx - 1);
        const auto cc = static_cast<Transmog::CarrierChar>(ci);

        {
            std::scoped_lock lk(s_catalog_mtx);
            // LT disabled registers nothing at all, explicit picks included.
            //
            // flag_enabled gates the socket override, but gating only there leaves the swap map armed, and the
            // struct-copy substitution keeps rewriting any carrier mesh the engine builds. That is invisible until a
            // carrier coincides with what is actually worn. Enabled off then restores the real gear everywhere
            // except that slot, which keeps its transmog. apply_all_transmog cannot cover this: it forces mappings
            // inactive in a LOCAL copy, so the real slot_mappings still read active here.
            const bool lt_enabled = Transmog::flag_enabled().load(std::memory_order_relaxed);

            for (std::size_t i = 0; i < slot_n; ++i)
            {
                if (!lt_enabled)
                    continue;

                auto &cat = s_slot_catalogs[i];
                auto tgt_idx = s_sel_tgt_idx_per_char[ci][i];
                const bool from_pick_row = (tgt_idx >= 0);

                // No explicit prefab pick for this slot? Derive one from the slot's target ITEM.
                //
                // This is what drops the carrier's impersonation of the target. A target prefab derived here means
                // the swap supplies the visual, and the carrier only has to be an item the character can legitimately
                // equip.
                //
                // An explicit pick always wins - the picker is the user stating exactly which prefab they want, and a
                // derived one must never override it.
                if (tgt_idx < 0)
                {
                    // An UNTICKED slot registers nothing.
                    //
                    // An untick leaves target_item_id set - it only clears `active` - so a derivation from the id
                    // alone keeps a swap entry alive for a slot LT no longer dresses. That is invisible while the
                    // carrier differs from what is worn, and wrong the moment they coincide: with the real gear BEING
                    // the carrier item, the engine builds exactly the mesh the entry keys on, and the substitution
                    // put the transmog back on an unticked slot - undyed, since a bare substitution carries no dye.
                    const auto &mapping = Transmog::slot_mappings()[i];
                    const auto target_item_id = mapping.active ? mapping.target_item_id : std::uint16_t{0};
                    if (target_item_id != 0)
                    {
                        // An item can own one mesh PER BODY RIG, so pick the one this character's body wears.
                        //
                        // The variants are returned in entry order, and for boss/NPC sets entry[0] is the mesh
                        // authored on the NPC's own body (`cd_m0001_...`). An unconditional entry[0] renders a boss
                        // set with the NPC's own rig on a character whose body differs, because the item also owns a
                        // per-rig mesh such as `cd_phw_m0001_00_samuel_ub_00_0001`.
                        //
                        // The rig lives in the mesh name's prefix: `cd_phw_` female, `cd_phm_` male. Which one this
                        // character wears is the Body Type dropdown's answer - the same per-character override the
                        // item-eligibility filter uses - so the visual agrees with the list the user picked from.
                        // An explicit override wins; "Auto" falls back to the character's hardcoded body.
                        //
                        // Fall back to first-catalog-resident when nothing matches the rig, which is the common case:
                        // most items ship a single mesh, and a wrong-rig visual still beats no visual at all (the
                        // carrier's own mesh shows instead).
                        const auto body_kind = effective_body_kind_for_char(cc);
                        const std::string_view rig_prefix = (body_kind == ItemNameTable::BodyKind::Female)
                                                                ? std::string_view{"cd_phw_"}
                                                                : std::string_view{"cd_phm_"};

                        const auto variants = Transmog::variant_meshes_for_item(target_item_id);
                        for (int pass = 0; pass < 2 && tgt_idx < 0; ++pass)
                        {
                            const bool rig_pass = (pass == 0);
                            for (const auto &mesh : variants)
                            {
                                if (mesh.empty())
                                    continue;
                                if (rig_pass && !mesh.starts_with(rig_prefix))
                                    continue;
                                for (std::size_t k = 0; k < cat.size(); ++k)
                                {
                                    if (cat[k].name == mesh)
                                    {
                                        tgt_idx = static_cast<int>(k);
                                        break;
                                    }
                                }
                                if (tgt_idx >= 0)
                                {
                                    logger.debug(
                                        "[prefab-swap]   char[{}] slot[{}] target derived from item {:#06x} "
                                        "-> \"{}\" ({})",
                                        ci,
                                        i,
                                        target_item_id,
                                        mesh,
                                        rig_pass ? "rig-matched" : "no rig variant - first resident"
                                    );
                                    break;
                                }
                            }
                        }
                        // A slot that wants a target but resolves none installs nothing, and the carrier - equipped
                        // as itself - is what stays on screen. Silence here reads exactly like "slot not requested",
                        // so say which meshes were tried and how big the slot catalog is.
                        if (tgt_idx < 0)
                        {
                            std::string tried;
                            for (const auto &mesh : Transmog::variant_meshes_for_item(target_item_id))
                            {
                                if (mesh.empty())
                                    continue;
                                if (!tried.empty())
                                    tried += ", ";
                                tried += mesh;
                            }
                            logger.warning(
                                "[prefab-swap]   char[{}] slot[{}] target NOT derived from item {:#06x} - "
                                "variant meshes [{}] absent from this slot's catalog ({} entries); the "
                                "carrier's own visual will show",
                                ci,
                                i,
                                target_item_id,
                                tried.empty() ? "<none>" : tried,
                                cat.size()
                            );
                        }
                    }
                }

                if (tgt_idx < 0 || tgt_idx >= static_cast<int>(cat.size()))
                    continue;

                // src resolution priority:
                //   1. Active character's runtime carrier source mesh (carrier_source_seed derives it from the
                //      carrier item_id's variant list) matched by name in the slot's catalog - the authoritative
                //      source identity for the currently-installing character.
                //   2. Per-char s_sel_src_idx_per_char fallback when the carrier lookup misses (no carrier
                //      source, or the
                //      derived mesh is not present in the slot's catalog).
                //   3. cat0 cross-slot adoption when the slot's own catalog never received the carrier prefab (paired
                //      slots whose tag patterns missed at boot, or prefabs absent from the loader registry).
                std::size_t resolved_src_idx = SIZE_MAX;
                const std::string expected_src_str = carrier_source_seed(cc, static_cast<Transmog::TransmogSlot>(i));
                const char *expected_src = expected_src_str.empty() ? nullptr : expected_src_str.c_str();
                if (expected_src && expected_src[0] != '\0')
                {
                    for (std::size_t k = 0; k < cat.size(); ++k)
                    {
                        if (cat[k].name == expected_src)
                        {
                            resolved_src_idx = k;
                            break;
                        }
                    }
                    if (resolved_src_idx == SIZE_MAX && !s_slot_catalogs.empty())
                    {
                        const auto &cat0 = s_slot_catalogs[0];
                        for (std::size_t k = 0; k < cat0.size(); ++k)
                        {
                            if (cat0[k].name == expected_src)
                            {
                                cat.push_back(cat0[k]);
                                resolved_src_idx = cat.size() - 1;
                                break;
                            }
                        }
                    }
                }
                if (resolved_src_idx == SIZE_MAX)
                {
                    const auto src_idx = s_sel_src_idx_per_char[ci][i];
                    if (src_idx >= 0 && src_idx < static_cast<int>(cat.size()))
                        resolved_src_idx = static_cast<std::size_t>(src_idx);
                }
                if (resolved_src_idx == SIZE_MAX)
                    continue;

                plans[i].src_name = cat[resolved_src_idx].name;
                plans[i].tgt_name = cat[tgt_idx].name;
                if (!cat[tgt_idx].wrappers.empty())
                    plans[i].tgt_wrapper = cat[tgt_idx].wrappers.front();

                // Resolve the target's opposite side, for slots whose prefabs are socket-suffixed. Only the TARGET
                // needs looking up - the counterpart SOURCE costs nothing, since the map is keyed by name hash and
                // the counterpart name is a string edit away.
                if (const auto tgt_side = side_suffix_of(plans[i].tgt_name); !tgt_side.empty())
                {
                    plans[i].side_tgt_name = with_side_suffix(plans[i].tgt_name, (tgt_side == "_l") ? "_r" : "_l");
                    if (!plans[i].side_tgt_name.empty())
                    {
                        // Counterpart prefabs can be filed under a different slot's catalog, so search all of them.
                        for (const auto &c2 : s_slot_catalogs)
                        {
                            for (const auto &ce : c2)
                                if (ce.name == plans[i].side_tgt_name && !ce.wrappers.empty())
                                {
                                    plans[i].side_tgt_wrapper = ce.wrappers.front();
                                    break;
                                }
                            if (plans[i].side_tgt_wrapper != 0)
                                break;
                        }
                    }
                }

                // Register EVERY body-rig sibling of the resolved source, not just the rig matching the character's
                // name-derived body. The carrier item shares one mesh across rigs (e.g. Kliff_Mask ->
                // cd_phm_00_/cd_phw_01_/cd_pom_01_mask_00_0271_a). Which rig the engine emits depends on the
                // wearer's REAL body, which a body-swap mod can change out from under the name->body assumption in
                // carrier_defaults. The slot catalog tag (e.g. "_mask_00_") already holds every rig variant, so we
                // union the wrappers of all entries whose rig-stripped stem matches - the natpipe hook then catches
                // whichever rig is emitted. Excludes the chosen target wrapper so we never redirect it onto itself.
                // Falls back to the single resolved entry when the name has no rig-stem shape (e.g. lanterns) or has no
                // siblings resident.
                const std::string src_stem = rig_stripped_stem(cat[resolved_src_idx].name);
                std::size_t siblingCount = 0;
                for (std::size_t k = 0; k < cat.size(); ++k)
                {
                    const bool is_sibling = src_stem.empty()
                                                ? (k == resolved_src_idx)
                                                : is_render_variant_of(rig_stripped_stem(cat[k].name), src_stem);
                    if (!is_sibling)
                        continue;
                    plans[i].src_names.push_back(cat[k].name);
                    ++siblingCount;
                    logger.trace(
                        "[prefab-swap]   char[{}] slot[{}] src-sibling \"{}\"{}",
                        ci,
                        i,
                        cat[k].name,
                        (k == resolved_src_idx) ? " (primary rig)" : ""
                    );
                }
                logger.trace(
                    "[prefab-swap]   char[{}] slot[{}] src-register primary=\"{}\" stem=\"{}\" "
                    "rigSiblings={} tgt=\"{}\" (0x{:X}) via={}",
                    ci,
                    i,
                    cat[resolved_src_idx].name,
                    src_stem,
                    siblingCount,
                    cat[tgt_idx].name,
                    plans[i].tgt_wrapper,
                    from_pick_row ? "pick-row" : "item-derived"
                );

                // An item emits exactly ONE mesh - measured across every slot, carrier and target alike. So a
                // backpack's strap and holder (cd_phm_00_bag_belt_*, cd_phm_00_bag_*_z) belong to NEITHER item. The
                // engine attaches them whenever a bag is worn. They are live parts, not orphans, which is why no
                // removal path affects them. Redirecting one needs whatever selects the holder for an equipped bag,
                // and that is not reachable from the item descriptor.
            }
        }

        // Sources need no wrapper discovery at all: the map is keyed by name hash, and every instance of a name
        // carries that hash. Instance discovery instead costs a heap walk over the whole address space on every
        // apply - both the dominant cost of an apply and incomplete, because the catalog holds canonical instances
        // while the engine passes pool instances, so a correctly-named binding sits in the map and never matches.

        // Build s_swap_map_per_char[ci] atomically under s_map_mtx. The bucket covers exactly the active character's
        // body for this apply. The natpipe hook reads from it during install-time wrapper traversal and dispatches via
        // s_active_char_idx, so other characters' buckets stay live for their own pending teardowns.
        // s_target_wrappers_per_char[ci] is this character's cleanup ledger. deactivate_for_clear drains the
        // substitution ledger, then merges all three buckets into one target set and validates every drained record
        // against it.
        std::size_t resolved = 0;
        {
            std::scoped_lock lk(s_map_mtx);
            s_swap_map_per_char[ci].clear();
            s_target_wrappers_per_char[ci].clear();
            for (auto &w : s_slot_target_wrapper_per_char[ci])
                w = 0;
            for (std::size_t i = 0; i < slot_n; ++i)
            {
                auto &p = plans[i];
                if (p.src_name.empty() || p.tgt_name.empty())
                    continue;
                if (p.src_names.empty() || p.tgt_wrapper == 0)
                {
                    logger.warning(
                        "[prefab-swap]   char[{}] slot[{}] UNRESOLVED \"{}\" -> \"{}\" (srcNames={} tgtWrapper=0x{:X})",
                        ci,
                        i,
                        p.src_name,
                        p.tgt_name,
                        p.src_names.size(),
                        p.tgt_wrapper
                    );
                    continue;
                }
                // Keyed by NAME HASH: every rig sibling registers itself, and no wrapper instance has to be found
                // for any of them. The target still needs a real pointer, because the substitution writes one.
                const auto tgt_hash = prefab_name_hash(p.tgt_name);
                for (const auto &sn : p.src_names)
                {
                    if (sn.empty())
                        continue;
                    const auto h = prefab_name_hash(sn);
                    if (h == tgt_hash)
                        continue; // never redirect the target onto itself
                    s_swap_map_per_char[ci].insert_or_assign(h, SwapEntry{p.tgt_wrapper, sn});
                }
                s_target_wrappers_per_char[ci].insert(p.tgt_wrapper);
                if (i < Transmog::SLOT_COUNT)
                    s_slot_target_wrapper_per_char[ci][i] = p.tgt_wrapper;
                ++resolved;
                logger.debug(
                    "[prefab-swap]   char[{}] slot[{}] RESOLVED \"{}\" ({} src name(s)) -> \"{}\" (0x{:X})",
                    ci,
                    i,
                    p.src_name,
                    p.src_names.size(),
                    p.tgt_name,
                    p.tgt_wrapper
                );

                // Opposite socket. Every source name that carries a side suffix also registers its counterpart,
                // pointed at the target's counterpart mesh. This is what a paired slot was missing: the descriptor
                // names one side, the engine may install the other, and only the named side was ever bound.
                if (p.side_tgt_wrapper != 0)
                {
                    std::size_t side_bound = 0;
                    for (const auto &sn : p.src_names)
                    {
                        const auto ss = side_suffix_of(sn);
                        if (ss.empty())
                            continue;
                        auto counterpart = with_side_suffix(sn, (ss == "_l") ? "_r" : "_l");
                        if (counterpart.empty())
                            continue;
                        const auto h = prefab_name_hash(counterpart);
                        if (h == prefab_name_hash(p.side_tgt_name))
                            continue; // never redirect the target onto itself
                        s_swap_map_per_char[ci].insert_or_assign(h, SwapEntry{p.side_tgt_wrapper, counterpart});
                        ++side_bound;
                    }
                    s_target_wrappers_per_char[ci].insert(p.side_tgt_wrapper);
                    logger.debug(
                        "[prefab-swap]   char[{}] slot[{}] side-bound -> \"{}\" (0x{:X}, {} counterpart name(s))",
                        ci,
                        i,
                        p.side_tgt_name,
                        p.side_tgt_wrapper,
                        side_bound
                    );
                }
                else if (!p.side_tgt_name.empty())
                {
                    // Not every item ships both sides. When the counterpart mesh does not exist there is nothing to
                    // render on that socket, and the carrier's own mesh stays - say so rather than failing silently.
                    logger.warning(
                        "[prefab-swap]   char[{}] slot[{}] side UNBOUND: no wrapper for \"{}\" - that "
                        "socket keeps the carrier's mesh",
                        ci,
                        i,
                        p.side_tgt_name
                    );
                }
            }
        }
        if (resolved > 0)
            s_maps_retained.store(true, std::memory_order_release); // enable natpipe cleanup even after s_active drops

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
            return 0; // nothing selected - leave the current state alone
        const auto resolved = apply_selections_to_swap_map();
        if (resolved == 0)
        {
            DMK::log().debug("[prefab-swap] slot-apply arm: no selections resolved - leaving swap state unchanged");
            return 0;
        }
        if (!s_active.exchange(true, std::memory_order_acq_rel))
            DMK::log().debug("[prefab-swap] slot-apply arm: ACTIVATED ({} slot(s) bound)", resolved);
        return resolved;
    }

    // Reactivate using the current per-slot dropdown selections. This is the auto-apply path triggered when the
    // slot-row body combos change in the overlay. It runs a deactivate-then-activate cycle, so the new selection
    // becomes visible without a hotkey press.
    std::size_t reactivate_with_selections() noexcept
    {
        auto &logger = DMK::log();
        if (!s_orig)
            return 0; // hook not installed - nothing to do
        if (s_active.load(std::memory_order_acquire))
        {
            // Cleanly tear down the prior substitution (scene-graph +0x40 reverts plus the staging-record
            // reverse-write) so the new map does not double-bind the previous targets.
            deactivate_for_clear();
        }
        if (!has_any_selection())
        {
            // Nothing left to bind - stay deactivated.
            return 0;
        }
        const auto resolved = apply_selections_to_swap_map();
        if (resolved == 0)
        {
            logger.warning("[prefab-swap] reactivate_with_selections: no slot selections resolved - staying INACTIVE");
            return 0;
        }
        s_active.store(true, std::memory_order_release);
        s_call_count.store(0, std::memory_order_relaxed);
        s_subst_count.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lk(s_last_apply_mtx);
            s_last_apply_valid = false;
            std::memset(s_last_apply_items, 0, sizeof(s_last_apply_items));
        }
        logger.info("[prefab-swap] reactivated via UI selections ({} slot(s) bound)", resolved);
        return resolved;
    }

    // Hook callback

    // Per-actor scoping for the substitution hooks
    //
    // `on_struct_copy` receives a staging-vector slot as its first argument, not a body, so it cannot tell which
    // actor it assembles for. A dispatch on `s_active_char_idx` instead is only sound while LT drives every apply and
    // therefore knows the answer. The engine assembles several bodies concurrently (player plus companions plus
    // wildlife), so that assumption does not hold on the natural path.
    //
    // The assembly node DOES identify itself: `+0x18` is a StringInfo wrapper holding the appearance asset path, e.g.
    // `character/appearance/1_pc/1_phm/cd_phm_macduff/...` for a protagonist or `character/appearance/2_mon/...` for
    // wildlife. CDCore already maps such a path to a protagonist index, so the scope is derived, not learned - correct
    // from the first call, with no cold-start window and no per-node cache to keep coherent.
    //
    // Zero means "not a protagonist, or unreadable"; consumers fall back to the previous behavior in that case, so a
    // failed resolve degrades to exactly what LT did before rather than dropping the substitution.
    static thread_local std::uint32_t t_scopeCharIdx = 0;

    static constexpr std::ptrdiff_t NODE_APPEARANCE_PATH_OFF = 0x18;

    /// Resolve an assembly node's appearance path to a protagonist index (1..3), or 0 when it is not a protagonist.
    [[nodiscard]] static std::uint32_t scope_char_for_node(std::int64_t node) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(node)}))
            return 0;
        const auto wrapper =
            DMK::memory::read<std::uint64_t>(DMK::Address{static_cast<std::uintptr_t>(node) + NODE_APPEARANCE_PATH_OFF})
                .value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
            return 0;
        const auto str_ptr =
            DMK::memory::read<std::uint64_t>(DMK::Address{static_cast<std::uintptr_t>(wrapper)}).value_or(0);
        const auto len =
            DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(wrapper) + 8}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{str_ptr}) || len == 0 || len >= 512)
            return 0;

        char buf[512];
        if (!DMK::memory::read_into(
                 DMK::Address{static_cast<std::uintptr_t>(str_ptr)},
                 std::span{reinterpret_cast<std::byte *>(buf), len}
            )
                 .has_value())
            return 0;
        return CDCore::classify_appearance_by_path(std::string_view{buf, len});
    }

    using PartListMergeFn = std::int64_t(__fastcall *)(std::int64_t, std::int64_t, std::int64_t);
    static PartListMergeFn s_orig_part_list_merge = nullptr;

    // Publishes the actor scope for the duration of the assembly call. The previous value is saved and restored rather
    // than cleared, because this function can nest (an actor whose assembly triggers another) and a blind clear
    // strands the outer scope at zero for the rest of its own run.
    // Appearance claim-list observation
    //
    // The claim list (`node+0x58` data / `node+0x60` count) is the layer ABOVE realized components. A claim added
    // or removed makes the engine build or drop the part itself, so that is where a fast removal and the empty-slot
    // add case have to happen. The claim add/remove pair lives on the stash node at `a1+0x58`.
    //
    // This logs the protagonist's node address and claim-list shape because the node is otherwise unobtainable from
    // outside the hook. It names a concrete live object to watch for claim-list writes, which is how the engine's own
    // claim mutators get located - without guessing at byte signatures.
    //
    // Kept out of on_part_list_merge's body on purpose: that function uses __try/__finally, and MSVC rejects __try in
    // any function that also needs object unwinding (C2712), which std::string and std::format both require.
    static std::mutex s_claim_log_mtx;
    static std::unordered_set<std::string> s_claim_logged;

    // Observed assembly nodes per protagonist (index 0..2), guarded by s_body_node_mtx.
    //
    // An actor owns SEVERAL of these nodes, each holding a different slice of the attached-record vector - one for
    // the head, others for body parts. With only the most recent one kept, the sweep runs against whichever node
    // assembled last, so a removal hits the wrong node and a mask never clears. Every node has to be swept.
    static std::mutex s_body_node_mtx;
    static std::unordered_set<std::uintptr_t> s_body_nodes_per_char[3];

    /**
     * @brief Attached-record enumeration cap for the sweep's diagnostics. Past this the `have` list is truncated,
     * which is what makes a miss unprovable rather than merely uneventful.
     */
    static constexpr std::uint32_t SWEEP_ENUMERATION_CAP = 64;

    // Wrappers LT installed on a PREVIOUS apply, still without a decision. Guarded by s_map_mtx.
    //
    // They cannot be swept at deactivate time: an apply only re-installs the slots that changed
    // (`slot_needs_work`), so a sweep of the whole target set there detaches the untouched slots too and nothing puts
    // them back. A change to one real slot then wipes every other slot's visual.
    //
    // Instead the old set parks here, the install runs, and the sweep afterwards takes only what the new set does NOT
    // contain - so an unchanged slot's wrapper appears in both and survives.
    static std::unordered_set<std::uintptr_t> s_pending_stale_per_char[3];

    static void log_claim_shape(std::int64_t node, std::uint32_t char_idx) noexcept
    {
        if (char_idx < 1 || char_idx > 3 ||
            !DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(node)}))
            return;
        {
            std::scoped_lock lk(s_body_node_mtx);
            auto &set = s_body_nodes_per_char[char_idx - 1];
            if (set.size() < 32) // bounded: an actor has a handful of nodes, so a runaway set means a wrong key
                set.insert(static_cast<std::uintptr_t>(node));
        }
        const auto claim_data =
            DMK::memory::read<std::uint64_t>(DMK::Address{static_cast<std::uintptr_t>(node) + 0x58}).value_or(0);
        const auto claim_count =
            DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(node) + 0x60}).value_or(0);
        const auto ready_count =
            DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(node) + 0x48}).value_or(0);

        auto key = std::format("{:X}|{}|{}", static_cast<std::uintptr_t>(node), claim_count, ready_count);
        {
            std::scoped_lock lk(s_claim_log_mtx);
            if (!s_claim_logged.insert(std::move(key)).second)
                return;
        }
        DMK::log().debug(
            "[body] char={} node=0x{:X} attached(count={} data=0x{:X}) countFieldAddr=0x{:X} readyCount={}",
            char_idx,
            static_cast<std::uintptr_t>(node),
            claim_count,
            static_cast<std::uintptr_t>(claim_data),
            static_cast<std::uintptr_t>(node) + 0x60,
            ready_count
        );
    }

    // Defined further down with the natpipe diagnostics. Forward-declared so the claim hooks above can name the
    // wrappers they see.
    static std::string wrapper_name_for_log(std::uintptr_t wrapper) noexcept;

    // Appearance "remove claims of prefab" observation
    //
    // This is the claim-layer removal primitive. It drops a claim instead of a full tear-down of a realized
    // component, so it needs neither `SafeTearDown` nor a scene-graph walk. If it does what it appears to, it is the
    // stale-mesh cleanup LT wants: an apply installs first and drops the stale claim after, in place of a serial
    // tear-down up front.
    //
    // OBSERVATIONAL ONLY. Two things are unknown and both must come from the engine rather than from a guess:
    //   - `a3` / `a4` have no established meaning, so LT cannot construct a call yet. Logging the engine's own values
    //     is how we learn them.
    //   - Whether a dropped claim removes the ALREADY-REALIZED mesh, or only affects the next rebuild. The
    //     before/after claim counts plus the visual answer that.
    //
    // Signature: f(a1 = appearance node, a2 = __int64* -> name wrapper, a3, a4).
    using UnlinkByWrapperFn = std::int64_t(__fastcall *)(std::int64_t, std::int64_t, std::int64_t, std::int64_t);
    static UnlinkByWrapperFn s_orig_unlink_by_wrapper = nullptr;

    /**
     * @brief Resolve `*a2` to a prefab name for the log. Claims are matched on the wrapper reachable at `owner+0x40`,
     * so a plain catalog lookup can miss. An empty result reports as the raw pointer rather than hidden.
     */
    static std::string remove_claims_name(std::int64_t a2) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(a2)}))
            return {};
        const auto wrapper =
            DMK::memory::read<std::uint64_t>(DMK::Address{static_cast<std::uintptr_t>(a2)}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
            return {};
        auto nm = wrapper_name_for_log(static_cast<std::uintptr_t>(wrapper));
        return nm.empty() ? std::format("0x{:X}", wrapper) : nm;
    }

    // Synthesized NaturalPipeline detach
    //
    // `UnlinkByWrapper` removes an entry from the body's attached-record vector, but the realized component keeps
    // rendering - measured: 10 records identified, 10 unlinked, mesh still on screen. Record bookkeeping and
    // scene-graph detach are separate jobs.
    //
    // `SafeTearDown` performs the actual detach by building a `(wrapper, flag)` list at 16-byte stride and calling
    // NaturalPipeline with it. Its expensive half is `ExpandToMeshes`, which resolves WHICH wrappers to remove from
    // the inventory-equipped item - and LT already knows which: the wrappers it installed.
    //
    // So: build the list ourselves and call NaturalPipeline directly. Same detach, none of the lookup.
    //
    // Called through the TRAMPOLINE so the synthetic call bypasses LT's own natpipe hook. The hook otherwise treats
    // LT's own list as an engine unlink and substitutes into it.
    struct NatpipeContainer
    {
        void *data{nullptr};
        std::uint32_t count{0};
        std::uint32_t cap{0};
    };

    // 16-byte stride. The pipeline reads only the wrapper at +0. The second qword is engine-internal refcount
    // metadata and is safe to leave zero for a synthetic list we own.
    struct NatpipeEntry16
    {
        std::uintptr_t wrapper{0};
        std::uintptr_t meta{0};
    };

    /**
     * @brief SEH-guarded call into the NaturalPipeline trampoline. Isolated because MSVC forbids __try in a function
     * that also needs object unwinding (C2712). Returns -1 on fault.
     */
    static std::int64_t
    call_natpipe_outer_seh(std::uintptr_t parent, NatpipeContainer *a2, NatpipeContainer *a3) noexcept
    {
        if (!s_orig_natural_pipeline || !DMK::memory::is_plausible_ptr(DMK::Address{parent}) || !a2 || !a3)
            return -1;
        __try
        {
            return s_orig_natural_pipeline(
                static_cast<std::int64_t>(parent),
                reinterpret_cast<std::uint64_t *>(a2),
                reinterpret_cast<std::uint64_t *>(a3)
            );
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    /**
     * @brief Drop claim entries whose owner went null, and fix the count.
     *
     * The synthesized NaturalPipeline detach clears an entry's owner at `entry+0x08` but leaves `node+0x60`
     * counting it, so the vector keeps a hole. The engine walks it unguarded -
     * `*(QWORD*)(*(QWORD*)(entry+8) + 40)` - and takes an access violation on the null owner.
     *
     * The engine's own erase does what this does: shift the tail down and decrement. No release is needed here,
     * because the entries being dropped already have a null owner.
     *
     * Layout: data `node+0x58`, count `node+0x60`, 16-byte stride, `entry+0x00` dword key, `entry+0x08` owner.
     */
    static std::size_t compact_claim_vector(std::uintptr_t node) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{node}))
            return 0;
        const auto data = DMK::memory::read<std::uint64_t>(DMK::Address{node + 0x58}).value_or(0);
        const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{node + 0x60}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{data}) || count == 0 || count > 256)
            return 0;

        std::uint32_t write = 0;
        std::size_t dropped = 0;
        for (std::uint32_t read = 0; read < count; ++read)
        {
            const auto src = static_cast<std::uintptr_t>(data) + static_cast<std::size_t>(read) * 16;
            const auto owner = DMK::memory::read<std::uint64_t>(DMK::Address{src + 8});
            if (!owner)
                return 0; // the vector faulted mid-walk - leave the count alone
            if (*owner == 0)
            {
                ++dropped;
                continue;
            }
            if (write != read)
            {
                const auto dst = static_cast<std::uintptr_t>(data) + static_cast<std::size_t>(write) * 16;
                const auto key = DMK::memory::read<std::uint32_t>(DMK::Address{src});
                if (!key)
                    return 0;
                if (!DMK::memory::write_in_place<std::uint32_t>(DMK::Address{dst}, *key))
                    return 0;
                if (!DMK::memory::write_in_place<std::uint64_t>(DMK::Address{dst + 8}, *owner))
                    return 0;
            }
            ++write;
        }
        if (dropped != 0 && !DMK::memory::write_in_place<std::uint32_t>(DMK::Address{node + 0x60}, write))
            return 0;
        return dropped;
    }

    /**
     * @brief SEH-guarded single-wrapper unlink. It sits in its own function because MSVC forbids __try in any
     * function that also needs C++ object unwinding (C2712).
     *
     * `a2` is a pointer TO a variable holding the wrapper - the engine dereferences it twice (`**a2`). `a3`/`a4` are
     * optional out-vectors and are safe to pass 0.
     *
     * Returns the engine's unlink count, or -1 on fault.
     */
    static std::int64_t call_unlink_by_wrapper_seh(std::uintptr_t parent, std::uintptr_t *wrapper_var) noexcept
    {
        if (!s_orig_unlink_by_wrapper || !DMK::memory::is_plausible_ptr(DMK::Address{parent}) || !wrapper_var ||
            !DMK::memory::is_plausible_ptr(DMK::Address{*wrapper_var}))
            return -1;
        __try
        {
            // Call the TRAMPOLINE, not the hooked address: this is LT's own call, and a route through our own
            // observation hook logs it as if the engine made it.
            return s_orig_unlink_by_wrapper(
                static_cast<std::int64_t>(parent),
                reinterpret_cast<std::int64_t>(wrapper_var),
                0,
                0
            );
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static std::int64_t __fastcall on_remove_claims(std::int64_t a1, std::int64_t a2, std::int64_t a3, std::int64_t a4)
    {
        const auto trampoline = s_orig_unlink_by_wrapper;
        if (!trampoline)
            return 0;

        const auto before =
            DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(a1) + 0x60}).value_or(0);
        auto name = remove_claims_name(a2);

        // Snapshot what a4 points at. a4 is always a stack address that differs per call, which leaves two readings:
        // an OUT-PARAM the callee writes, or a caller-owned context it only reads. Those need different handling if
        // LT ever constructs this call, because a bogus pointer for the first corrupts the caller's frame.
        // Comparing before/after settles it without any guesswork.
        std::uint64_t a4_before[4]{};
        std::uint64_t a4_after[4]{};
        const bool a4_readable = DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(a4)}) &&
                                 DMK::memory::read_into(
                                     DMK::Address{static_cast<std::uintptr_t>(a4)},
                                     std::span{reinterpret_cast<std::byte *>(a4_before), sizeof(a4_before)}
                                 )
                                     .has_value();

        const auto result = trampoline(a1, a2, a3, a4);

        const auto after =
            DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(a1) + 0x60}).value_or(0);
        const bool a4_readable_after =
            a4_readable && DMK::memory::read_into(
                               DMK::Address{static_cast<std::uintptr_t>(a4)},
                               std::span{reinterpret_cast<std::byte *>(a4_after), sizeof(a4_after)}
                           )
                               .has_value();
        const bool a4_written = a4_readable_after && std::memcmp(a4_before, a4_after, sizeof(a4_before)) != 0;

        // Report only calls where something actually HAPPENED: a claim was erased, or the engine wrote through a4.
        //
        // This hook fires for every claim erase in the process, not just LT's. The two questions it was built to
        // answer are answered - a4 is an out-param (it is written on a minority of calls), and the argument shapes
        // the engine passes are known, with the conclusion recorded at the canonical-wrapper comment in the sweep
        // below. What is left is the running signal: which erases succeed. Logging the no-ops as well produced 1514
        // of 1533 lines saying "nothing was removed from something I could not even name" - 41% of the whole log,
        // three quarters of which resolved no prefab name at all.
        if (result == 0 && !a4_written)
            return result;

        // Report each distinct (name, delta, a3, a4) shape once. a3/a4 are in the key because a repeat with different
        // args is new information, not noise.
        auto key = std::format("{}|{}->{}|{:X}|{}", name, before, after, static_cast<std::uintptr_t>(a3), a4_written);
        bool fresh = false;
        {
            std::scoped_lock lk(s_claim_log_mtx);
            fresh = s_claim_logged.insert("RC|" + key).second;
        }
        if (fresh)
        {
            DMK::log().debug(
                "[claim-remove] node=0x{:X} prefab=\"{}\" claims {}->{} ret=0x{:X} a3=0x{:X} a4=0x{:X} "
                "a4Written={} a4[0..3] before=[{:X} {:X} {:X} {:X}] after=[{:X} {:X} {:X} {:X}]",
                static_cast<std::uintptr_t>(a1),
                name,
                before,
                after,
                static_cast<std::uintptr_t>(result),
                static_cast<std::uintptr_t>(a3),
                static_cast<std::uintptr_t>(a4),
                a4_written ? 1 : 0,
                a4_before[0],
                a4_before[1],
                a4_before[2],
                a4_before[3],
                a4_after[0],
                a4_after[1],
                a4_after[2],
                a4_after[3]
            );
        }
        return result;
    }

    // Defined below. The claim and substitution diagnostics both name wrappers.
    static std::string wrapper_inline_name(std::uintptr_t wrapper) noexcept;

    static std::int64_t __fastcall on_part_list_merge(std::int64_t a1, std::int64_t a2, std::int64_t a3)
    {
        const auto trampoline = s_orig_part_list_merge;
        if (!trampoline)
            return 0;

        const auto prev = t_scopeCharIdx;
        t_scopeCharIdx = scope_char_for_node(a1);
        log_claim_shape(a1, t_scopeCharIdx);
        std::int64_t result = 0;
        __try
        {
            result = trampoline(a1, a2, a3);
        }
        __finally
        {
            t_scopeCharIdx = prev;
        }
        return result;
    }

    static std::int64_t __fastcall on_struct_copy(std::int64_t a1, std::int64_t a2)
    {
        const auto trampoline = s_orig;
        if (!trampoline)
            return 0;

        // Cheap guards before any indirect read.
        if (!s_active.load(std::memory_order_acquire))
            return trampoline(a1, a2);

        // LT disabled substitutes nothing, whatever the map still holds.
        //
        // Gating the map BUILD is not enough: a clear does not rebuild the map, it deactivates and restores. The
        // restore re-equips the real item, and while that item is also the carrier its mesh is exactly what the
        // surviving entries key on - so toggling Enabled off tore the fake down and then substituted it straight
        // back on during the restore. The map is data; this is its one consumer, so this is where "off" has to mean
        // off.
        if (!Transmog::flag_enabled().load(std::memory_order_relaxed))
            return trampoline(a1, a2);

        // Deliberately NOT gated on in_transmog(). The engine's own equip must be substituted too, otherwise a gear
        // change attaches and draws the real mesh before LT's debounced apply can replace it - the visible flash of
        // real gear. The swap map is the filter: it holds wrappers only for slots LT is actively driving, and
        // per-actor scoping keeps it to the right body.
        // Counted before the filters below, so "the installer never ran" can be told apart from "it ran and the
        // filters rejected everything". Those are different faults with the same downstream symptom.
        const bool census = s_census_armed.load(std::memory_order_relaxed);
        if (census)
            s_census_raw.fetch_add(1, std::memory_order_relaxed);

        if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(a2)}))
            return trampoline(a1, a2);

        s_call_count.fetch_add(1, std::memory_order_relaxed);
        if (census)
            s_census_calls.fetch_add(1, std::memory_order_relaxed);

        const auto src_wrapper =
            DMK::memory::read<std::uint64_t>(DMK::Address{static_cast<std::uintptr_t>(a2)}).value_or(0);
        // Filter the StringInfo vtable sentinel: a2 sometimes points at the entry's +0x08 vtable slot rather than its
        // wrapper-ptr slot, and src_wrapper then equals the sentinel address. The sentinel resolves through
        // string_info_vtable(). A zero here means the cascade missed, so the equality test never matches and
        // the swap-map lookup proceeds unchanged.
        const auto si_vtable = s_string_info_vtable.load(std::memory_order_acquire);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{src_wrapper}) || src_wrapper == si_vtable)
            return trampoline(a1, a2);

        std::uintptr_t tgt_wrapper = 0;
        // Swap-map key of the entry this call matched, recorded into the census only once the write has actually
        // landed. A record at match time reports a slot as substituted even when the stack-temporary guard or the
        // guarded write refused it, which is exactly the failure the census exists to surface.
        std::uint32_t matched_key = 0;
        if (s_active.load(std::memory_order_acquire))
        {
            // Bucket selection, and the guard against cross-actor bleed.
            //
            // The map is keyed by prefab NAME HASH, which identifies a prefab, not an actor. Now that substitution
            // also runs during the engine's own equips, to pre-empt the gear-change flash, any actor that emits a
            // registered wrapper is a candidate - including NPCs and creatures that wear the same mesh.
            //
            //   scoped 1..3  -> the assembly node identified a protagonist, so use that bucket. Others are
            //                   safe automatically, since each bucket holds only its own character's targets.
            //   scoped 0 + in_transmog -> LT is driving the chokepoint itself (its own apply, no assembly node in
            //                   scope), so the active character is the right answer.
            //   scoped 0 + !in_transmog -> an assembly for something that is NOT a protagonist. Falling back here is
            //                   what re-skins an NPC that wears the same item, so refuse instead.
            const auto scoped = t_scopeCharIdx;
            std::uint32_t active_idx = 0;
            if (scoped >= 1 && scoped <= 3)
                active_idx = scoped;
            else if (Transmog::in_transmog().load(std::memory_order_relaxed))
                active_idx = s_active_char_idx.load(std::memory_order_acquire);
            else
            {
                if (census)
                {
                    s_census_scope_reject.fetch_add(1, std::memory_order_relaxed);
                    // Record WHAT was refused, not just how many. A refusal is expected for NPCs, but it is also what
                    // a protagonist part looks like when its assembly runs after the apply window has closed - the
                    // scope is thread-local and in_transmog is already down by then. Those two are indistinguishable
                    // by count alone, and only the second one means a slot silently fails to install.
                    //
                    // This is the detour's most-travelled exit, so the saturation flag is tested before the name
                    // hash: once the list is full there is nothing left to learn and the branch costs one load.
                    if (!s_census_rej_full.load(std::memory_order_relaxed))
                    {
                        const auto rej_hash = wrapper_name_hash(static_cast<std::uintptr_t>(src_wrapper));
                        if (rej_hash != 0)
                        {
                            std::scoped_lock ck(s_census_mtx);
                            if (s_census_rej_seen.size() < CENSUS_REJ_CAP &&
                                std::find(s_census_rej_seen.begin(), s_census_rej_seen.end(), rej_hash) ==
                                    s_census_rej_seen.end())
                            {
                                s_census_rej_seen.push_back(rej_hash);
                                if (s_census_rej_seen.size() >= CENSUS_REJ_CAP)
                                    s_census_rej_full.store(true, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                return trampoline(a1, a2); // non-protagonist assembly - never substitute
            }
            if (active_idx >= 1 && active_idx <= 3)
            {
                const auto bucket = static_cast<std::size_t>(active_idx - 1);
                // One dword read identifies the wrapper: every instance of a prefab name carries the same hash, so
                // which instance the engine happens to hand us stops mattering.
                const auto src_hash = wrapper_name_hash(static_cast<std::uintptr_t>(src_wrapper));

                if (census)
                {
                    if (src_hash == 0)
                        s_census_hash_fail.fetch_add(1, std::memory_order_relaxed);
                    else if (!s_census_seen_full.load(std::memory_order_relaxed))
                    {
                        std::scoped_lock ck(s_census_mtx);
                        if (s_census_seen.size() < CENSUS_SEEN_CAP &&
                            std::find(s_census_seen.begin(), s_census_seen.end(), src_hash) == s_census_seen.end())
                        {
                            s_census_seen.push_back(src_hash);
                            if (s_census_seen.size() >= CENSUS_SEEN_CAP)
                                s_census_seen_full.store(true, std::memory_order_relaxed);
                        }
                    }
                }
                if (src_hash != 0)
                {
                    std::scoped_lock lk(s_map_mtx);
                    auto &m = s_swap_map_per_char[bucket];
                    const auto it = m.find(src_hash);
                    if (census)
                        (it != m.end() ? s_census_hit : s_census_miss).fetch_add(1, std::memory_order_relaxed);
                    if (it != m.end())
                    {
                        // Confirm the name on a hit. 32 bits over the whole prefab corpus is not collision-proof, and
                        // a collision here renders some unrelated mesh. Only hits pay for this.
                        const auto nm = wrapper_inline_name(static_cast<std::uintptr_t>(src_wrapper));
                        if (nm == it->second.src_name)
                        {
                            tgt_wrapper = it->second.tgt_wrapper;
                            matched_key = src_hash;
                        }
                        else
                            DMK::log().warning(
                                "[prefab-swap] hash collision ignored: wrapper \"{}\" hashes to the same value as "
                                "\"{}\" (0x{:08X}) - not substituting",
                                nm,
                                it->second.src_name,
                                src_hash
                            );
                    }
                }
            }
        }
        if (tgt_wrapper == 0)
            return trampoline(a1, a2);

        // Bump target's refcount BEFORE substitution so that the destination's eventual decrement-on-destruct stays
        // balanced.
        // Refuse any substitution whose destination is not a stack temporary. Checked BEFORE the refcount bump so a
        // rejected write cannot leak a reference. See is_on_current_thread_stack for why this is the safety boundary.
        if (!is_on_current_thread_stack(static_cast<std::uintptr_t>(a2)))
        {
            const auto n = s_guard_rejects.fetch_add(1, std::memory_order_relaxed);
            if (n < 20)
            {
                DMK::log().warning(
                    "[prefab-swap] GUARD: refused substitution - dest 0x{:X} is not on the calling thread's stack "
                    "(src=0x{:X} ra=0x{:X}). The stack-temporary invariant does not hold on this path; skipping the "
                    "write so nothing persistent can be touched.",
                    static_cast<std::uintptr_t>(a2),
                    src_wrapper,
                    reinterpret_cast<std::uintptr_t>(_ReturnAddress())
                );
            }
            return trampoline(a1, a2);
        }

        increment_wrapper_refcount(tgt_wrapper);

        // Substitute: the caller's source struct (a2) now points at our target wrapper. The struct-copy trampoline
        // will move the wrapper-ptr to dest+0 and write the sentinel back to source+0. The caller's cleanup then sees
        // a sentinel and skips the decrement of the now-unreferenced original wrapper - a small +1 leak we tolerate.
        const auto subst_write = DMK::memory::write_in_place<std::uint64_t>(
            DMK::Address{static_cast<std::uintptr_t>(a2)},
            static_cast<std::uint64_t>(tgt_wrapper)
        );
        if (!subst_write)
        {
            // Write refused - pass through. This leaks a refcount bump on tgt_wrapper. Rare path, accept it.
            return trampoline(a1, a2);
        }

        if (census && matched_key != 0)
        {
            std::scoped_lock ck(s_census_mtx);
            if (std::find(s_census_hit_keys.begin(), s_census_hit_keys.end(), matched_key) == s_census_hit_keys.end())
                s_census_hit_keys.push_back(matched_key);
        }

        const auto sc = s_subst_count.fetch_add(1, std::memory_order_relaxed);
        if (sc < 50)
        {
            // Diagnostic for the first N substitutions. `+0x0C` is the wrapper's name hash, the key the swap map
            // is built on.
            const auto src_hash =
                DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(src_wrapper) + 0x0C})
                    .value_or(0);
            const auto tgt_hash =
                DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(tgt_wrapper) + 0x0C})
                    .value_or(0);
            DMK::log().info(
                "[prefab-swap] SWAP src=0x{:X} -> tgt=0x{:X} (subst #{}) srcName=\"{}\" srcHash=0x{:08X} "
                "tgtName=\"{}\" tgtHash=0x{:08X}",
                src_wrapper,
                tgt_wrapper,
                sc + 1,
                wrapper_inline_name(static_cast<std::uintptr_t>(src_wrapper)),
                src_hash,
                wrapper_inline_name(static_cast<std::uintptr_t>(tgt_wrapper)),
                tgt_hash
            );
        }

        // Run the trampoline - it MOVEs *a2 (our target wrapper) to *a1 (dest+0) and sentinels *a2.
        const auto rc = trampoline(a1, a2);

        // Track the dest so deactivate_for_clear can reverse-write the original Kliff wrapper, restoring engine state
        // to a form LT's auth-table-driven tear_down can walk cleanly.
        {
            std::scoped_lock lk(s_subst_log_mtx);
            if (s_subst_log.size() < MAX_SUBST_LOG)
            {
                s_subst_log.push_back({static_cast<std::uintptr_t>(a1), src_wrapper});
            }
        }

        return rc;
    }

    // Natural-pipeline unlink hook.
    //
    // Walks RDX's wrapper list at hook entry. For each entry whose wrapper matches the active character's src in
    // s_swap_map_per_char[s_active_char_idx-1], substitutes to the corresponding target. Calls trampoline. Restores the
    // originals afterwards so the caller's refcount-release loop on the list operates on the same wrappers it
    // incremented.
    //
    // Verbose logging covers four shapes: hook entry (hit#, a1 body, list ptr, count, return address), each entry
    // (orig wrapper, the SUBST or PASSTHROUGH decision, tgt when substituted), the post-call totals (substitutions
    // performed, list count) and the restore (each restoration plus the final list state).
    //
    // Unlink-traversal diagnostic
    //
    // The natural-pipeline hook is the ONLY unlink path LT has: it presents the substituted wrapper during the
    // engine's own traversal so the detach finds what was installed. When it silently matches nothing, the old mesh
    // stays painted and there is no error anywhere - the hook passes through.
    //
    // Silence is therefore ambiguous between "the traversal never ran", "it ran with an empty list", and "it ran with
    // wrappers we never registered". Those need different fixes, so report the list CONTENTS once per distinct shape.
    static std::mutex s_unlink_mtx;
    static std::unordered_map<std::uintptr_t, std::string> s_unlink_names;
    static std::unordered_set<std::string> s_unlink_reported;
    static bool s_unlink_names_built = false;

    /**
     * @brief Read a wrapper's inline prefab name straight out of the object.
     *
     * Layout (verified live): `+0x00` string pointer, `+0x08` u32 length, `+0x0C` hash, `+0x10` refcount.
     *
     * This works for ANY instance. The catalog-index lookup (`wrapper_name_for_log`) knows only the instances
     * present at boot, and the attached-record vector routinely holds others, so a name match against the index
     * fails silently and a hide detach removes nothing.
     */
    static std::string wrapper_inline_name(std::uintptr_t wrapper) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{wrapper}))
            return {};
        const auto str_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{wrapper}).value_or(0);
        const auto len = DMK::memory::read<std::uint32_t>(DMK::Address{wrapper + 8}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{str_ptr}) || len == 0 || len >= 256)
            return {};
        char buf[256];
        if (!DMK::memory::read_into(
                 DMK::Address{static_cast<std::uintptr_t>(str_ptr)},
                 std::span{reinterpret_cast<std::byte *>(buf), len}
            )
                 .has_value())
            return {};
        return std::string(buf, len);
    }

    static std::string wrapper_name_for_log(std::uintptr_t wrapper) noexcept
    {
        std::scoped_lock lk(s_unlink_mtx);
        if (!s_unlink_names_built)
        {
            std::scoped_lock ck(s_catalog_mtx);
            for (const auto &e : s_slot_catalogs[0])
                for (const auto w : e.wrappers)
                    if (DMK::memory::is_plausible_ptr(DMK::Address{w}))
                        s_unlink_names.emplace(w, e.name);
            s_unlink_names_built = true;
        }
        const auto it = s_unlink_names.find(wrapper);
        return (it == s_unlink_names.end()) ? std::string{} : it->second;
    }

    static std::int64_t __fastcall on_natural_pipeline(std::int64_t a1, std::uint64_t *a2, std::uint64_t *a3)
    {
        const auto trampoline = s_orig_natural_pipeline;
        if (!trampoline)
            return 0;

        // Passthrough only when the feature has NEVER bound a swap for this world. Deliberately NOT gated on
        // s_active alone: an installed target must still be unlinked when its body is torn down even if the
        // now-active character has no swap (s_active==false) - the cross-character orphan path. The empty-list fast
        // path below keeps the cost negligible for the common zero-length unlink calls.
        if (!s_active.load(std::memory_order_acquire) && !s_maps_retained.load(std::memory_order_acquire))
            return trampoline(a1, a2, a3);

        auto &logger = DMK::log();
        const auto hit_seq = s_natpipe_hit_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto caller_ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

        // Read the list shape: a2 -> { data: u64*, count: u32, ... }
        // Stride 16 bytes per entry: (wrapper_qword, byte_flag, padding).
        std::uint64_t *list_data = nullptr;
        std::uint32_t list_count = 0;
        if (a2)
        {
            const auto raw =
                DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(a2)}).value_or(0);
            list_data = reinterpret_cast<std::uint64_t *>(raw);
            list_count = DMK::memory::read<std::uint32_t>(
                             DMK::Address{reinterpret_cast<std::uintptr_t>(reinterpret_cast<const char *>(a2) + 8)}
            )
                             .value_or(0);
        }

        s_natpipe_list_entries.fetch_add(list_count, std::memory_order_relaxed);

        // The engine takes TWO lists, a2 AND a3, and works when EITHER is populated. On the SafeTearDown path a2
        // carries the real list and a3 is an empty collection. Other call sites populate a3 instead. LT substitutes
        // out of a2 only, which is correct for the path that matters here.

        // Report each distinct non-empty list once, naming every wrapper it carries and whether the swap map knows it.
        if (list_data && list_count > 0 && list_count <= 64)
        {
            std::string desc;
            for (std::uint32_t i = 0; i < list_count; ++i)
            {
                const auto w =
                    DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(&list_data[i * 2])})
                        .value_or(0);
                auto nm = wrapper_name_for_log(static_cast<std::uintptr_t>(w));
                desc += (desc.empty() ? "" : ", ") + (nm.empty() ? std::format("0x{:X}", w) : nm);
            }
            bool fresh = false;
            {
                std::scoped_lock lk(s_unlink_mtx);
                fresh = s_unlink_reported.insert(desc).second;
            }
            if (fresh)
            {
                logger.trace(
                    "[natpipe] unlink list count={} active={} retained={} ra=0x{:X} [{}]",
                    list_count,
                    s_active.load(std::memory_order_acquire) ? 1 : 0,
                    s_maps_retained.load(std::memory_order_acquire) ? 1 : 0,
                    caller_ra,
                    desc
                );
            }
        }

        // Empty-list fast path: the engine fires this function from many call sites (the render/animation tick among
        // them) with list_count=0. There is nothing for us to do, and a log line per call floods the trace stream. Skip
        // the hook body entirely and call the trampoline.
        constexpr std::uint32_t max_entries = 64;
        std::uint64_t saved[max_entries] = {};
        std::uint32_t subst_count = 0;
        const auto cnt = (list_count < max_entries) ? list_count : max_entries;

        if (!list_data || cnt == 0)
            return trampoline(a1, a2, a3);

        // Resolve which character's bucket this teardown/install path applies to. s_active_char_idx is set by
        // PresetManager::apply_to_state BEFORE the engine drives any wrapper traversal, so by the time the hook fires
        // it already points at the body being assembled or torn down. With no active character bound we pass through
        // unchanged.
        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
            return trampoline(a1, a2, a3);
        const auto bucket = static_cast<std::size_t>(active_idx - 1);

        // Walk the list under the guarded memory verbs and substitute matching src wrappers. PASSTHROUGH entries
        // (wrapper not in this character's bucket, or a low address) intentionally do not log. The engine queries
        // many unrelated wrappers, and the noise drowns out the rare SUBST events that matter for the body-mesh
        // cleanup path.
        for (std::uint32_t i = 0; i < cnt; ++i)
        {
            const auto orig =
                DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(&list_data[i * 2])})
                    .value_or(0);
            saved[i] = orig;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{orig}))
                continue;

            // Lookup in the active character's bucket. Read-only on the hot path, and the bucket is immutable while
            // s_active is true, per LT design.
            const auto orig_hash = wrapper_name_hash(static_cast<std::uintptr_t>(orig));
            std::uintptr_t tgt = 0;
            if (orig_hash != 0)
            {
                std::scoped_lock lk(s_map_mtx);
                auto &m = s_swap_map_per_char[bucket];
                auto it = m.find(orig_hash);
                if (it != m.end())
                    tgt = it->second.tgt_wrapper;
            }

            if (tgt != 0)
            {
                // Active bucket owns this wrapper => a1 is the active character's body (this fires during its
                // assembly). Learn a1 -> bucket so a LATER teardown of this same body, while a DIFFERENT character
                // is active, resolves the correct bucket instead of orphaning the target.
                {
                    std::scoped_lock lk(s_body_map_mtx);
                    s_body_to_char[static_cast<std::uintptr_t>(a1)] = static_cast<int>(bucket);
                }

                // Deliberately NOT registered as a sweep node here.
                //
                // `bucket` is the ACTIVE character, but this hook also fires while the engine assembles OTHER
                // bodies, and a substitution can match there because carriers and source meshes are shared between
                // characters. Registering a1 on a match therefore filed one character's body under another's bucket,
                // and that character's post-apply sweep then detached wrappers from a body it does not own.
                //
                // The part-list merge hook classifies nodes by their appearance path, which is per-actor and cannot
                // confuse two bodies. That is the only source the sweep's node set takes.
            }
            else
            {
                // Active bucket missed: the engine is unlinking a body OTHER than the active character (e.g.
                // Damiane's body torn down during a switch to Oongka - the orphan path). Find which OTHER bucket(s)
                // own this wrapper. Exactly ONE owner is UNAMBIGUOUS, so the substitution is correct with zero
                // cross-talk (there is only one possible target). Only MULTIPLE owners are ambiguous - the same
                // shared carrier swapped to DIFFERENT targets on more than one character. In that case disambiguate
                // by a1 (the body under process, learned during its own assembly). If a1 is unknown, SKIP rather than
                // risk an unlink of the wrong body's mesh. This is why keying by the wrapper's owning bucket, not by
                // s_active_char_idx, is cross-talk-free: a shared wrapper is the ONLY case that can collide, and it is
                // handled explicitly.
                std::size_t match_bucket = 3;
                std::size_t match_count = 0;
                {
                    std::scoped_lock lk(s_map_mtx);
                    for (std::size_t b = 0; b < 3; ++b)
                    {
                        if (b == bucket)
                            continue;
                        const auto it2 = s_swap_map_per_char[b].find(orig_hash);
                        if (it2 != s_swap_map_per_char[b].end())
                        {
                            ++match_count;
                            match_bucket = b;
                            tgt = it2->second.tgt_wrapper;
                        }
                    }
                }
                if (match_count == 0)
                    continue; // not one of our sources in any bucket
                if (match_count > 1)
                {
                    // Shared source in multiple buckets -> resolve by the body, or skip to avoid cross-talk.
                    int body_bucket = -1;
                    {
                        std::scoped_lock lk(s_body_map_mtx);
                        const auto bit = s_body_to_char.find(static_cast<std::uintptr_t>(a1));
                        if (bit != s_body_to_char.end())
                            body_bucket = bit->second;
                    }
                    tgt = 0;
                    if (body_bucket >= 0 && static_cast<std::size_t>(body_bucket) != bucket)
                    {
                        std::scoped_lock lk(s_map_mtx);
                        auto &bm = s_swap_map_per_char[static_cast<std::size_t>(body_bucket)];
                        const auto it2 = bm.find(orig_hash);
                        if (it2 != bm.end())
                        {
                            tgt = it2->second.tgt_wrapper;
                            match_bucket = static_cast<std::size_t>(body_bucket);
                        }
                    }
                    if (tgt == 0)
                    {
                        logger.trace(
                            "[natpipe-hook] hit#{} entry[{}] src 0x{:X} owned by {} buckets, a1=0x{:X} "
                            "unresolved - SKIP (avoid cross-talk)",
                            hit_seq,
                            i,
                            orig,
                            match_count,
                            static_cast<std::uint64_t>(a1)
                        );
                        continue;
                    }
                }
                static constexpr const char *char_name[3] = {
                    "Kliff",
                    "Damiane",
                    "Oongka",
                };
                logger.info(
                    "[prefab-swap] cleanup: unlinked orphaned body-mesh swap on {}'s body as it was torn "
                    "down (target 0x{:X} <- src 0x{:X}, {} owner) - fixes the persistent fake-part after a "
                    "drop / character switch",
                    (match_bucket < 3 ? char_name[match_bucket] : "?"),
                    tgt,
                    orig,
                    match_count
                );
            }

            const auto entry_write = DMK::memory::write_in_place<std::uint64_t>(
                DMK::Address{reinterpret_cast<std::uintptr_t>(&list_data[i * 2])},
                static_cast<std::uint64_t>(tgt)
            );
            if (entry_write)
            {
                ++subst_count;
                logger.trace(
                    "[natpipe-hook] hit#{} entry[{}] SUBST 0x{:X} -> 0x{:X} (src -> tgt) caller_ra=0x{:X}",
                    hit_seq,
                    i,
                    orig,
                    tgt,
                    caller_ra
                );
            }
            else
            {
                logger.warning("[natpipe-hook] hit#{} entry[{}] write FAULTED - skipping", hit_seq, i);
            }
        }

        s_natpipe_subst_count.fetch_add(subst_count, std::memory_order_relaxed);

        // Run the natural pipeline. Engine walks parent+88 looking for our target wrappers, finds them, unlinks them.
        const auto result = trampoline(a1, a2, a3);

        // No substitutions -> no restore needed and no log output. Fall through and return without further work.
        if (subst_count == 0)
            return result;

        // Restore originals so the caller's refcount-release loop on the list decrements the same wrappers it
        // incremented.
        std::uint32_t restored = 0;
        for (std::uint32_t i = 0; i < cnt; ++i)
        {
            if (saved[i] == 0)
                continue;
            const auto cur =
                DMK::memory::read<std::uint64_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(&list_data[i * 2])})
                    .value_or(0);
            if (cur == saved[i])
                continue; // not substituted
            if (DMK::memory::write_in_place<std::uint64_t>(
                    DMK::Address{reinterpret_cast<std::uintptr_t>(&list_data[i * 2])},
                    saved[i]
                ))
                ++restored;
        }
        logger.trace(
            "[natpipe-hook] hit#{} done: substituted {} restored {} result=0x{:X}",
            hit_seq,
            subst_count,
            restored,
            static_cast<std::uintptr_t>(result)
        );
        return result;
    }

    // Init / shutdown

    void register_config() noexcept
    {
        // Body-mesh swap has no INI keys. The hook installs at boot. Source defaults derive at runtime from each
        // character's carrier item in carrier_defaults.hpp::CARRIERS. Target selection is overlay-driven.
    }

    // Self-arm test path
    //
    // Arms the swap map WITHOUT going through the apply pipeline, which is the only way to test the question that
    // matters: can the engine's own assembly install a substitution keyed on the item the player is really wearing?
    //
    // Routing this through the overlay picker cannot answer that. Picking a prefab auto-applies, the apply installs a
    // carrier fake, and the tear-down removes the real item - so by the time any natural assembly runs there is no
    // real item left underneath and the real-item sources in the map describe something that is no longer equipped.
    // The test has to arm and then stay completely out of the way.
    //
    // Sources: every prefab of the REAL equipped helm (all rigs). Target: one hardcoded resident prefab. No apply, no
    // carrier, no tear-down, no bypass. The player then plays normally, and any substitution logged with
    // in_transmog=0 is the engine that carries our swap on its own.

    bool init(DMK::hook::HookStack &hooks)
    {
        auto &logger = DMK::log();

        // RipRelative singleton cascades
        //
        // Three data-pointer cascades resolve StringInfoRegistry, StringInfoVtable and LoaderRegistry. They store
        // into atomic globals consumed by walk_string_info, the loader-registry enumerator and the vtable sentinel
        // filter. Cascade misses are non-fatal: each consumer treats a zero atomic as a soft bypass (the catalog walk
        // returns empty, the vtable filter accepts all entries) so the rest of init() still completes.
        {
            const auto si_reg = anchor_address(AnchorId::StringInfoRegistry);
            if (si_reg)
            {
                s_string_info_registry.store(si_reg, std::memory_order_release);
                logger.debug("[prefab-swap] StringInfoRegistry resolved at 0x{:X}", si_reg);
            }
            else
            {
                logger.warning("[prefab-swap] StringInfoRegistry cascade FAILED - catalog walk will return 0 entries.");
            }

            const auto si_vt = anchor_address(AnchorId::StringInfoVtable);
            if (si_vt)
            {
                s_string_info_vtable.store(si_vt, std::memory_order_release);
                logger.debug("[prefab-swap] StringInfoVtable resolved at 0x{:X}", si_vt);
            }
            else
            {
                logger.warning("[prefab-swap] StringInfoVtable cascade FAILED - StringInfo entry filter degraded.");
            }

            const auto loader_reg = anchor_address(AnchorId::LoaderRegistry);
            if (loader_reg)
            {
                s_loader_registry_singleton.store(loader_reg, std::memory_order_release);
                logger.debug("[prefab-swap] LoaderRegistry resolved at 0x{:X}", loader_reg);
            }
            else
            {
                logger.warning(
                    "[prefab-swap] LoaderRegistry cascade FAILED - AppearanceTableLoader enumeration disabled."
                );
            }
        }

        // Every code address below arrives from the anchor registry, whose code_site validator already rejected a
        // value outside the host image or on a byte that cannot begin an instruction. That is the backstop the
        // walk-back rows need: a drifted prologue length resolves them short while the pattern still matches, and an
        // inline detour written mid-instruction is a delayed crash somewhere unrelated. A rejected anchor arrives as
        // 0 and the feature degrades instead.
        const auto addr = anchor_address(AnchorId::StructCopy);
        if (!addr)
        {
            logger.warning("[prefab-swap] AOB scan failed - feature disabled");
            return false;
        }

        auto struct_copy = DMK::hook::inline_at(
            DMK::hook::InlineRequest{
                .name = "PrefabWrapperSwap_StructCopy",
                .target = DMK::Address{addr},
            },
            &on_struct_copy
        );
        if (!struct_copy)
        {
            logger.warning("[prefab-swap] hook creation failed at 0x{:X}: {}", addr, struct_copy.error().message());
            return false;
        }

        // Publish the trampoline BEFORE enable() arms the patch, so no game thread can enter the detour while its
        // original pointer is still null.
        s_orig = struct_copy->original<StructCopyFn>();
        if (auto armed = struct_copy->enable(); !armed)
        {
            logger.warning("[prefab-swap] hook could not be armed at 0x{:X}: {}", addr, armed.error().message());
            s_orig = nullptr;
            return false;
        }
        hooks.push(std::move(*struct_copy));

        s_active.store(false, std::memory_order_release);

        // Hook gates on Transmog::in_transmog() so real-item flow is untouched - semantic invariant, not session
        // state.
        logger.info(
            "[prefab-swap] installed at 0x{:X} (INACTIVE - press the toggle hotkey to resolve pairs and activate).",
            addr
        );

        // Natural-pipeline unlink hook. Substitutes src wrappers with target wrappers in the input list before the
        // engine walks parent+88 looking for matches. Resolved through the natural_pipeline() cascade (3
        // anchors, see aob_resolver.hpp). On cascade failure the helm/cloak leak persists, but the rest of the mod
        // still loads.
        {
            const auto natpipe_abs = anchor_address(AnchorId::NaturalPipeline);
            if (!natpipe_abs)
            {
                logger.warning(
                    "[prefab-swap] NaturalPipeline AOB resolve "
                    "FAILED - helm/cloak leak will persist. Other swap features remain active."
                );
            }
            else
            {
                // The target is the pre-unlink wrapper-list walker. The hook substitutes src -> tgt at entry.
                logger.debug("[prefab-swap] NaturalPipeline resolved at 0x{:X}", natpipe_abs);
                // The detour is a no-op when LT swap is OFF. While active, it substitutes src wrappers with
                // target wrappers in the engine's natural unlink list.
                auto natpipe = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "PrefabWrapperSwap_NaturalPipeline",
                        .target = DMK::Address{natpipe_abs},
                    },
                    &on_natural_pipeline
                );
                if (!natpipe)
                {
                    logger.warning(
                        "[prefab-swap] NaturalPipeline hook creation FAILED ({}) - helm/cloak leak will persist.",
                        natpipe.error().message()
                    );
                }
                else
                {
                    s_orig_natural_pipeline = natpipe->original<NaturalPipelineFn>();
                    if (auto armed = natpipe->enable(); !armed)
                    {
                        logger.warning(
                            "[prefab-swap] NaturalPipeline hook could not be armed ({}) - helm/cloak "
                            "leak will persist.",
                            armed.error().message()
                        );
                        s_orig_natural_pipeline = nullptr;
                    }
                    else
                    {
                        hooks.push(std::move(*natpipe));
                    }
                }
            }
        }

        // Boot-time auto-scan. Start a worker that waits for the world, then walks StringInfo to populate the
        // per-slot catalog. That also triggers the heap-walk merge for parallel-pool wrappers, and it attempts the
        // per-character source seed derived from each carrier's runtime meshes. The catalog is the single source of
        // truth for the picker UI and for the apply-time swap-map rebuild.
        try
        {
            s_boot_scan_worker.emplace(
                "LtPwsBootScan",
                [](std::stop_token stop)
                {
                    auto &log = DMK::log();
                    // Wait for world ready, with no attempt cap: a user can sit at the main menu indefinitely and the
                    // catalog must still populate whenever they finally load. The stop token bounds the wait at
                    // teardown, so a dev reload can unmap the image while the process keeps running.
                    while (!Transmog::is_world_ready())
                    {
                        if (stop.stop_requested())
                        {
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    log.info("[prefab-swap] boot-scan: world ready, populating per-slot catalog...");
                    populate_slot_catalogs();

                    // Re-sync the active preset's body-mesh selections now that the catalog is populated. Presets
                    // loaded before the heap walk finished have unresolved prefab_name values, and this retroactive
                    // apply lands them. Then trigger the apply pipeline so the swap activates. This mirrors the
                    // load-time auto-apply path the item-name table uses when its deferred catalog scan completes.
                    if (stop.stop_requested())
                    {
                        return;
                    }
                    Transmog::PresetManager::instance().apply_to_state();
                    Transmog::manual_apply();
                    log.info("[prefab-swap] boot-scan: preset prefabs re-synced and apply scheduled.");
                }
            );
        }
        catch (const std::exception &e)
        {
            DMK::log().warning("[prefab-swap] could not start the boot-scan worker: {}", e.what());
        }

        // Claim-removal observation hook. See on_remove_claims - observational only, no behavior change.
        {
            const auto rc_addr = anchor_address(AnchorId::UnlinkByWrapper);
            if (!rc_addr)
            {
                logger.warning("[claim-remove] AOB failed - claim-layer removal observation unavailable");
            }
            else
            {
                auto unlink = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "UnlinkByWrapper",
                        .target = DMK::Address{rc_addr},
                    },
                    &on_remove_claims
                );
                if (!unlink)
                {
                    logger.warning(
                        "[claim-remove] hook creation failed at 0x{:X}: {}",
                        rc_addr,
                        unlink.error().message()
                    );
                }
                else
                {
                    s_orig_unlink_by_wrapper = unlink->original<UnlinkByWrapperFn>();
                    if (auto armed = unlink->enable(); !armed)
                    {
                        logger.warning(
                            "[claim-remove] hook could not be armed at 0x{:X}: {}",
                            rc_addr,
                            armed.error().message()
                        );
                        s_orig_unlink_by_wrapper = nullptr;
                    }
                    else
                    {
                        hooks.push(std::move(*unlink));
                        logger.debug("[claim-remove] hooked at 0x{:X} (observational)", rc_addr);
                    }
                }
            }
        }

        // Per-actor scoping hook. Publishes which protagonist (if any) the assembly running on this thread belongs to,
        // so the struct-copy substitution picks the right per-character bucket instead of assuming LT drove the apply.
        // Purely observational: it reads one field and sets a thread-local, then calls straight through.
        {
            const auto merge_addr = anchor_address(AnchorId::PartListMerge);
            if (!merge_addr)
            {
                logger.warning(
                    "[prefab-swap] PartListMerge AOB failed - per-actor scoping unavailable; falling back "
                    "to the active-character index (correct only while LT drives every apply)."
                );
            }
            else
            {
                auto merge = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "PartListMerge",
                        .target = DMK::Address{merge_addr},
                    },
                    &on_part_list_merge
                );
                if (!merge)
                {
                    logger.warning(
                        "[prefab-swap] PartListMerge hook creation failed ({}) - per-actor scoping unavailable",
                        merge.error().message()
                    );
                }
                else
                {
                    s_orig_part_list_merge = merge->original<PartListMergeFn>();
                    if (auto armed = merge->enable(); !armed)
                    {
                        logger.warning(
                            "[prefab-swap] PartListMerge hook could not be armed ({}) - per-actor scoping unavailable",
                            armed.error().message()
                        );
                        s_orig_part_list_merge = nullptr;
                    }
                    else
                    {
                        hooks.push(std::move(*merge));
                        logger.info(
                            "[prefab-swap] PartListMerge hooked at 0x{:X} - per-actor scoping active",
                            merge_addr
                        );
                    }
                }
            }
        }

        return true;
    }

    // Defined below, next to deactivate_for_clear which is its other caller.
    static void sweep_stale_visuals(
        std::string_view reason,
        const std::unordered_set<std::uintptr_t> *src_per_char,
        std::size_t &unlinked,
        std::size_t &attempted
    ) noexcept;

    void shutdown()
    {
        // Stop the boot scan first: it polls for a world that can never load, and an unstopped poll holds this
        // image mapped across a main-menu reload. ~StoppableWorker requests stop and joins, so the reset IS the join.
        s_boot_scan_worker.reset();

        // Sweep BEFORE tearing down state. On a dev hot-reload the game keeps running, so the meshes LT installed stay
        // attached. The reloaded DLL starts with empty target sets and no captured body, so it can never identify
        // them again. This is the last moment that knowledge exists.
        //
        // Ordering matters: the trampolines and target sets used by the sweep are cleared further down this function.
        {
            std::size_t unlinked = 0;
            std::size_t attempted = 0;
            sweep_stale_visuals("shutdown", nullptr, unlinked, attempted);
            if (attempted > 0)
            {
                DMK::log().info(
                    "[prefab-swap] shutdown sweep: unlinked {} record(s) from {} attached visual(s)",
                    unlinked,
                    attempted
                );
            }
        }

        s_active.store(false, std::memory_order_release);
        s_orig = nullptr;
        {
            std::scoped_lock lk(s_subst_log_mtx);
            s_subst_log.clear();
        }
        {
            std::scoped_lock lk(s_catalog_mtx);
            for (auto &v : s_slot_catalogs)
                v.clear();
            s_sel_src_idx.fill(-1);
            s_sel_tgt_idx.fill(-1);
        }
        s_catalog_populated.store(false, std::memory_order_release);
        std::scoped_lock lk(s_map_mtx);
        for (auto &m : s_swap_map_per_char)
            m.clear();
        for (auto &s : s_target_wrappers_per_char)
            s.clear();
        for (auto &s : s_direct_fakes_per_char)
            s.clear();
        s_call_count.store(0, std::memory_order_relaxed);
        s_subst_count.store(0, std::memory_order_relaxed);

        // Reset AppearanceTableLoader capture state. Do NOT null the lookup function pointers - they are
        // trampoline-resolved addresses and HookManager owns the trampoline lifetime. The next init() re-resolves
        // them.
    }

    void notify_apply_starting(const std::uint16_t (&itemIds)[5])
    {
        // Arm the chokepoint census. Dump whatever the PREVIOUS window accumulated first: the substitutions that
        // matter arrive on the engine's async rebuild, milliseconds AFTER notify_apply_finished returns, so the
        // window deliberately stays open past the end of an apply and is only closed here, by the next one. The
        // line finished() prints is the apply itself. This one is the apply plus its rebuild tail.
        if (s_census_armed.load(std::memory_order_relaxed))
            log_census("tail");
        {
            std::scoped_lock ck(s_census_mtx);
            s_census_seen.clear();
            s_census_hit_keys.clear();
            s_census_rej_seen.clear();
            // Clear the saturation mirrors under the same lock that empties the lists, so the detour can never see
            // "full" against a list that has already been reset.
            s_census_seen_full.store(false, std::memory_order_relaxed);
            s_census_rej_full.store(false, std::memory_order_relaxed);
        }
        s_census_raw.store(0, std::memory_order_relaxed);
        s_census_calls.store(0, std::memory_order_relaxed);
        s_census_scope_reject.store(0, std::memory_order_relaxed);
        s_census_hash_fail.store(0, std::memory_order_relaxed);
        s_census_miss.store(0, std::memory_order_relaxed);
        s_census_hit.store(0, std::memory_order_relaxed);
        s_census_armed.store(true, std::memory_order_relaxed);
        // Which character this window is for. set_active_char_idx runs ahead of the apply, so this is already bound.
        s_census_bucket.store(s_active_char_idx.load(std::memory_order_acquire), std::memory_order_relaxed);
        // Apply-only activation lifecycle. Mirrors the carrier hybrid pattern: picker mutations only update
        // s_sel_src_idx/s_sel_tgt_idx (pending state). The actual swap-map rebuild and activation happen here, at the
        // start of each apply pass. If the user cleared all selections, this deactivates cleanly.
        // Park direct fakes BEFORE anything else, and unconditionally.
        //
        // deactivate_for_clear is the other parking site, but it bails on `!s_active` - and a direct fake needs no
        // swap, so a character wearing only direct fakes never activates the swap at all. Clearing one then parked
        // nothing, left the sweep with nothing to subtract, and the mesh stayed on forever. Parking here instead ties
        // the cycle to the apply itself: the slot applies that follow re-register whatever is still selected, and the
        // post-apply sweep treats the remainder as orphans.
        {
            std::scoped_lock lk(s_map_mtx);
            for (std::size_t ci = 0; ci < 3; ++ci)
            {
                if (s_direct_fakes_per_char[ci].empty())
                    continue;
                s_pending_stale_per_char[ci].insert(
                    s_direct_fakes_per_char[ci].begin(),
                    s_direct_fakes_per_char[ci].end()
                );
                s_direct_fakes_per_char[ci].clear();
            }
        }

        if (!s_orig)
            return; // hook not installed - nothing to do

        // Decide between "this apply has fakes to install" and "this is a cleanup-only pass" based on `itemIds`, NOT on
        // has_any_selection().
        //
        // Why: has_any_selection() reads the picker's s_sel_src_idx/s_sel_tgt_idx state, which only tracks the most
        // recent dropdown choice. It is decoupled from the user's Enabled toggle and from the per-slot mapping.active
        // flags. A cleanup-only pass (Enabled off, or every slot unticked - both arrive here with itemIds = {0, 0, 0,
        // 0, 0}) therefore still reports a selection and re-arms the swap map.
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

        // `itemIds` covers the five armor slots only. Every other enabled slot - Necklace, Lantern, Glasses, Mask,
        // Backpack - gets its visual from the same swap now that the carrier is equipped as itself, so judging the
        // pass by the armor array alone reads a Necklace-only apply as cleanup-only. The swap then never arms, no swap
        // map is built, and the carrier's own mesh is what renders.
        //
        // Consult the full per-slot mapping for the remainder. Same test, wider input: a slot is a fake to install
        // when it is ticked AND names a target item.
        if (!any_active_fake)
        {
            for (const auto &m : Transmog::slot_mappings())
            {
                if (m.active && m.target_item_id != 0)
                {
                    any_active_fake = true;
                    break;
                }
            }
        }
        if (any_active_fake)
        {
            // At least one fake will be installed in this pass, so run the regular rebuild + activate cycle.
            (void)reactivate_with_selections();
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
        // needed - every apply rebuilds the swap map fresh from selections).
        std::scoped_lock lk(s_last_apply_mtx);
        std::memcpy(s_last_apply_items, itemIds, sizeof(s_last_apply_items));
        s_last_apply_valid = true;
    }

    /**
     * @brief Resolve an item id to every catalog wrapper instance that backs its meshes.
     *
     * It searches ALL slot catalogs, not one: catalogs are per-slot, and an item's meshes are filed under the slot
     * they natively belong to, which the item id alone cannot name here.
     */
    static void collect_wrappers_for_item(std::uint16_t item_id, std::unordered_set<std::uintptr_t> &out) noexcept
    {
        const auto meshes = Transmog::variant_meshes_for_item(item_id);
        if (meshes.empty())
            return;
        std::scoped_lock ck(s_catalog_mtx);
        for (const auto &mesh : meshes)
        {
            if (mesh.empty())
                continue;
            for (const auto &cat : s_slot_catalogs)
            {
                bool found = false;
                for (const auto &ce : cat)
                {
                    if (ce.name != mesh)
                        continue;
                    for (const auto w : ce.wrappers)
                        if (DMK::memory::is_plausible_ptr(DMK::Address{w}))
                            out.insert(w);
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
        }
    }

    void register_direct_fake(std::uint16_t item_id) noexcept
    {
        if (item_id == 0)
            return;
        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
            return;

        std::unordered_set<std::uintptr_t> wrappers;
        collect_wrappers_for_item(item_id, wrappers);
        if (wrappers.empty())
        {
            DMK::log().trace("[prefab-swap] direct-fake 0x{:04x}: no catalog wrapper resolved", item_id);
            return;
        }
        {
            std::scoped_lock lk(s_map_mtx);
            auto &set = s_direct_fakes_per_char[static_cast<std::size_t>(active_idx - 1)];
            set.insert(wrappers.begin(), wrappers.end());
        }
        DMK::log().trace(
            "[prefab-swap] direct-fake 0x{:04x}: registered {} wrapper(s) for char {}",
            item_id,
            wrappers.size(),
            active_idx
        );
    }

    /**
     * @brief Remove visuals that the just-completed apply did NOT re-install.
     *
     * It runs after the install, so a slot whose target is unchanged keeps its wrapper in the new target set and
     * drops out. Only genuinely-orphaned wrappers - changed slots, cleared slots - detach. That is also what makes
     * the apply feel immediate: the new visual is already on screen before any removal.
     */
    static void sweep_pending_stale() noexcept
    {
        std::unordered_set<std::uintptr_t> victims_per_char[3];
        bool any = false;

        // "Active + none" (hide a slot). Nothing LT installed is involved: the mesh to remove is the REAL item's, so
        // the pending-stale set can never contain it. Resolve the real item's prefabs and add them as victims, so the
        // same detach that removes LT's own visuals also clears a hidden slot.
        //
        // This is the one case `SafeTearDown` was still doing on LT's behalf - it resolved the equipped item's
        // wrappers via `ExpandToMeshes`. `variant_meshes_for_item` answers the same question.
        {
            const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
            const auto a1 = Transmog::player_a1().load(std::memory_order_acquire);
            if (active_idx >= 1 && active_idx <= 3 && a1)
            {
                const auto ci = static_cast<std::size_t>(active_idx - 1);
                for (std::size_t i = 0; i < static_cast<std::size_t>(Transmog::TransmogSlot::Count); ++i)
                {
                    const auto &m = Transmog::slot_mappings()[i];
                    if (!m.active || m.target_item_id != 0)
                        continue; // only "ticked, but no target" means hide
                    const auto game_tag = Transmog::game_slot_from_transmog(static_cast<Transmog::TransmogSlot>(i));
                    const auto real_id = Transmog::real_part_tear_down::get_real_item_id(
                        reinterpret_cast<void *>(a1),
                        static_cast<std::uint16_t>(game_tag)
                    );
                    if (real_id == 0)
                        continue; // nothing worn there - already hidden

                    std::unordered_set<std::uintptr_t> wrappers;
                    collect_wrappers_for_item(real_id, wrappers);
                    if (!wrappers.empty())
                    {
                        victims_per_char[ci].insert(wrappers.begin(), wrappers.end());
                        any = true;
                    }
                }
            }
        }
        {
            std::scoped_lock lk(s_map_mtx);
            for (std::size_t ci = 0; ci < 3; ++ci)
            {
                for (auto w : s_pending_stale_per_char[ci])
                {
                    // Still installed by the new set -> not stale. Both ledgers count: a slot can be re-filled either
                    // by a substitution (target set) or by re-equipping the item as itself (direct-fake set).
                    if (s_target_wrappers_per_char[ci].find(w) != s_target_wrappers_per_char[ci].end())
                        continue;
                    if (s_direct_fakes_per_char[ci].find(w) != s_direct_fakes_per_char[ci].end())
                        continue;
                    victims_per_char[ci].insert(w);
                    any = true;
                }
                s_pending_stale_per_char[ci].clear();
            }
        }
        if (!any)
            return;

        std::size_t unlinked = 0;
        std::size_t attempted = 0;
        sweep_stale_visuals("post-apply", victims_per_char, unlinked, attempted);
        if (attempted > 0)
        {
            DMK::log().debug(
                "[prefab-swap] post-apply stale sweep: unlinked {} record(s) from {} orphaned visual(s)",
                unlinked,
                attempted
            );
        }
    }

    void notify_apply_finished(const std::uint16_t (&itemIds)[5])
    {
        // Report the census, but leave it ARMED - see notify_apply_starting. Emitted before the `!s_active`
        // early-out below, because the most interesting case - a slot that renders untransmogged - can be one
        // where the swap never activated at all.
        log_census("apply");
        // Sweep BEFORE the s_active gate: a cleanup-only pass (every slot cleared) deactivates, and its parked
        // wrappers still have to be removed.
        //
        // The detach leaves null-owner holes in the node's claim vector, which the engine walks unguarded and
        // faults on when it dereferences `entry+0x08`. sweep_stale_visuals compacts the vector right after the
        // detach, which is what makes this safe to run.
        sweep_pending_stale();

        if (!s_active.load(std::memory_order_acquire))
            return;
        std::scoped_lock lk(s_last_apply_mtx);
        std::memcpy(s_last_apply_items, itemIds, sizeof(s_last_apply_items));
        s_last_apply_valid = true;
    }

    /**
     * @brief Rebuild the target table from the preset when its stamp no longer matches the world and character.
     *
     * Runs on whichever thread reads the table, which includes the engine's part-build thread - so it is a plain
     * comparison in the common case and only does work once per world generation per character.
     *
     * The rebuild discards uncommitted picks and re-derives everything from the active preset, which is the correct
     * meaning of entering a new world: the preset on disk is the truth, and edits that were never committed to it
     * must not dress the new body. Mid-session edits are unaffected because the stamp still matches.
     *
     * FIXME: move the rebuild to the load-detect worker, the same way it owns the body-ownership table, and let
     * this path read a published result.
     *
     * The rebuild runs on whichever thread asked, and the per-socket descriptor detour asks from an engine thread.
     * The resync, the preset re-mirror and the swap-map rebuild therefore land on the frame's critical path. Nothing
     * bounds that cost, and nothing stops a future caller from reaching it more often.
     */
    static void ensure_target_table_current() noexcept
    {
        const auto world_gen = CDCore::world_generation();
        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
            return;

        // Deriving a target needs the per-slot prefab catalog, and on a cold load that arrives in TWO phases: the
        // StringInfo walk, then the loader-registry merge, with the catalog published only after the second. A reader
        // landing in that window resolves every target against an empty catalog, warns "variant meshes ... absent
        // from this slot's catalog (0 entries)" on every slot, and shows the carrier's visual instead.
        //
        // Worse than the noise: the stamp is written BEFORE the rebuild, so that empty result records as current
        // for this (world, character) and no later reader retries it.
        //
        // Deferring is the fix. Leaving the stamp untouched means the next reader - the next socket build or apply,
        // both of which happen right after the catalog lands - rebuilds properly.
        if (!s_catalog_populated.load(std::memory_order_acquire))
            return;

        {
            std::scoped_lock lk(s_target_table_stamp_mtx);
            if (s_target_table_world_gen == world_gen && s_target_table_char_idx == active_idx)
                return;

            // Stamp BEFORE rebuilding. The rebuild re-enters pws and can bind the active character, so a second
            // reader arriving mid-rebuild must not start its own.
            s_target_table_world_gen = world_gen;
            s_target_table_char_idx = active_idx;
        }

        resync_to_preset();
        PresetManager::instance().apply_to_state();
        (void)apply_selections_to_swap_map();

        DMK::log().info(
            "[prefab-swap] target table rebuilt for world {} char[{}] (uncommitted picks discarded)",
            world_gen,
            active_idx - 1
        );
    }

    std::uint32_t target_table_char_idx() noexcept
    {
        std::scoped_lock lk(s_target_table_stamp_mtx);
        return s_target_table_char_idx;
    }

    std::uintptr_t target_wrapper_for_slot(std::size_t slot_idx) noexcept
    {
        if (slot_idx >= Transmog::SLOT_COUNT)
            return 0;

        // Never serve a table that belongs to a different world or character - see ensure_target_table_current.
        ensure_target_table_current();

        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
            return 0;
        std::scoped_lock lk(s_map_mtx);
        return s_slot_target_wrapper_per_char[active_idx - 1][slot_idx];
    }

    void resync_to_preset() noexcept
    {
        // Drop every uncommitted prefab pick, then rebuild the per-slot target table from what remains.
        //
        // The selection rows deliberately SURVIVE a lot - they exist so switching editing character does not throw
        // away work in progress. A save-load is different: the preset on disk is the truth, and a pick that was never
        // committed to it must not dress the new body. PresetManager::apply_to_state re-mirrors the preset's own
        // picks immediately after this, so clearing here loses nothing the preset still asks for.
        //
        // TARGETS ONLY. The source column is not a pick - nothing in the UI writes it, every set_selection caller
        // passes the source straight back in unchanged, and ensure_default_sources_seeded derives it from the
        // character's carrier item exactly once behind a latch that never resets. Clearing it here therefore does not
        // get re-derived: selection_src_index returns -1 for every slot of every character from the first save-load
        // or character switch onward, and stays there. The visible symptom is the picker's "Prefabs" checkbox
        // vanishing everywhere, since that is gated on the slot having a source. The swap itself keeps working, which
        // is what hides the breakage - apply_selections_to_swap_map derives its source from the carrier item rather
        // than from this column. Same reasoning as the note in reset_per_char_state.
        {
            std::scoped_lock lk(s_map_mtx);
            for (auto &row : s_sel_tgt_idx_per_char)
                row.fill(-1);
        }
    }

    void rebuild_target_table() noexcept
    {
        // Unconditional, unlike ensure_armed_for_slot_apply, which bails when no explicit pick exists. The table is
        // also fed by targets DERIVED from each slot's item, so it has to be rebuilt even with no picks at all -
        // otherwise it keeps whatever the last world left in it, and the slot comes up wearing the stale target.
        (void)apply_selections_to_swap_map();
    }

    void park_slot_target_for_sweep(std::uint16_t prev_item_id) noexcept
    {
        if (prev_item_id == 0)
            return;

        const auto active_idx = s_active_char_idx.load(std::memory_order_acquire);
        if (active_idx < 1 || active_idx > 3)
            return; // no character bound - nothing to attribute the parked wrappers to
        const auto ci = static_cast<std::size_t>(active_idx - 1);

        std::unordered_set<std::uintptr_t> wrappers;
        collect_wrappers_for_item(prev_item_id, wrappers);
        if (wrappers.empty())
            return;

        {
            std::scoped_lock lk(s_map_mtx);
            s_pending_stale_per_char[ci].insert(wrappers.begin(), wrappers.end());
        }
        DMK::log().debug(
            "[prefab-swap] slot-apply: parked {} wrapper(s) of replaced target {:#06x}",
            wrappers.size(),
            prev_item_id
        );
    }

    void sweep_after_slot_apply() noexcept
    {
        sweep_pending_stale();
    }

    /**
     * @brief Detach and unlink every visual LT installed, on every body it installed to.
     *
     * Shared by the apply-time deactivate and by shutdown. Shutdown matters for the dev hot-reload path: a reloaded
     * Logic DLL starts with empty target sets and no captured body, so it cannot identify anything as "ours" and the
     * installed meshes stay attached until a save reload rebuilds the body. The sweep on the way out, while that
     * knowledge still exists, is the only point where it can run.
     */
    static void sweep_stale_visuals(
        std::string_view reason,
        const std::unordered_set<std::uintptr_t> *src_per_char,
        std::size_t &unlinked,
        std::size_t &attempted
    ) noexcept
    {
        // Stale-visual sweep. The SubstRecord reverse-write above is structurally unable to do this: its `dest_addr`
        // is `on_struct_copy`'s `a1`, which is a slot in a STAGING VECTOR on the caller's stack - that frame has long
        // since returned by the time we get here, so `revert_one_subst` never validates and `reverted` is always 0.
        //
        // Unlink the wrappers LT actually installed, from the body they were installed on, using the engine's own
        // per-wrapper unlink. That needs neither `SafeTearDown` nor a synthesized NaturalPipeline call.
        if (s_orig_unlink_by_wrapper)
        {
            // Only the character this pass belongs to. Sweeping every bucket let an apply for one protagonist detach
            // on another's body. A "shutdown" reason is the exception, where every body is genuinely going away.
            const auto sweep_idx = s_active_char_idx.load(std::memory_order_acquire);
            const std::size_t sweep_ci =
                (sweep_idx >= 1 && sweep_idx <= 3) ? static_cast<std::size_t>(sweep_idx - 1) : 3;
            const bool all_chars = (src_per_char == nullptr); // shutdown sweep

            for (std::size_t ci = 0; ci < 3; ++ci)
            {
                if (!all_chars && sweep_ci < 3 && ci != sweep_ci)
                    continue;

                std::vector<std::uintptr_t> bodies;
                {
                    std::scoped_lock lk(s_body_node_mtx);
                    bodies.assign(s_body_nodes_per_char[ci].begin(), s_body_nodes_per_char[ci].end());
                }
                for (const auto body : bodies)
                {
                    if (!DMK::memory::is_plausible_ptr(DMK::Address{body}))
                        continue; // never observed assembling - nothing to unlink from

                    std::unordered_set<std::uintptr_t> targets;
                    if (src_per_char)
                    {
                        targets = src_per_char[ci];
                    }
                    else
                    {
                        std::scoped_lock lk(s_map_mtx);
                        targets = s_target_wrappers_per_char[ci];
                        // Shutdown has to take direct fakes with it too - a reloaded Logic DLL starts with empty
                        // ledgers and can no longer identify them as ours.
                        targets.insert(s_direct_fakes_per_char[ci].begin(), s_direct_fakes_per_char[ci].end());
                    }
                    if (targets.empty())
                        continue;

                    // Resolve each target wrapper to its NAME once, so attached records can be matched by identity OR
                    // by name. A prefab has several live wrapper instances in this binary (the `_indexNN` helm variants
                    // and the ready-list instances the catalog cannot name are both examples), so the instance LT
                    // installed into the swap map is frequently NOT the instance sitting in the attached record.
                    std::unordered_set<std::string> target_names;
                    for (auto t : targets)
                    {
                        auto nm = wrapper_inline_name(t);
                        if (nm.empty())
                            nm = wrapper_name_for_log(t); // catalog fallback
                        if (!nm.empty())
                            target_names.insert(std::move(nm));
                    }

                    // Enumerate what is ACTUALLY attached and unlink through each record's OWN identity pointer. Our
                    // swap-map wrapper instead makes the engine's content-keyed walk miss the real record, or match
                    // some other one, so the unlink count is non-zero while the stale mesh stays on screen.
                    const auto data = DMK::memory::read<std::uint64_t>(DMK::Address{body + 0x58}).value_or(0);
                    const auto count = DMK::memory::read<std::uint32_t>(DMK::Address{body + 0x60}).value_or(0);
                    if (!DMK::memory::is_plausible_ptr(DMK::Address{data}) || count == 0 || count > 256)
                        continue;

                    std::vector<std::uintptr_t> victims;
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        // 16-byte entries. The record pointer is the SECOND qword.
                        const auto rec =
                            DMK::memory::read<std::uint64_t>(DMK::Address{data + static_cast<std::size_t>(i) * 16 + 8})
                                .value_or(0);
                        if (!DMK::memory::is_plausible_ptr(DMK::Address{rec}))
                            continue;
                        const auto ident = DMK::memory::read<std::uint64_t>(DMK::Address{rec + 0x40}).value_or(0);
                        if (!DMK::memory::is_plausible_ptr(DMK::Address{ident}))
                            continue;
                        const bool by_ptr = targets.find(static_cast<std::uintptr_t>(ident)) != targets.end();
                        bool by_name = false;
                        if (!by_ptr && !target_names.empty())
                        {
                            // Read the attached instance's own name - it is frequently NOT a catalog instance.
                            auto nm = wrapper_inline_name(static_cast<std::uintptr_t>(ident));
                            by_name = !nm.empty() && target_names.find(nm) != target_names.end();
                        }
                        if (by_ptr || by_name)
                        {
                            // Offer the CANONICAL wrapper, never the attached record's identity.
                            //
                            // The engine's erase does not search the claim vector by pointer. It first resolves the
                            // wrapper to that prefab's key list through a global registry, then binary-searches by key.
                            // A wrapper that is not the registered canonical instance is absent from that registry, so
                            // the lookup fails, the key list comes back empty, and the erase removes NOTHING - which
                            // is the `claims N->N` this sweep has reported all along.
                            //
                            // It also explains why the detach never retracted anything: NaturalPipeline gates its
                            // commit on the erase having matched, so a lookup miss makes the whole call a no-op.
                            //
                            // A pointer match is already a catalog wrapper (the canonical instance). A name match is
                            // not
                            // - the attached record usually carries its own instance - so resolve that back to the
                            // catalog before offering it, and fall back to the identity only when no canonical instance
                            // is known, which is no worse than what this did before.
                            std::uintptr_t canonical = by_ptr ? static_cast<std::uintptr_t>(ident) : 0;
                            if (canonical == 0)
                            {
                                const auto nm = wrapper_inline_name(static_cast<std::uintptr_t>(ident));
                                std::scoped_lock ck(s_catalog_mtx);
                                for (const auto &cat : s_slot_catalogs)
                                {
                                    for (const auto &ce : cat)
                                        if (ce.name == nm && !ce.wrappers.empty() &&
                                            DMK::memory::is_plausible_ptr(DMK::Address{ce.wrappers.front()}))
                                        {
                                            canonical = ce.wrappers.front();
                                            break;
                                        }
                                    if (canonical != 0)
                                        break;
                                }
                            }
                            victims.push_back(canonical != 0 ? canonical : static_cast<std::uintptr_t>(ident));
                        }
                    }

                    if (victims.empty())
                    {
                        // Nothing we were asked to remove is present in the attached-record vector. Report what IS
                        // there, once per distinct shape: a wrapper we cannot find is indistinguishable from one
                        // attached somewhere this enumeration does not reach, and those need different fixes.
                        std::string want;
                        for (const auto &n : target_names)
                            want += (want.empty() ? "" : ", ") + n;
                        std::string have;
                        for (std::uint32_t i = 0; i < count && i < SWEEP_ENUMERATION_CAP; ++i)
                        {
                            const auto rec = DMK::memory::read<std::uint64_t>(
                                                 DMK::Address{data + static_cast<std::size_t>(i) * 16 + 8}
                            )
                                                 .value_or(0);
                            if (!DMK::memory::is_plausible_ptr(DMK::Address{rec}))
                                continue;
                            const auto ident = DMK::memory::read<std::uint64_t>(DMK::Address{rec + 0x40}).value_or(0);
                            auto nm = wrapper_inline_name(static_cast<std::uintptr_t>(ident));
                            if (!nm.empty())
                                have += (have.empty() ? "" : ", ") + nm;
                        }
                        bool fresh = false;
                        {
                            std::scoped_lock lk(s_claim_log_mtx);
                            fresh = s_claim_logged.insert("MISS|" + want).second;
                        }

                        // Say which of the two cases this is, instead of warning about both.
                        //
                        // Victims are matched by POINTER **or** by NAME above, so an empty victim list means none of
                        // the wanted meshes is on this body under either identity. When the enumeration also covered
                        // every attached record, that is a proof of absence: there is nothing to retract and the sweep
                        // received targets that were registered but never installed on this body. The parked
                        // set carries no provenance, so it cannot tell the two apart. That is routine and must not
                        // read as a failure.
                        //
                        // A TRUNCATED enumeration is the case worth a warning: the wrapper can be attached beyond
                        // the records examined, and then a stale mesh really is left on screen.
                        const bool enumeration_complete = count <= SWEEP_ENUMERATION_CAP;
                        if (fresh)
                        {
                            auto &lg = DMK::log();
                            if (enumeration_complete)
                                lg.debug(
                                    "[prefab-swap] sweep no-op ({}): none of [{}] is attached to body 0x{:X} "
                                    "({} record(s), all examined) - nothing to retract",
                                    reason,
                                    want,
                                    body,
                                    count
                                );
                            else
                                lg.warning(
                                    "[prefab-swap] sweep MISS ({}): wanted [{}] but body 0x{:X} has [{}] "
                                    "- enumeration stopped at {} of {} records, so absence is NOT proven",
                                    reason,
                                    want,
                                    body,
                                    have,
                                    SWEEP_ENUMERATION_CAP,
                                    count
                                );
                        }
                        continue;
                    }
                    attempted += victims.size();

                    // Detach FIRST via a synthesized NaturalPipeline call - this is what actually stops the mesh
                    // rendering. One list holding every victim wrapper, plus the empty second list SafeTearDown passes.
                    std::vector<NatpipeEntry16> entries;
                    entries.reserve(victims.size());
                    for (auto v : victims)
                        entries.push_back(NatpipeEntry16{v, 0});

                    NatpipeContainer list{
                        entries.data(),
                        static_cast<std::uint32_t>(entries.size()),
                        static_cast<std::uint32_t>(entries.size()),
                    };
                    NatpipeContainer empty{
                        nullptr,
                        0,
                        0,
                    };

                    // Removal is UnlinkByWrapper alone - it IS the engine's claim erase.
                    //
                    // Given a wrapper it resolves that prefab's key list, binary-searches the claim vector, and for
                    // each match releases the owner, shifts the tail down and decrements the count. The vector's
                    // invariant is maintained by construction.
                    //
                    // Deliberately NO synthesized NaturalPipeline detach ahead of it. That removed nothing in any
                    // measurement, and it nulled owners in place without touching the count - leaving holes the engine
                    // walks unguarded, which crashes on a preset switch. The engine function also returns void, so any
                    // "unlink count" taken from it is not a real number.
                    //
                    // Detach first - this is what retracts the REALIZED part. Erasing the claim afterwards is
                    // bookkeeping. On its own it drops the claim count and leaves the mesh on screen.
                    //
                    // The detach nulls the owner at `entry+0x08`, which makes that entry unmatchable by the erase below
                    // (it compares `owner+0x40`) and leaves a hole the engine's unguarded walk faults on. The
                    // compaction at the end closes exactly those holes, which is what makes running both safe.
                    const auto detach_rc = call_natpipe_outer_seh(body, &list, &empty);

                    // Claim count either side of the erase. The engine function returns void, so the ONLY way to see
                    // whether it matched anything is whether the vector shrank.
                    const auto claims_before = DMK::memory::read<std::uint32_t>(DMK::Address{body + 0x60}).value_or(0);

                    std::size_t erased = 0;
                    for (auto v : victims)
                    {
                        std::uintptr_t wrapper_var = v; // engine dereferences twice - pass the ADDRESS of a local
                        call_unlink_by_wrapper_seh(body, &wrapper_var);
                        ++erased;
                    }
                    unlinked += erased;

                    const auto claims_after = DMK::memory::read<std::uint32_t>(DMK::Address{body + 0x60}).value_or(0);

                    // Belt and braces: if anything still left a null-owner hole, close it before the engine walks it.
                    const auto dropped = compact_claim_vector(body);

                    DMK::log().debug(
                        "[prefab-swap] stale-erase ({}): {} wrapper(s) offered on body 0x{:X} detachRc={} "
                        "claims {}->{} compacted={}",
                        reason,
                        erased,
                        body,
                        detach_rc,
                        claims_before,
                        claims_after,
                        dropped
                    );
                } // per-body
            }
        }
    }

    void deactivate_for_clear()
    {
        if (!s_active.load(std::memory_order_acquire))
            return;
        s_active.store(false, std::memory_order_release);

        // Flush dye-injector counters before the natpipe hook tears down the swap. The injector itself is stateless
        // across teardown (per-slot state is thread-local and consumed once per slotpop), but the counter dump helps
        // post-mortem when diagnosing missing dye records.
        Transmog::dye_record_inject::restore_all();

        auto &logger = DMK::log();

        // Reverse-write every record we substituted: restore its ORIGINAL source wrapper and release the refcount bump
        // the install did on the target. This is the cleanup the SubstRecord ledger exists for (see its doc-block).
        // The natural-pipeline hook only unlinks a target during ACTIVE re-assembly of that slot on the active
        // character, so a DROPPED swap (preset -> none) or a CROSS-CHARACTER teardown never routes through it. The
        // target wrapper then orphans in the scene graph and leaks its refcount, which climbs with every repeated
        // apply. A drain of the ledger here detaches those orphans.
        //
        // Per-record safety, which is why this is immune to the cross-talk a broader unlink hook causes: each
        // SubstRecord carries its OWN dest_addr and orig_wrapper, so the restore is always correct even when characters
        // share a carrier/source. It is self-validating. A record reverts ONLY when its slot STILL holds one of our
        // target wrappers, so a freed / reused / re-substituted record fails that test and is skipped. Every raw
        // access runs through a guarded read or write.
        std::vector<SubstRecord> drained_records;
        {
            std::scoped_lock lk(s_subst_log_mtx);
            drained_records.swap(s_subst_log);
        }
        std::unordered_set<std::uintptr_t> our_targets;
        {
            std::scoped_lock lk(s_map_mtx);
            for (const auto &s : s_target_wrappers_per_char)
                our_targets.insert(s.begin(), s.end());
        }
        std::size_t reverted = 0;
        for (const auto &r : drained_records)
            if (revert_one_subst(r.dest_addr, r.orig_wrapper, our_targets))
                ++reverted;

        // Swap map and target-wrapper sets are PRESERVED for instant re-activation. Only the per-install substitution
        // ledger (drained above) is consumed. A re-arm substitutes fresh records via on_struct_copy.
        // Park the currently-installed set rather than sweeping it now - see s_pending_stale_per_char.
        std::size_t unlinked = 0;
        std::size_t attempted = 0;
        {
            std::scoped_lock lk(s_map_mtx);
            const auto active_idx_now = s_active_char_idx.load(std::memory_order_acquire);
            const std::size_t active_ci =
                (active_idx_now >= 1 && active_idx_now <= 3) ? static_cast<std::size_t>(active_idx_now - 1) : 3;
            for (std::size_t ci = 0; ci < 3; ++ci)
            {
                // ONLY the character being applied. Parking every bucket scheduled the OTHER characters' installed
                // targets for removal, and the sweep then detached them from their own bodies - an apply for one
                // protagonist stripped the others on load.
                if (active_ci < 3 && ci != active_ci)
                    continue;

                s_pending_stale_per_char[ci].insert(
                    s_target_wrappers_per_char[ci].begin(),
                    s_target_wrappers_per_char[ci].end()
                );
                // Direct fakes park on the same terms: the apply that follows re-registers whichever ones are still
                // selected, so anything left unclaimed falls out as an orphan.
                s_pending_stale_per_char[ci].insert(
                    s_direct_fakes_per_char[ci].begin(),
                    s_direct_fakes_per_char[ci].end()
                );
                s_direct_fakes_per_char[ci].clear();
                // Clear the installed set as well. Nothing is installed once this returns: an install pass rebuilds it
                // immediately in apply_selections_to_swap_map, and a cleanup-only pass (a "None" preset, or Clear)
                // has no rebuild at all.
                //
                // A populated set breaks hiding: the post-apply sweep takes "parked MINUS still-installed", and on a
                // cleanup pass the still-installed set is a stale copy of the parked set, so the subtraction cancels
                // every victim and nothing is detached. Shutdown escapes that because it sweeps the installed set
                // directly rather than the difference.
                s_target_wrappers_per_char[ci].clear();
            }
        }

        logger.info(
            "[prefab-swap] DEACTIVATED - reverted {} substitution(s); stale-sweep unlinked {} record(s) from "
            "{} target(s); swap map RETAINED for next activation.",
            reverted,
            unlinked,
            attempted
        );
    }

} // namespace Transmog::prefab_wrapper_swap
