#pragma once

// Per-slot capture state: which matInst pointers + content_hashes
// were observed by the publisher hook during the slot's apply window.
// Setter hook queries this to decide whether a write belongs to one
// of LT's transmog carriers.
//
// The hash set has a 500ms burst-lock (after no new hash for
// `k_hashSetBurstLockMs`, growth is refused) so late-arriving NPC /
// mount / scenery matInsts can't pollute the slot.

#include "shared_state.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride::CarrierSet
{
    using ::Transmog::k_slotCount;

    constexpr std::size_t k_maxCarrierMatInst = 256;
    constexpr std::size_t k_maxCarrierHashes  = 64;

    /// Add `mi` to slot's carrier set. Returns true on success or
    /// already-present.
    bool add_matinst(int slot, std::uintptr_t mi) noexcept;

    /// Content-hash set (independent of matInst pointer). Respects
    /// the burst-lock: refuses growth after 500ms of no new add.
    /// Returns true if hash is in the set after the call.
    bool add_hash(int slot, std::uint32_t hash) noexcept;

    /// Linear sweep across every slot's hash set, returns the first
    /// slot containing `hash`, or -1 if no slot has it. Used by the
    /// setter when MatInstOwner has no entry yet (publisher hook
    /// may not have fired for this exact matInst pointer but the
    /// content_hash was registered through another carrier).
    int find_slot_by_hash(std::uint32_t hash) noexcept;

    /// Wipe both sets for `slot`. Called by `wipe_slot` /
    /// `reset_all` from the public facade.
    void clear_slot(int slot) noexcept;
    void clear_all() noexcept;
}
