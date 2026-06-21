#pragma once

// Engine shader-property token classification.
//
// The engine maintains per-property token ids in two registrar-owned tables (Table B at `dword_1461730E0`, Table A at
// `dword_146173160`). Token IDs are re-bucketed by the engine's string interner every patch, so they cannot be
// hardcoded.
//
// AOB-anchored slot discovery (see color_token_discovery.hpp) walks the loaded module at startup, finds every registrar
// call site for our known dye-property names, and records the slot ADDRESSES. The classifier here reads those slots
// live on every call -- patches that shift token ids are absorbed automatically.

#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride::TokenTable
{
    /**
     * Trigger AOB-anchored token-slot discovery. Safe to call once at module init.
     */
    void bootstrap_snapshot();

    // ---- Layer/channel classification ---------------------------------
    //
    // Layer kind: 0=tint, 1=color-mask, 2=detail-layer, 3=hair, 4=scratch (wear/damage overlay tint), -1=unknown /
    // non-dye-color.
    // Channel: 0=R, 1=G, 2=B, -1=unknown.

    int token_layer(std::uint32_t tok) noexcept;
    int channel_kind(std::uint32_t tok) noexcept;

    /**
     * Short UI channel label. Caller passes the result of `channel_kind` and gets a static string back: "R" / "G" / "B"
     * / "\xC2\xB7" (middle-dot).
     */
    const char *channel_short_name(int channel) noexcept;

    /**
     * Long property-family name without the R/G/B suffix. Used by the picker so labels read as the full shader property
     * rather than one-word truncations ("hair", "detail"). Returns:
     *   0 -> "_tintColor"
     *   1 -> "_dyeingColorMask"
     *   2 -> "_dyeingDetailLayerColorMask"
     *   3 -> "_hairDyeingColor"
     *   4 -> "_scratchTintColor"
     *   else -> "misc"
     */
    const char *layer_long_name(int layer) noexcept;

    /**
     * Human-readable shader-property name for `tok` (e.g. "_tintColorR"). Returns nullptr for unknown tokens; callers
     * fall back to a "0xXXXX" hex display.
     */
    const char *token_label_for(std::uint16_t tok) noexcept;

    /**
     * Alias for `channel_kind`. The picker code calls `token_channel`; keep both names available.
     */
    inline int token_channel(std::uint16_t tok) noexcept
    {
        return channel_kind(tok);
    }

    /**
     * Resolve a property-name string (e.g. "_tintColorR") to the engine's current-session token id, or 0 if not in the
     * snapshot. Used by persistence reload to remap on patch day.
     */
    std::uint16_t token_id_for_name(const char *name) noexcept;

    // ---- _permutations token (runtime-resolved via name-interner) -----

    /**
     * Token id for "_permutations" -- used by the publisher hook to detect when the engine's interner re-hands out a
     * different id this session. Returns 0 until set_permutations_token() is called for the first time.
     */
    std::uint16_t permutations_token() noexcept;

    /**
     * Update the cached permutations token. The publisher hook reads `*(mi + k_offMi_PermutTok)` per fire and forwards
     * live values here so downstream `permutations_token()` consumers see the current id.
     */
    void set_permutations_token(std::uint16_t tok) noexcept;
} // namespace Transmog::ColorOverride::TokenTable
