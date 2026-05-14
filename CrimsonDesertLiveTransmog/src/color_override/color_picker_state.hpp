#pragma once

// ColorOverride picker state. Tracks the per-slot "user has enabled
// override" flag. The substitute path reads the flag to decide
// whether to walk the per-row override table at all; the actual user
// RGB lives in `SwatchTable::override_row()` keyed by submesh+token.

#include "shared_state.hpp"

#include <cstdint>

namespace Transmog::ColorOverride::PickerState
{
    using ::Transmog::k_slotCount;

    /// Enable/disable user color override for `slot`. When false,
    /// substitute path bails so engine renders default colors.
    void set_slot_override_active(int slot, bool active) noexcept;
    bool is_slot_override_active(int slot) noexcept;
}
