#include "preset_manager.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
#include "generated/dye_color_table.hpp"
#include "item_name_table.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace Transmog
{
    // Resolve a dye channel's group by its persisted `group_name`
    // (the data file's _stringKey, e.g. "Her_Color_Group_I").
    // Populates `group_hash` from the running game's table. If the
    // name is empty or no longer in the table, leaves group_hash at 0
    // and the channel becomes inactive at injection time. Other
    // channels of the same slot are unaffected.
    static void resolve_dye_group(ChannelDye &ch)
    {
        using namespace Transmog::DyeColorTable;
        ch.group_hash = 0; // always re-derive from the name

        if (ch.group_name.empty()) return;

        if (const auto *g =
                find_group_by_name(ch.group_name.c_str()))
        {
            ch.group_hash = g->key;
            return;
        }

        DMK::Logger::get_instance().warning(
            "[preset] dye group '{}' not found in current game's "
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
        // Only emit prefabName when set, to keep the JSON tidy for
        // the common (no body-mesh override) case.
        if (!s.prefabName.empty())
            j["prefabName"] = s.prefabName;
        // Sparse dye_mods array: emit one object per active channel
        // only. group_hash == 0 means "no override for this channel".
        if (any_dye_active(s.dye))
        {
            json mods = json::array();
            for (std::size_t idx = 0; idx < s.dye.size(); ++idx)
            {
                const auto &ch = s.dye[idx];
                if (!ch.active())
                    continue;
                // group_name (the data file's _stringKey) is the
                // sole stable identifier we persist. group_hash is
                // resolved from it at load and never written back.
                json m{
                    {"idx",        idx},
                    {"group_name", ch.group_name},
                    {"r",          ch.r},
                    {"g",          ch.g},
                    {"b",          ch.b},
                };
                if (ch.material_id != 0xFFFF)
                    m["material"] = ch.material_id;
                if (ch.repair_byte != 0)
                    m["repair"] = ch.repair_byte;
                mods.push_back(std::move(m));
            }
            j["dye_mods"] = std::move(mods);
            // Persist whether this dye block was sourced from a real
            // auth-table capture (sparse inject on apply) versus
            // user-curated picker selections (dense inject). Only
            // emitted when there are dye_mods so unused slots stay
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
                if (!m.is_object()) continue;
                const auto idx =
                    m.value("idx", std::size_t{k_dyeChannelCount});
                if (idx >= s.dye.size()) continue;
                auto &ch = s.dye[idx];
                ch.group_name =
                    m.value("group_name", std::string());
                ch.r = m.value("r", std::uint8_t{0});
                ch.g = m.value("g", std::uint8_t{0});
                ch.b = m.value("b", std::uint8_t{0});
                ch.material_id =
                    m.value("material", std::uint16_t{0xFFFF});
                ch.repair_byte = m.value("repair", std::uint8_t{0});
                resolve_dye_group(ch); // populates ch.group_hash
            }
            // Default false (dense, picker-style) when absent so older
            // presets without the field continue to behave as before.
            s.dyeSparse = j.value("dye_sparse", false);
        }

        if (s.itemName.empty())
            return s;

        // itemName is the sole persistent identifier. If the catalog
        // is already built we resolve immediately; otherwise itemId
        // stays 0 until reresolve_all_names() runs after the deferred
        // scan completes.
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
            DMK::Logger::get_instance().warning(
                "[preset] item name '{}' not in current catalog -- "
                "slot disabled; re-pick in the overlay",
                s.itemName);
            s.active = false;
        }
        return s;
    }

    // Keyed-object slot serialization. Old format was a positional
    // array (slots[0]=Helm, [1]=Chest, ...) which broke whenever the
    // TransmogSlot enum gained a row -- legacy presets shifted by
    // one. Object form keys each slot by its slot_metadata display
    // name ("Helm", "Chest", ...) so the JSON survives enum
    // additions/removals as long as the names stay stable.
    //
    // Migration is automatic: preset_from_json accepts BOTH the
    // legacy array form AND the new object form. The next save
    // rewrites every loaded file in object form.
    //
    // Disabled slots and default-empty rows are omitted from the
    // object to keep the JSON tidy. A completely-empty preset
    // serializes as `{"slots": {}}`.
    static json preset_to_json(const Preset &p)
    {
        json slotsObj = json::object();
        for (std::size_t i = 0; i < k_slotCount; ++i) {
            const auto &s = p.slots[i];
            if (!Transmog::slot_enabled(i))
                continue;
            const bool defaultRow = !s.active && s.itemName.empty() &&
                                    s.prefabName.empty() &&
                                    !any_dye_active(s.dye);
            if (defaultRow)
                continue;
            slotsObj[Transmog::slot_meta(static_cast<TransmogSlot>(i))
                         .displayName] = slot_to_json(s);
        }
        return json{{"name", p.name}, {"slots", slotsObj}};
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
                    DMK::Logger::get_instance().warning(
                        "[preset] '{}' has unknown slot key '{}' -- "
                        "ignored (slot may have been renamed or "
                        "removed)",
                        p.name, it.key());
                }
            }
            return p;
        }

        // Legacy format: positional array. Indices map directly to
        // TransmogSlot enum order. Auto-migrated to object form on
        // next save.
        if (slotsJ.is_array())
        {
            DMK::Logger::get_instance().info(
                "[preset] '{}' loaded from legacy array format -- "
                "next save will rewrite as keyed object",
                p.name);
            for (std::size_t i = 0;
                 i < k_slotCount && i < slotsJ.size();
                 ++i)
                p.slots[i] = slot_from_json(slotsJ[i]);
        }
        return p;
    }

    static json character_to_json(const CharacterPresets &cp)
    {
        json presetsArr = json::array();
        for (auto &p : cp.presets)
            presetsArr.push_back(preset_to_json(p));

        return json{
            {"activePreset", cp.activePreset},
            {"presets", presetsArr},
            {"bodyKind", cp.bodyKind},
        };
    }

    static CharacterPresets character_from_json(const json &j)
    {
        CharacterPresets cp;
        cp.activePreset = j.value("activePreset", 0);
        cp.bodyKind = j.value("bodyKind", std::string("Auto"));

        if (j.contains("presets") && j["presets"].is_array())
        {
            for (auto &pj : j["presets"])
                cp.presets.push_back(preset_from_json(pj));
        }

        // Clamp active index.
        if (!cp.presets.empty())
            cp.activePreset = std::clamp(cp.activePreset, 0,
                                         static_cast<int>(cp.presets.size()) - 1);
        else
            cp.activePreset = 0;

        return cp;
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

            // `activeCharacter` JSON field is no longer used -- the
            // runtime active character is driven by the live WS
            // chain via the char-swap detector. Kept here only to
            // silently tolerate old presets.json files that still
            // carry it; the value is discarded.
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

            logger.info("Presets loaded: {} character(s) from '{}'",
                        m_characters.size(), path);

            if (!ItemNameTable::instance().ready())
            {
                logger.info(
                    "[preset] item catalog not ready at load time -- name "
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

        // Just-loaded state matches disk; clear any leftover dirty
        // signal from earlier sessions of the process and snapshot
        // the active preset's dye as our reference baseline.
        dye_dirty().store(false, std::memory_order_release);
        capture_dye_snapshot();
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
        // Schema v3: slots carry only itemName (stable identifier).
        // itemId is resolved at runtime from the item catalog.
        root["version"] = 3;
        // `activeCharacter` is intentionally NOT written -- the live
        // controlled character drives apply at runtime, so the field
        // would just be a stale last-viewed-UI hint.

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
        dye_dirty().store(false, std::memory_order_release);
        // The just-saved state IS the new baseline -- subsequent
        // edits become "dirty" relative to this point.
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
        return m_activeCharacter;
    }

    void PresetManager::set_active_character(const std::string &name)
    {
        m_activeCharacter = name;
        ensure_character(name);
    }

    std::string PresetManager::body_kind_of(const std::string &charName) const
    {
        auto it = m_characters.find(charName);
        if (it == m_characters.end())
            return "Auto";
        return it->second.bodyKind.empty() ? std::string("Auto")
                                            : it->second.bodyKind;
    }

    void PresetManager::set_body_kind_of(const std::string &charName,
                                         const std::string &bodyKind)
    {
        auto &cp = ensure_character(charName);
        // Normalise: accept only known values; anything else collapses
        // to "Auto" so the JSON stays clean and the picker filter has
        // a well-defined fallback.
        if (bodyKind == "Male" || bodyKind == "Female" ||
            bodyKind == "Both" || bodyKind == "Auto")
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
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return 0;
        return it->second.activePreset;
    }

    int PresetManager::preset_count() const
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return 0;
        return static_cast<int>(it->second.presets.size());
    }

    const Preset *PresetManager::active_preset() const
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0,
                              static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const Preset *PresetManager::active_preset_of(
        const std::string &charName) const
    {
        // Read-only sibling of active_preset() that targets an arbitrary
        // character. Used by the body-mesh prefab picker to borrow the
        // Kairos (default-carrier) preset's itemIds while leaving the
        // manager's active_character untouched -- mutating active_character
        // here would side-trigger load-detect / radial-swap state and
        // cascade into a save.
        auto it = m_characters.find(charName);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0,
                              static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    Preset *PresetManager::active_preset_mut()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0,
                              static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const std::vector<Preset> &PresetManager::presets() const
    {
        static const std::vector<Preset> s_empty;
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return s_empty;
        return it->second.presets;
    }

    void PresetManager::append_from_state()
    {
        auto &cp = ensure_character(m_activeCharacter);
        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        Preset blank;
        blank.name = name;
        for (auto &s : blank.slots)
        {
            s.active = true;
            s.itemId = 0;
            s.itemName.clear();
        }
        cp.presets.push_back(std::move(blank));
        cp.activePreset = idx;

        apply_to_state();

        DMK::Logger::get_instance().info(
            "Preset appended: '{}' (index {}, all slots ticked + none)",
            name, idx);
        save();
    }

    void PresetManager::duplicate_current()
    {
        auto &cp = ensure_character(m_activeCharacter);
        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        auto captured = capture_from_state(name);
        // Preserve dye from the source preset (slot_mappings does not
        // carry dye state -- see replace_current_from_state).
        if (auto *src = active_preset_mut())
        {
            for (std::size_t i = 0;
                 i < src->slots.size() && i < captured.slots.size();
                 ++i)
            {
                captured.slots[i].dye = src->slots[i].dye;
                // Carry the source slot's sparse/dense origin flag
                // across the duplicate. capture_from_state() builds
                // a fresh PresetSlot from slot_mappings, which has
                // no notion of dyeSparse, so the captured slot is
                // left at the PresetSlot default. That default does
                // not necessarily match the source preset's mode
                // (a captured preset uses sparse, a picker-built
                // preset may not), so we copy the source value
                // explicitly to keep duplicates visually identical
                // to their origin.
                captured.slots[i].dyeSparse = src->slots[i].dyeSparse;
            }
        }
        cp.presets.push_back(std::move(captured));
        cp.activePreset = idx;

        apply_to_state();

        DMK::Logger::get_instance().info(
            "Preset duplicated: '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::replace_current_from_state()
    {
        auto *p = active_preset_mut();
        if (!p)
        {
            auto &cp = ensure_character(m_activeCharacter);
            int idx = static_cast<int>(cp.presets.size());
            std::string name = "Preset " + std::to_string(idx);
            cp.presets.push_back(capture_from_state(name));
            cp.activePreset = idx;

            DMK::Logger::get_instance().info("Preset replaced: '{}' (index {})", name, idx);
            save();
            return;
        }

        auto captured = capture_from_state(p->name);
        // Dye state is not part of slot_mappings; the picker writes
        // it directly onto the active preset's PresetSlot::dye.
        // capture_from_state() rebuilds slots from slot_mappings, so
        // it would clobber both the dye edits AND the dyeSparse
        // flag. Carry both forward from the existing preset so the
        // capture_outfit -> save / "Replace" button path preserves
        // user-curated dye and its sparse-vs-dense origin (a fresh
        // capture_outfit sets dyeSparse=true; the dye picker leaves
        // it whatever it already was).
        for (std::size_t i = 0;
             i < p->slots.size() && i < captured.slots.size(); ++i)
        {
            captured.slots[i].dye = p->slots[i].dye;
            captured.slots[i].dyeSparse = p->slots[i].dyeSparse;
        }
        p->slots = captured.slots;

        DMK::Logger::get_instance().info("Preset replaced: '{}'", p->name);
        save();
    }

    void PresetManager::remove_current()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        auto idx = std::clamp(cp.activePreset, 0,
                              static_cast<int>(cp.presets.size()) - 1);

        DMK::Logger::get_instance().info("Preset removed: '{}'",
                                         cp.presets[static_cast<std::size_t>(idx)].name);

        cp.presets.erase(cp.presets.begin() + idx);

        if (cp.presets.empty())
            cp.activePreset = 0;
        else
            cp.activePreset = std::min(idx, static_cast<int>(cp.presets.size()) - 1);

        save();
    }

    void PresetManager::next_preset()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Discard unsaved dye edits before cycling.
        revert_active_dye_to_snapshot();

        auto &cp = it->second;
        cp.activePreset = (cp.activePreset + 1) % static_cast<int>(cp.presets.size());
        capture_dye_snapshot();

        DMK::Logger::get_instance().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
            cp.activePreset + 1, cp.presets.size());

        // See set_active_preset for the rationale -- preset switch
        // must force re-apply so dye state is rebuilt.
        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
        save();
    }

    void PresetManager::prev_preset()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        revert_active_dye_to_snapshot();

        auto &cp = it->second;
        int count = static_cast<int>(cp.presets.size());
        cp.activePreset = (cp.activePreset - 1 + count) % count;
        capture_dye_snapshot();

        DMK::Logger::get_instance().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
            cp.activePreset + 1, cp.presets.size());

        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
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
        for (std::size_t i = 0;
             i < p->slots.size() && i < m_dyeSnapshot.size(); ++i)
        {
            m_dyeSnapshot[i] = p->slots[i].dye;
        }
        m_dyeSnapshotValid = true;
    }

    void PresetManager::revert_active_dye_to_snapshot() noexcept
    {
        if (!m_dyeSnapshotValid) return;
        if (!dye_dirty().load(std::memory_order_acquire)) return;
        auto *p = active_preset_mut();
        if (!p) return;
        for (std::size_t i = 0;
             i < p->slots.size() && i < m_dyeSnapshot.size(); ++i)
        {
            p->slots[i].dye = m_dyeSnapshot[i];
        }
        dye_dirty().store(false, std::memory_order_release);
        DMK::Logger::get_instance().info(
            "[preset] reverted unsaved dye edits on '{}'", p->name);
    }

    void PresetManager::set_active_preset(int index)
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        // Drop any unsaved dye edits on the OUTGOING active preset
        // before we switch -- the new preset's dye state will be
        // captured fresh below.
        revert_active_dye_to_snapshot();

        auto &cp = it->second;
        cp.activePreset = std::clamp(index, 0,
                                     static_cast<int>(cp.presets.size()) - 1);
        // Capture the NEW active preset's dye as the baseline; any
        // future edits become "dirty" relative to this baseline.
        capture_dye_snapshot();
        // Switching to a different preset means we're now showing the
        // on-disk state for that preset; nothing to save until the
        // user edits.
        dye_dirty().store(false, std::memory_order_release);
        // The dispatcher's slotNeedsWork check is item-id based and
        // doesn't notice dye state changes. When two presets share the
        // same carrier ids (e.g. Kairos preset + a body-mesh prefab
        // preset both built on Kairos carrier 0x1521), the dispatcher
        // would skip the apply and leave stale dye records bleeding
        // through. Force re-apply for every enabled slot so DyeCopier
        // gets a fresh injection from the new preset's dye state
        // (including the case where the new preset has no dye -- our
        // hook then skips injection and the engine's natural records
        // win). Cost: full slot rebuild on every preset switch, but
        // that's already the user's intent on a switch.
        for (std::size_t i = 0; i < k_slotCount; ++i)
            if (slot_enabled(i))
                force_apply_pending()[i] = true;
        apply_to_state();
    }

    // --- State bridge ---

    void PresetManager::apply_to_state() const
    {
        const auto *p = active_preset();
        if (!p)
            return;

        auto &mappings = slot_mappings();

        // NOTE: must NOT touch last_applied_ids() here. lastIds tracks what
        // apply_all_transmog has actively injected into the game -- the diff
        // logic depends on lastIds holding the PREVIOUS applied state so it
        // can compute drop-masks on preset switching. Writing to lastIds
        // here would pre-populate it with the NEW preset values before the
        // apply runs, breaking drop detection.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            // Disabled slots (multi-prefab non-armor / duplicate-tag
            // -- see slot_metadata.hpp `enabled` doc) cannot be applied
            // and are hidden from the picker. A legacy preset saved
            // before these slots were disabled may still carry
            // active=true with a non-zero itemId. Force everything off
            // here so downstream code (lastIds clearing, carrier-borrow,
            // PWS sync below) doesn't see a stale ticked state for a
            // slot the user has no way to interact with.
            if (!Transmog::slot_enabled(i))
            {
                mappings[i].active = false;
                mappings[i].targetItemId = 0;
                continue;
            }

            mappings[i].active = p->slots[i].active;
            mappings[i].targetItemId = p->slots[i].itemId;

            // Body-mesh slots only persist `prefabName`; the carrier
            // itemId is derived here from the active character's
            // default Kliff/Damiane/Oongka plate set. This keeps the
            // user-facing JSON clean (no spurious "Kliff_PlateArmor_*"
            // names cluttering body-mesh-only preset entries).
            if (mappings[i].targetItemId == 0 &&
                !p->slots[i].prefabName.empty())
            {
                const auto carrier = Transmog::default_carrier_for_slot(
                    static_cast<TransmogSlot>(i), m_activeCharacter);
                if (carrier != 0)
                {
                    mappings[i].targetItemId = carrier;
                    mappings[i].active = true;
                }
            }
        }

        // Sync body-mesh prefab selections. For each slot, if the
        // preset stores a prefabName, look it up in the slot's catalog
        // and set the PWS target index. Empty prefabName clears the
        // target (slot reverts to plain carrier rendering). When the
        // catalog isn't yet populated (boot heap walk in progress)
        // resolution silently misses; the boot thread re-runs
        // apply_to_state when populate_slot_catalogs finishes so the
        // selection lands as soon as the data is available.
        namespace PWS = Transmog::PrefabWrapperSwap;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto tslot = static_cast<TransmogSlot>(i);
            const int curSrc = PWS::selection_src_index(tslot);
            // Force-clear any persisted PWS selection on disabled
            // slots so a legacy preset's body-mesh prefab can't keep
            // a target index live (which the natural-pipeline hook
            // would otherwise still apply against).
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
            // Disabled slots (multi-prefab non-armor / duplicate-tag,
            // see slot_metadata.hpp) are WIP -- don't bloat the JSON
            // with their current in-memory state. Leaving p.slots[i]
            // default-constructed serializes the row as
            // `{"active":false,"itemName":""}` so the array indexing
            // stays positional but the entry is empty. Any stale data
            // from an older preset (saved before the slot was
            // disabled) is discarded on the next save.
            if (!Transmog::slot_enabled(i))
                continue;

            p.slots[i].active = mappings[i].active;
            p.slots[i].itemId = mappings[i].targetItemId;

            // Capture the active body-mesh prefab name (target side
            // of the swap) so it is restored on next load. The src
            // side is the hardcoded Kliff default and need not be
            // persisted.
            const auto tslot = static_cast<TransmogSlot>(i);
            const int tgtIdx = PWS::selection_tgt_index(tslot);
            if (tgtIdx >= 0)
            {
                const auto &cat = PWS::slot_catalog(tslot);
                if (static_cast<std::size_t>(tgtIdx) < cat.size())
                    p.slots[i].prefabName = cat[tgtIdx].name;
            }

            // When a body-mesh prefab is set, the carrier itemId is
            // an internal implementation detail (the auto-borrowed
            // Kairos plate that feeds the source wrapper). Don't
            // persist itemName for these slots -- the JSON only
            // shows the user-meaningful prefabName, and load-time
            // re-derives the carrier via default_carrier_for_slot.
            if (!p.slots[i].prefabName.empty())
            {
                p.slots[i].itemName.clear();
                continue;
            }

            // Plain carrier slot (no body-mesh override). Resolve
            // itemName for round-trip persistence. id 0 means "none"
            // -- don't resolve a name for it; the catalog's entry at
            // index 0 is a real item (Pyeonjeon_Arrow) and saving
            // that name would cause the preset to load an unintended
            // item on reresolve.
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
            logger.warning(
                "[preset] reresolve_all_names called before catalog ready -- "
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
                        logger.warning(
                            "[preset] '{}' not in catalog -- disabling "
                            "(char='{}' preset='{}')",
                            slot.itemName, charName, preset.name);
                        slot.itemId = 0;
                        slot.active = false;
                        ++disabled;
                    }
                }
            }
        }

        logger.info(
            "[preset] reresolve complete: {} resolved, {} disabled",
            resolved, disabled);
        return resolved;
    }

    // --- Private ---

    CharacterPresets &PresetManager::ensure_character(const std::string &name)
    {
        return m_characters[name]; // Default-constructs if absent.
    }

} // namespace Transmog
