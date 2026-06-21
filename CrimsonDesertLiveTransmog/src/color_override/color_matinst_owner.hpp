#pragma once

// matInst* -> lt_slot owner map with per-entry expected_hash check
// (cheap pool-recycle detection). The setter hook reads this map to resolve which LT slot a setter event belongs to
// without depending on the apply-window timer.

#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride::MatInstOwner
{
    constexpr std::size_t k_capacity = 1024;

    /**
     * Bind `mi` to `slot` with the matInst's current content hash.
     * Idempotent: re-bind overrides the existing entry.
     */
    void set(std::uintptr_t mi, std::uint32_t expected_hash, int slot) noexcept;

    /**
     * Look up the slot for `mi` only if its expected_hash matches `live_hash`. Returns -1 on miss or hash mismatch
     * (pool recycle).
     */
    int lookup_verified(std::uintptr_t mi, std::uint32_t live_hash) noexcept;

    /// Drop every entry bound to `slot`.
    void clear_for_slot(int slot) noexcept;
    void clear_all() noexcept;
} // namespace Transmog::ColorOverride::MatInstOwner
