// overlay_ui/transmog_tab.cpp
//
// Top-of-Transmog-tab UI: header, global toggles, character picker, presets section. Each function corresponds to one
// depth-1 banner in draw_overlay_content and ends with the trailing separator that closes its block.

#include "overlay_ui/transmog_tab.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "constants.hpp"
#include "item_name_table.hpp"
#include "lang.hpp"
#include "language_pack.hpp"
#include "overlay_font.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <DetourModKit/filesystem.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Transmog
{

    /**
     * @brief Size ImGui rasterizes its built-in font at, and the fallback when the host reports none.
     * @details ImGui forces ProggyClean to 13px, but leaves a TTF at the unspecified-size fallback of 20px. A
     *          measurement that comes back as 0 therefore has to assume the former, not the latter.
     */
    inline constexpr float PROGGY_CLEAN_SIZE_PX = 13.0f;

    /**
     * @brief Size multiplier applied when the host still uses ImGui's built-in bitmap font.
     * @details That font fills its 13px cell, while a TrueType face at 13px has a much smaller cap height and reads
     *          as noticeably smaller beside it. Scaling up matches this mod's text to the rest of the overlay. It is
     *          applied only against the built-in font, since a host already on a TTF needs no correction.
     */
    inline constexpr float BITMAP_TO_TTF_SIZE_RATIO = 1.2f;

    /// Locale whose glyphs the live font cannot draw, refreshed each frame by ensure_host_font(). Empty when
    /// every glyph on screen renders.
    static std::string s_font_missing_locale;
    /**
     * @brief Fonts this mod added to the host atlas, keyed by the "names|ui" locale pair they were built for.
     * @details The add decision keys on THIS map, never on `ImFont::IsGlyphInFont`. That call reports whether a font
     *          SOURCE covers a codepoint, not whether the codepoint will draw: a source added to an already-baked
     *          font answers true while the stale baked cache still refuses to render it. Keying on our own state
     *          keeps detection and rendering from disagreeing. A null value records a failed add, so one missing
     *          system font does not retry every frame.
     */
    static std::map<std::string, void *> s_host_fonts;
    /**
     * @brief Host base size measured BEFORE this mod pushed a font of its own, so the measurement stays honest.
     * @details Measuring every frame reads back this mod's own pushed, already-scaled font and rescales it, which
     *          compounds toward zero within a second. Measure once while the host's font is still current, and reset
     *          only when the host clears its atlas and the measurement could legitimately have changed.
     */
    static float s_host_base_size = 0.0f;
    /// Font-config generation the cached fonts were built against, so an INI reload rebuilds them.
    static unsigned s_font_config_generation = 0;

    /**
     * @brief Cached locale list and labels for the item-name combo.
     * @details The pack index does not change while the process runs, so the list is built once and reused rather
     *          than rebuilt per frame on the render path. `built` drops to false whenever a new font lands, because
     *          a label that fell back to its bare tag may now be drawable as its endonym. It is at namespace scope
     *          because the font setup reads the selection as well as the combo that writes it.
     */
    struct LocaleChoices
    {
        std::vector<LanguagePack::LocaleInfo> locales;
        std::vector<std::string> labels;
        std::vector<const char *> label_ptrs;
        int selected = 0;
        bool built = false;
    };
    static LocaleChoices s_choices;

    /**
     * @brief Cached tag list and labels for the interface-language combo.
     * @details Separate from @ref s_choices because the two combos list different things: every locale the pack
     *          carries, against only the locales the translations file has been filled in for.
     */
    struct InterfaceChoices
    {
        std::vector<std::string> tags;
        std::vector<std::string> labels;
        std::vector<const char *> label_ptrs;
        int selected = 0;
        bool built = false;
    };
    static InterfaceChoices s_choices_ui;

    /// First codepoint above ASCII in a UTF-8 string, or 0 when the string is pure ASCII or malformed.
    static char32_t first_non_ascii(std::string_view text) noexcept
    {
        for (std::size_t i = 0; i < text.size();)
        {
            const auto lead = static_cast<unsigned char>(text[i]);
            if (lead < 0x80)
            {
                ++i;
                continue;
            }
            std::size_t length = 0;
            char32_t code = 0;
            if ((lead & 0xE0u) == 0xC0u)
            {
                length = 2;
                code = lead & 0x1Fu;
            }
            else if ((lead & 0xF0u) == 0xE0u)
            {
                length = 3;
                code = lead & 0x0Fu;
            }
            else if ((lead & 0xF8u) == 0xF0u)
            {
                length = 4;
                code = lead & 0x07u;
            }
            else
            {
                return 0;
            }
            if (i + length > text.size())
                return 0;
            for (std::size_t k = 1; k < length; ++k)
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            return code;
        }
        return 0;
    }

    /**
     * @brief Check whether the live font can draw @p text, judged by its first non-ASCII codepoint.
     * @details Pure ASCII, and anything past the BMP, answer true without a probe. ImWchar is 16 bits in this
     *          build, so a probe above U+FFFF would test a truncated codepoint and could hide a label that renders.
     */
    static bool font_can_draw(std::string_view text)
    {
        const char32_t code = first_non_ascii(text);
        if (code == 0 || code > 0xFFFF)
            return true;
        ImFont *const font = ImGui::GetFont();
        return font == nullptr || font->IsGlyphInFont(static_cast<ImWchar>(code));
    }

    /// Copies the persisted overlay preferences into the live UI state once per session.
    static void load_ui_prefs_once()
    {
        static bool s_loaded = false;
        if (s_loaded)
            return;
        s_loaded = true;
        const auto prefs = PresetManager::instance().ui_prefs();
        s_ui_scale = prefs.ui_scale;
        s_auto_apply = prefs.instant_apply;
        s_keep_search_text = prefs.keep_search_text;
        s_preset_rows = prefs.preset_rows;
    }

    /// Writes the live UI state back to presets.json.
    static void save_ui_prefs()
    {
        PresetManager::instance().set_ui_prefs({
            .ui_scale = s_ui_scale,
            .instant_apply = s_auto_apply,
            .keep_search_text = s_keep_search_text,
            .preset_rows = s_preset_rows,
        });
    }

    void invalidate_host_font_cache()
    {
        s_host_fonts.clear();
        s_host_locale_font = nullptr;
        s_host_locale_font_size = 0.0f;
        // The host may have switched to a different font, so the next measurement must be taken fresh.
        s_host_base_size = 0.0f;
    }

    /**
     * @brief Width that fits the widest entry of a combo, and no more.
     * @param labels The entries the combo will list.
     * @details A fixed multiple of the font size has to be guessed against the longest label it might ever
     *          hold, so it is always too wide for the labels actually present, and the window auto-sizes to
     *          whatever is widest on the row. Measuring the real entries keeps the row as narrow as its
     *          content allows in every language.
     */
    static float combo_width_for(const std::vector<const char *> &labels)
    {
        float widest = 0.0f;
        for (const char *const label : labels)
            widest = (std::max)(widest, ImGui::CalcTextSize(label).x);
        // The arrow button is a square of one frame height, and the text sits inside the frame padding.
        return widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    /// Reloads the interface strings for whichever locale the two stored preferences resolve to.
    static void refresh_interface_language()
    {
        auto &pm = PresetManager::instance();
        lang::load_for(
            std::filesystem::path{DMK::filesystem::get_runtime_directory()} / INTERFACE_TRANSLATIONS_FILE,
            pm.interface_locale(),
            pm.display_name_locale()
        );
    }

    /**
     * @brief Interface-language combo, drawn only when a translations file offers a choice.
     *
     * @details Separate from the item-name locale above: item names come from the game's own tables and exist in
     *          all 15 of its languages, while the interface exists only where someone has translated it. The
     *          default, Auto, follows the item-name locale, so the common case stays a single choice and this
     *          combo is only touched by someone who wants the two to differ.
     */
    static void draw_interface_locale_combo()
    {
        if (!s_choices_ui.built)
        {
            const auto path =
                std::filesystem::path{DMK::filesystem::get_runtime_directory()} / INTERFACE_TRANSLATIONS_FILE;
            const auto translated = lang::available_locales(path);
            // Mark it built even with nothing to show. This reads and parses a file, and leaving the flag clear
            // would repeat that on every frame of every install that has no translations, which is most of them.
            // The count guard below is what actually hides the combo.
            s_choices_ui.built = true;
            if (translated.empty())
                return;

            s_choices_ui.tags.clear();
            s_choices_ui.labels.clear();
            s_choices_ui.label_ptrs.clear();

            // Auto first because it is the default, then plain English, which is what the mod carries inline and
            // needs no entry in the file.
            s_choices_ui.tags.emplace_back(lang::FOLLOW_ITEM_NAMES);
            s_choices_ui.labels.emplace_back(lang::t("header.interface.auto", "Auto"));
            s_choices_ui.tags.emplace_back("eng");
            s_choices_ui.labels.emplace_back("English");

            for (const auto &locale : translated)
            {
                if (locale.tag == "eng")
                    continue;
                s_choices_ui.tags.push_back(locale.tag);
                // Fall back to the bare tag when the live font cannot draw the endonym, so the row stays
                // readable and selectable rather than becoming a run of '?'.
                s_choices_ui.labels.push_back(
                    (locale.endonym.empty() || !font_can_draw(locale.endonym)) ? locale.tag : locale.endonym
                );
            }

            // Only after `labels` stops growing: a push_back can reallocate and invalidate every c_str().
            for (const auto &label : s_choices_ui.labels)
                s_choices_ui.label_ptrs.push_back(label.c_str());

            const std::string current = PresetManager::instance().interface_locale();
            for (std::size_t i = 0; i < s_choices_ui.tags.size(); ++i)
            {
                if (s_choices_ui.tags[i] == current)
                {
                    s_choices_ui.selected = static_cast<int>(i);
                    break;
                }
            }
        }

        const auto count = static_cast<int>(s_choices_ui.label_ptrs.size());
        if (count < 2)
            return;

        ImGui::SameLine(0.0f, ui_px(16.0f));
        ui_text(lang::t("header.interface", "Interface:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(combo_width_for(s_choices_ui.label_ptrs));
        if (ImGui::Combo("##interfaceLocale", &s_choices_ui.selected, s_choices_ui.label_ptrs.data(), count, -1))
        {
            PresetManager::instance().set_interface_locale(
                s_choices_ui.tags[static_cast<std::size_t>(s_choices_ui.selected)]
            );
            refresh_interface_language();
            // The Auto label is itself translated, so rebuild the list against the language just chosen.
            s_choices_ui.built = false;
        }
        if (ImGui::IsItemHovered())
        {
            ui_tooltip(
                lang::t(
                    "header.interface.tip",
                    "Language for this mod's own text. Auto follows the item name language above."
                )
            );
        }
    }

    /**
     * @brief Item-name locale combo, drawn only when the language pack offers a choice.
     *
     * @details The pack index never changes while the process runs, so the label vectors build once. Rebuilding them
     *          per frame would allocate on the render path for no gain.
     *
     *          A label reads "<endonym> (zho-cn)" when the live font can draw the endonym, and falls back to the
     *          bare ASCII tag when it cannot, so no row is ever a run of '?'. Only the SELECTED locale's script is
     *          merged, so most endonyms are undrawable until their locale is chosen.
     */
    static void draw_item_name_locale_combo()
    {
        const auto &pack = LanguagePack::instance();
        if (!pack.ready())
            return;

        if (!s_choices.built)
        {
            // Mark it built before the empty check. `locales()` takes a lock and returns a fresh vector, so
            // leaving the flag clear would repeat both on every frame of a pack that carries no names block.
            s_choices.built = true;
            s_choices.locales = pack.locales();
            if (s_choices.locales.empty())
                return;
            s_choices.labels.clear();
            s_choices.label_ptrs.clear();

            for (const auto &locale : s_choices.locales)
            {
                // Only the SELECTED locale's font gets merged, so most endonyms in this list have no glyphs. Show
                // the bare tag rather than a row of '?' for those, and rebuild once a merge widens coverage.
                s_choices.labels.push_back(
                    (locale.endonym.empty() || !font_can_draw(locale.endonym))
                        ? locale.tag
                        : locale.endonym + " (" + locale.tag + ")"
                );
            }
            // Fill the pointer vector only after `labels` stops growing. A push_back can reallocate and invalidate
            // every c_str() taken before it.
            for (const auto &label : s_choices.labels)
                s_choices.label_ptrs.push_back(label.c_str());

            const std::string current = PresetManager::instance().display_name_locale();
            for (std::size_t i = 0; i < s_choices.locales.size(); ++i)
            {
                if (s_choices.locales[i].tag == current)
                {
                    s_choices.selected = static_cast<int>(i);
                    break;
                }
            }
        }

        const auto count = static_cast<int>(s_choices.label_ptrs.size());
        if (count < 2)
            return;

        ImGui::SameLine(0.0f, ui_px(24.0f));
        ui_text(lang::t("header.item_names", "Item Names:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(combo_width_for(s_choices.label_ptrs));
        if (ImGui::Combo("##itemNameLocale", &s_choices.selected, s_choices.label_ptrs.data(), count, -1))
        {
            const std::string &tag = s_choices.locales[static_cast<std::size_t>(s_choices.selected)].tag;
            // Store the preference first. A failed reload then still reopens on the chosen locale next session
            // instead of silently reverting.
            PresetManager::instance().set_display_name_locale(tag);
            // Reloaded inline, on the render thread, which costs one hitch. It is the required thread: the reload
            // clears the sorted cache that the picker holds a reference into. See item_name_table.hpp.
            ItemNameTable::instance().load_display_names(
                tag,
                std::filesystem::path{DMK::filesystem::get_runtime_directory()} / DISPLAY_NAMES_FILE
            );
            // The interface follows this locale unless it has been pinned to one of its own.
            refresh_interface_language();
        }
        if (ImGui::IsItemHovered())
        {
            ui_tooltip(
                lang::t(
                    "header.item_names.tip",
                    "Language for item names in the picker.\n"
                    "Names come from the game's own localization tables."
                )
            );
        }
    }

    /**
     * @brief Gives this mod's widgets their own font, for whatever locale is in effect.
     *
     * @details Kept apart from the locale combo above, and called whether or not that combo exists. Which optional
     *          data files an install carries must not change how the interface is laid out, and the combo appears
     *          only once the language pack has loaded. Without this call a pack-less install would draw with the
     *          host's own font, ImGui's built-in 13px ProggyClean under ReShade: a narrower, smaller, MONOSPACE
     *          face that shrinks the tab and clips text tuned against a proportional one.
     */
    static void ensure_host_font()
    {
        s_font_missing_locale.clear();
        auto &pm = PresetManager::instance();
        // The combo's selection when there is one, the saved preference otherwise. With no pack at all this
        // is simply the default locale, and the point is only to get THIS mod's base font pushed.
        const std::string names_locale = s_choices.built && !s_choices.locales.empty()
                                             ? s_choices.locales[static_cast<std::size_t>(s_choices.selected)].tag
                                             : pm.display_name_locale();
        // Item names and interface text are drawn side by side and are separate settings, so ONE font has to
        // cover both. Keying on the item-name locale alone leaves a Chinese interface over English item names
        // with a font that carries no hanzi.
        const std::string ui_locale{lang::effective_locale(pm.interface_locale(), names_locale)};
        const std::string cache_key = names_locale + "|" + ui_locale;
        const char32_t names_probe = overlay_font::script_probe_codepoint(names_locale);
        const char32_t ui_probe = overlay_font::script_probe_codepoint(ui_locale);

        // One mechanism for BOTH overlay modes: build one font carrying base and script together, then push it.
        // Merging into an already-baked font does NOT work: ImFontAtlasFontSourceAddToFont clears the absent-glyph
        // cache only when MergeMode is false, so a late source is never consulted for the missing codepoints.
        // An INI reload that changed FontPath invalidates everything built against the old value.
        if (const unsigned generation = overlay_font::config_generation(); generation != s_font_config_generation)
        {
            s_font_config_generation = generation;
            invalidate_host_font_cache();
        }

        // Measure the host exactly once, while its own font is still the current one. Style.FontSizeBase is
        // rewritten by the host every frame from its own config and sits at 0 when that config is unset, at
        // which point ImGui falls back per font, and its 20px default for a sizeless TTF differs from the
        // host font's 13px.
        if (s_host_base_size <= 0.0f && s_host_locale_font == nullptr)
        {
            ImFont *const current = ImGui::GetFont();
            float measured = ImGui::GetStyle().FontSizeBase;
            if (measured <= 0.0f)
            {
                measured =
                    (current != nullptr && current->LegacySize > 0.0f) ? current->LegacySize : PROGGY_CLEAN_SIZE_PX;
            }
            // U+0141 sits in Latin Extended-A, which the built-in bitmap font lacks and every candidate TTF
            // has, so it identifies which kind of face the host is drawing with.
            if (current == nullptr || !current->IsGlyphInFont(static_cast<ImWchar>(0x0141)))
                measured *= BITMAP_TO_TTF_SIZE_RATIO;
            s_host_base_size = measured;
        }
        const float size_px = s_host_base_size > 0.0f ? s_host_base_size : PROGGY_CLEAN_SIZE_PX;
        // ReShade owns the atlas but never locks it: it sets ImGuiBackendFlags_RendererHasTextures, so ImGui
        // leaves the atlas writable and rasterizes new glyphs on demand. Give this mod's widgets their own font
        // in that atlas, which fixes the display with no restart and no ReShade setting touched.
        overlay_font::ForeignAtlas host;
        host.atlas = ImGui::GetIO().Fonts;
        ImGui::GetAllocatorFunctions(
            reinterpret_cast<ImGuiMemAllocFunc *>(&host.alloc_fn),
            reinterpret_cast<ImGuiMemFreeFunc *>(&host.free_fn),
            &host.alloc_user
        );

        // Drop every cached font once the host has cleared its atlas, because each pointer is dangling from that
        // moment. Any LIVE entry proves the generation, so one probe settles the whole cache. A null entry records a
        // failed add and proves nothing, so skip past those rather than let one suppress the check.
        const auto probe = std::find_if(
            s_host_fonts.begin(),
            s_host_fonts.end(),
            [](const auto &entry) { return entry.second != nullptr; }
        );
        if (probe != s_host_fonts.end() && !overlay_font::host_font_is_live(host, probe->second))
        {
            s_host_fonts.clear();
            s_host_locale_font = nullptr;
        }

        // Build one for EVERY locale, not only the scripted ones. A tab that borrows the host font for
        // English and this mod's font for Chinese changes apparent size on each switch, because two fonts at
        // one pixel size do not occupy the same space.
        auto it = s_host_fonts.find(cache_key);
        if (it == s_host_fonts.end())
        {
            // Cache the result even on failure, so one missing font does not retry every frame. Reusing the
            // cache also stops a repeated locale switch from appending a fresh font to the host atlas each time.
            it = s_host_fonts.emplace(cache_key, overlay_font::add_host_font(host, names_locale, ui_locale, size_px))
                     .first;
            if (it->second != nullptr)
            {
                s_host_locale_font = it->second;
                s_host_locale_font_size = size_px;
                // Bound from the NEXT frame, never the one that created it. Both combos relabel against it too:
                // an endonym that fell back to its bare tag may be drawable now.
                s_choices.built = false;
                s_choices_ui.built = false;
                return;
            }
        }
        s_host_locale_font = it->second;
        if (s_host_locale_font != nullptr)
        {
            s_host_locale_font_size = size_px;
            return; // this mod's own font covers the locale
        }
        if (names_probe == 0 && ui_probe == 0)
            return; // nothing beyond Latin-1 was needed anyway

        s_font_missing_locale = (ui_probe != 0) ? ui_locale : names_locale;
    }

    /// The warning marker for a locale the live font cannot draw, drawn beside the combo that chose it.
    static void draw_font_missing_marker()
    {
        if (s_font_missing_locale.empty())
            return;
        ImGui::SameLine();
        ui_text_colored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "  [!]");
    }

    /**
     * @brief Tells the user when the selected locale has no font on this PC that can draw it.
     *
     * @details This mod adds its own font to whichever atlas is live, ReShade's included, so neither overlay mode
     *          needs a host font setting changed and nothing here writes one. `FontPath` feeds that same font in
     *          both modes, which makes it the one remedy worth naming.
     */
    static void draw_font_notice()
    {
        if (s_font_missing_locale.empty())
            return;
        ui_text_colored(
            ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
            lang::t("font.missing", "This language needs a font this PC does not have.")
        );
        ImGui::SameLine();
        ui_text_disabled(lang::t("font.set_fontpath", "Set FontPath in CrimsonDesertLiveTransmog.ini."));
    }

    void draw_header(bool pending, bool pending_save)
    {
        ImGui::TextUnformatted(MOD_NAME);
        ImGui::SameLine();
        ui_text_disabled("v%s", MOD_VERSION);

        load_ui_prefs_once();

        // Available in BOTH overlay modes. Standalone scales its own atlas through FontGlobalScale; under ReShade
        // this mod pushes its own font, so the scale multiplies that pushed size and leaves ReShade's own UI alone.
        // Persisted in presets.json.
        {
            // Continuous rather than a fixed ladder: the readable size depends on the display, the chosen font and
            // the host's own scale, and the right value often sits between two steps. Ctrl+click the slider to type
            // an exact figure.
            // Separate from the version/mod-title group with a gap and a visible label so users discover it.
            ImGui::SameLine(0.0f, ui_px(24.0f));
            ui_text(lang::t("header.ui_scale", "UI Scale:"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            // Commit on RELEASE, never live. The scale resizes the font and the style, which moves and resizes
            // this very slider mid-drag: the grab then sits somewhere else under the cursor, the value jumps, and
            // the widget fights the pointer. Holding the edit in a pending value keeps the slider still while the
            // drag is in progress, and applying once on release also spares presets.json a write per drag pixel.
            static float s_pending_scale = -1.0f;
            float scale = (s_pending_scale > 0.0f) ? s_pending_scale : s_ui_scale;
            if (ImGui::SliderFloat(
                    "##uiScale",
                    &scale,
                    PresetManager::UiPrefs::UI_SCALE_MIN,
                    PresetManager::UiPrefs::UI_SCALE_MAX,
                    "%.2fx"
                ))
                s_pending_scale =
                    std::clamp(scale, PresetManager::UiPrefs::UI_SCALE_MIN, PresetManager::UiPrefs::UI_SCALE_MAX);
            if (ImGui::IsItemDeactivatedAfterEdit() && s_pending_scale > 0.0f)
            {
                s_ui_scale = s_pending_scale;
                save_ui_prefs();
            }
            // Clear on ANY deactivation, so an abandoned drag or a cancelled Ctrl+click entry does not leave a
            // stale pending value shadowing the real one.
            if (ImGui::IsItemDeactivated())
                s_pending_scale = -1.0f;
            if (ImGui::IsItemHovered())
            {
                // Name the live font here. It is the only place a user can confirm that FontPath took effect
                // without reading the log or judging letterforms by eye.
                const std::string tip = lang::t(
                                            "header.ui_scale.tip",
                                            "Overlay scale for this mod's tab only. Ctrl+click to type a value.\n"
                                            "Stacks on the automatic resolution scale.\n"
                                            "Glyphs may blur at higher values.\n"
                                            "\n"
                                            "Font in use: "
                                        ) +
                                        overlay_font::base_font_name();
                ui_tooltip(tip.c_str());
            }
        }

        if (pending)
        {
            ImGui::SameLine();
            ui_text_colored(
                ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                lang::t("header.pending_badge", "  [PENDING - click Apply All]")
            );
        }

        if (pending_save)
        {
            ImGui::SameLine();
            ui_text_colored(
                ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                lang::t("header.unsaved_badge", "  [UNSAVED - click Save]")
            );
        }

        ImGui::Separator();
    }

    void draw_global_toggles()
    {
        bool enabled = flag_enabled().load(std::memory_order_relaxed);
        if (ImGui::Checkbox(lang::t("toggles.enabled", "Enabled"), &enabled))
        {
            flag_enabled().store(enabled, std::memory_order_relaxed);
            if (enabled)
                manual_apply();
            else
                manual_clear();
        }

        // ensure_host_font runs whether or not either combo drew, so a pack-less install gets the same face,
        // and therefore the same layout, as one with the pack.
        draw_item_name_locale_combo();
        draw_interface_locale_combo();
        ensure_host_font();
        draw_font_missing_marker();

        if (ImGui::Checkbox(lang::t("toggles.instant_apply", "Instant Apply"), &s_auto_apply))
            save_ui_prefs();
        if (ImGui::IsItemHovered())
            ui_tooltip(
                lang::t(
                    "toggles.instant_apply.tip",
                    "Apply changes immediately on hover, pick, toggle, and clear - no Apply All needed"
                )
            );
        ImGui::SameLine();
        ui_text_disabled("(?)");
        if (ImGui::IsItemHovered())
            ui_tooltip(
                lang::t(
                    "toggles.instant_apply.prefab_note",
                    "Prefab picker (Prefabs checkbox inside the popup) does not honor Instant Apply. "
                    "Click a prefab row to commit it. Hover-preview works on items only."
                )
            );

        ImGui::SameLine();
        if (ImGui::Checkbox(lang::t("toggles.keep_search", "Keep Search Text"), &s_keep_search_text))
            save_ui_prefs();
        if (ImGui::IsItemHovered())
            ui_tooltip(lang::t("toggles.keep_search.tip", "Preserve the search field when re-opening a slot picker"));

        draw_font_notice();
        ImGui::Separator();
    }

    void draw_character_picker(PresetManager &pm)
    {
        // Presets are stored per character (Kliff / Damiane / Oongka). Selecting a character swaps the active preset
        // list to that character's, re-applies its active preset, and clears the drop-detection state so the next apply
        // pass does not confuse the previous character's cached itemIds with the new one's.
        //
        // The controlled character is auto-detected by CDCore's appearance-config classifier and the worker's
        // load-detect thread keeps the active preset bound to whoever the user controls in-game. This dropdown is
        // therefore an editing override - picking a non-controlled character pins the editor onto that character's
        // preset list while the body on screen remains the controlled one (cross-body apply).

        // Fixed stack buffer - the game has a small, closed roster of playable characters (Kliff / Damiane / Oongka at
        // time of writing). max_chars is oversized so adding a new playable character later does not need a code change
        // here beyond adding its preset in the JSON.
        constexpr std::size_t max_chars = 8;
        const auto names = pm.character_names();
        const std::size_t n = (names.size() < max_chars) ? names.size() : max_chars;
        if (n > 0)
        {
            const char *cstrs[max_chars]{};
            int selected_idx = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                cstrs[i] = names[i].c_str();
                if (names[i] == pm.editing_character())
                    selected_idx = static_cast<int>(i);
            }

            // Dropdown writes the editing character only. The pin engages automatically when editing differs from
            // controlled; picking the controlled character clears the pin. Apply is cross-body when pinned: the
            // controlled body wears the editing character's preset items via pws / carrier substitution. Gendered or
            // character-specific items may not have a renderable variant on the controlled body and silently no-op.
            ImGui::SetNextItemWidth(ui_px(130.0f));
            const char *char_label = lang::t("character.picker", "Character##char_picker");
            if (ImGui::Combo(char_label, &selected_idx, cstrs, static_cast<int>(n), -1))
            {
                const auto &pick = names[static_cast<std::size_t>(selected_idx)];
                if (pick != pm.editing_character())
                {
                    pm.set_editing_character(pick);
                    for (auto &m : slot_mappings())
                    {
                        m.active = false;
                        m.target_item_id = 0;
                    }
                    pm.apply_to_state();
                    // Retain last_applied_ids / real_damaged / last_applied_real_ids / last_applied_carrier_ids across
                    // the editing-character switch. apply_all_transmog's
                    // Phase A tear-down uses `last_ids[slot] != 0 && !mods[slot].active` to detect stale carriers from
                    // the outgoing character's preset; pre-wiping any of those arrays leaves stale fakes installed on
                    // the body.
                    manual_apply();
                    pm.save();
                }
            }
            if (ImGui::IsItemHovered())
            {
                if (pm.editing_pinned())
                    ui_tooltip(
                        lang::t(
                            "character.pinned.tip",
                            "Editing pinned: "
                            "this character's preset is loaded into the slot rows, but the body on screen is whoever "
                            "you control in-game. "
                            "Cross-body apply - some items (gender-specific, weapons) may not render. "
                            "Pick the controlled character to unpin."
                        )
                    );
                else
                    ui_tooltip(
                        lang::t(
                            "character.picker.tip",
                            "Pick a different character to edit their preset while controlling someone else. "
                            "The controlled body will wear the picked character's preset items (cross-body apply - "
                            "partial coverage expected)."
                        )
                    );
            }
            if (pm.editing_pinned())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(lang::t("character.unpin", "Unpin##char_unpin")))
                {
                    pm.clear_editing_pin();
                    for (auto &m : slot_mappings())
                    {
                        m.active = false;
                        m.target_item_id = 0;
                    }
                    pm.apply_to_state();
                    // Retain last_applied_* arrays for the same Phase A tear-down reason documented in the dropdown
                    // handler.
                    manual_apply();
                    pm.save();
                }
                if (ImGui::IsItemHovered())
                    ui_tooltip(
                        lang::t(
                            "character.unpin.tip",
                            "Drop the editing pin and follow the controlled character again."
                        )
                    );
            }

            // Body-kind override. Lets body-swap mod users mark e.g. Kliff as Female so the picker shows the
            // female-body-token pool. "Auto" defers to the hardcoded default (Kliff/Oongka = Male, Damiane = Female).
            //
            // Saved per character in presets.json. Only affects the picker filter - no effect on apply / render
            // behavior.
            static constexpr const char *body_items[] = {
                "Auto",
                "Male",
                "Female",
                "Both",
            };
            constexpr int body_count = static_cast<int>(sizeof(body_items) / sizeof(body_items[0]));

            const std::string current_body = pm.body_kind_of(pm.editing_character());
            int body_idx = 0;
            for (int i = 0; i < body_count; ++i)
            {
                if (current_body == body_items[i])
                {
                    body_idx = i;
                    break;
                }
            }
            // Stored value and shown text are kept apart. `body_items` is what goes into presets.json and what
            // `body_kind_of` compares against, so it stays English in every interface language; only the labels
            // the combo draws are translated.
            static_assert(body_count == 4, "body_labels must stay in step with body_items");
            const char *body_labels[body_count] = {
                lang::t("body.auto", "Auto"),
                lang::t("body.male", "Male"),
                lang::t("body.female", "Female"),
                lang::t("body.both", "Both"),
            };

            ImGui::SameLine(0.0f, ui_px(24.0f));
            ImGui::SetNextItemWidth(ui_px(100.0f));
            if (ImGui::Combo(lang::t("character.body_kind", "Body##body_kind"), &body_idx, body_labels, body_count, -1))
            {
                pm.set_body_kind_of(pm.editing_character(), body_items[body_idx]);
            }
            if (ImGui::IsItemHovered())
                ui_tooltip(
                    lang::t(
                        "character.body_kind.tip",
                        "Override the body type used for picker filtering. "
                        "Use if you run a body-swap mod that changes this character's skeleton (e.g. Kliff -> Female)."
                    )
                );

            // Apply-to-selected-character toggle. Determines whether overlay-UI edits on a pinned non-controlled
            // character render on that character's body or cross-apply onto the controlled body. Persists via [General]
            // ApplyToSelectedCharacter in the INI.
            bool apply_to_editing = flag_apply_to_editing().load(std::memory_order_relaxed);
            if (ImGui::Checkbox(lang::t("character.apply_to_selected", "Apply To Selected"), &apply_to_editing))
            {
                flag_apply_to_editing().store(apply_to_editing, std::memory_order_relaxed);
            }
            if (ImGui::IsItemHovered())
                ui_tooltip(
                    lang::t(
                        "character.apply_to_selected.tip",
                        "When ticked, picking another character in the dropdown (and subsequent picker / slot edits "
                        "while pinned) applies the preset to THAT character's body, if they are loaded. "
                        "Engine equip events still render the controlled character's transmog as normal. "
                        "Untick to restore the legacy cross-body behavior where the controlled body wears the selected "
                        "character's preset items."
                    )
                );
        }

        ImGui::Separator();
    }

    void draw_presets_section(PresetManager &pm)
    {
        if (!ImGui::CollapsingHeader(lang::t("presets.header", "Presets"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Separator();
            return;
        }

        auto &preset_list = pm.presets();
        const int active_index = pm.active_preset_index();
        const int count = pm.preset_count();

        // Bounded so a long list cannot push Apply All off the screen, and ResizeY lets the user pick the bound,
        // because any fixed one is wrong for someone. The height is persisted in ROWS, not pixels, so it survives a
        // font or UI-scale change.
        const float preset_row_h = ImGui::GetTextLineHeightWithSpacing();
        s_preset_rows =
            std::clamp(s_preset_rows, PresetManager::UiPrefs::PRESET_ROWS_MIN, PresetManager::UiPrefs::PRESET_ROWS_MAX);
        ImGui::BeginChild(
            "##presetlist",
            ImVec2(0.0f, preset_row_h * s_preset_rows),
            ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY
        );

        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);

            const bool is_active = (i == active_index);

            if (s_rename_active && s_rename_index == i)
            {
                ImGui::SetNextItemWidth(ui_px(140.0f));
                if (ImGui::InputText(
                        "##rename",
                        s_rename_preset_buf,
                        sizeof(s_rename_preset_buf),
                        ImGuiInputTextFlags_EnterReturnsTrue,
                        nullptr,
                        nullptr
                    ))
                {
                    pm.set_active_preset(i);
                    if (auto *p = pm.active_preset_mut())
                        p->name = s_rename_preset_buf;
                    pm.set_active_preset(active_index);
                    pm.save();
                    s_rename_active = false;
                    s_rename_index = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button(lang::t("common.ok", "OK"), ImVec2(0, 0)))
                {
                    pm.set_active_preset(i);
                    if (auto *p = pm.active_preset_mut())
                        p->name = s_rename_preset_buf;
                    pm.set_active_preset(active_index);
                    pm.save();
                    s_rename_active = false;
                    s_rename_index = -1;
                }
            }
            else
            {
                char label[128];
                std::snprintf(
                    label,
                    sizeof(label),
                    "%s [%d]##preset",
                    preset_list[static_cast<std::size_t>(i)].name.c_str(),
                    i
                );

                if (ImGui::Selectable(label, is_active, 0, ImVec2(0, 0)))
                {
                    // Tear down any active body-mesh prefab picks BEFORE switching presets. Otherwise the hook keeps
                    // substituting the old src wrappers while the new preset's items are being equipped, which produces
                    // stale visuals across the transition.
                    const auto had_pick = clear_all_picked_prefabs_and_deactivate();
                    pm.set_active_preset(i);
                    pm.apply_to_state();
                    // Post-apply_to_state reconciliation: for each slot where a body-mesh pick was just cleared AND the
                    // new preset's carrier equals last_applied (the early-out case), set the force-apply flag so the
                    // dispatcher bypasses the `targetId == prev_id` early-out while leaving last_ids intact. Phase A
                    // `tear_down_fake` then runs against the prior carrier and the natpipe-hook cleans up the prior
                    // body-mesh tgt wrapper. When carriers differ the regular tear_down_fake path already handles
                    // cleanup naturally, so the flag is harmless.
                    auto &last_ids = last_applied_ids();
                    auto &mods = slot_mappings();
                    for (std::size_t s = 0; s < SLOT_COUNT; ++s)
                    {
                        if (had_pick[s] && mods[s].target_item_id == last_ids[s])
                        {
                            force_apply_pending()[s] = true;
                        }
                    }
                    manual_apply();
                    pm.save();
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    s_rename_active = true;
                    s_rename_index = i;
                    std::snprintf(
                        s_rename_preset_buf,
                        sizeof(s_rename_preset_buf),
                        "%s",
                        preset_list[static_cast<std::size_t>(i)].name.c_str()
                    );
                }
            }

            ImGui::PopID();
        }

        // Read back what the drag settled on, in rows. ImGuiChildFlags_ResizeY makes ImGui own the height after the
        // first frame, so this reads what the user dragged to rather than what was requested above.
        const float dragged_rows = ImGui::GetWindowHeight() / preset_row_h;
        ImGui::EndChild();
        // Commit only a real drag: baseline at button-down, write when it differs at release. ImGui keeps the
        // child's PIXEL height while this reads rows, so a font or UI-scale change moves the row count on its own,
        // and IsMouseReleased fires for any left release anywhere. Comparing against the store would commit that.
        static float s_rows_at_press = -1.0f;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            s_rows_at_press = dragged_rows;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const bool dragged = s_rows_at_press > 0.0f && std::abs(dragged_rows - s_rows_at_press) > 0.01f;
            s_rows_at_press = -1.0f;
            if (dragged)
            {
                s_preset_rows = std::clamp(
                    dragged_rows,
                    PresetManager::UiPrefs::PRESET_ROWS_MIN,
                    PresetManager::UiPrefs::PRESET_ROWS_MAX
                );
                save_ui_prefs();
            }
        }

        if (count == 0)
            ui_text_disabled(lang::t("presets.empty", "No presets - use Append to create one"));

        ImGui::Spacing();

        if (ImGui::Button(lang::t("presets.append", "Append"), ImVec2(0, 0)))
        {
            pm.append_from_state();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip(
                lang::t(
                    "presets.append.tip",
                    "Append a new preset with Helm/Chest/Cloak/Gloves/Boots ticked + none (hides those five armor "
                    "pieces). Other slots stay unticked so items like the lantern still work. Then apply it."
                )
            );

        ImGui::SameLine();

        // Copy forks the active preset's saved state into a brand-new preset, ignoring any unsaved edits in
        // slot_mappings. Use to get a clean clone to start altering on. For forking the current pending edits, use
        // "Save as New" instead.
        if (ImGui::Button(lang::t("presets.copy", "Copy"), ImVec2(0, 0)))
        {
            pm.duplicate_current();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip(
                lang::t(
                    "presets.copy.tip",
                    "Clone the active preset's saved state into a new preset (pending edits are discarded). "
                    "Use to start altering a clean copy without touching the source."
                )
            );

        ImGui::SameLine();

        // Save as New forks the current pending state (slot_mappings + in-place dye/swatch) into a new preset, leaving
        // the active preset's saved rows untouched. Lets the user alter freely mid-edit and bottle the result up
        // without overwriting.
        if (ImGui::Button(lang::t("presets.save_as_new", "Save as New"), ImVec2(0, 0)))
        {
            pm.save_as_new_from_state();
            manual_apply();
        }
        if (ImGui::IsItemHovered())
            ui_tooltip(
                lang::t(
                    "presets.save_as_new.tip",
                    "Save the current pending state (including unsaved picks) as a new preset. "
                    "The active preset's saved rows are left unchanged."
                )
            );

        ImGui::SameLine();

        if (ImGui::Button(lang::t("presets.remove", "Remove"), ImVec2(0, 0)) && count > 0)
        {
            pm.remove_current();
            // remove_current auto-selects a neighbouring preset. Apply it so the visible outfit matches the new active
            // preset instead of leaving the old (now-deleted) one on screen.
            if (pm.preset_count() > 0)
                manual_apply();
            else
                manual_clear();
        }

        ImGui::SameLine();

        if (ImGui::Button(lang::t("presets.prev", "Prev"), ImVec2(0, 0)) && count > 1)
        {
            pm.prev_preset();
            manual_apply();
        }

        ImGui::SameLine();

        if (ImGui::Button(lang::t("presets.next", "Next"), ImVec2(0, 0)) && count > 1)
        {
            pm.next_preset();
            manual_apply();
        }

        if (count > 0)
            ui_text(lang::t("presets.active_count", "Active: %d / %d"), active_index + 1, count);

        ImGui::Separator();
    }

} // namespace Transmog
