#include "aob_resolver.hpp"
#include "auth_table.hpp"
#include "item_name_table.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog_map.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Transmog::RealPartTearDown
{
    namespace
    {
        /**
         * @brief Is scene-graph tear-down needed for this slot on THIS apply?
         *
         * @details Tear-down runs per slot, not per apply. It costs a visible stall, so it fires only where it
         *          removes something. Two cases need it, and only those:
         *
         *          - The slot ends EMPTY (`!active`, or ticked with no target). There is no new target to redirect to
         *            and the carrier is unequipped, so the visual has to come off through the engine. It also stops
         *            the engine going on believing the slot is equipped.
         *          - The slot gains its FIRST fake. What is installed is the REAL part, which the post-apply sweep
         *            cannot see (it only knows wrappers LT installed), and the carrier equip does not displace it.
         *
         *          A slot that changes from one target to another is left to the sweep, whose previous target is
         *          LT-installed. A tear-down here instead makes every target change pay the stall, which is exactly
         *          the cost this gate exists to avoid, so a sweep that fails to detach is fixed as a sweep bug rather
         *          than hidden behind a tear-down.
         *
         * @param gameSlotTag Engine slot tag under consideration.
         * @return true when the caller must run the engine tear-down for this slot.
         *
         * @note A backpack's strap and holder belong to neither carrier nor target, because an item emits exactly one
         *       mesh, and the engine attaches them whenever a bag is worn. They are live parts, so no tear-down
         *       removes them and none must.
         */
        [[nodiscard]] bool tear_down_needed_for_slot(std::uint16_t gameSlotTag) noexcept
        {
            const auto tmSlot = Transmog::slot_from_game_slot(static_cast<std::int16_t>(gameSlotTag));
            if (!tmSlot.has_value())
            {
                // Unknown tag - no mapping owns it, so LT is not filling it. Treat as empty and tear down: a skip
                // risks leaving a visual with nothing tracking it.
                return true;
            }

            const auto idx = static_cast<std::size_t>(*tmSlot);
            const auto &mapping = Transmog::slot_mappings()[idx];
            if (!mapping.active || mapping.targetItemId == 0)
                return true;

            return Transmog::last_applied_ids()[idx] == 0;
        }

        // Helper function pointer types. Addresses come from Transmog::resolved_addrs() (populated elsewhere in
        // init()):
        //
        // SafeTearDown - engine scene-graph tear-down. It calls an internal scene-detach primitive. It does NOT
        //   mutate the authoritative equip table at a1+AuthTable::k_containerPtrOffset. The function is AOB-resolved,
        //   so a reader who cross-checks against a disassembler database must match the byte pattern in
        //   safe_tear_down(), never a name.
        //
        // IndexedStringLookup - short->hash lookup. It takes the address of a uint16_t slot id. It returns a
        //   pointer whose first DWORD is the descriptor hash used by the rest of the equip pipeline. That hash is
        //   both SafeTearDown's second argument and the validity gate on the item id: a lookup that yields no
        //   pointer, or a hash of 0 or 0xFFFFFFFF, means the id names nothing and the tear-down is skipped.

        // SafeTearDown's second argument is the resolved 32-bit descriptor hash, never the 16-bit item id. Passing
        // the id makes the call a silent no-op: it matches no part, detaches nothing, and still returns normally.
        using SafeTearDownFn = std::int64_t(__fastcall *)(std::int64_t a1, std::uint32_t hash, std::int16_t slotTag);

        using IndexedStringLookupFn = void *(__fastcall *)(const std::uint16_t *slotIdPtr);

        /**
         * @brief Warn when SafeTearDown is about to run against a part list that is already empty.
         *
         * @details Mirrors the chain the engine walks on entry - component at [a1+0x08], its sub-object at +0x68,
         *          and that object's part-list head at +0x40 - and reports the one state that is invisible from
         *          the outside: a null head means the call cannot detach anything whatever the arguments say, and
         *          the function still returns normally. A populated list is the normal path and is not logged, so
         *          the line only appears when it means something.
         */
        void log_safe_tear_down_state(
            std::uintptr_t a1,
            std::uint32_t hash,
            std::uint16_t gameSlotTag,
            const char *site
        ) noexcept
        {
            const auto comp = DMK::memory::read<std::uintptr_t>(DMK::Address{a1 + 0x08}).value_or(0);
            const auto sub = comp ? DMK::memory::read<std::uintptr_t>(DMK::Address{comp + 0x68}).value_or(0) : 0;
            const auto listHead = sub ? DMK::memory::read<std::uintptr_t>(DMK::Address{sub + 0x40}).value_or(0) : 0;

            if (listHead == 0)
            {
                // try_log, not warning(): this helper runs on the apply path and is noexcept, so a formatting or
                // sink failure must not escape into the engine's frame.
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "[dispatch] {} slot={:#06x} hash={:#010x} comp={:#x} sub={:#x} listHead=0 - the engine exits "
                    "without detaching, whatever the arguments say",
                    site,
                    gameSlotTag,
                    hash,
                    comp,
                    sub
                );
            }
        }

        std::atomic<SafeTearDownFn> g_safeTearDown{nullptr};
        std::atomic<IndexedStringLookupFn> g_indexedStringLookup{nullptr};
        std::atomic<bool> g_ready{false};

        // One-shot sanity log of the first PartDef entry so a future patch reshaping the struct is immediately visible
        // in the log (the container ptr + entry offsets are not AOB-anchored).
        std::atomic<bool> g_loggedFirstEntry{false};

        // Auth-table verbose dump tracker. It fires the full enumeration once per distinct (a1, count) pair, so
        // character switches and table resizes re-emit the slot inventory. A session of stable equip state does not
        // spam the log on every tear-down call. The dump answers the slot-discovery question: does a character hold
        // live entries for tags beyond the documented Helm/Chest/Gloves/Boots/Cloak (0x03/0x04/0x05/0x06/0x10) set?
        // The dump function itself is defined below the layout constants because it reads them.
        std::atomic<std::uintptr_t> g_lastDumpedA1{0};
        std::atomic<std::uint32_t> g_lastDumpedCount{0};
        // Hash over the entry-array's primary IDs so the dedupe also catches gear changes within the same (a1, count).
        // The auth table's `count` is the slot-array capacity, not the equipped count. Empty slots persist as 0xFFFF
        // sentinels, so equip and unequip never bump `count`. Without the content hash, the dedupe key never re-fires
        // after the first dump per actor.
        std::atomic<std::uint64_t> g_lastDumpedContentHash{0};

        // Runtime struct layout
        //
        // The auth-table geometry itself (container pointer, array base, count, entry stride and field offsets) is in
        // auth_table.hpp, along with the patch-day analysis. It is shared because the whole struct moves as a unit and
        // three files walk it.
        //
        // The plausibility bounds below are local: they gate values read OUT of that table, and the sanity checks
        // (pointers through memory::is_plausible_ptr, count <= k_maxPlausibleEntries, slotTag in range) bail out
        // before touching anything dangerous if a future patch reshapes the struct.
        //
        // The alt item word at +0x88 used by older layouts is no longer read; the primary word is sufficient.

        constexpr std::uint32_t k_maxPlausibleEntries = 0x1000;
        // Slot-tag range covers the full engine taxonomy. This is a plausibility gate on a value read out of the auth
        // table, not a list of tags LT manages, so it stays inclusive of tags that map to no TransmogSlot. The upper
        // bound must cover the highest tag the engine emits, not the highest LT acts on: a tag above the bound is
        // rejected with a warning, so a bound left behind the engine turns a newly added slot into a confusing
        // rejection rather than a clean no-op. Re-read it from the auth-table dump when the engine gains a slot.
        constexpr std::uint16_t k_minPlausibleSlotTag = 0x0000;
        constexpr std::uint16_t k_maxPlausibleSlotTag = 0x0018;

        [[nodiscard]] bool plausible_slot_tag(std::uint16_t tag) noexcept
        {
            return tag >= k_minPlausibleSlotTag && tag <= k_maxPlausibleSlotTag;
        }

        /// A live auth-table entry holds a real item word and a non-zero gate. This is the only copy of the rule.
        [[nodiscard]] constexpr bool entry_is_live(std::uint16_t primary, std::uint64_t gate) noexcept
        {
            return primary != 0 && primary != 0xFFFF && gate != 0;
        }

        /**
         * @brief Structural view of the auth table hanging off one ClientEquipSlotActorComponent.
         * @details `container` stays 0 when the component itself reads back implausible, which lets a caller tell a
         *          bad component apart from a reshaped container. `valid` additionally requires a plausible array
         *          base and a count inside `[1, k_maxPlausibleEntries]`.
         */
        struct AuthTableView
        {
            std::uintptr_t container{};
            std::uintptr_t arrayBase{};
            std::uint32_t count{};
            bool valid{};
        };

        /**
         * @brief Read the container pointer, entry array base and entry count in one pass.
         * @param a1 ClientEquipSlotActorComponent pointer.
         * @return The view. Every hop runs through DMK's fault guard, so a bad link fails closed on its own instead
         *         of unwinding the caller's whole frame.
         */
        [[nodiscard]] AuthTableView read_auth_table(std::uintptr_t a1) noexcept
        {
            AuthTableView view{};
            if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
                return view;

            const auto container =
                DMK::memory::read<std::uintptr_t>(DMK::Address{a1 + AuthTable::k_containerPtrOffset}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{container}))
                return view;
            view.container = container;

            view.arrayBase =
                DMK::memory::read<std::uintptr_t>(DMK::Address{container + AuthTable::k_containerArrayBaseOffset})
                    .value_or(0);
            view.count = DMK::memory::read<std::uint32_t>(DMK::Address{container + AuthTable::k_containerCountOffset})
                             .value_or(0);
            view.valid = DMK::memory::is_plausible_ptr(DMK::Address{view.arrayBase}) && view.count >= 1 &&
                         view.count <= k_maxPlausibleEntries;
            return view;
        }

        /// One auth-table entry the search matched.
        struct AuthEntry
        {
            std::uintptr_t address{};
            std::uint16_t itemId{};
            bool found{};
        };

        /**
         * @brief Locate the live entry the engine holds for one slot tag.
         * @param view A view @ref read_auth_table already validated.
         * @param gameSlotTag Engine slot tag to match.
         * @return The entry address and its raw item word, or `found == false` when no live entry carries the tag.
         */
        [[nodiscard]] AuthEntry find_auth_entry(const AuthTableView &view, std::uint16_t gameSlotTag) noexcept
        {
            for (std::uint32_t i = 0; i < view.count; ++i)
            {
                const auto entry = AuthTable::entry_at(view.arrayBase, i);

                const auto primary =
                    DMK::memory::read<std::uint16_t>(DMK::Address{entry + AuthTable::k_entryItemIdOffset}).value_or(0);
                const auto gate =
                    DMK::memory::read<std::uint64_t>(DMK::Address{entry + AuthTable::k_entryGateOffset}).value_or(0);
                if (!entry_is_live(primary, gate))
                    continue;

                const auto tag =
                    DMK::memory::read<std::uint16_t>(DMK::Address{entry + AuthTable::k_entrySlotTagOffset});
                if (!tag.has_value() || *tag != gameSlotTag)
                    continue;

                return AuthEntry{.address = entry, .itemId = primary, .found = true};
            }
            return AuthEntry{};
        }

        // Engine-only slot tags absent from `TransmogSlot` (and therefore from `slot_metadata.hpp::k_slotMetadata`) but
        // still part of the engine taxonomy. Kept here so the dump emits a readable label instead of "?".
        // `is_documented_slot` deliberately returns false for these so the dump still flags them as NEW SLOT TAG
        // candidates the mod has not lifted into TransmogSlot yet.
        //
        // Only reached for a tag `slot_from_game_tag` does not resolve, so a tag lifted into TransmogSlot must be
        // dropped from this table: its case is then unreachable.
        [[nodiscard]] const char *engine_only_slot_name(std::uint16_t tag) noexcept
        {
            switch (tag)
            {
            case 0x0015:
                return "OongkaRocket";
            default:
                return "?";
            }
        }

        // Slot label resolver. It defers to `slot_metadata.hpp` (single source of truth for per-slot static data) when
        // the tag maps to a `TransmogSlot`. Otherwise it falls back to the engine-only override table for the tags LT
        // does not manage.
        [[nodiscard]] const char *known_slot_name(std::uint16_t tag) noexcept
        {
            if (const auto s = slot_from_game_tag(static_cast<std::int16_t>(tag)))
                return slot_meta(*s).displayName;
            return engine_only_slot_name(tag);
        }

        // A tag is "documented" iff it round-trips through `slot_metadata` (and therefore has a `TransmogSlot` enum
        // entry). The engine-only tag 0x0015 (OongkaRocket) lives outside the enum on purpose, so it falls through to
        // the false branch and gets flagged in the dump.
        [[nodiscard]] bool is_documented_slot(std::uint16_t tag) noexcept
        {
            return slot_from_game_tag(static_cast<std::int16_t>(tag)).has_value();
        }

        // Friendly name for the LT-internal TransmogSlot category that ItemNameTable::category_of returns when it
        // classifies an item by its descriptor (independent of the auth-table slot tag). It defers to slot_name(),
        // which knows every TransmogSlot enum entry. "Other" is the catch-all for items whose descriptor type-code did
        // not classify (it returned TransmogSlot::Count).
        [[nodiscard]] const char *transmog_category_str(TransmogSlot s) noexcept
        {
            if (s == TransmogSlot::Count)
                return "Other";
            return slot_name(s);
        }

        /**
         * @brief Log the whole auth table once per (a1, count, content) change.
         *
         * @details Walks every live entry and logs (index, slotTag, primary, gate-non-null) plus the resolved item
         *          name and LT-classified category. The caller validates arrayBase and count first. The cost is
         *          bounded: at most k_maxPlausibleEntries iterations, and one log line per live entry. Item-name
         *          resolution falls back to "<unresolved>" when ItemNameTable is not built yet, for example early in
         *          load.
         *
         * @param a1 ClientEquipSlotActorComponent pointer, part of the dedupe key.
         * @param arrayBase Entry array base.
         * @param count Entry array capacity.
         *
         * @note Two terms in the emitted counters. A NEW SLOT TAG entry is a candidate for a TransmogSlot enum
         *       extension: the engine fills that tag but the mod has no enumerator for it. runtime_obs_total counts
         *       the session-wide (itemId -> slot) bindings the picker now uses to override the static type-code
         *       mapping.
         */
        void dump_full_auth_table_if_changed(std::uintptr_t a1, std::uintptr_t arrayBase, std::uint32_t count) noexcept
        {
            // Cheap pre-pass: FNV-1a 64-bit hash over the primary IDs so equip and unequip events, which leave count
            // unchanged, stay visible to the dedupe gate. Two bytes hashed per entry, so the cost is negligible.
            std::uint64_t contentHash = 0xCBF29CE484222325ULL;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = AuthTable::entry_at(arrayBase, i);
                const auto primary =
                    DMK::memory::read<std::uint16_t>(DMK::Address{entry + AuthTable::k_entryItemIdOffset}).value_or(0);
                contentHash ^= static_cast<std::uint64_t>(primary);
                contentHash *= 0x100000001B3ULL;
            }

            const auto prevA1 = g_lastDumpedA1.load(std::memory_order_acquire);
            const auto prevCount = g_lastDumpedCount.load(std::memory_order_acquire);
            const auto prevHash = g_lastDumpedContentHash.load(std::memory_order_acquire);
            if (prevA1 == a1 && prevCount == count && prevHash == contentHash)
                return;

            // Stamp the dedupe tracker only when ItemNameTable is ready. Otherwise the first dump fires from the early
            // load-detect retry probe, which runs before the catalog is built. Names then show as "<unresolved>", and
            // a stamped tracker blocks every later dump. The deferred stamp lets the dump re-fire on each retry tick
            // through the load window. The cost is one log batch per tick, and the FINAL emission carries full item
            // names.
            auto &logger = DMK::log();
            auto &itemTable = ItemNameTable::instance();
            const bool itemTableReady = itemTable.size() > 0;
            if (itemTableReady)
            {
                g_lastDumpedA1.store(a1, std::memory_order_release);
                g_lastDumpedCount.store(count, std::memory_order_release);
                g_lastDumpedContentHash.store(contentHash, std::memory_order_release);
            }

            logger.trace(
                "[slot-discovery] auth-table dump begin a1=0x{:X} arrayBase=0x{:X} count={} stride={:#x} "
                "slotTag@+{:#x} ItemNameTable={}",
                static_cast<std::uint64_t>(a1),
                static_cast<std::uint64_t>(arrayBase),
                count,
                static_cast<std::uint64_t>(AuthTable::k_entryStride),
                static_cast<std::uint64_t>(AuthTable::k_entrySlotTagOffset),
                itemTableReady ? "ready" : "not-ready"
            );

            std::uint32_t live = 0;
            std::uint32_t documented = 0;
            std::uint32_t newTags = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = AuthTable::entry_at(arrayBase, i);
                const auto primary =
                    DMK::memory::read<std::uint16_t>(DMK::Address{entry + AuthTable::k_entryItemIdOffset}).value_or(0);
                const auto gate =
                    DMK::memory::read<std::uint64_t>(DMK::Address{entry + AuthTable::k_entryGateOffset}).value_or(0);
                if (!entry_is_live(primary, gate))
                    continue;

                const auto tag =
                    DMK::memory::read<std::uint16_t>(DMK::Address{entry + AuthTable::k_entrySlotTagOffset}).value_or(0);

                ++live;
                const bool documentedTag = is_documented_slot(tag);
                if (documentedTag)
                    ++documented;
                else
                    ++newTags;

                const char *tagName = known_slot_name(tag);
                const char *newTagMarker = documentedTag ? "" : "  *** NEW SLOT TAG ***";

                if (!itemTableReady)
                {
                    // Catalog not built yet. Skip the resolved fields entirely, because they are all default-init
                    // noise (cat=Other), and print the raw engine-side fields only. This branch re-fires on the next
                    // probe tick after names land.
                    logger.trace(
                        "[slot-discovery]   [{:>2}] tag={:#06x} ({:<12}) primary={:#06x} <unresolved>{}",
                        i,
                        tag,
                        tagName,
                        primary,
                        newTagMarker
                    );
                    continue;
                }

                std::string itemName = itemTable.name_of(primary);
                if (itemName.empty())
                    itemName = "<unresolved>";

                const char *categoryStr = transmog_category_str(itemTable.category_of(primary));

                // Auto-record (itemId -> TransmogSlot) for the picker catalog. The auth-table tag states which slot
                // this item belongs in. That is ground truth, and it overrides the catalog classification. A tag with
                // no TransmogSlot mapping records nothing (for example tag 0x15 OongkaRocket, which is intentionally
                // outside TransmogSlot).
                //
                // A DISABLED slot records nothing either, because the binding is sticky for the session and its only
                // consumer is the picker, which never lists a disabled slot. The overflow slots make that concrete:
                // the engine parks a weapon there whenever its primary slot is taken (a shield lands in OffHand2 while
                // a dual-wielded sword holds OffHand, a bow lands in Ranged2 while a sprayer holds Ranged). A record
                // there rebinds the item to a slot with no picker and drops it out of the primary slot's list for the
                // rest of the session, even after the engine moves it back. A fall-through to the catalog
                // classification keeps such an item in the list it belongs to.
                const auto tslot = slot_from_game_slot(static_cast<std::int16_t>(tag));

                // Patch-day drift check on `k_slotMetadata`'s gameTag column.
                //
                // Those tags index the character's equip-slot enum and have not moved across any game version this mod
                // has shipped against, so the mod does NOT derive them at runtime the way it derives the item
                // taxonomy. It also cannot: the auth table only lists FILLED slots, so a live derivation could never
                // cover the table LT needs at apply time.
                //
                // What is free is verification. The engine states `(tag, itemId)` here, and the catalog independently
                // classifies that item by group NAME, so the two must agree. A stale tag column otherwise fails
                // silently and routes an apply into the wrong slot.
                //
                // Compared against `catalog_category_of`, NOT `category_of`: the latter consults the observations this
                // same loop records, so the check confirms its own writes and never fires.
                if (tslot.has_value())
                {
                    const TransmogSlot cataloged = itemTable.catalog_category_of(primary);
                    // Paired and overflow slots legitimately disagree. Both halves of a pair share one item type, so
                    // the catalog reports the first half for either. The engine parks a weapon in an overflow slot
                    // whenever its primary is taken, so a bow cataloged as Ranged can sit in Ranged2.
                    const bool overflow = (*tslot == TransmogSlot::OffHand2 && cataloged == TransmogSlot::OffHand) ||
                                          (*tslot == TransmogSlot::Ranged2 && cataloged == TransmogSlot::Ranged);
                    if (cataloged != TransmogSlot::Count && !slots_share_picker(cataloged, *tslot) && !overflow)
                    {
                        logger.warning(
                            "[slot-discovery] TAG DRIFT: engine put \"{}\" (item {:#06x}) in tag {:#06x}, "
                            "which k_slotMetadata calls {}, but the catalog classifies it as {}. "
                            "Re-verify the gameTag column in slot_metadata.hpp.",
                            itemName,
                            primary,
                            tag,
                            slot_name(*tslot),
                            transmog_category_str(cataloged)
                        );
                    }
                }

                if (tslot.has_value() && Transmog::slot_enabled(*tslot))
                {
                    itemTable.record_observed_slot(primary, *tslot);
                }

                logger.trace(
                    "[slot-discovery]   [{:>2}] tag={:#06x} ({:<12}) primary={:#06x} cat={:<13} name=\"{}\"{}",
                    i,
                    tag,
                    tagName,
                    primary,
                    categoryStr,
                    itemName,
                    newTagMarker
                );
            }

            logger.trace(
                "[slot-discovery] auth-table dump end live={} documented={} new_tags={} runtime_obs_total={}",
                live,
                documented,
                newTags,
                itemTableReady ? itemTable.observed_slot_count() : std::size_t{0}
            );
        }
    } // namespace

    bool is_ready() noexcept
    {
        return g_ready.load(std::memory_order_acquire);
    }

    bool is_actor_apply_ready(void *a1Raw) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);

        // Stage 1: structural. Every hop is guarded on its own, so a bad link fails closed without a frame unwind.
        const auto view = read_auth_table(a1);
        if (!view.valid)
            return false;

        // Stage 2: engine readiness. SafeTearDown performs a deep dereference of the shape
        //     sub = *(CCC + 0x130);
        //     ... *sub ...
        // During cold-load the field at `CCC + 0x130` is null. After the engine wires up the actor's scene graph, the
        // field holds a heap-allocated sub-handler pointer. SafeTearDown's own null-check guards only against a null
        // CCC, not against this deeper field, so a call before the field is populated faults inside the engine. A
        // probe of this one pointer is necessary, because SafeTearDown faults when it is null. It is also sufficient,
        // because the transition coincides with the end of the cold-load fault window.
        //
        // The CCC instance is located by MSVC RTTI mangled name rather than by a fixed slot offset, so the slot drift
        // documented in CDCore controlled_char does not affect this probe. Only the +0x130 sub-handler offset lives in
        // LT. The a1 -> CCOIA -> p1 chain offsets live inside CDCore.
        constexpr std::ptrdiff_t k_offCccSubHandler = 0x130;
        // Generation-checked: the cache re-validates itself when the engine rebuilds the world, so a stale
        // vtable pointer from a previous save-load can never satisfy a lookup.
        static DMK::rtti::PointerTableCache s_cccVt;
        static constexpr std::string_view k_cccName = ".?AVClientCharacterControlActorComponent@pa@@";

        const auto cccAddr = CDCore::find_component_for_equipslot(a1, k_cccName, s_cccVt);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{cccAddr}))
            return false;

        const auto subHandler =
            DMK::memory::read<std::uintptr_t>(DMK::Address{cccAddr + k_offCccSubHandler}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{subHandler}))
            return false;

        dump_full_auth_table_if_changed(a1, view.arrayBase, view.count);
        return true;
    }

    bool resolve_helpers() noexcept
    {
        auto &logger = DMK::log();

        const auto &addrs = resolved_addrs();
        const auto safeAddr = addrs.safeTearDown;
        const auto lookupAddr = addrs.indexedStringLookup;

        if (!safeAddr)
        {
            logger.warning("[dispatch] tear_down: safeTearDown address not resolved (AOB scan failed in init)");
            return false;
        }
        if (!lookupAddr)
        {
            logger.warning(
                "[dispatch] tear_down: indexedStringLookup address not resolved (ItemNameTable chain walk has not "
                "run yet)"
            );
            return false;
        }

        // safeTearDown needs no prologue check here. Its anchor row carries the code_site validator with
        // require_validator set, and that validator already calls scan::is_likely_function_prologue, so a rejected
        // site arrives as 0 and the test above catches it. indexedStringLookup comes from the ItemNameTable chain
        // walk rather than from an anchor, so it is the one address that still needs its own proof.
        std::array<std::byte, 8> lookupBytes{};
        if (!DMK::memory::read_into(DMK::Address{lookupAddr}, lookupBytes))
        {
            logger.warning(
                "[dispatch] tear_down: cannot read indexedStringLookup@{:#x}",
                static_cast<std::uint64_t>(lookupAddr)
            );
            return false;
        }

        std::vector<int> lookupDump;
        lookupDump.reserve(lookupBytes.size());
        for (const auto b : lookupBytes)
            lookupDump.push_back(static_cast<int>(b));

        logger.trace(
            "[dispatch] tear_down bytes: lookup@{:#x}={}",
            static_cast<std::uint64_t>(lookupAddr),
            DMK::format::format_int_vector(lookupDump)
        );

        if (!DMK::scan::is_likely_function_prologue(DMK::Address{lookupAddr}))
        {
            logger.warning("[dispatch] tear_down: indexedStringLookup prologue reject");
            return false;
        }

        g_safeTearDown.store(reinterpret_cast<SafeTearDownFn>(safeAddr), std::memory_order_release);
        g_indexedStringLookup.store(reinterpret_cast<IndexedStringLookupFn>(lookupAddr), std::memory_order_release);

        g_ready.store(true, std::memory_order_release);

        logger.trace(
            "[dispatch] tear_down helpers resolved: safe={:#x} lookup={:#x}",
            static_cast<std::uint64_t>(safeAddr),
            static_cast<std::uint64_t>(lookupAddr)
        );
        return true;
    }

    bool tear_down_real_part(void *a1Raw, std::uint16_t gameSlotTag) noexcept
    {
        if (!tear_down_needed_for_slot(gameSlotTag))
            return false; // see tear_down_needed_for_slot

        auto &logger = DMK::log();

        if (!g_ready.load(std::memory_order_acquire))
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn = g_indexedStringLookup.load(std::memory_order_acquire);
        if (!safeFn || !lookupFn)
            return false;

        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return false;

        if (!plausible_slot_tag(gameSlotTag))
        {
            logger.warning(
                "[dispatch] tear_down: slot tag {:#06x} outside plausible range [{:#x}..{:#x}] - rejecting",
                gameSlotTag,
                k_minPlausibleSlotTag,
                k_maxPlausibleSlotTag
            );
            return false;
        }

        const auto view = read_auth_table(a1);
        if (!view.valid)
        {
            // Only report a container that read back cleanly but whose contents no longer make sense. An implausible
            // component pointer is the ordinary early-load case and stays silent.
            if (view.container != 0)
            {
                logger.warning(
                    "[dispatch] tear_down: container sanity failed (arrayBase=0x{:X} count={}) - layout may have "
                    "shifted",
                    static_cast<std::uint64_t>(view.arrayBase),
                    view.count
                );
            }
            return false;
        }

        // First-entry sanity log, once per session.
        bool expected = false;
        if (g_loggedFirstEntry.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            const auto p0 =
                DMK::memory::read<std::uint16_t>(DMK::Address{view.arrayBase + AuthTable::k_entryItemIdOffset})
                    .value_or(0);
            const auto t0 =
                DMK::memory::read<std::uint16_t>(DMK::Address{view.arrayBase + AuthTable::k_entrySlotTagOffset})
                    .value_or(0);
            const auto g0 =
                DMK::memory::read<std::uint64_t>(DMK::Address{view.arrayBase + AuthTable::k_entryGateOffset})
                    .value_or(0);
            logger.trace(
                "[dispatch] tear_down first-entry sanity: count={} primary={:#06x} slotTag={:#06x}@+{:#x} "
                "gate={:#018x}",
                view.count,
                p0,
                t0,
                static_cast<std::uint64_t>(AuthTable::k_entrySlotTagOffset),
                static_cast<std::uint64_t>(g0)
            );
        }

        // Verbose slot-discovery dump: enumerates every live entry so additional slot tags (lower body, mask, neck,
        // etc.) populated by the engine for the active character become visible. One-shot per distinct (a1, count)
        // pair.
        dump_full_auth_table_if_changed(a1, view.arrayBase, view.count);

        const auto found = find_auth_entry(view, gameSlotTag);
        if (!found.found)
        {
            logger.trace(
                "[dispatch] tear_down slot={:#06x} entryFound=false (walked {} entries)",
                gameSlotTag,
                view.count
            );
            return false;
        }

        std::uint32_t hash = 0;
        std::int64_t rc = 0;

        // The two engine calls keep their own SEH frame. Every read around them is guarded on its own.
        __try
        {
            // Resolve hash via the engine's interner.
            std::uint16_t localWord = found.itemId;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true primary={:#06x} hash=<lookup_null>",
                    gameSlotTag,
                    found.itemId
                );
                return false;
            }
            hash =
                DMK::memory::read<std::uint32_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(hashPtr)}).value_or(0);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true primary={:#06x} hash={:#010x} <rejected>",
                    gameSlotTag,
                    found.itemId,
                    hash
                );
                return false;
            }

            // Safe scene-graph tear-down. It routes through the engine's scene-detach primitive and does NOT mutate
            // the authoritative entry array, so the apply dispatcher can call it.
            log_safe_tear_down_state(a1, hash, gameSlotTag, "tear_down");
            // Log the engine's return rather than a constant. A tear-down that matched nothing and one that
            // detached a part both come back normally, so the return is the only thing that tells them apart.
            rc = safeFn(static_cast<std::int64_t>(a1), hash, static_cast<std::int16_t>(gameSlotTag));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace("[dispatch] tear_down slot={:#06x} SEH caught fault", gameSlotTag);
            return false;
        }

        logger.trace(
            "[dispatch] tear_down slot={:#06x} entryFound=true primary={:#06x} hash={:#010x} engineRc={:#x}",
            gameSlotTag,
            found.itemId,
            hash,
            static_cast<std::uint64_t>(rc)
        );
        return true;
    }

    std::uint16_t get_real_item_id(void *a1Raw, std::uint16_t gameSlotTag) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return 0;

        if (!plausible_slot_tag(gameSlotTag))
            return 0;

        const auto view = read_auth_table(a1);
        if (!view.valid)
            return 0;

        const auto found = find_auth_entry(view, gameSlotTag);
        return found.found ? found.itemId : 0;
    }

    bool tear_down_by_item_id(void *a1Raw, std::uint16_t itemId, std::uint16_t gameSlotTag) noexcept
    {
        if (!tear_down_needed_for_slot(gameSlotTag))
            return false; // see tear_down_needed_for_slot

        auto &logger = DMK::log();

        if (!g_ready.load(std::memory_order_acquire) || itemId == 0)
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn = g_indexedStringLookup.load(std::memory_order_acquire);
        if (!safeFn || !lookupFn)
            return false;

        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return false;

        std::uint32_t hash = 0;
        std::int64_t engineRc = 0;
        bool result = false;

        // The two engine calls keep their own SEH frame. Every read around them is guarded on its own.
        __try
        {
            std::uint16_t localWord = itemId;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace(
                    "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} hash=<lookup_null>",
                    gameSlotTag,
                    itemId
                );
                return false;
            }

            hash =
                DMK::memory::read<std::uint32_t>(DMK::Address{reinterpret_cast<std::uintptr_t>(hashPtr)}).value_or(0);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace(
                    "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} hash={:#010x} <rejected>",
                    gameSlotTag,
                    itemId,
                    hash
                );
                return false;
            }

            log_safe_tear_down_state(a1, hash, gameSlotTag, "tear_down_fake");
            // Kept for the summary line below. A tear-down that matched nothing and one that detached a part both
            // return normally, so the engine's return is the only thing that tells them apart.
            engineRc = safeFn(static_cast<std::int64_t>(a1), hash, static_cast<std::int16_t>(gameSlotTag));
            result = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace("[dispatch] tear_down_fake slot={:#06x} SEH caught fault", gameSlotTag);
            return false;
        }

        logger.trace(
            "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} hash={:#010x} result={} engineRc={:#x}",
            gameSlotTag,
            itemId,
            hash,
            result,
            static_cast<std::uint64_t>(engineRc)
        );
        return result;
    }
} // namespace Transmog::RealPartTearDown
