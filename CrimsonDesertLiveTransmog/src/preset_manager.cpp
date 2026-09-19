#include "preset_manager.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_reinit.hpp"
#include "color_override/color_token_table.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
#include "generated/dye_color_table.hpp"
#include "item_name_table.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit/logger.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace Transmog
{
    // Resolve a dye channel's group by its persisted `group_name` (the data file's _stringKey, e.g.
    // "Her_Color_Group_I"). Populates `group_hash` from the running game's table. If the name is empty or no longer in
    // the table, leaves group_hash at 0 and the channel becomes inactive at injection time. Other channels of the same
    // slot are unaffected.
    static void resolve_dye_group(ChannelDye &ch)
    {
        using namespace Transmog::dye_color_table;
        ch.group_hash = 0; // always re-derive from the name

        if (ch.group_name.empty())
            return;

        if (const auto *g = find_group_by_name(ch.group_name.c_str()))
        {
            ch.group_hash = g->key;
            return;
        }

        DMK::log().warning(
            "[preset] dye group '{}' not found in current game's color table; dye mod dropped",
            ch.group_name
        );
    }

    // JSON serialization

    static json slot_to_json(const PresetSlot &s)
    {
        json j{
            {"active", s.active},
            {"itemName", s.item_name},
        };
        // Only emit prefab_name when set, to keep the JSON tidy for the common (no body-mesh override) case.
        if (!s.prefab_name.empty())
            j["prefabName"] = s.prefab_name;
        // Sparse dye_mods array: emit one object per active channel only. group_hash == 0 means "no override for this
        // channel".
        if (any_dye_active(s.dye))
        {
            json mods = json::array();
            for (std::size_t idx = 0; idx < s.dye.size(); ++idx)
            {
                const auto &ch = s.dye[idx];
                if (!ch.active())
                    continue;
                // group_name (the data file's _stringKey) is the sole stable identifier the JSON carries. The
                // loader resolves group_hash from it and never writes the hash back.
                json m{
                    {"idx", idx},
                    {"group_name", ch.group_name},
                    {"r", ch.r},
                    {"g", ch.g},
                    {"b", ch.b},
                };
                if (ch.material_id != 0xFFFF)
                    m["material"] = ch.material_id;
                if (ch.repair_byte != 0)
                    m["repair"] = ch.repair_byte;
                mods.push_back(std::move(m));
            }
            j["dye_mods"] = std::move(mods);
            // Persist whether this dye block was sourced from a real auth-table capture (sparse inject on apply) versus
            // user-curated picker selections (dense inject). Only emitted when there are dye_mods so unused slots stay
            // tidy.
            if (s.dye_sparse)
                j["dye_sparse"] = true;
        }
        return j;
    }

    static PresetSlot slot_from_json(const json &j)
    {
        PresetSlot s;
        s.active = j.value("active", false);
        s.item_name = j.value("itemName", std::string());
        s.prefab_name = j.value("prefabName", std::string());

        if (j.contains("dye_mods") && j["dye_mods"].is_array())
        {
            for (const auto &m : j["dye_mods"])
            {
                if (!m.is_object())
                    continue;
                const auto idx = m.value("idx", std::size_t{DYE_CHANNEL_COUNT});
                if (idx >= s.dye.size())
                    continue;
                auto &ch = s.dye[idx];
                ch.group_name = m.value("group_name", std::string());
                ch.r = m.value("r", std::uint8_t{0});
                ch.g = m.value("g", std::uint8_t{0});
                ch.b = m.value("b", std::uint8_t{0});
                ch.material_id = m.value("material", std::uint16_t{0xFFFF});
                ch.repair_byte = m.value("repair", std::uint8_t{0});
                resolve_dye_group(ch); // populates ch.group_hash
            }
            // Default false (dense, picker-style) when absent so older presets without the field continue to behave as
            // before.
            s.dye_sparse = j.value("dye_sparse", false);
        }

        if (s.item_name.empty())
            return s;

        // item_name is the sole persistent identifier. A built catalog resolves it here. Otherwise item_id stays 0
        // until reresolve_all_names() runs after the deferred scan completes.
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return s;

        auto resolved = table.id_of(s.item_name);
        if (resolved.has_value())
        {
            s.item_id = *resolved;
        }
        else
        {
            DMK::log().warning(
                "[preset] item name '{}' not in current catalog - slot disabled; re-pick in the overlay",
                s.item_name
            );
            s.active = false;
        }
        return s;
    }

    // color_override swatch persistence helpers
    //
    // Identity: `(submesh_name, token_name)` - stable across sessions AND patches. Independent of `dye_mods`
    // (dye_record_inject's ARMOR_MOD path).
    //
    // Two parallel JSON sections, each `{slot: {submesh: {token: [r,g,b]}}}`:
    //
    //   "swatch_defaults": engine-captured baselines (the asset's
    //                      natural color per row). Refreshed on every
    //                      save and during Re-init Finalize. The user
    //                      can revert to these without re-running
    //                      Re-init.
    //
    //   "swatch_overrides": rows the user explicitly picked a color
    //                       for. ONE entry per user pick - this
    //                       section stays small even when the slot
    //                       has 50+ rows in defaults.
    //
    // Per-row tick state on load = "is there a swatch_overrides entry matching (slot, submesh, token)?". Keeps
    // overrides small even when the slot has many reference rows in defaults, and only user-picked rows load as ticked.

    static json submeshes_to_json(const std::vector<color_override::swatch_table::PersistEntry> &entries)
    {
        json out = json::object();
        for (const auto &e : entries)
        {
            if (e.submesh_name.empty() || e.token_name.empty())
                continue;
            out[e.submesh_name][e.token_name] = json::array({e.r, e.g, e.b});
        }
        return out;
    }

    // Parse a swatch_overrides node: nested {submesh: {token: [r,g,b]}}. Each leaf is a user-picked color. Arrays
    // shorter than 3 or non-arrays are silently ignored. Longer arrays are tolerated for forward compatibility (the
    // first 3 elements are taken as r/g/b).
    static void
    append_overrides_from_json(std::vector<color_override::swatch_table::PersistEntry> &dst, const json &node)
    {
        if (!node.is_object())
            return;
        for (auto sit = node.begin(); sit != node.end(); ++sit)
        {
            const std::string &submesh = sit.key();
            const auto &tokens = sit.value();
            if (!tokens.is_object())
                continue;
            for (auto tit = tokens.begin(); tit != tokens.end(); ++tit)
            {
                const std::string &token_name = tit.key();
                const auto &rgb = tit.value();
                if (!rgb.is_array() || rgb.size() < 3)
                    continue;
                color_override::swatch_table::PersistEntry e{};
                e.submesh_name = submesh;
                e.token_name = token_name;
                e.r = rgb[0].get<std::uint8_t>();
                e.g = rgb[1].get<std::uint8_t>();
                e.b = rgb[2].get<std::uint8_t>();
                dst.push_back(std::move(e));
            }
        }
    }

    // Serialize the captured-row palette. Each submesh maps to a flat array of token-name strings (no RGB values). Lets
    // the loader re-create placeholder rows for every (submesh, token) the user has seen, without bloating the JSON
    // with default color duplicates.
    static json palette_to_json(const std::vector<color_override::swatch_table::PersistEntry> &entries)
    {
        json out = json::object();
        for (const auto &e : entries)
        {
            if (e.submesh_name.empty() || e.token_name.empty())
                continue;
            if (!out.contains(e.submesh_name))
                out[e.submesh_name] = json::array();
            out[e.submesh_name].push_back(e.token_name);
        }
        return out;
    }

    // Parse a swatch_palette node: {submesh: ["_tok1", "_tok2", ...]}.
    static void append_palette_from_json(std::vector<color_override::swatch_table::PersistEntry> &dst, const json &node)
    {
        if (!node.is_object())
            return;
        for (auto sit = node.begin(); sit != node.end(); ++sit)
        {
            const std::string &submesh = sit.key();
            const auto &tokens = sit.value();
            if (!tokens.is_array())
                continue;
            for (const auto &tok : tokens)
            {
                if (!tok.is_string())
                    continue;
                color_override::swatch_table::PersistEntry e{};
                e.submesh_name = submesh;
                e.token_name = tok.get<std::string>();
                dst.push_back(std::move(e));
            }
        }
    }

    // Forward declared so preset_from_json (above the definition) can call this helper for both object-form and
    // legacy-array load paths.
    static void preset_swatches_from_json(Preset &p, const json &j);

    // Keyed-object slot serialization. The object form keys each slot by its slot_metadata display name ("Helm",
    // "Chest", ...), so the JSON survives an enum addition or removal as long as the names stay stable.
    //
    // The loader accepts BOTH the keyed-object form and the positional-array form. The writer always emits the
    // keyed-object form.
    //
    // The object omits disabled slots and default-empty rows to keep the JSON tidy. A completely-empty preset
    // serializes as `{"slots": {}}`.
    static json preset_to_json(const Preset &p)
    {
        json slots_obj = json::object();
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            const auto &s = p.slots[i];
            if (!Transmog::slot_enabled(i))
                continue;
            const bool default_row =
                !s.active && s.item_name.empty() && s.prefab_name.empty() && !any_dye_active(s.dye);
            if (default_row)
                continue;
            slots_obj[Transmog::slot_meta(static_cast<TransmogSlot>(i)).display_name] = slot_to_json(s);
        }

        json out{{"name", p.name}, {"slots", slots_obj}};

        // Two parallel sections for color_override state:
        //   swatch_overrides: ONLY user-picked rows, nested
        //     {slot: {submesh: {token: [r,g,b]}}}. One entry per
        //     pick - stays small even when the slot has 50+ rows.
        //   swatch_palette:   the captured-row structure (no
        //     values), nested {slot: {submesh: [token, token, ...]}}.
        //     Lets the loader recreate every row the user has seen,
        //     so untickled rows reappear in the picker on switch back.
        json overrides_obj = json::object();
        json palette_obj = json::object();
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            const auto &slot_name = Transmog::slot_meta(static_cast<TransmogSlot>(i)).display_name;
            const auto &ov_entries = p.swatch_overrides[i];
            if (!ov_entries.empty())
            {
                json submeshes = submeshes_to_json(ov_entries);
                if (!submeshes.empty())
                    overrides_obj[slot_name] = std::move(submeshes);
            }
            const auto &pa_entries = p.swatch_palette[i];
            if (!pa_entries.empty())
            {
                json palette = palette_to_json(pa_entries);
                if (!palette.empty())
                    palette_obj[slot_name] = std::move(palette);
            }
        }
        if (!overrides_obj.empty())
            out["swatch_overrides"] = std::move(overrides_obj);
        if (!palette_obj.empty())
            out["swatch_palette"] = std::move(palette_obj);

        // Per-slot master-enable flags. Only emit when at least one slot is true, to keep the JSON tidy for unused
        // presets.
        bool any_enabled = false;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            if (p.swatch_slot_enabled[i])
            {
                any_enabled = true;
                break;
            }
        if (any_enabled)
        {
            json enabled_obj = json::object();
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                if (!p.swatch_slot_enabled[i])
                    continue;
                enabled_obj[Transmog::slot_meta(static_cast<TransmogSlot>(i)).display_name] = true;
            }
            out["swatch_slot_enabled"] = std::move(enabled_obj);
        }

        return out;
    }

    static Preset preset_from_json(const json &j)
    {
        Preset p;
        p.name = j.value("name", std::string("Unnamed"));

        if (!j.contains("slots"))
            return p;

        const auto &slots_j = j["slots"];

        // New format: object keyed by slot display_name.
        if (slots_j.is_object())
        {
            for (auto it = slots_j.begin(); it != slots_j.end(); ++it)
            {
                bool matched = false;
                for (std::size_t i = 0; i < SLOT_COUNT; ++i)
                {
                    const auto &m = SLOT_METADATA[i];
                    if (it.key() == m.display_name)
                    {
                        p.slots[i] = slot_from_json(it.value());
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    DMK::log().warning(
                        "[preset] '{}' has unknown slot key '{}' - ignored (slot may have been renamed or removed)",
                        p.name,
                        it.key()
                    );
                }
            }
            preset_swatches_from_json(p, j);
            return p; // skip legacy-array fallthrough
        }

        // Positional-array form. Indices map directly to TransmogSlot enum order. The next save rewrites it as a
        // keyed object.
        if (slots_j.is_array())
        {
            DMK::log().info(
                "[preset] '{}' loaded from legacy array format - next save will rewrite as keyed object",
                p.name
            );
            for (std::size_t i = 0; i < SLOT_COUNT && i < slots_j.size(); ++i)
                p.slots[i] = slot_from_json(slots_j[i]);
        }
        preset_swatches_from_json(p, j);
        return p;
    }

    // Apply the swatch_overrides + swatch_slot_enabled JSON blocks (top-level on a preset, alongside `slots`). Tolerant
    // of both object form (keyed by slot display_name) and array form (positional by TransmogSlot index). Missing
    // entirely = no overrides for this preset.
    static void preset_swatches_from_json(Preset &p, const json &j)
    {
        if (j.contains("swatch_overrides") && j["swatch_overrides"].is_object())
        {
            const auto &so = j["swatch_overrides"];
            for (auto it = so.begin(); it != so.end(); ++it)
            {
                for (std::size_t i = 0; i < SLOT_COUNT; ++i)
                {
                    if (it.key() == SLOT_METADATA[i].display_name)
                    {
                        auto &dst = p.swatch_overrides[i];
                        dst.clear();
                        append_overrides_from_json(dst, it.value());
                        break;
                    }
                }
            }
        }

        if (j.contains("swatch_palette") && j["swatch_palette"].is_object())
        {
            const auto &sp = j["swatch_palette"];
            for (auto it = sp.begin(); it != sp.end(); ++it)
            {
                for (std::size_t i = 0; i < SLOT_COUNT; ++i)
                {
                    if (it.key() == SLOT_METADATA[i].display_name)
                    {
                        auto &dst = p.swatch_palette[i];
                        dst.clear();
                        append_palette_from_json(dst, it.value());
                        break;
                    }
                }
            }
        }

        if (j.contains("swatch_slot_enabled"))
        {
            const auto &se = j["swatch_slot_enabled"];
            if (se.is_object())
            {
                for (auto it = se.begin(); it != se.end(); ++it)
                {
                    for (std::size_t i = 0; i < SLOT_COUNT; ++i)
                    {
                        if (it.key() == SLOT_METADATA[i].display_name)
                        {
                            p.swatch_slot_enabled[i] = it.value().is_boolean() && it.value().get<bool>();
                            break;
                        }
                    }
                }
            }
            else if (se.is_array())
            {
                for (std::size_t i = 0; i < SLOT_COUNT && i < se.size(); ++i)
                    p.swatch_slot_enabled[i] = se[i].is_boolean() && se[i].get<bool>();
            }
        }
    }

    static json character_to_json(const CharacterPresets &cp)
    {
        json presets_arr = json::array();
        for (const auto &p : cp.presets)
            presets_arr.push_back(preset_to_json(p));

        json out{
            {"activePreset", cp.active_preset},
            {"presets", presets_arr},
            {"bodyKind", cp.body_kind},
        };
        // color_override session toggle. Persisted only when true to keep the JSON tidy for fresh users.
        if (cp.dye_advanced_view)
            out["dyeAdvancedView"] = true;
        return out;
    }

    static CharacterPresets character_from_json(const json &j)
    {
        CharacterPresets cp;
        cp.active_preset = j.value("activePreset", 0);
        cp.body_kind = j.value("bodyKind", std::string("Auto"));
        cp.dye_advanced_view = j.value("dyeAdvancedView", false);

        if (j.contains("presets") && j["presets"].is_array())
        {
            for (const auto &pj : j["presets"])
                cp.presets.push_back(preset_from_json(pj));
        }

        // Clamp active index.
        if (!cp.presets.empty())
            cp.active_preset = std::clamp(cp.active_preset, 0, static_cast<int>(cp.presets.size()) - 1);
        else
            cp.active_preset = 0;

        return cp;
    }

    // Per-preset color_override snapshot/restore helpers
    //
    // Used by every preset-switch / character-switch / load path to sync `Preset::swatch_overrides` +
    // `Preset::swatch_slot_enabled` with the live swatch_table state. The pattern is:
    //
    //   snapshot_live_swatches_into(prev_preset)  // capture current
    //   color_override::reset_all()                // wipe live tables
    //   active_preset = new                       // switch
    //   restore_swatches_from(new_preset)         // re-seed live tables
    //   apply_to_state()                         // push slot mappings
    //   save()                                   // persist (this also
    //                                            //   re-snapshots into
    //                                            //   the new active)
    //
    // INDEPENDENT of dye_mods / dye_record_inject - that path lives in PresetSlot::dye and is untouched here. `force`
    // is true on the explicit-save path (replace_current_from_state / Save button), which always captures live state
    // into the active preset. Default false: a caller on the preset-switch path skips the capture while dye_dirty()
    // is true, which discards pending edits on switch. The user must click Save to commit them.
    //
    // ORDER: every preset switch calls this BEFORE revert_active_dye_to_snapshot. The revert clears dye_dirty, so a
    // snapshot that runs after it captures the user's unsaved swatch picks and auto-saves them into the outgoing
    // preset.
    static void snapshot_live_swatches_into(Preset &p, bool force = false)
    {
        if (!force && dye_dirty().load(std::memory_order_acquire))
        {
            // Pending edits get discarded on switch. Leave `p.swatch_overrides` at its loaded baseline so a later
            // switch BACK to this preset restores the on-disk state, not the unsaved edits.
            return;
        }
        // When the color_override feature is disabled the live swatch_table is inert and its persist helpers return
        // empty vectors. A write of those over the preset's loaded baseline silently destroys any swatch_overrides,
        // swatch_palette or swatch_slot_enabled entry the JSON already carries. Leave the preset's swatch fields
        // untouched in that case so the saved data round-trips intact through "disabled" sessions.
        if (!flag_color_override().load(std::memory_order_acquire))
            return;
        namespace st = color_override::swatch_table;
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
        {
            auto live_ov = st::get_persistable_overrides(static_cast<int>(s));
            auto live_pal = st::get_persistable_palette(static_cast<int>(s));
            // Live swatch_table can legitimately be empty for a slot even when the user has saved overrides for it:
            //   - On cold game-load, populate_from_persisted drops entries whose token names the engine did not
            //     intern yet (token_id_for_name returns 0). No placeholder row gets seeded, so the live table stays
            //     empty for that slot until either the setter intercepts an engine write or a later retry pass resolves
            //     the token.
            //   - The slot may never have been equipped this session.
            //   - The slot's apply pipeline may have skipped because dispatcher state filtered it out.
            // Writing the empty live vector over the preset's baseline in any of those cases silently destroys the
            // user's saved JSON. Preserve the baseline. The live table repopulates as tokens resolve, and the next
            // genuine edit marks dye_dirty so a real capture proceeds through the force=true save path.
            const bool live_empty = live_ov.empty() && live_pal.empty();
            const bool saved_exists = !p.swatch_overrides[s].empty() || !p.swatch_palette[s].empty();
            // User-intentional empty case: Reset Slot wipes the live table and flags the slot. Without this exception
            // the guard treats the wipe as a token-race and reverts to the JSON baseline, which leaves Reset Slot
            // impossible to commit through Save.
            const bool wiped = st::slot_was_explicitly_wiped(static_cast<int>(s));
            if (live_empty && saved_exists && !wiped)
                continue;
            p.swatch_overrides[s] = std::move(live_ov);
            p.swatch_palette[s] = std::move(live_pal);
            p.swatch_slot_enabled[s] = st::slot_enabled_get(static_cast<int>(s));
            // Empty state is now committed in the preset. Drop the wipe flag so subsequent saves follow the normal
            // guard (saved_exists becomes false, the empty live write is a no-op).
            if (wiped)
                st::clear_explicit_wipe_flag(static_cast<int>(s));
        }
    }

    static void restore_swatches_from(const Preset &p)
    {
        namespace st = color_override::swatch_table;
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
        {
            // Queue ONLY the user-override RGB into the pending_overrides map. The palette needs no queue entry -
            // those rows get seeded directly in auto_reinit_from via populate_from_persisted.
            if (!p.swatch_overrides[s].empty())
                st::restore_persisted_state(static_cast<int>(s), p.swatch_overrides[s]);
            st::slot_enabled_set(static_cast<int>(s), p.swatch_slot_enabled[s]);
        }
    }

    // Auto-init slots that have saved overrides:
    // populate_from_persisted seeds placeholders from JSON. The preset-switch path already tears down + reapplies every
    // slot, so the engine re-emits setter writes and lookup_or_insert's promotion pass on those writes promotes
    // each placeholder. No extra retick is needed here.
    //
    // Slots WITHOUT saved data are NOT auto-init'd here - the user clicks Re-init manually when they want to discover
    // a new outfit's swatches.
    static void auto_reinit_from(const Preset &p)
    {
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
        {
            if (p.swatch_palette[s].empty() && p.swatch_overrides[s].empty())
                continue;
            color_override::swatch_table::populate_from_persisted(
                static_cast<int>(s),
                p.swatch_palette[s],
                p.swatch_overrides[s]
            );
        }
    }

    // PresetManager

    PresetManager &PresetManager::instance()
    {
        static PresetManager s_instance;
        return s_instance;
    }

    bool PresetManager::load(const std::filesystem::path &path)
    {
        auto &logger = DMK::log();
        m_file_path = path;

        std::ifstream file(path);
        if (!file.is_open())
        {
            logger.info("Presets file not found - starting with defaults");

            // Create default entries for known characters.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            return true;
        }

        try
        {
            json root = json::parse(file);

            // The read discards `activeCharacter` so an unknown key cannot fail the parse. The live WS chain drives
            // the controlled character through the char-swap detector, and the editing character is session-only.
            (void)root.value("activeCharacter", std::string{});

            if (root.contains("characters") && root["characters"].is_object())
            {
                for (auto &[name, cj] : root["characters"].items())
                {
                    // Skip a nameless entry a previous run persisted. It cannot be selected or applied, and
                    // loading it would put an empty row in the character dropdown.
                    if (name.empty())
                        continue;
                    m_characters[name] = character_from_json(cj);
                }
            }

            // Global preferences. An absent block keeps the constructed defaults, so a file written before the block
            // existed loads unchanged.
            if (root.contains("settings") && root["settings"].is_object())
            {
                const auto &settings = root["settings"];
                std::lock_guard<std::mutex> lk(m_settings_mutex);
                m_display_name_locale = settings.value("displayNameLocale", std::string{"eng"});
                m_interface_locale = settings.value("interfaceLocale", std::string{"auto"});
                if (m_interface_locale.empty())
                    m_interface_locale = "auto";
                if (m_display_name_locale.empty())
                    m_display_name_locale = "eng";
                // A corrupt or hand-edited value must not make the overlay unusable, so clamp each one to the range
                // its own control offers.
                m_ui_prefs.ui_scale =
                    std::clamp(settings.value("uiScale", 1.0f), UiPrefs::UI_SCALE_MIN, UiPrefs::UI_SCALE_MAX);
                m_ui_prefs.instant_apply = settings.value("instantApply", false);
                m_ui_prefs.keep_search_text = settings.value("keepSearchText", true);
                m_ui_prefs.preset_rows =
                    std::clamp(settings.value("presetRows", 10.0f), UiPrefs::PRESET_ROWS_MIN, UiPrefs::PRESET_ROWS_MAX);
            }

            // Stash every top-level key this build does not know, so save() can write it back. Without it an older
            // build would silently strip a newer build's preferences from a shared file.
            {
                json foreign = json::object();
                for (const auto &[key, value] : root.items())
                {
                    if (key != "version" && key != "characters" && key != "settings" && key != "activeCharacter")
                        foreign[key] = value;
                }
                m_foreign_root_keys = foreign.empty() ? std::string{} : foreign.dump();
            }

            // Ensure known characters exist even if not in the file.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            logger.info("Presets loaded: {} character(s) from '{}'", m_characters.size(), to_utf8(path));

            if (!ItemNameTable::instance().ready())
            {
                logger.info(
                    "[preset] item catalog not ready at load time - name "
                    "resolution deferred until background scan completes"
                );
            }
        }
        catch (const json::exception &e)
        {
            logger.warning("Failed to parse presets file: {}", e.what());
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");
            return false;
        }

        // The loaded state matches disk. Clear any dirty signal left by an earlier session of the process and
        // snapshot the active preset's dye as the reference baseline.
        dye_dirty().store(false, std::memory_order_release);
        capture_dye_snapshot();

        // Push the active preset's persisted color_override swatch state into the live swatch_table. Independent of
        // dye_mods (which is part of slot_mappings, not swatch_table). Safe to run before color_override::init() -
        // restore mutates storage arrays directly and does not require hooks to be live.
        {
            auto it = m_characters.find(m_editing_character);
            if (it != m_characters.end() && !it->second.presets.empty())
            {
                const auto idx =
                    std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
                const auto &preset = it->second.presets[static_cast<std::size_t>(idx)];
                restore_swatches_from(preset);
                // Direct-seed the picker rows from saved data, so the user sees their colors on game load with
                // no manual Re-init. The first engine write promotes each placeholder to a live identity.
                auto_reinit_from(preset);
            }
        }

        return true;
    }

    bool PresetManager::save() const
    {
        return save(m_file_path);
    }

    std::string PresetManager::display_name_locale() const
    {
        std::lock_guard<std::mutex> lk(m_settings_mutex);
        return m_display_name_locale;
    }

    PresetManager::UiPrefs PresetManager::ui_prefs() const noexcept
    {
        std::lock_guard<std::mutex> lk(m_settings_mutex);
        return m_ui_prefs;
    }

    bool PresetManager::set_ui_prefs(const UiPrefs &prefs)
    {
        {
            std::lock_guard<std::mutex> lk(m_settings_mutex);
            m_ui_prefs = prefs;
        }
        return save_settings();
    }

    bool PresetManager::set_display_name_locale(std::string_view tag)
    {
        {
            std::lock_guard<std::mutex> lk(m_settings_mutex);
            m_display_name_locale = tag.empty() ? std::string{"eng"} : std::string{tag};
        }
        return save_settings();
    }

    std::string PresetManager::interface_locale() const
    {
        std::lock_guard<std::mutex> lk(m_settings_mutex);
        return m_interface_locale;
    }

    bool PresetManager::set_interface_locale(std::string_view tag)
    {
        {
            std::lock_guard<std::mutex> lk(m_settings_mutex);
            m_interface_locale = tag.empty() ? std::string{"auto"} : std::string{tag};
        }
        return save_settings();
    }

    /// Builds the `settings` block, so the full save and the settings-only save cannot drift apart.
    [[nodiscard]] static json settings_block(
        std::string_view display_name_locale,
        std::string_view interface_locale,
        const PresetManager::UiPrefs &prefs
    )
    {
        json settings = json::object();
        settings["displayNameLocale"] = display_name_locale;
        settings["interfaceLocale"] = interface_locale;
        settings["uiScale"] = prefs.ui_scale;
        settings["instantApply"] = prefs.instant_apply;
        settings["keepSearchText"] = prefs.keep_search_text;
        settings["presetRows"] = prefs.preset_rows;
        return settings;
    }

    bool PresetManager::save_settings() const
    {
        auto &logger = DMK::log();
        if (m_file_path.empty())
            return false;

        // Start from the file as it stands, so a preference write preserves characters, version and any key a newer
        // build wrote. A file that exists but cannot be read or parsed is left ALONE: rebuilding the root from a
        // preference write would drop every character it holds, and a preference is not worth that. Only an absent
        // file is created from scratch, and the next full save() fills it out.
        json root = json::object();
        std::error_code ec;
        if (std::filesystem::exists(m_file_path, ec))
        {
            std::ifstream file(m_file_path);
            json parsed = file.is_open() ? json::parse(file, nullptr, false) : json{};
            if (!parsed.is_object())
            {
                logger.warning(
                    "Settings not written: '{}' is unreadable and would lose its presets",
                    to_utf8(m_file_path)
                );
                return false;
            }
            root = std::move(parsed);
        }
        root["settings"] = settings_block(display_name_locale(), interface_locale(), ui_prefs());

        std::ofstream out(m_file_path);
        if (!out.is_open())
        {
            logger.warning("Failed to write settings to '{}'", to_utf8(m_file_path));
            return false;
        }
        out << root.dump(2);
        return true;
    }

    bool PresetManager::save(const std::filesystem::path &path) const
    {
        auto &logger = DMK::log();

        json root;
        // Schema v3: slots carry only item_name (stable identifier). item_id is resolved at runtime from the item
        // catalog.
        root["version"] = 3;
        // Neither the controlled character nor the editing character are serialized. Controlled is driven by the live
        // WS chain at runtime. Editing is a session-only UI affordance that resets to controlled on load.

        // Capture the live swatch_table into the active preset so the user's current picks land in JSON. Independent of
        // dye_mods (which lives in PresetSlot::dye and is captured/edited via the picker UI directly).
        //
        // const_cast on the active preset is sound for the same reason `mutable m_dye_snapshot` is: this is
        // logical-const cache state - the preset's `swatch_overrides` field is a write-through cache of the live
        // swatch_table, synced at save / switch / character-swap. Save remains externally const (the on-disk file is
        // the source of truth for what the user committed. This refreshes the in-memory copy to match what is about to
        // be written).
        {
            auto it = m_characters.find(m_editing_character);
            if (it != m_characters.end() && !it->second.presets.empty())
            {
                const auto idx =
                    std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
                auto &mut_preset = const_cast<Preset &>(it->second.presets[static_cast<std::size_t>(idx)]);
                // force=true: save is the explicit commit path, so capture pending edits even when dye_dirty is set.
                snapshot_live_swatches_into(mut_preset, /*force=*/true);
            }
        }

        json chars = json::object();
        for (const auto &[name, cp] : m_characters)
        {
            // Never write a nameless character. Dropping it here keeps the entry out of the file and heals a
            // file that already carries one. See the matching guards in load, character_names and
            // ensure_character.
            if (name.empty())
                continue;
            chars[name] = character_to_json(cp);
        }

        root["characters"] = chars;

        root["settings"] = settings_block(display_name_locale(), interface_locale(), ui_prefs());

        // Replay the keys a newer build wrote. The stash holds only keys this build does not know, and the
        // contains() guard repeats that check against the root just built, so a replay can never overwrite a value
        // this save produced.
        if (!m_foreign_root_keys.empty())
        {
            if (const json foreign = json::parse(m_foreign_root_keys, nullptr, false); foreign.is_object())
            {
                for (const auto &[key, value] : foreign.items())
                {
                    if (!root.contains(key))
                        root[key] = value;
                }
            }
        }

        std::ofstream file(path);
        if (!file.is_open())
        {
            logger.warning("Failed to write presets to '{}'", to_utf8(path));
            return false;
        }

        file << root.dump(2);
        logger.info("Presets saved to '{}'", to_utf8(path));
        // Deliberately no dump_all_slots() here. It walks every row of every populated slot, so an automatic call on
        // each save buries the log under hundreds of lines describing state the written JSON already holds. The
        // function stays available (see color_swatch_table.hpp) for a deliberate, on-demand dump while a color
        // problem is under investigation.
        dye_dirty().store(false, std::memory_order_release);
        // The saved state IS the new baseline. A later edit counts as dirty relative to this point.
        capture_dye_snapshot();
        return true;
    }

    // Character management

    std::vector<std::string> PresetManager::character_names() const
    {
        std::vector<std::string> names;
        names.reserve(m_characters.size());
        for (const auto &entry : m_characters)
        {
            // The dropdown is built straight from this, so a nameless entry would draw as a blank row.
            if (entry.first.empty())
                continue;
            names.push_back(entry.first);
        }
        return names;
    }

    const std::string &PresetManager::active_character() const
    {
        return m_controlled_character;
    }

    void PresetManager::rotate_editing_target_to(const std::string &new_name)
    {
        // Snapshot the OUTGOING editing character's active preset live swatches before the flip. Without it, a
        // switch back loses the per-shader-property picks made on the old character. dye_mods (PresetSlot::dye)
        // is independent and managed by the dye snapshot path below.
        auto prev = m_characters.find(m_editing_character);
        if (prev != m_characters.end() && !prev->second.presets.empty())
        {
            const auto idx =
                std::clamp(prev->second.active_preset, 0, static_cast<int>(prev->second.presets.size()) - 1);
            snapshot_live_swatches_into(prev->second.presets[static_cast<std::size_t>(idx)]);
        }
        color_override::reset_all();

        revert_active_dye_to_snapshot();
        m_editing_character = new_name;
        ensure_character(new_name);
        capture_dye_snapshot();

        // Re-seed the live swatch_table from the NEW editing character's active preset. Auto-reinit the slots that
        // carry saved overrides so the locked tables repopulate.
        auto it = m_characters.find(m_editing_character);
        if (it != m_characters.end() && !it->second.presets.empty())
        {
            const auto idx = std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
            const auto &preset = it->second.presets[static_cast<std::size_t>(idx)];
            restore_swatches_from(preset);
            auto_reinit_from(preset);
        }
    }

    void PresetManager::set_active_character(const std::string &name)
    {
        DMK::log().info(
            "[preset] set_active_character('{}') prev='{}' (changed={})",
            name,
            m_controlled_character,
            m_controlled_character != name
        );
        m_controlled_character = name;
        ensure_character(name);
        if (!m_editing_pinned)
        {
            // Editing follows controlled whenever the user has not explicitly pinned a different editing target. Rotate
            // both the dye snapshot AND color_override swatch state on the outgoing editing character so unsaved edits
            // do not bleed into the incoming character's preset.
            if (name != m_editing_character)
            {
                rotate_editing_target_to(name);
            }
        }
        else if (name == m_editing_character)
        {
            // The player is now controlling the very character the user had pinned for editing. The pin no longer
            // represents anything distinct, so it auto-clears.
            m_editing_pinned = false;
        }
    }

    const std::string &PresetManager::editing_character() const
    {
        return m_editing_character;
    }

    void PresetManager::set_editing_character(const std::string &name)
    {
        if (name == m_editing_character)
        {
            // No-op switch: keep the pin state consistent against controlled and return without touching the snapshot.
            m_editing_pinned = (name != m_controlled_character);
            ensure_character(name);
            return;
        }

        // Switching the editing character is a larger context shift than cycling presets: rotate the dye snapshot AND
        // the color_override swatch state onto the incoming preset. Mirrors set_active_preset().
        rotate_editing_target_to(name);
        // The pin engages when the user picks anyone other than the controlled character. A pick of the controlled
        // character is the unpin gesture from the dropdown.
        m_editing_pinned = (name != m_controlled_character);
    }

    bool PresetManager::editing_pinned() const noexcept
    {
        return m_editing_pinned;
    }

    void PresetManager::clear_editing_pin()
    {
        if (m_editing_character != m_controlled_character)
        {
            // Rotate the dye snapshot AND the color_override swatch state onto the controlled character's active
            // preset.
            rotate_editing_target_to(m_controlled_character);
        }
        m_editing_pinned = false;
    }

    std::string PresetManager::body_kind_of(const std::string &char_name) const
    {
        auto it = m_characters.find(char_name);
        if (it == m_characters.end())
            return "Auto";
        return it->second.body_kind.empty() ? std::string("Auto") : it->second.body_kind;
    }

    void PresetManager::set_body_kind_of(const std::string &char_name, const std::string &body_kind)
    {
        auto &cp = ensure_character(char_name);
        // Normalize to a known value. Anything else collapses to "Auto", which keeps the JSON clean and gives the
        // picker filter a defined fallback.
        if (body_kind == "Male" || body_kind == "Female" || body_kind == "Both" || body_kind == "Auto")
        {
            cp.body_kind = body_kind;
        }
        else
        {
            cp.body_kind = "Auto";
        }
        save();
    }

    // Preset management

    int PresetManager::active_preset_index() const
    {
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end())
            return 0;
        return it->second.active_preset;
    }

    int PresetManager::preset_count() const
    {
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end())
            return 0;
        return static_cast<int>(it->second.presets.size());
    }

    const Preset *PresetManager::active_preset() const
    {
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const Preset *PresetManager::active_preset_of(const std::string &char_name) const
    {
        // Read-only sibling of active_preset() that targets an arbitrary character. Used by the body-mesh prefab picker
        // to borrow the Kairos (default-carrier) preset's itemIds without disturbing the controlled or editing axes -
        // a mutation of either field cascades into a save and races with the load-detect commit branch.
        auto it = m_characters.find(char_name);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    Preset *PresetManager::active_preset_mut()
    {
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.active_preset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    Preset *PresetManager::active_preset_mut_or_create()
    {
        if (auto *p = active_preset_mut())
            return p;
        // No preset for the editing character yet - mint one from the current in-game/edit state so the caller (e.g.
        // the Dye picker) has somewhere to write, instead of swallowing the interaction. Mirrors the auto-create branch
        // of replace_current_from_state().
        auto &cp = ensure_character(m_editing_character);
        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        cp.presets.push_back(capture_from_state(name));
        cp.active_preset = idx;
        DMK::log().info("Preset auto-created for dye/edit: '{}' (index {})", name, idx);
        save();
        return active_preset_mut();
    }

    const std::vector<Preset> &PresetManager::presets() const
    {
        static const std::vector<Preset> s_empty;
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end())
            return s_empty;
        return it->second.presets;
    }

    void PresetManager::append_from_state()
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto &cp = ensure_character(editing);
        // Snapshot the OUTGOING preset's swatches so they survive the active-index flip. Then wipe live state,
        // because the new blank preset carries none.
        if (!cp.presets.empty())
        {
            const auto prev_idx = std::clamp(cp.active_preset, 0, static_cast<int>(cp.presets.size()) - 1);
            snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(prev_idx)]);
        }
        color_override::reset_all();

        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        Preset blank;
        blank.name = name;
        // Default ticks: only the five armor slots (Helm, Chest, Cloak, Gloves, Boots). Gear/accessory slots (Lantern,
        // weapons, rings, etc.) stay unticked so a freshly-appended preset does not hide working in-game
        // items (e.g. a lantern the user still wants lit).
        for (std::size_t i = 0; i < blank.slots.size(); ++i)
        {
            const auto slot = static_cast<TransmogSlot>(i);
            blank.slots[i].active =
                (slot == TransmogSlot::Helm || slot == TransmogSlot::Chest || slot == TransmogSlot::Cloak ||
                 slot == TransmogSlot::Gloves || slot == TransmogSlot::Boots);
            blank.slots[i].item_id = 0;
            blank.slots[i].item_name.clear();
        }
        cp.presets.push_back(std::move(blank));
        cp.active_preset = idx;

        apply_to_state();

        DMK::log().info("Preset appended: '{}' (index {}, armor slots ticked + none)", name, idx);
        save();
    }

    void PresetManager::duplicate_current()
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto &cp = ensure_character(editing);
        const int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);

        Preset clone;
        clone.name = name;
        // Pure clone of the source preset's in-memory state. Pending edits in slot_mappings are intentionally ignored
        // so the new preset matches the source as a reload from disk produces it. To fork pending edits instead,
        // call save_as_new_from_state.
        if (const auto *src = active_preset())
        {
            clone.slots = src->slots;
            clone.swatch_overrides = src->swatch_overrides;
            clone.swatch_palette = src->swatch_palette;
            clone.swatch_slot_enabled = src->swatch_slot_enabled;
        }
        cp.presets.push_back(std::move(clone));
        cp.active_preset = idx;

        apply_to_state();

        DMK::log().info("Preset duplicated (clean clone): '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::save_as_new_from_state()
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto &cp = ensure_character(editing);
        const int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        auto captured = capture_from_state(name);
        // Preserve dye/swatch from the source preset. slot_mappings does not carry dye state, and dye/swatch are
        // written in-place on the active preset by the picker, so src already reflects the user's current live state on
        // those axes.
        if (const auto *src = active_preset())
        {
            for (std::size_t i = 0; i < src->slots.size() && i < captured.slots.size(); ++i)
            {
                captured.slots[i].dye = src->slots[i].dye;
                // Carry the source slot's sparse/dense origin flag across the fork. capture_from_state() builds a fresh
                // PresetSlot from slot_mappings, which has no notion of dye_sparse, so the captured slot is left at the
                // PresetSlot default. That default does not necessarily match the source preset's mode (a captured
                // preset uses sparse, a picker-built preset need not), so the copy takes the source value
                // explicitly and keeps the fork visually identical to its origin.
                captured.slots[i].dye_sparse = src->slots[i].dye_sparse;
            }
            captured.swatch_overrides = src->swatch_overrides;
            captured.swatch_palette = src->swatch_palette;
            captured.swatch_slot_enabled = src->swatch_slot_enabled;
        }
        cp.presets.push_back(std::move(captured));
        cp.active_preset = idx;

        apply_to_state();

        DMK::log().info("Preset saved as new (pending state): '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::replace_current_from_state()
    {
        auto *p = active_preset_mut();
        if (!p)
        {
            auto &cp = ensure_character(m_editing_character);
            int idx = static_cast<int>(cp.presets.size());
            std::string name = "Preset " + std::to_string(idx);
            cp.presets.push_back(capture_from_state(name));
            cp.active_preset = idx;

            DMK::log().info("Preset replaced: '{}' (index {})", name, idx);
            save();
            return;
        }

        auto captured = capture_from_state(p->name);
        // Dye state is not part of slot_mappings. The picker writes it directly onto the active preset's
        // PresetSlot::dye, and capture_from_state() rebuilds slots from slot_mappings, so it clobbers both the dye
        // edits AND the dye_sparse flag. Carry both forward from the existing preset, so the capture_outfit -> save
        // and "Replace" button paths preserve user-curated dye and its sparse-or-dense origin. A fresh capture_outfit
        // sets dye_sparse=true. The dye picker leaves it at whatever it already held.
        for (std::size_t i = 0; i < p->slots.size() && i < captured.slots.size(); ++i)
        {
            captured.slots[i].dye = p->slots[i].dye;
            captured.slots[i].dye_sparse = p->slots[i].dye_sparse;
        }
        p->slots = captured.slots;
        // color_override swatch overrides are NOT part of slot_mappings - snapshot the live swatch_table so the
        // "Replace" button persists the user's current per-shader-property picks too. `force=true` overrides the
        // dye_dirty gate in the snapshot helper: the user explicitly asked to save, so capture pending edits even when
        // the dirty flag is set.
        snapshot_live_swatches_into(*p, /*force=*/true);

        DMK::log().info("Preset replaced: '{}'", p->name);
        save();
    }

    void PresetManager::remove_current()
    {
        auto it = m_characters.find(m_editing_character);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        auto idx = std::clamp(cp.active_preset, 0, static_cast<int>(cp.presets.size()) - 1);

        DMK::log().info("Preset removed: '{}'", cp.presets[static_cast<std::size_t>(idx)].name);

        cp.presets.erase(cp.presets.begin() + idx);

        if (cp.presets.empty())
            cp.active_preset = 0;
        else
            cp.active_preset = std::min(idx, static_cast<int>(cp.presets.size()) - 1);

        // The deleted preset took its swatch state with it. Wipe the live swatch_table so the next apply rebuilds
        // cleanly, then restore from the new active preset (if any). Independent of dye_mods.
        color_override::reset_all();
        if (!cp.presets.empty())
        {
            const auto &preset = cp.presets[static_cast<std::size_t>(cp.active_preset)];
            restore_swatches_from(preset);
            auto_reinit_from(preset);
        }

        save();
    }

    void PresetManager::next_preset()
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto it = m_characters.find(editing);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Snapshot the OUTGOING swatch state first (see snapshot_live_swatches_into for the order rule).
        auto &cp = it->second;
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(
            std::clamp(cp.active_preset, 0, static_cast<int>(cp.presets.size()) - 1)
        )]);

        // Discard unsaved dye-mod edits before cycling.
        revert_active_dye_to_snapshot();
        color_override::reset_all();

        cp.active_preset = (cp.active_preset + 1) % static_cast<int>(cp.presets.size());
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);

        DMK::log().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.active_preset)].name,
            cp.active_preset + 1,
            cp.presets.size()
        );

        // See set_active_preset for the rationale - preset switch must force re-apply so dye state is rebuilt.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        // Strict-init: trigger reinit on every slot with saved overrides so the locked tables re-populate from this
        // preset's outfit.
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);
        save();
    }

    void PresetManager::prev_preset()
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto it = m_characters.find(editing);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Snapshot the OUTGOING swatch state first (see snapshot_live_swatches_into for the order rule).
        auto &cp = it->second;
        int count = static_cast<int>(cp.presets.size());
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(std::clamp(cp.active_preset, 0, count - 1))]);

        revert_active_dye_to_snapshot();
        color_override::reset_all();

        cp.active_preset = (cp.active_preset - 1 + count) % count;
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);

        DMK::log().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.active_preset)].name,
            cp.active_preset + 1,
            cp.presets.size()
        );

        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);
        save();
    }

    void PresetManager::capture_dye_snapshot() const noexcept
    {
        const auto *p = active_preset();
        if (!p)
        {
            m_dye_snapshot_valid = false;
            return;
        }
        m_dye_snapshot = {};
        for (std::size_t i = 0; i < p->slots.size() && i < m_dye_snapshot.size(); ++i)
        {
            m_dye_snapshot[i] = p->slots[i].dye;
        }
        m_dye_snapshot_valid = true;
    }

    void PresetManager::revert_active_dye_to_snapshot() noexcept
    {
        if (!m_dye_snapshot_valid)
            return;
        if (!dye_dirty().load(std::memory_order_acquire))
            return;
        auto *p = active_preset_mut();
        if (!p)
            return;
        for (std::size_t i = 0; i < p->slots.size() && i < m_dye_snapshot.size(); ++i)
        {
            p->slots[i].dye = m_dye_snapshot[i];
        }
        dye_dirty().store(false, std::memory_order_release);
        DMK::log().info("[preset] reverted unsaved dye edits on '{}'", p->name);
    }

    void PresetManager::set_active_preset(int index)
    {
        // Snapshot the editing axis once, for the reason stated on PresetManager::m_editing_character.
        const std::string editing = m_editing_character;
        auto it = m_characters.find(editing);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        DMK::log().info(
            "[preset] set_active_preset(index={}) char='{}' (prev_active={})",
            index,
            editing,
            it->second.active_preset
        );

        auto &cp = it->second;
        // Snapshot the OUTGOING color_override swatches first (see snapshot_live_swatches_into for the order rule).
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(
            std::clamp(cp.active_preset, 0, static_cast<int>(cp.presets.size()) - 1)
        )]);

        // Now drop any unsaved dye-mod edits on the OUTGOING preset - the new preset's dye state will be captured
        // fresh below.
        revert_active_dye_to_snapshot();
        color_override::reset_all();

        cp.active_preset = std::clamp(index, 0, static_cast<int>(cp.presets.size()) - 1);
        // Capture the NEW active preset's dye as the baseline. A later edit counts as dirty relative to it.
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);
        // A switch to a different preset shows the on-disk state for that preset. Nothing is left to save
        // until the user edits.
        dye_dirty().store(false, std::memory_order_release);
        // The dispatcher's slot_needs_work check is item-id based and misses a dye state change. When two presets
        // share the same carrier ids (e.g. a Kairos preset and a body-mesh prefab preset both built on Kairos carrier
        // 0x1521), the dispatcher skips the apply and stale dye records bleed through. Force re-apply for
        // every enabled slot so DyeCopier gets a fresh injection from the new preset's dye state (including the case
        // where the new preset carries no dye, so the injector skips and the engine's natural records win). It
        // costs a full slot rebuild on every preset switch, which is already the user's intent on a switch.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.active_preset)]);
    }

    void PresetManager::reseed_unresolved_persisted_swatches() const
    {
        if (!flag_color_override().load(std::memory_order_acquire))
            return;
        // While dye_dirty is set the user holds uncommitted edits: a per-slot Reset cleared the live table, a
        // checkbox toggle dropped a row, a color pick moved RGB. A re-seed from the JSON baseline here fights those
        // edits and resurrects rows the user wiped. The next Save snapshots whatever live state the user committed
        // to, so the retry only ever runs on a clean, freshly-loaded preset (dye_dirty == false). A cold-load
        // token-resolution race is still repaired, because load clears dirty before the per-frame retry starts.
        if (dye_dirty().load(std::memory_order_acquire))
            return;
        const auto *p = active_preset();
        if (!p)
            return;
        namespace st = color_override::swatch_table;
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
        {
            const auto &pal = p->swatch_palette[s];
            const auto &ovr = p->swatch_overrides[s];
            if (pal.empty() && ovr.empty())
                continue;
            // Live row count is the cheap "did anything seed yet" signal. Re-firing populate_from_persisted on a slot
            // that already has rows is a no-op for resolved entries (find_seeded short-circuits), so the only cost
            // worth avoiding is re-walking saved vectors on slots that are already done.
            if (st::detected_swatch_count(static_cast<int>(s)) != 0)
                continue;
            st::populate_from_persisted(static_cast<int>(s), pal, ovr);
        }
    }

    // state bridge

    void PresetManager::apply_to_state() const
    {
        namespace pws = Transmog::prefab_wrapper_swap;

        // Bind the editing character into pws before the active_preset() early-out below. Only apply_to_state() binds
        // s_active_char_idx. The picker's body-mesh path (set_selection plus the Instant-Apply manual_apply) does not.
        // For a character with no saved preset (active_preset() == nullptr) the bind therefore has to happen here,
        // ahead of the early return - otherwise the prefab-swap map stays unbound, apply_selections_to_swap_map()
        // bails at its active_idx < 1 guard, and the picked body mesh renders as the bare carrier instead of the chosen
        // prefab. The bind is independent of the preset, so set_selection can still mirror picks into the correct
        // per-char row and the swap arms.
        //
        // Snapshot the editing character ONCE and resolve everything from that snapshot.
        //
        // m_editing_character must NOT be read twice here - once for the bind and again inside active_preset(), which
        // re-reads the member. The load-detect thread mutates it through set_active_character ->
        // rotate_editing_target_to, so it can change BETWEEN the two reads: the bind then names one character while
        // the preset that follows belongs to another, and the restore loop below writes that preset's prefab picks
        // into the bound character's per-character row. Every later identity check passes, because by then the rows
        // genuinely ARE the bound character's registered selections, so neither the body ownership guards nor the
        // mappings-owner stamp can catch it.
        //
        // active_preset_of() is the read-only by-name sibling that exists for this reason.
        const std::string editing = m_editing_character;
        const auto bound_idx = CDCore::character_idx_from_name(editing);
        pws::set_active_char_idx(bound_idx);

        auto &mappings = slot_mappings();

        const auto *p = active_preset_of(editing);
        if (!p)
        {
            // A character with no saved preset still BINDS above - see the note there. What must not happen is
            // returning with the bind pointing at this character while `mappings` still holds the PREVIOUS
            // character's slots: apply_selections_to_swap_map writes the bound character's bucket from those
            // mappings, so the last character's targets land on this one.
            //
            // "No preset" means "nothing active", so say that rather than leaving someone else's answer in place.
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                mappings[i].active = false;
                mappings[i].target_item_id = 0;
            }
            slot_mappings_owner().store(bound_idx, std::memory_order_release);
            return;
        }

        // NOTE: must NOT touch last_applied_ids() here. last_ids tracks what apply_all_transmog has actively injected
        // into the game - the diff logic depends on last_ids holding the PREVIOUS applied state so it can compute
        // drop-masks on a preset switch. A write to last_ids here pre-populates it with the NEW preset values before
        // the apply runs, which breaks drop detection.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            // Disabled slots (multi-prefab non-armor / duplicate-tag - see slot_metadata.hpp `enabled` doc) cannot be
            // applied and are hidden from the picker. A legacy preset saved before these slots were disabled may still
            // carry active=true with a non-zero item_id. Force everything off here so downstream code (last_ids
            // clearing, carrier-borrow, pws sync below) does not see a stale ticked state for a slot the user has no
            // way to interact with.
            if (!Transmog::slot_enabled(i))
            {
                mappings[i].active = false;
                mappings[i].target_item_id = 0;
                continue;
            }

            mappings[i].active = p->slots[i].active;
            mappings[i].target_item_id = p->slots[i].item_id;

            // Body-mesh slots only persist `prefab_name`; the carrier item_id is derived here so the body actually
            // rendering the slot emits the wrapper pws's substitution map keys on. With per-character pws rows now
            // contributing to a single union swap map, the correct carrier owner is the editing character whenever the
            // pin is engaged - their body (or the controlled body, when cross-body apply is selected) needs to emit
            // the editing character's expected src wrapper so the editing character's row in s_swapMap matches and
            // substitutes to the picked prefab. The controlled character's carrier here under a pin installs the
            // controlled character's prefab on the targeted body, where it either no-ops or collides with the
            // controlled character's own row in the union map and renders that character's tgt by mistake.
            if (mappings[i].target_item_id == 0 && !p->slots[i].prefab_name.empty())
            {
                const auto carrier =
                    Transmog::default_carrier_for_slot(static_cast<TransmogSlot>(i), Transmog::current_apply_owner());
                if (carrier != 0)
                {
                    mappings[i].target_item_id = carrier;
                    mappings[i].active = true;
                }
            }
        }

        // Stamp ownership only now that `mappings` actually holds this character's slots. Stamping before the fill
        // leaves a window where the stamp says one character and the contents are still the previous one's.
        slot_mappings_owner().store(bound_idx, std::memory_order_release);

        // Sync body-mesh prefab selections. For each slot, if the preset stores a prefab_name, look it up in the slot's
        // catalog and set the pws target index. Empty prefab_name clears the target (slot reverts to plain carrier
        // rendering). While the catalog is unpopulated (boot heap walk in progress) resolution silently misses. The
        // boot thread re-runs apply_to_state when populate_slot_catalogs finishes, so the selection lands as soon as
        // the data is available.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            const auto tslot = static_cast<TransmogSlot>(i);
            const int cur_src = pws::selection_src_index(tslot);
            // Force-clear any persisted pws selection on disabled slots so a legacy preset's body-mesh prefab cannot
            // keep a target index live, which the natural-pipeline hook otherwise still applies against.
            if (!Transmog::slot_enabled(i))
            {
                pws::set_selection(tslot, cur_src, -1, "apply_to_state", bound_idx);
                continue;
            }
            const auto &name = p->slots[i].prefab_name;
            if (name.empty())
            {
                pws::set_selection(tslot, cur_src, -1, "apply_to_state", bound_idx);
                continue;
            }
            const auto &cat = pws::slot_catalog(tslot);
            int found = -1;
            for (std::size_t k = 0; k < cat.size(); ++k)
            {
                if (cat[k].name == name)
                {
                    found = static_cast<int>(k);
                    break;
                }
            }
            pws::set_selection(tslot, cur_src, found, "apply_to_state", bound_idx);
        }
    }

    Preset PresetManager::capture_from_state(const std::string &name)
    {
        Preset p;
        p.name = name.empty() ? "Captured" : name;

        auto &mappings = slot_mappings();
        const auto &table = ItemNameTable::instance();
        namespace pws = Transmog::prefab_wrapper_swap;

        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            // Disabled slots (multi-prefab non-armor / duplicate-tag entries, see slot_metadata.hpp) are not yet
            // supported by the apply path. Do not bloat the JSON with their current in-memory state: leaving p.slots[i]
            // default-constructed serializes the row as `{"active":false,"item_name":""}` so the array indexing stays
            // positional but the entry is empty. Any stale data from an older preset (saved before the slot was
            // disabled) is discarded on the next save.
            if (!Transmog::slot_enabled(i))
                continue;

            p.slots[i].active = mappings[i].active;
            p.slots[i].item_id = mappings[i].target_item_id;

            // Capture the active body-mesh prefab name (target side of the swap) so it is restored on next load. The
            // src side is the hardcoded Kliff default and need not be persisted.
            const auto tslot = static_cast<TransmogSlot>(i);
            const int tgt_idx = pws::selection_tgt_index(tslot);
            if (tgt_idx >= 0)
            {
                const auto &cat = pws::slot_catalog(tslot);
                if (static_cast<std::size_t>(tgt_idx) < cat.size())
                    p.slots[i].prefab_name = cat[tgt_idx].name;
            }

            // With a body-mesh prefab set, the carrier item_id is an internal implementation detail: the
            // auto-borrowed Kairos plate that feeds the source wrapper. Do not persist item_name for these slots. The
            // JSON shows only the user-meaningful prefab_name, and load re-derives the carrier through
            // default_carrier_for_slot.
            if (!p.slots[i].prefab_name.empty())
            {
                p.slots[i].item_name.clear();
                continue;
            }

            // Plain carrier slot (no body-mesh override). Resolve item_name for round-trip persistence. id 0 means
            // "none", so no name resolves for it. The catalog's entry at index 0 is a real item (Pyeonjeon_Arrow),
            // and a save of that name loads an unintended item on the next reresolve.
            if (table.ready() && mappings[i].target_item_id != 0)
                p.slots[i].item_name = table.name_of(mappings[i].target_item_id);
            else
                p.slots[i].item_name.clear();
        }
        return p;
    }

    std::size_t PresetManager::reresolve_all_names()
    {
        auto &logger = DMK::log();
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
        {
            logger.warning("[preset] reresolve_all_names called before catalog ready - no-op");
            return 0;
        }

        std::size_t resolved = 0;
        std::size_t disabled = 0;

        for (auto &[char_name, cp] : m_characters)
        {
            for (auto &preset : cp.presets)
            {
                for (auto &slot : preset.slots)
                {
                    if (slot.item_name.empty())
                        continue;

                    auto fresh = table.id_of(slot.item_name);
                    if (fresh.has_value())
                    {
                        slot.item_id = *fresh;
                        ++resolved;
                    }
                    else
                    {
                        logger.warning(
                            "[preset] '{}' not in catalog - disabling (char='{}' preset='{}')",
                            slot.item_name,
                            char_name,
                            preset.name
                        );
                        slot.item_id = 0;
                        slot.active = false;
                        ++disabled;
                    }
                }
            }
        }

        logger.info("[preset] reresolve complete: {} resolved, {} disabled", resolved, disabled);
        return resolved;
    }

    // Private

    CharacterPresets &PresetManager::ensure_character(const std::string &name)
    {
        // A nameless entry is possible: the save-load wipe path clears the controlled character before the next
        // one is detected, and a write can land in that window. It cannot be selected, applied or loaded, so
        // load(), save() and character_names() all drop it rather than let it reach the file or the dropdown.
        return m_characters[name]; // Default-constructs if absent.
    }

    const std::string &current_apply_owner() noexcept
    {
        auto &pm = PresetManager::instance();
        return pm.editing_pinned() ? pm.editing_character() : pm.active_character();
    }

} // namespace Transmog
