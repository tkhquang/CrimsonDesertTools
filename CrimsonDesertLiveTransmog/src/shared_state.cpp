#include "shared_state.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit/memory.hpp>

#include <mutex>
#include <optional>

namespace Transmog
{
    std::string to_utf8(const std::filesystem::path &path)
    {
        // u8string is the only accessor that states the encoding. string() would use the ANSI codepage.
        const std::u8string utf8 = path.u8string();
        return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
    }

    namespace
    {
        ResolvedAddresses s_resolved_addrs{};
        std::array<SlotMapping, SLOT_COUNT> s_slot_mappings{};
        std::array<uint16_t, SLOT_COUNT> s_last_applied_ids{};

        std::atomic<bool> s_player_only{true};
        std::atomic<bool> s_enabled{true};
        std::atomic<bool> s_shutdown_requested{false};
        std::atomic<bool> s_color_override{false};
        std::atomic<bool> s_helm_audio_unmuffle{true};
        std::atomic<bool> s_dump_item_prefabs{false};
        std::atomic<bool> s_dump_item_catalog{false};
        std::atomic<bool> s_apply_to_editing{true};

        SlotPopulatorFn s_slot_populator = nullptr;
        PartSlotRefreshFn s_part_slot_refresh = nullptr;
        SlotTagToHandleFn s_slot_tag_to_handle = nullptr;
        ItemToSlotResolveFn s_item_to_slot_resolve = nullptr;
        InitSwapEntryFn s_init_swap_entry = nullptr;

        std::atomic<bool> s_in_transmog{false};
        std::atomic<__int64> s_player_a_1{0};
        std::atomic<uintptr_t> s_world_system_ptr{0};
        std::array<bool, SLOT_COUNT> s_real_damaged{};
        std::array<std::uint16_t, SLOT_COUNT> s_last_applied_real_ids{};
        std::atomic<bool> s_clear_pending{false};
        std::atomic<bool> s_dye_dirty{false};
        std::atomic<std::size_t> s_pending_slot_index{SLOT_COUNT};
        std::array<std::uint16_t, SLOT_COUNT> s_last_applied_carrier_ids{};
        std::array<bool, SLOT_COUNT> s_force_apply_pending{};

        // Per-character buffered snapshots of the four applied-state arrays above. Indexed by (idx-1) where idx is
        // the 1-based CDCore protagonist index (1=Kliff, 2=Damiane, 3=Oongka). The worker hydrates the globals from
        // the relevant slot before each apply and writes the post-apply globals back, so Phase A teardown always sees
        // a per-body truth source.
        std::array<std::array<std::uint16_t, SLOT_COUNT>, BODY_OWNER_CAP> s_last_applied_ids_per_char{};
        std::array<std::array<bool, SLOT_COUNT>, BODY_OWNER_CAP> s_real_damaged_per_char{};
        std::array<std::array<std::uint16_t, SLOT_COUNT>, BODY_OWNER_CAP> s_last_applied_real_ids_per_char{};
        std::array<std::array<std::uint16_t, SLOT_COUNT>, BODY_OWNER_CAP> s_last_applied_carrier_ids_per_char{};

        /// Maps a 1-based protagonist index to its per-character bucket, or nothing when the index names no bucket.
        std::optional<std::size_t> bucket_for_char(std::uint32_t idx) noexcept
        {
            if (idx < 1 || idx > BODY_OWNER_CAP)
                return std::nullopt;
            return static_cast<std::size_t>(idx - 1);
        }
    } // namespace

    ResolvedAddresses &resolved_addrs()
    {
        return s_resolved_addrs;
    }
    std::array<SlotMapping, SLOT_COUNT> &slot_mappings()
    {
        return s_slot_mappings;
    }
    std::array<uint16_t, SLOT_COUNT> &last_applied_ids()
    {
        return s_last_applied_ids;
    }

    std::atomic<bool> &flag_player_only()
    {
        return s_player_only;
    }
    std::atomic<bool> &flag_enabled()
    {
        return s_enabled;
    }
    std::atomic<bool> &shutdown_requested()
    {
        return s_shutdown_requested;
    }
    std::atomic<bool> &flag_color_override()
    {
        return s_color_override;
    }
    std::atomic<bool> &flag_helm_audio_unmuffle()
    {
        return s_helm_audio_unmuffle;
    }
    std::atomic<bool> &flag_dump_item_prefabs()
    {
        return s_dump_item_prefabs;
    }
    std::atomic<bool> &flag_dump_item_catalog()
    {
        return s_dump_item_catalog;
    }
    std::atomic<bool> &flag_apply_to_editing()
    {
        return s_apply_to_editing;
    }

    PartSlotRefreshFn &part_slot_refresh_fn()
    {
        return s_part_slot_refresh;
    }

    SlotTagToHandleFn &slot_tag_to_handle_fn()
    {
        return s_slot_tag_to_handle;
    }

    ItemToSlotResolveFn &item_to_slot_resolve_fn()
    {
        return s_item_to_slot_resolve;
    }

    SlotPopulatorFn &slot_populator_fn()
    {
        return s_slot_populator;
    }
    InitSwapEntryFn &init_swap_entry_fn()
    {
        return s_init_swap_entry;
    }

    std::atomic<bool> &in_transmog()
    {
        return s_in_transmog;
    }
    std::atomic<__int64> &player_a1()
    {
        return s_player_a_1;
    }

    std::string current_controlled_character_name() noexcept
    {
        // Delegates to the shared Core resolver, whose focus-broadcast cache carries last-known-good and structural
        // Kliff fallbacks. Returns an empty string when the resolver observes no known identity this session.
        const auto name = CDCore::current_controlled_character_name();
        return std::string(name);
    }
    std::atomic<uintptr_t> &world_system_ptr()
    {
        return s_world_system_ptr;
    }
    std::array<bool, SLOT_COUNT> &real_damaged()
    {
        return s_real_damaged;
    }
    std::array<std::uint16_t, SLOT_COUNT> &last_applied_real_ids()
    {
        return s_last_applied_real_ids;
    }
    std::array<std::uint16_t, SLOT_COUNT> &last_applied_carrier_ids()
    {
        return s_last_applied_carrier_ids;
    }
    std::array<bool, SLOT_COUNT> &force_apply_pending()
    {
        return s_force_apply_pending;
    }
    std::atomic<bool> &clear_pending()
    {
        return s_clear_pending;
    }
    std::atomic<bool> &dye_dirty()
    {
        return s_dye_dirty;
    }
    std::atomic<std::size_t> &pending_slot_index()
    {
        return s_pending_slot_index;
    }

    void rehydrate_applied_state_for_char(std::uint32_t idx) noexcept
    {
        const auto slot = bucket_for_char(idx);
        if (!slot)
            return;
        const auto bucket = *slot;
        s_last_applied_ids = s_last_applied_ids_per_char[bucket];
        s_real_damaged = s_real_damaged_per_char[bucket];
        s_last_applied_real_ids = s_last_applied_real_ids_per_char[bucket];
        s_last_applied_carrier_ids = s_last_applied_carrier_ids_per_char[bucket];
    }

    void capture_applied_state_for_char(std::uint32_t idx) noexcept
    {
        const auto slot = bucket_for_char(idx);
        if (!slot)
            return;
        const auto bucket = *slot;
        s_last_applied_ids_per_char[bucket] = s_last_applied_ids;
        s_real_damaged_per_char[bucket] = s_real_damaged;
        s_last_applied_real_ids_per_char[bucket] = s_last_applied_real_ids;
        s_last_applied_carrier_ids_per_char[bucket] = s_last_applied_carrier_ids;
    }

    void reset_applied_state_for_char(std::uint32_t idx) noexcept
    {
        const auto slot = bucket_for_char(idx);
        if (!slot)
            return;
        const auto bucket = *slot;
        s_last_applied_ids_per_char[bucket].fill(0);
        s_real_damaged_per_char[bucket].fill(false);
        s_last_applied_real_ids_per_char[bucket].fill(0);
        s_last_applied_carrier_ids_per_char[bucket].fill(0);
        // Also wipe the live globals. apply_all_transmog reads these directly (last_applied_ids / real_damaged /
        // last_applied_real_ids / last_applied_carrier_ids), so a stale global drives the no-change early-out even
        // after the bucket is cleared. rehydrate_applied_state_for_char normally overwrites the globals from the
        // bucket, but the body-reallocation path skips rehydrate by design and calls this instead.
        s_last_applied_ids.fill(0);
        s_real_damaged.fill(false);
        s_last_applied_real_ids.fill(0);
        s_last_applied_carrier_ids.fill(0);
    }

    // Protagonist body-ownership table
    //
    // One producer (the load-detect worker) and many readers (engine threads inside the socket-build detour). The
    // producer owns the expensive part, the actor-array walk, so a reader never pays for it.
    //
    // Each row keeps the CCOIA it was derived from alongside the equip-slot address, because the address alone is not
    // a safe key. The engine pools both objects, so between two publishes a body can be freed and its addresses
    // handed to an unrelated actor. A bare address compare then reports a protagonist index for somebody else's body.
    //
    // A hit therefore confirms two independent things before it trusts the row, because either alone leaves a hole:
    //   - the CCOIA still classifies as the same character, which rules out a pool reissue of the actor. A
    //     structural walk cannot detect that on its own, because the successor object occupies the identical layout.
    //   - the CCOIA still resolves to this equip slot, which rules out the slot being reissued on its own.
    // Both are paid only by the handful of bodies that match an entry, never by the NPCs and creatures that make up
    // the traffic, and together they turn a silent mis-identification into an ordinary miss.
    //
    // A plain mutex rather than a reader/writer lock: the guarded region is a scan of at most three integers, and the
    // detour reaches it on the order of once per second, so shared-reader parallelism buys nothing that the narrower
    // critical section does not already give.
    namespace
    {
        /// One published protagonist body. Plain data, no invariant beyond what publish_body_owner_table enforces.
        struct BodyOwnerRow
        {
            std::uintptr_t ccoia;
            std::uintptr_t equip_slot;
            std::uint32_t char_idx;
        };

        std::array<BodyOwnerRow, BODY_OWNER_CAP> s_body_owners{};
        std::size_t s_body_owner_count = 0;
        std::mutex s_body_owner_mutex;
    } // namespace

    void publish_body_owner_table(const std::uintptr_t *ccoias, const std::uint32_t *char_idxs, std::size_t n) noexcept
    {
        if (ccoias == nullptr || char_idxs == nullptr)
            return;

        // Resolve before the lock. equip_slot_for_ccoia walks engine memory under SEH, and a lock held across a
        // foreign-memory read exposes every reader to whatever that walk costs on a torn chain.
        std::array<BodyOwnerRow, BODY_OWNER_CAP> built{};
        std::size_t written = 0;
        for (std::size_t i = 0; i < n && i < built.size(); ++i)
        {
            const auto slot = CDCore::equip_slot_for_ccoia(ccoias[i]);
            if (slot == 0)
                continue; // component chain not wired yet; the next publish picks this body up
            built[written] = BodyOwnerRow{ccoias[i], slot, char_idxs[i]};
            ++written;
        }

        // An exhausted snapshot leaves written at 0, which publishes an empty table. That is deliberate. Rows held
        // through a teardown are the dangerous direction, because their bodies are freed and their addresses
        // reissued, so a surviving row names a dead character as the owner.
        std::scoped_lock lk(s_body_owner_mutex);
        s_body_owners = built;
        s_body_owner_count = written;
    }

    std::uint32_t char_idx_for_equip_slot_uncached(std::uintptr_t a1) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return 0;
        std::array<CDCore::BodyCacheEntry, BODY_OWNER_CAP> entries{};
        const auto n = CDCore::snapshot_body_cache(entries.data(), entries.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            if (CDCore::equip_slot_for_ccoia(entries[i].body) == a1)
                return entries[i].char_idx;
        }
        return 0;
    }

    std::uint32_t char_idx_for_equip_slot(std::uintptr_t a1) noexcept
    {
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return 0;

        std::uintptr_t ccoia = 0;
        std::uint32_t char_idx = 0;
        {
            std::scoped_lock lk(s_body_owner_mutex);
            for (std::size_t i = 0; i < s_body_owner_count; ++i)
            {
                if (s_body_owners[i].equip_slot == a1)
                {
                    ccoia = s_body_owners[i].ccoia;
                    char_idx = s_body_owners[i].char_idx;
                    break;
                }
            }
        }
        if (ccoia == 0)
            return 0; // every NPC and creature lands here, having paid at most three integer compares

        // Confirmed outside the lock, for the reasons given on the table above.
        if (CDCore::character_idx_for_ccoia(ccoia) != char_idx)
            return 0;
        return CDCore::equip_slot_for_ccoia(ccoia) == a1 ? char_idx : 0;
    }

    std::atomic<std::uint32_t> &slot_mappings_owner() noexcept
    {
        static std::atomic<std::uint32_t> s_owner{0};
        return s_owner;
    }

    void reset_all_applied_state() noexcept
    {
        s_last_applied_ids.fill(0);
        s_real_damaged.fill(false);
        s_last_applied_real_ids.fill(0);
        s_last_applied_carrier_ids.fill(0);
        for (auto &row : s_last_applied_ids_per_char)
            row.fill(0);
        for (auto &row : s_real_damaged_per_char)
            row.fill(false);
        for (auto &row : s_last_applied_real_ids_per_char)
            row.fill(0);
        for (auto &row : s_last_applied_carrier_ids_per_char)
            row.fill(0);
    }

} // namespace Transmog
