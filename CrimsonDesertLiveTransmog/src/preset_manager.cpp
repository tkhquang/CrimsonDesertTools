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

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
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
        using namespace Transmog::DyeColorTable;
        ch.group_hash = 0; // always re-derive from the name

        if (ch.group_name.empty())
            return;

        if (const auto *g = find_group_by_name(ch.group_name.c_str()))
        {
            ch.group_hash = g->key;
            return;
        }

        DMK::Logger::get_instance().warning("[preset] dye group '{}' not found in current game's "
                                            "color table; dye mod dropped",
                                            ch.group_name);
    }

    // --- JSON serialization ---

    static json slot_to_json(const PresetSlot &s)
    {
        json j{
            {"active", s.active},
            {"itemName", s.itemName},
        };
        // Only emit prefabName when set, to keep the JSON tidy for the common (no body-mesh override) case.
        if (!s.prefabName.empty())
            j["prefabName"] = s.prefabName;
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
                // group_name (the data file's _stringKey) is the sole stable identifier we persist. group_hash is
                // resolved from it at load and never written back.
                json m{
                    {"idx", idx}, {"group_name", ch.group_name}, {"r", ch.r}, {"g", ch.g}, {"b", ch.b},
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
            if (s.dyeSparse)
                j["dye_sparse"] = true;
        }
        return j;
    }

    static PresetSlot slot_from_json(const json &j)
    {
        PresetSlot s;
        s.active = j.value("active", false);
        s.itemName = j.value("itemName", std::string());
        s.prefabName = j.value("prefabName", std::string());

        if (j.contains("dye_mods") && j["dye_mods"].is_array())
        {
            for (const auto &m : j["dye_mods"])
            {
                if (!m.is_object())
                    continue;
                const auto idx = m.value("idx", std::size_t{k_dyeChannelCount});
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
            s.dyeSparse = j.value("dye_sparse", false);
        }

        if (s.itemName.empty())
            return s;

        // itemName is the sole persistent identifier. If the catalog is already built we resolve immediately; otherwise
        // itemId stays 0 until reresolve_all_names() runs after the deferred scan completes.
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return s;

        auto resolved = table.id_of(s.itemName);
        if (resolved.has_value())
        {
            s.itemId = *resolved;
        }
        else
        {
            DMK::Logger::get_instance().warning("[preset] item name '{}' not in current catalog -- "
                                                "slot disabled; re-pick in the overlay",
                                                s.itemName);
            s.active = false;
        }
        return s;
    }

    // ---- ColorOverride swatch persistence helpers ------------------
    //
    // Identity: `(submesh_name, token_name)` -- stable across sessions AND patches. Independent of `dye_mods`
    // (DyeRecordInject's ARMOR_MOD path).
    //
    // Two parallel JSON sections, each `{slot: {submesh: {token: [r,g,b]}}}`:
    //
    //   "swatch_defaults": engine-captured baselines (the asset's
    //                      natural colour per row). Refreshed on every
    //                      save and during Re-init Finalize. The user
    //                      can revert to these without re-running
    //                      Re-init.
    //
    //   "swatch_overrides": rows the user explicitly picked a colour
    //                       for. ONE entry per user pick -- this
    //                       section stays small even when the slot
    //                       has 50+ rows in defaults.
    //
    // Per-row tick state on load = "is there a swatch_overrides entry matching (slot, submesh, token)?". Keeps
    // overrides small even when the slot has many reference rows in defaults, and only user-picked rows load as ticked.

    static json submeshes_to_json(const std::vector<ColorOverride::SwatchTable::PersistEntry> &entries)
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

    // Parse a swatch_overrides node: nested {submesh: {token: [r,g,b]}}. Each leaf is a user-picked colour. Arrays
    // shorter than 3 or non-arrays are silently ignored. Longer arrays are tolerated for forward compatibility (the
    // first 3 elements are taken as r/g/b).
    static void append_overrides_from_json(std::vector<ColorOverride::SwatchTable::PersistEntry> &dst, const json &node)
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
                const std::string &tokenName = tit.key();
                const auto &rgb = tit.value();
                if (!rgb.is_array() || rgb.size() < 3)
                    continue;
                ColorOverride::SwatchTable::PersistEntry e{};
                e.submesh_name = submesh;
                e.token_name = tokenName;
                e.r = rgb[0].get<std::uint8_t>();
                e.g = rgb[1].get<std::uint8_t>();
                e.b = rgb[2].get<std::uint8_t>();
                dst.push_back(std::move(e));
            }
        }
    }

    // Serialize the captured-row palette. Each submesh maps to a flat array of token-name strings (no RGB values). Lets
    // the loader re-create placeholder rows for every (submesh, token) the user has seen, without bloating the JSON
    // with default colour duplicates.
    static json palette_to_json(const std::vector<ColorOverride::SwatchTable::PersistEntry> &entries)
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
    static void append_palette_from_json(std::vector<ColorOverride::SwatchTable::PersistEntry> &dst, const json &node)
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
                ColorOverride::SwatchTable::PersistEntry e{};
                e.submesh_name = submesh;
                e.token_name = tok.get<std::string>();
                dst.push_back(std::move(e));
            }
        }
    }

    // Forward declared so preset_from_json (above the definition) can call this helper for both object-form and
    // legacy-array load paths.
    static void preset_swatches_from_json(Preset &p, const json &j);

    // Keyed-object slot serialization. Old format was a positional array (slots[0]=Helm, [1]=Chest, ...) which broke
    // whenever the TransmogSlot enum gained a row -- legacy presets shifted by one. Object form keys each slot by its
    // slot_metadata display name ("Helm", "Chest", ...) so the JSON survives enum additions/removals as long as the
    // names stay stable.
    //
    // Migration is automatic: preset_from_json accepts BOTH the legacy array form AND the new object form. The next
    // save rewrites every loaded file in object form.
    //
    // Disabled slots and default-empty rows are omitted from the object to keep the JSON tidy. A completely-empty
    // preset serializes as `{"slots": {}}`.
    static json preset_to_json(const Preset &p)
    {
        json slotsObj = json::object();
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto &s = p.slots[i];
            if (!Transmog::slot_enabled(i))
                continue;
            const bool defaultRow = !s.active && s.itemName.empty() && s.prefabName.empty() && !any_dye_active(s.dye);
            if (defaultRow)
                continue;
            slotsObj[Transmog::slot_meta(static_cast<TransmogSlot>(i)).displayName] = slot_to_json(s);
        }

        json out{{"name", p.name}, {"slots", slotsObj}};

        // Two parallel sections for ColorOverride state:
        //   swatch_overrides: ONLY user-picked rows, nested
        //     {slot: {submesh: {token: [r,g,b]}}}. One entry per
        //     pick -- stays small even when the slot has 50+ rows.
        //   swatch_palette:   the captured-row structure (no
        //     values), nested {slot: {submesh: [token, token, ...]}}.
        //     Lets the loader recreate every row the user has seen,
        //     so untickled rows reappear in the picker on switch back.
        json overridesObj = json::object();
        json paletteObj = json::object();
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            const auto &slotName = Transmog::slot_meta(static_cast<TransmogSlot>(i)).displayName;
            const auto &ovEntries = p.swatch_overrides[i];
            if (!ovEntries.empty())
            {
                json submeshes = submeshes_to_json(ovEntries);
                if (!submeshes.empty())
                    overridesObj[slotName] = std::move(submeshes);
            }
            const auto &paEntries = p.swatch_palette[i];
            if (!paEntries.empty())
            {
                json palette = palette_to_json(paEntries);
                if (!palette.empty())
                    paletteObj[slotName] = std::move(palette);
            }
        }
        if (!overridesObj.empty())
            out["swatch_overrides"] = std::move(overridesObj);
        if (!paletteObj.empty())
            out["swatch_palette"] = std::move(paletteObj);

        // Per-slot master-enable flags. Only emit when at least one slot is true, to keep the JSON tidy for unused
        // presets.
        bool anyEnabled = false;
        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (p.swatch_slot_enabled[i])
            {
                anyEnabled = true;
                break;
            }
        if (anyEnabled)
        {
            json enabledObj = json::object();
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                if (!p.swatch_slot_enabled[i])
                    continue;
                enabledObj[Transmog::slot_meta(static_cast<TransmogSlot>(i)).displayName] = true;
            }
            out["swatch_slot_enabled"] = std::move(enabledObj);
        }

        return out;
    }

    static Preset preset_from_json(const json &j)
    {
        Preset p;
        p.name = j.value("name", std::string("Unnamed"));

        if (!j.contains("slots"))
            return p;

        const auto &slotsJ = j["slots"];

        // New format: object keyed by slot displayName.
        if (slotsJ.is_object())
        {
            for (auto it = slotsJ.begin(); it != slotsJ.end(); ++it)
            {
                bool matched = false;
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    const auto &m = k_slotMetadata[i];
                    if (it.key() == m.displayName)
                    {
                        p.slots[i] = slot_from_json(it.value());
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    DMK::Logger::get_instance().warning("[preset] '{}' has unknown slot key '{}' -- "
                                                        "ignored (slot may have been renamed or "
                                                        "removed)",
                                                        p.name, it.key());
                }
            }
            preset_swatches_from_json(p, j);
            return p; // skip legacy-array fallthrough
        }

        // Legacy format: positional array. Indices map directly to TransmogSlot enum order. Auto-migrated to object
        // form on next save.
        if (slotsJ.is_array())
        {
            DMK::Logger::get_instance().info("[preset] '{}' loaded from legacy array format -- "
                                             "next save will rewrite as keyed object",
                                             p.name);
            for (std::size_t i = 0; i < k_slotCount && i < slotsJ.size(); ++i)
                p.slots[i] = slot_from_json(slotsJ[i]);
        }
        preset_swatches_from_json(p, j);
        return p;
    }

    // Apply the swatch_overrides + swatch_slot_enabled JSON blocks (top-level on a preset, alongside `slots`). Tolerant
    // of both object form (keyed by slot displayName) and array form (positional by TransmogSlot index). Missing
    // entirely = no overrides for this preset.
    static void preset_swatches_from_json(Preset &p, const json &j)
    {
        if (j.contains("swatch_overrides") && j["swatch_overrides"].is_object())
        {
            const auto &so = j["swatch_overrides"];
            for (auto it = so.begin(); it != so.end(); ++it)
            {
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (it.key() == k_slotMetadata[i].displayName)
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
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (it.key() == k_slotMetadata[i].displayName)
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
                    for (std::size_t i = 0; i < k_slotCount; ++i)
                    {
                        if (it.key() == k_slotMetadata[i].displayName)
                        {
                            p.swatch_slot_enabled[i] = it.value().is_boolean() && it.value().get<bool>();
                            break;
                        }
                    }
                }
            }
            else if (se.is_array())
            {
                for (std::size_t i = 0; i < k_slotCount && i < se.size(); ++i)
                    p.swatch_slot_enabled[i] = se[i].is_boolean() && se[i].get<bool>();
            }
        }
    }

    static json character_to_json(const CharacterPresets &cp)
    {
        json presetsArr = json::array();
        for (auto &p : cp.presets)
            presetsArr.push_back(preset_to_json(p));

        json out{
            {"activePreset", cp.activePreset},
            {"presets", presetsArr},
            {"bodyKind", cp.bodyKind},
        };
        // ColorOverride session toggle. Persisted only when true to keep the JSON tidy for fresh users.
        if (cp.dyeAdvancedView)
            out["dyeAdvancedView"] = true;
        return out;
    }

    static CharacterPresets character_from_json(const json &j)
    {
        CharacterPresets cp;
        cp.activePreset = j.value("activePreset", 0);
        cp.bodyKind = j.value("bodyKind", std::string("Auto"));
        cp.dyeAdvancedView = j.value("dyeAdvancedView", false);

        if (j.contains("presets") && j["presets"].is_array())
        {
            for (auto &pj : j["presets"])
                cp.presets.push_back(preset_from_json(pj));
        }

        // Clamp active index.
        if (!cp.presets.empty())
            cp.activePreset = std::clamp(cp.activePreset, 0, static_cast<int>(cp.presets.size()) - 1);
        else
            cp.activePreset = 0;

        return cp;
    }

    // ---- Per-preset ColorOverride snapshot/restore helpers ---------
    //
    // Used by every preset-switch / character-switch / load path to sync `Preset::swatch_overrides` +
    // `Preset::swatch_slot_enabled` with the live SwatchTable state. The pattern is:
    //
    //   snapshot_live_swatches_into(prev_preset)  // capture current
    //   ColorOverride::reset_all()                // wipe live tables
    //   activePreset = new                       // switch
    //   restore_swatches_from(new_preset)         // re-seed live tables
    //   apply_to_state()                         // push slot mappings
    //   save()                                   // persist (this also
    //                                            //   re-snapshots into
    //                                            //   the new active)
    //
    // INDEPENDENT of dye_mods / DyeRecordInject -- that path lives in PresetSlot::dye and is untouched here. `force` is
    // true on the explicit-save path (replace_current_from_state / Save button) where we always want to capture live
    // state into the active preset. Default false: callers in the preset-switch path skip the capture when dye_dirty()
    // is true, which discards pending edits on switch -- the user must click Save to commit.
    static void snapshot_live_swatches_into(Preset &p, bool force = false)
    {
        if (!force && dye_dirty().load(std::memory_order_acquire))
        {
            // Pending edits get discarded on switch. Leave `p.swatch_overrides` at its loaded baseline so a later
            // switch BACK to this preset restores the on-disk state, not the unsaved edits.
            return;
        }
        // When the ColorOverride feature is disabled the live SwatchTable is inert and its persist helpers return empty
        // vectors. Writing those over the preset's loaded baseline would silently destroy any swatch_overrides /
        // swatch_palette / swatch_slot_enabled entries the JSON already carries. Leave the preset's swatch fields
        // untouched in that case so the saved data round-trips intact through "disabled" sessions.
        if (!flag_color_override().load(std::memory_order_acquire))
            return;
        namespace ST = ColorOverride::SwatchTable;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            auto liveOv = ST::get_persistable_overrides(static_cast<int>(s));
            auto livePal = ST::get_persistable_palette(static_cast<int>(s));
            // Live SwatchTable can legitimately be empty for a slot even when the user has saved overrides for it:
            //   - On cold game-load, populate_from_persisted drops entries whose token names haven't been interned by
            //     the engine yet (token_id_for_name returns 0). No placeholder row gets seeded, so the live table stays
            //     empty for that slot until either the setter intercepts an engine write or a later retry pass resolves
            //     the token.
            //   - The slot may never have been equipped this session.
            //   - The slot's apply pipeline may have skipped because dispatcher state filtered it out.
            // Writing the empty live vector over the preset's baseline in any of those cases silently destroys the
            // user's saved JSON. Preserve the baseline; the live table will repopulate naturally as tokens resolve, and
            // the next genuine edit will mark dye_dirty so a real capture proceeds via the force=true save path.
            const bool liveEmpty = liveOv.empty() && livePal.empty();
            const bool savedExists = !p.swatch_overrides[s].empty() || !p.swatch_palette[s].empty();
            // User-intentional empty case: Reset Slot wipes the live table and flags the slot. Without this exception
            // the guard would treat the wipe identically to a token-race and revert to the JSON baseline -- making
            // Reset Slot impossible to commit via Save.
            const bool wiped = ST::slot_was_explicitly_wiped(static_cast<int>(s));
            if (liveEmpty && savedExists && !wiped)
                continue;
            p.swatch_overrides[s] = std::move(liveOv);
            p.swatch_palette[s] = std::move(livePal);
            p.swatch_slot_enabled[s] = ST::slot_enabled_get(static_cast<int>(s));
            // Empty state is now committed in the preset. Drop the wipe flag so subsequent saves follow the normal
            // guard (savedExists becomes false, the empty live write is a no-op).
            if (wiped)
                ST::clear_explicit_wipe_flag(static_cast<int>(s));
        }
    }

    static void restore_swatches_from(const Preset &p)
    {
        namespace ST = ColorOverride::SwatchTable;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            // Queue ONLY the user-override RGB into the PendingOverrides map. The palette doesn't need queueing --
            // those rows get seeded directly in auto_reinit_from via populate_from_persisted.
            if (!p.swatch_overrides[s].empty())
                ST::restore_persisted_state(static_cast<int>(s), p.swatch_overrides[s]);
            ST::slot_enabled_set(static_cast<int>(s), p.swatch_slot_enabled[s]);
        }
    }

    // Auto-init slots that have saved overrides:
    // populate_from_persisted seeds placeholders from JSON. The preset-switch path already tears down + reapplies every
    // slot, so the engine naturally re-emits setter writes -- our placeholders get promoted via lookup_or_insert's
    // promotion pass on those writes. No need to force an extra retick here.
    //
    // Slots WITHOUT saved data are NOT auto-init'd here -- the user clicks Re-init manually when they want to discover
    // a new outfit's swatches.
    static void auto_reinit_from(const Preset &p)
    {
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            if (p.swatch_palette[s].empty() && p.swatch_overrides[s].empty())
                continue;
            ColorOverride::SwatchTable::populate_from_persisted(static_cast<int>(s), p.swatch_palette[s],
                                                                p.swatch_overrides[s]);
        }
    }

    // --- PresetManager ---

    PresetManager &PresetManager::instance()
    {
        static PresetManager s_instance;
        return s_instance;
    }

    bool PresetManager::load(const std::string &path)
    {
        auto &logger = DMK::Logger::get_instance();
        m_filePath = path;

        std::ifstream file(path);
        if (!file.is_open())
        {
            logger.info("Presets file not found -- starting with defaults");

            // Create default entries for known characters.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            return true;
        }

        try
        {
            json root = json::parse(file);

            // `activeCharacter` was a legacy JSON field; the controlled character is now driven entirely by the live WS
            // chain via the char-swap detector and the editing character is session-only. Old presets.json files may
            // still carry the field, so it is read and discarded here to avoid a "missing key" parse error.
            (void)root.value("activeCharacter", std::string{});

            if (root.contains("characters") && root["characters"].is_object())
            {
                for (auto &[name, cj] : root["characters"].items())
                    m_characters[name] = character_from_json(cj);
            }

            // Ensure known characters exist even if not in the file.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            logger.info("Presets loaded: {} character(s) from '{}'", m_characters.size(), path);

            if (!ItemNameTable::instance().ready())
            {
                logger.info("[preset] item catalog not ready at load time -- name "
                            "resolution deferred until background scan completes");
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

        // Just-loaded state matches disk; clear any leftover dirty signal from earlier sessions of the process and
        // snapshot the active preset's dye as our reference baseline.
        dye_dirty().store(false, std::memory_order_release);
        capture_dye_snapshot();

        // Push the active preset's persisted ColorOverride swatch state into the live SwatchTable. Independent of
        // dye_mods (which is part of slot_mappings, not SwatchTable). Safe to run before ColorOverride::init() --
        // restore mutates storage arrays directly and does not require hooks to be live.
        {
            auto it = m_characters.find(m_editingCharacter);
            if (it != m_characters.end() && !it->second.presets.empty())
            {
                const auto idx =
                    std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
                const auto &preset = it->second.presets[static_cast<std::size_t>(idx)];
                restore_swatches_from(preset);
                // Direct-seed picker rows from saved data so user sees their colors immediately on game load -- no
                // manual
                // Re-init needed. Placeholders are promoted to live identity on first engine write.
                auto_reinit_from(preset);
            }
        }

        return true;
    }

    bool PresetManager::save() const
    {
        return save(m_filePath);
    }

    bool PresetManager::save(const std::string &path) const
    {
        auto &logger = DMK::Logger::get_instance();

        json root;
        // Schema v3: slots carry only itemName (stable identifier). itemId is resolved at runtime from the item
        // catalog.
        root["version"] = 3;
        // Neither the controlled character nor the editing character are serialised. Controlled is driven by the live
        // WS chain at runtime; editing is a session-only UI affordance that resets to controlled on load.

        // Capture the live SwatchTable into the active preset so the user's current picks land in JSON. Independent of
        // dye_mods (which lives in PresetSlot::dye and is captured/edited via the picker UI directly).
        //
        // const_cast on the active preset is sound for the same reason `mutable m_dyeSnapshot` is: this is
        // logical-const cache state -- the preset's `swatch_overrides` field is a write-through cache of the live
        // SwatchTable, synced at save / switch / character-swap. Save remains externally const (the on-disk file is the
        // source of truth for what the user committed; this just refreshes the in-memory copy to match what's about to
        // be written).
        {
            auto it = m_characters.find(m_editingCharacter);
            if (it != m_characters.end() && !it->second.presets.empty())
            {
                const auto idx =
                    std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
                auto &mutPreset = const_cast<Preset &>(it->second.presets[static_cast<std::size_t>(idx)]);
                // force=true: save is the explicit commit path, so capture pending edits even when dye_dirty is set.
                snapshot_live_swatches_into(mutPreset, /*force=*/true);
            }
        }

        json chars = json::object();
        for (auto &[name, cp] : m_characters)
            chars[name] = character_to_json(cp);

        root["characters"] = chars;

        std::ofstream file(path);
        if (!file.is_open())
        {
            logger.warning("Failed to write presets to '{}'", path);
            return false;
        }

        file << root.dump(2);
        logger.info("Presets saved to '{}'", path);
        // Diagnostic: dump the live swatch tree so the log captures exactly what was persisted alongside the JSON write
        // event.
        ColorOverride::SwatchTable::dump_all_slots();
        dye_dirty().store(false, std::memory_order_release);
        // The just-saved state IS the new baseline -- subsequent edits become "dirty" relative to this point.
        capture_dye_snapshot();
        return true;
    }

    // --- Character management ---

    std::vector<std::string> PresetManager::character_names() const
    {
        std::vector<std::string> names;
        names.reserve(m_characters.size());
        for (auto &[name, _] : m_characters)
            names.push_back(name);
        return names;
    }

    const std::string &PresetManager::active_character() const
    {
        return m_controlledCharacter;
    }

    void PresetManager::rotate_editing_target_to(const std::string &new_name)
    {
        // Snapshot OUTGOING editing character's active preset live swatches before flipping -- otherwise
        // per-shader-property picks on the old character would be lost when switching back. dye_mods (PresetSlot::dye)
        // is independent and managed by the dye snapshot path below.
        auto prev = m_characters.find(m_editingCharacter);
        if (prev != m_characters.end() && !prev->second.presets.empty())
        {
            const auto idx =
                std::clamp(prev->second.activePreset, 0, static_cast<int>(prev->second.presets.size()) - 1);
            snapshot_live_swatches_into(prev->second.presets[static_cast<std::size_t>(idx)]);
        }
        ColorOverride::reset_all();

        revert_active_dye_to_snapshot();
        m_editingCharacter = new_name;
        ensure_character(new_name);
        capture_dye_snapshot();

        // Re-seed live SwatchTable from the NEW editing character's active preset; auto-Reinit slots with saved
        // overrides so the locked tables re-populate.
        auto it = m_characters.find(m_editingCharacter);
        if (it != m_characters.end() && !it->second.presets.empty())
        {
            const auto idx = std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
            const auto &preset = it->second.presets[static_cast<std::size_t>(idx)];
            restore_swatches_from(preset);
            auto_reinit_from(preset);
        }
    }

    void PresetManager::set_active_character(const std::string &name)
    {
        DMK::Logger::get_instance().info("[preset] set_active_character('{}') prev='{}' "
                                         "(changed={})",
                                         name, m_controlledCharacter, m_controlledCharacter != name);
        m_controlledCharacter = name;
        ensure_character(name);
        if (!m_editingPinned)
        {
            // Editing follows controlled whenever the user has not explicitly pinned a different editing target. Rotate
            // both the dye snapshot AND ColorOverride swatch state on the outgoing editing character so unsaved edits
            // do not bleed into the incoming character's preset.
            if (name != m_editingCharacter)
            {
                rotate_editing_target_to(name);
            }
        }
        else if (name == m_editingCharacter)
        {
            // The player is now controlling the very character the user had pinned for editing. The pin no longer
            // represents anything distinct, so it auto-clears.
            m_editingPinned = false;
        }
    }

    const std::string &PresetManager::editing_character() const
    {
        return m_editingCharacter;
    }

    void PresetManager::set_editing_character(const std::string &name)
    {
        if (name == m_editingCharacter)
        {
            // No-op switch: keep the pin state consistent against controlled and return without touching the snapshot.
            m_editingPinned = (name != m_controlledCharacter);
            ensure_character(name);
            return;
        }

        // Switching the editing character is a larger context shift than cycling presets: rotate the dye snapshot AND
        // the ColorOverride swatch state onto the incoming preset. Mirrors set_active_preset().
        rotate_editing_target_to(name);
        // Pin engages when the user picked anyone other than the controlled character; picking the controlled character
        // is the unpin gesture from the dropdown.
        m_editingPinned = (name != m_controlledCharacter);
    }

    bool PresetManager::editing_pinned() const noexcept
    {
        return m_editingPinned;
    }

    void PresetManager::clear_editing_pin()
    {
        if (m_editingCharacter != m_controlledCharacter)
        {
            // Rotate the dye snapshot AND the ColorOverride swatch state onto the controlled character's active preset.
            rotate_editing_target_to(m_controlledCharacter);
        }
        m_editingPinned = false;
    }

    std::string PresetManager::body_kind_of(const std::string &charName) const
    {
        auto it = m_characters.find(charName);
        if (it == m_characters.end())
            return "Auto";
        return it->second.bodyKind.empty() ? std::string("Auto") : it->second.bodyKind;
    }

    void PresetManager::set_body_kind_of(const std::string &charName, const std::string &bodyKind)
    {
        auto &cp = ensure_character(charName);
        // Normalise: accept only known values; anything else collapses to "Auto" so the JSON stays clean and the picker
        // filter has a well-defined fallback.
        if (bodyKind == "Male" || bodyKind == "Female" || bodyKind == "Both" || bodyKind == "Auto")
        {
            cp.bodyKind = bodyKind;
        }
        else
        {
            cp.bodyKind = "Auto";
        }
        save();
    }

    // --- Preset management ---

    int PresetManager::active_preset_index() const
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end())
            return 0;
        return it->second.activePreset;
    }

    int PresetManager::preset_count() const
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end())
            return 0;
        return static_cast<int>(it->second.presets.size());
    }

    const Preset *PresetManager::active_preset() const
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const Preset *PresetManager::active_preset_of(const std::string &charName) const
    {
        // Read-only sibling of active_preset() that targets an arbitrary character. Used by the body-mesh prefab picker
        // to borrow the Kairos (default-carrier) preset's itemIds without disturbing the controlled or editing axes --
        // any mutation of either field would cascade into a save and could race with the load-detect commit branch.
        auto it = m_characters.find(charName);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    Preset *PresetManager::active_preset_mut()
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0, static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const std::vector<Preset> &PresetManager::presets() const
    {
        static const std::vector<Preset> s_empty;
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end())
            return s_empty;
        return it->second.presets;
    }

    void PresetManager::append_from_state()
    {
        auto &cp = ensure_character(m_editingCharacter);
        // Snapshot the OUTGOING preset's swatches so they survive the active-index flip; then wipe live state because
        // the new blank preset has none.
        if (!cp.presets.empty())
        {
            const auto prevIdx = std::clamp(cp.activePreset, 0, static_cast<int>(cp.presets.size()) - 1);
            snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(prevIdx)]);
        }
        ColorOverride::reset_all();

        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        Preset blank;
        blank.name = name;
        // Default ticks: only the five armor slots (Helm, Chest, Cloak, Gloves, Boots). Gear/accessory slots (Lantern,
        // weapons, rings, etc.) stay unticked so a freshly-appended preset doesn't unintentionally hide working in-game
        // items (e.g. a lantern that should still light up).
        for (std::size_t i = 0; i < blank.slots.size(); ++i)
        {
            const auto slot = static_cast<TransmogSlot>(i);
            blank.slots[i].active =
                (slot == TransmogSlot::Helm || slot == TransmogSlot::Chest || slot == TransmogSlot::Cloak ||
                 slot == TransmogSlot::Gloves || slot == TransmogSlot::Boots);
            blank.slots[i].itemId = 0;
            blank.slots[i].itemName.clear();
        }
        cp.presets.push_back(std::move(blank));
        cp.activePreset = idx;

        apply_to_state();

        DMK::Logger::get_instance().info("Preset appended: '{}' (index {}, armor slots ticked + none)", name, idx);
        save();
    }

    void PresetManager::duplicate_current()
    {
        auto &cp = ensure_character(m_editingCharacter);
        const int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);

        Preset clone;
        clone.name = name;
        // Pure clone of the source preset's in-memory state. Pending edits in slot_mappings are intentionally ignored
        // so the new preset reflects what the source would look like reloaded from disk. For forking pending edits
        // instead, use save_as_new_from_state.
        if (const auto *src = active_preset())
        {
            clone.slots = src->slots;
            clone.swatch_overrides = src->swatch_overrides;
            clone.swatch_palette = src->swatch_palette;
            clone.swatch_slot_enabled = src->swatch_slot_enabled;
        }
        cp.presets.push_back(std::move(clone));
        cp.activePreset = idx;

        apply_to_state();

        DMK::Logger::get_instance().info("Preset duplicated (clean clone): '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::save_as_new_from_state()
    {
        auto &cp = ensure_character(m_editingCharacter);
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
                // PresetSlot from slot_mappings, which has no notion of dyeSparse, so the captured slot is left at the
                // PresetSlot default. That default does not necessarily match the source preset's mode (a captured
                // preset uses sparse, a picker-built preset may not), so we copy the source value explicitly to keep
                // the fork visually identical to its origin.
                captured.slots[i].dyeSparse = src->slots[i].dyeSparse;
            }
            captured.swatch_overrides = src->swatch_overrides;
            captured.swatch_palette = src->swatch_palette;
            captured.swatch_slot_enabled = src->swatch_slot_enabled;
        }
        cp.presets.push_back(std::move(captured));
        cp.activePreset = idx;

        apply_to_state();

        DMK::Logger::get_instance().info("Preset saved as new (pending state): '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::replace_current_from_state()
    {
        auto *p = active_preset_mut();
        if (!p)
        {
            auto &cp = ensure_character(m_editingCharacter);
            int idx = static_cast<int>(cp.presets.size());
            std::string name = "Preset " + std::to_string(idx);
            cp.presets.push_back(capture_from_state(name));
            cp.activePreset = idx;

            DMK::Logger::get_instance().info("Preset replaced: '{}' (index {})", name, idx);
            save();
            return;
        }

        auto captured = capture_from_state(p->name);
        // Dye state is not part of slot_mappings; the picker writes it directly onto the active preset's
        // PresetSlot::dye. capture_from_state() rebuilds slots from slot_mappings, so it would clobber both the dye
        // edits AND the dyeSparse flag. Carry both forward from the existing preset so the
        // capture_outfit -> save / "Replace" button path preserves
        // user-curated dye and its sparse-vs-dense origin (a fresh capture_outfit sets dyeSparse=true; the dye picker
        // leaves it whatever it already was).
        for (std::size_t i = 0; i < p->slots.size() && i < captured.slots.size(); ++i)
        {
            captured.slots[i].dye = p->slots[i].dye;
            captured.slots[i].dyeSparse = p->slots[i].dyeSparse;
        }
        p->slots = captured.slots;
        // ColorOverride swatch overrides are NOT part of slot_mappings -- snapshot the live SwatchTable so the
        // "Replace" button persists the user's current per-shader-property picks too. `force=true` overrides the
        // dye_dirty gate in the snapshot helper: the user explicitly asked to save, so capture pending edits even when
        // the dirty flag is set.
        snapshot_live_swatches_into(*p, /*force=*/true);

        DMK::Logger::get_instance().info("Preset replaced: '{}'", p->name);
        save();
    }

    void PresetManager::remove_current()
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        auto idx = std::clamp(cp.activePreset, 0, static_cast<int>(cp.presets.size()) - 1);

        DMK::Logger::get_instance().info("Preset removed: '{}'", cp.presets[static_cast<std::size_t>(idx)].name);

        cp.presets.erase(cp.presets.begin() + idx);

        if (cp.presets.empty())
            cp.activePreset = 0;
        else
            cp.activePreset = std::min(idx, static_cast<int>(cp.presets.size()) - 1);

        // The deleted preset took its swatch state with it. Wipe the live SwatchTable so the next apply rebuilds
        // cleanly, then restore from the new active preset (if any). Independent of dye_mods.
        ColorOverride::reset_all();
        if (!cp.presets.empty())
        {
            const auto &preset = cp.presets[static_cast<std::size_t>(cp.activePreset)];
            restore_swatches_from(preset);
            auto_reinit_from(preset);
        }

        save();
    }

    void PresetManager::next_preset()
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Snapshot OUTGOING swatch state FIRST so the dye_dirty gate inside snapshot_live_swatches_into still sees the
        // user's pending picks (and skips capture). The revert helper below clears dye_dirty; running it before
        // snapshot would inadvertently auto-save unsaved swatch edits.
        auto &cp = it->second;
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(
            std::clamp(cp.activePreset, 0, static_cast<int>(cp.presets.size()) - 1))]);

        // Discard unsaved dye-mod edits before cycling.
        revert_active_dye_to_snapshot();
        ColorOverride::reset_all();

        cp.activePreset = (cp.activePreset + 1) % static_cast<int>(cp.presets.size());
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);

        DMK::Logger::get_instance().info("Preset cycled to: '{}' ({}/{})",
                                         cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
                                         cp.activePreset + 1, cp.presets.size());

        // See set_active_preset for the rationale -- preset switch must force re-apply so dye state is rebuilt.
        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        // Strict-init: trigger Reinit on every slot with saved overrides so the locked tables re-populate from this
        // preset's outfit.
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);
        save();
    }

    void PresetManager::prev_preset()
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Snapshot first so dye_dirty still gates the swatch capture (see set_active_preset for the rationale -- revert
        // clears dye_dirty and would let unsaved swatch picks auto-save).
        auto &cp = it->second;
        int count = static_cast<int>(cp.presets.size());
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(std::clamp(cp.activePreset, 0, count - 1))]);

        revert_active_dye_to_snapshot();
        ColorOverride::reset_all();

        cp.activePreset = (cp.activePreset - 1 + count) % count;
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);

        DMK::Logger::get_instance().info("Preset cycled to: '{}' ({}/{})",
                                         cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
                                         cp.activePreset + 1, cp.presets.size());

        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);
        save();
    }

    void PresetManager::capture_dye_snapshot() const noexcept
    {
        const auto *p = active_preset();
        if (!p)
        {
            m_dyeSnapshotValid = false;
            return;
        }
        m_dyeSnapshot = {};
        for (std::size_t i = 0; i < p->slots.size() && i < m_dyeSnapshot.size(); ++i)
        {
            m_dyeSnapshot[i] = p->slots[i].dye;
        }
        m_dyeSnapshotValid = true;
    }

    void PresetManager::revert_active_dye_to_snapshot() noexcept
    {
        if (!m_dyeSnapshotValid)
            return;
        if (!dye_dirty().load(std::memory_order_acquire))
            return;
        auto *p = active_preset_mut();
        if (!p)
            return;
        for (std::size_t i = 0; i < p->slots.size() && i < m_dyeSnapshot.size(); ++i)
        {
            p->slots[i].dye = m_dyeSnapshot[i];
        }
        dye_dirty().store(false, std::memory_order_release);
        DMK::Logger::get_instance().info("[preset] reverted unsaved dye edits on '{}'", p->name);
    }

    void PresetManager::set_active_preset(int index)
    {
        auto it = m_characters.find(m_editingCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        DMK::Logger::get_instance().info("[preset] set_active_preset(index={}) char='{}' "
                                         "(prev_active={})",
                                         index, m_editingCharacter, it->second.activePreset);

        auto &cp = it->second;
        // Snapshot OUTGOING ColorOverride swatches BEFORE the dye-mod revert. Order matters: the snapshot helper checks
        // dye_dirty() and skips when dirty (so unsaved swatch picks get discarded on switch);
        // revert_active_dye_to_snapshot CLEARS dye_dirty after reverting dye-mod state, so if we ran it first the
        // snapshot would proceed and capture the user's unsaved swatch RGB into the outgoing preset -- making swatch
        // edits effectively auto-save on every preset switch.
        snapshot_live_swatches_into(cp.presets[static_cast<std::size_t>(
            std::clamp(cp.activePreset, 0, static_cast<int>(cp.presets.size()) - 1))]);

        // Now drop any unsaved dye-mod edits on the OUTGOING preset -- the new preset's dye state will be captured
        // fresh below.
        revert_active_dye_to_snapshot();
        ColorOverride::reset_all();

        cp.activePreset = std::clamp(index, 0, static_cast<int>(cp.presets.size()) - 1);
        // Capture the NEW active preset's dye as the baseline; any future edits become "dirty" relative to this
        // baseline.
        capture_dye_snapshot();
        restore_swatches_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);
        // Switching to a different preset means we're now showing the on-disk state for that preset; nothing to save
        // until the user edits.
        dye_dirty().store(false, std::memory_order_release);
        // The dispatcher's slotNeedsWork check is item-id based and doesn't notice dye state changes. When two presets
        // share the same carrier ids (e.g. Kairos preset + a body-mesh prefab preset both built on Kairos carrier
        // 0x1521), the dispatcher would skip the apply and leave stale dye records bleeding through. Force re-apply for
        // every enabled slot so DyeCopier gets a fresh injection from the new preset's dye state (including the case
        // where the new preset has no dye -- our hook then skips injection and the engine's natural records win). Cost:
        // full slot rebuild on every preset switch, but that's already the user's intent on a switch.
        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        auto_reinit_from(cp.presets[static_cast<std::size_t>(cp.activePreset)]);
    }

    void PresetManager::reseed_unresolved_persisted_swatches() const
    {
        if (!flag_color_override().load(std::memory_order_acquire))
            return;
        // While dye_dirty is set the user has uncommitted edits in flight: a per-slot Reset just cleared the live
        // table, a checkbox toggle dropped a row, a colour pick moved RGB, etc. Re-seeding from the JSON baseline here
        // would fight those edits and resurrect rows the user explicitly wiped. The next Save snapshots whatever live
        // state the user committed to, so the retry only ever needs to run on a clean, just-loaded preset (dye_dirty ==
        // false). Cold-load token-resolution races still get repaired because load clears dirty before the per-frame
        // retry begins.
        if (dye_dirty().load(std::memory_order_acquire))
            return;
        const auto *p = active_preset();
        if (!p)
            return;
        namespace ST = ColorOverride::SwatchTable;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            const auto &pal = p->swatch_palette[s];
            const auto &ovr = p->swatch_overrides[s];
            if (pal.empty() && ovr.empty())
                continue;
            // Live row count is the cheap "did anything seed yet" signal. Re-firing populate_from_persisted on a slot
            // that already has rows is a no-op for resolved entries (find_seeded short-circuits), so the only cost
            // worth avoiding is re-walking saved vectors on slots that are already done.
            if (ST::detected_swatch_count(static_cast<int>(s)) != 0)
                continue;
            ST::populate_from_persisted(static_cast<int>(s), pal, ovr);
        }
    }

    // --- State bridge ---

    void PresetManager::apply_to_state() const
    {
        namespace PWS = Transmog::PrefabWrapperSwap;

        // Bind the editing character into PWS before the active_preset() early-out below. Only apply_to_state() binds
        // s_activeCharIdx; the picker's body-mesh path (set_selection plus the Instant-Apply manual_apply) never does.
        // For a character with no saved preset (active_preset() == nullptr) the bind therefore has to happen here,
        // ahead of the early return -- otherwise the prefab-swap map stays unbound, apply_selections_to_swap_map()
        // bails at its activeIdx < 1 guard, and the picked body mesh renders as the bare carrier instead of the chosen
        // prefab. The bind is independent of the preset, so set_selection can still mirror picks into the correct
        // per-char row and the swap arms.
        PWS::set_active_char_idx(CDCore::character_idx_from_name(m_editingCharacter));

        const auto *p = active_preset();
        if (!p)
            return;

        auto &mappings = slot_mappings();

        // NOTE: must NOT touch last_applied_ids() here. lastIds tracks what apply_all_transmog has actively injected
        // into the game -- the diff logic depends on lastIds holding the PREVIOUS applied state so it can compute
        // drop-masks on preset switching. Writing to lastIds here would pre-populate it with the NEW preset values
        // before the apply runs, breaking drop detection.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            // Disabled slots (multi-prefab non-armor / duplicate-tag -- see slot_metadata.hpp `enabled` doc) cannot be
            // applied and are hidden from the picker. A legacy preset saved before these slots were disabled may still
            // carry active=true with a non-zero itemId. Force everything off here so downstream code (lastIds clearing,
            // carrier-borrow, PWS sync below) doesn't see a stale ticked state for a slot the user has no way to
            // interact with.
            if (!Transmog::slot_enabled(i))
            {
                mappings[i].active = false;
                mappings[i].targetItemId = 0;
                continue;
            }

            mappings[i].active = p->slots[i].active;
            mappings[i].targetItemId = p->slots[i].itemId;

            // Body-mesh slots only persist `prefabName`; the carrier itemId is derived here so the body actually
            // rendering the slot emits the wrapper PWS's substitution map keys on. With per-character PWS rows now
            // contributing to a single union swap map, the correct carrier owner is the editing character whenever the
            // pin is engaged -- their body (or the controlled body, when cross-body apply is selected) needs to emit
            // the editing character's expected src wrapper so the editing character's row in s_swapMap matches and
            // substitutes to the picked prefab. Using the controlled character's carrier here under a pin would install
            // the controlled character's prefab on the targeted body, where it would either no-op or collide with the
            // controlled character's own row in the union map and render that character's tgt by mistake.
            if (mappings[i].targetItemId == 0 && !p->slots[i].prefabName.empty())
            {
                const auto carrier =
                    Transmog::default_carrier_for_slot(static_cast<TransmogSlot>(i), Transmog::current_apply_owner());
                if (carrier != 0)
                {
                    mappings[i].targetItemId = carrier;
                    mappings[i].active = true;
                }
            }
        }

        // Sync body-mesh prefab selections. For each slot, if the preset stores a prefabName, look it up in the slot's
        // catalog and set the PWS target index. Empty prefabName clears the target (slot reverts to plain carrier
        // rendering). When the catalog isn't yet populated (boot heap walk in progress) resolution silently misses; the
        // boot thread re-runs apply_to_state when populate_slot_catalogs finishes so the selection lands as soon as the
        // data is available.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto tslot = static_cast<TransmogSlot>(i);
            const int curSrc = PWS::selection_src_index(tslot);
            // Force-clear any persisted PWS selection on disabled slots so a legacy preset's body-mesh prefab can't
            // keep a target index live (which the natural-pipeline hook would otherwise still apply against).
            if (!Transmog::slot_enabled(i))
            {
                PWS::set_selection(tslot, curSrc, -1);
                continue;
            }
            const auto &name = p->slots[i].prefabName;
            if (name.empty())
            {
                PWS::set_selection(tslot, curSrc, -1);
                continue;
            }
            const auto &cat = PWS::slot_catalog(tslot);
            int found = -1;
            for (std::size_t k = 0; k < cat.size(); ++k)
            {
                if (cat[k].name == name)
                {
                    found = static_cast<int>(k);
                    break;
                }
            }
            PWS::set_selection(tslot, curSrc, found);
        }
    }

    Preset PresetManager::capture_from_state(const std::string &name)
    {
        Preset p;
        p.name = name.empty() ? "Captured" : name;

        auto &mappings = slot_mappings();
        const auto &table = ItemNameTable::instance();
        namespace PWS = Transmog::PrefabWrapperSwap;

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            // Disabled slots (multi-prefab non-armor / duplicate-tag entries, see slot_metadata.hpp) are not yet
            // supported by the apply path. Don't bloat the JSON with their current in-memory state: leaving p.slots[i]
            // default-constructed serializes the row as `{"active":false,"itemName":""}` so the array indexing stays
            // positional but the entry is empty. Any stale data from an older preset (saved before the slot was
            // disabled) is discarded on the next save.
            if (!Transmog::slot_enabled(i))
                continue;

            p.slots[i].active = mappings[i].active;
            p.slots[i].itemId = mappings[i].targetItemId;

            // Capture the active body-mesh prefab name (target side of the swap) so it is restored on next load. The
            // src side is the hardcoded Kliff default and need not be persisted.
            const auto tslot = static_cast<TransmogSlot>(i);
            const int tgtIdx = PWS::selection_tgt_index(tslot);
            if (tgtIdx >= 0)
            {
                const auto &cat = PWS::slot_catalog(tslot);
                if (static_cast<std::size_t>(tgtIdx) < cat.size())
                    p.slots[i].prefabName = cat[tgtIdx].name;
            }

            // When a body-mesh prefab is set, the carrier itemId is an internal implementation detail (the
            // auto-borrowed
            // Kairos plate that feeds the source wrapper). Don't persist itemName for these slots -- the JSON only
            // shows the user-meaningful prefabName, and load-time re-derives the carrier via default_carrier_for_slot.
            if (!p.slots[i].prefabName.empty())
            {
                p.slots[i].itemName.clear();
                continue;
            }

            // Plain carrier slot (no body-mesh override). Resolve itemName for round-trip persistence. id 0 means
            // "none" -- don't resolve a name for it; the catalog's entry at index 0 is a real item (Pyeonjeon_Arrow)
            // and saving that name would cause the preset to load an unintended item on reresolve.
            if (table.ready() && mappings[i].targetItemId != 0)
                p.slots[i].itemName = table.name_of(mappings[i].targetItemId);
            else
                p.slots[i].itemName.clear();
        }
        return p;
    }

    std::size_t PresetManager::reresolve_all_names()
    {
        auto &logger = DMK::Logger::get_instance();
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
        {
            logger.warning("[preset] reresolve_all_names called before catalog ready -- "
                           "no-op");
            return 0;
        }

        std::size_t resolved = 0;
        std::size_t disabled = 0;

        for (auto &[charName, cp] : m_characters)
        {
            for (auto &preset : cp.presets)
            {
                for (auto &slot : preset.slots)
                {
                    if (slot.itemName.empty())
                        continue;

                    auto fresh = table.id_of(slot.itemName);
                    if (fresh.has_value())
                    {
                        slot.itemId = *fresh;
                        ++resolved;
                    }
                    else
                    {
                        logger.warning("[preset] '{}' not in catalog -- disabling "
                                       "(char='{}' preset='{}')",
                                       slot.itemName, charName, preset.name);
                        slot.itemId = 0;
                        slot.active = false;
                        ++disabled;
                    }
                }
            }
        }

        logger.info("[preset] reresolve complete: {} resolved, {} disabled", resolved, disabled);
        return resolved;
    }

    // --- Private ---

    CharacterPresets &PresetManager::ensure_character(const std::string &name)
    {
        return m_characters[name]; // Default-constructs if absent.
    }

    const std::string &current_apply_owner() noexcept
    {
        auto &pm = PresetManager::instance();
        return pm.editing_pinned() ? pm.editing_character() : pm.active_character();
    }

} // namespace Transmog
