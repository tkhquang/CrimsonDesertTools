#include "color_token_table.hpp"

#include "color_token_discovery.hpp"
#include "color_token_interner_hook.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace DMK = DetourModKit;

namespace Transmog::ColorOverride::TokenTable
{
    namespace
    {
        // `_permutations` is resolved independently of the dye-token
        // discovery path: the publisher hook reads it live from a
        // known matInst and calls set_permutations_token() on its
        // first fire. Default 0 (unresolved) until then.
        std::atomic<std::uint16_t> g_tokPermutations{0u};
    } // namespace

    void bootstrap_snapshot()
    {
        TokenSlotDiscovery::run();
        DMK::Logger::get_instance().info(
            "[color-tokens] slot discovery: {} slot(s) recorded",
            TokenSlotDiscovery::slot_count());
        // Hook the engine's string interner so we capture every
        // (name, token) pair the engine intern-resolves. Catches
        // tokens not covered by the static registrar tables that
        // TokenSlotDiscovery walks.
        const bool internerOk = InternerHook::init();
        DMK::Logger::get_instance().info(
            "[color-tokens] interner hook: install={} captured={}",
            internerOk, InternerHook::capture_count());
    }

    namespace
    {
        // Classify by property NAME prefix. Returns layer (0..3) or
        // -1. Channel inferred from R/G/B suffix.
        int classify_by_name(const char *name, int *out_channel) noexcept
        {
            if (out_channel) *out_channel = -1;
            if (name == nullptr || name[0] != '_') return -1;
            const auto n = std::strlen(name);
            if (n == 0) return -1;
            const char last = name[n - 1];
            const int ch =
                (last == 'R') ? 0 : (last == 'G') ? 1 : (last == 'B') ? 2 : -1;
            if (out_channel) *out_channel = ch;

            auto starts = [&](const char *p) {
                const auto pl = std::strlen(p);
                return n >= pl && std::strncmp(name, p, pl) == 0;
            };

            // _scratchTintColor* -> scratch (4)
            //
            // Visually distinct from the main armor tint: the scratch
            // layer is the wear/damage overlay color. Keeping it
            // separate from `_tintColor*` (layer 0) lets users pick
            // these independently rather than always coupling them.
            if (starts("_scratchTintColor")) return 4;
            // _tintColor* -> tint (0)
            if (starts("_tintColor")) return 0;
            // _hair* (incl. _hairDyeing*) -> hair (3), no channel
            if (starts("_hair"))
            {
                if (out_channel) *out_channel = -1;
                return 3;
            }
            // _dyeingDetailLayer* -> detail (2)
            if (starts("_dyeingDetailLayer")) return 2;
            // _dyeing* (mask, custom, global, etc.) -> mask family (1)
            if (starts("_dyeing")) return 1;
            return -1;
        }
    }

    int token_layer(std::uint32_t tok) noexcept
    {
        // Primary: AOB-discovered static slot.
        const auto disc = TokenSlotDiscovery::classify_layer(tok);
        if (disc >= 0) return disc;
        // Fallback: look up name via interner dump, classify by
        // prefix. Catches tokens from name families our static
        // discovery doesn't cover (e.g. `_scratchTintColor*`).
        const auto *name = InternerHook::name_for_token(tok);
        if (name == nullptr) return -1;
        return classify_by_name(name, nullptr);
    }

    int channel_kind(std::uint32_t tok) noexcept
    {
        const auto disc = TokenSlotDiscovery::classify_channel(tok);
        if (disc >= 0) return disc;
        const auto *name = InternerHook::name_for_token(tok);
        if (name == nullptr) return -1;
        int ch = -1;
        classify_by_name(name, &ch);
        return ch;
    }

    const char *token_label_for(std::uint16_t tok) noexcept
    {
        // Primary: AOB-discovered slot's name (covers Table A/B
        // entries we statically located).
        const auto from_disc = TokenSlotDiscovery::name_for_token(
            static_cast<std::uint32_t>(tok));
        if (from_disc != nullptr) return from_disc;
        // Fallback: interner-hook capture (covers tokens registered
        // through any other engine code path).
        return InternerHook::name_for_token(
            static_cast<std::uint32_t>(tok));
    }

    const char *layer_short_name(int layer) noexcept
    {
        switch (layer)
        {
        case 0:  return "tint";
        case 1:  return "mask";
        case 2:  return "detail";
        case 3:  return "hair";
        case 4:  return "scratch";
        default: return "misc";
        }
    }

    const char *layer_long_name(int layer) noexcept
    {
        switch (layer)
        {
        case 0:  return "_tintColor";
        case 1:  return "_dyeingColorMask";
        case 2:  return "_dyeingDetailLayerColorMask";
        case 3:  return "_hairDyeingColor";
        case 4:  return "_scratchTintColor";
        default: return "misc";
        }
    }

    const char *channel_short_name(int channel) noexcept
    {
        switch (channel)
        {
        case 0:  return "R";
        case 1:  return "G";
        case 2:  return "B";
        default: return "\xC2\xB7"; // U+00B7 middle dot
        }
    }

    std::uint16_t token_id_for_name(const char *name) noexcept
    {
        if (name == nullptr || name[0] == '\0') return 0;
        const auto v = TokenSlotDiscovery::lookup_token_for_name(name);
        if (v != 0)
            return static_cast<std::uint16_t>(v & 0xFFFFu);
        // Fallback: interner-hook capture.
        const auto vi = InternerHook::token_for_name(name);
        return static_cast<std::uint16_t>(vi & 0xFFFFu);
    }

    std::uint16_t permutations_token() noexcept
    {
        return g_tokPermutations.load(std::memory_order_acquire);
    }

    void set_permutations_token(std::uint16_t tok) noexcept
    {
        if (tok != 0)
            g_tokPermutations.store(tok, std::memory_order_release);
    }
}
