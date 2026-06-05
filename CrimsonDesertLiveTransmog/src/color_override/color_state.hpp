#pragma once

// Per-slot apply-window state, batch gate, freeze flags, and
// timestamps shared across ColorOverride sub-modules.

#include "shared_state.hpp"

#include <atomic>
#include <cstdint>

namespace Transmog::ColorOverride::State
{
    using ::Transmog::k_slotCount;

    constexpr std::int64_t k_batchApplyExtendMs = 3000;
    constexpr std::int64_t k_hashSetBurstLockMs = 500;

    std::int64_t now_ms() noexcept;

    // Active slot for the current apply window (or -1).
    std::atomic<int> &active_apply_slot() noexcept;

    // Window validity deadline (steady_ms). Publisher inserts and
    // setter substitutes drop after this passes.
    std::atomic<std::int64_t> &active_apply_valid_until_ms() noexcept;

    // Per-slot freeze: locks the swatch-insert path so the engine's
    // unrelated render writes can't grow the table mid-flight.
    std::atomic<bool> &swatch_frozen(int slot) noexcept;

    // Publisher-insert block deadline: stamped by wipe_slot so the
    // engine's real-item mesh load (post-untick) can't seed bleed.
    std::atomic<std::int64_t> &block_publisher_inserts_until_ms() noexcept;

    // Per-slot hash-set burst-lock timestamp. After 500ms with no new
    // hash, the hash-set refuses to grow (ghost matInst guard).
    std::atomic<std::int64_t> &hash_set_last_add_ms(int slot) noexcept;
}
