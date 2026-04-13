#pragma once

#include "shared_state.hpp"

#include <cstdint>

namespace Transmog
{
    /// Applies transmog by calling SlotPopulator directly with target item.
    /// Builds the 16-byte item data structure and empty swap entry internally.
    void apply_transmog(__int64 a1, std::uint16_t targetId);

    /// Full apply pass: two-phase tear-down + SlotPopulator for all active
    /// slots. Updates dispatch cache, suppress mask, and last-applied state.
    void apply_all_transmog(__int64 a1);

    /// Two-pass clear: tears down orphan fakes (pass A), then re-applies
    /// real equipment from the authoritative entry table (pass B).
    void clear_all_transmog(__int64 a1);

} // namespace Transmog
