#include "overlay_font.hpp"

#include "shared_state.hpp"

#include <DetourModKit/defines.hpp>
#include <DetourModKit/logger.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#pragma warning(pop)

#include <Windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>

namespace Transmog::overlay_font
{
    namespace
    {
        /**
         * @brief One locale's font requirement and its coverage probe.
         */
        struct LocaleFont
        {
            /// Archive locale tag.
            std::string_view tag;
            /// Script font file names, newest preference first. Empty when the base font already covers the locale.
            std::array<std::string_view, 2> candidates;
            /// Representative codepoint the base font lacks. Zero when the base font covers the locale.
            char32_t probe;
        };

        // segoeui.ttf covers eng, fre, ger, ita, both spa, por-br, pol, tur, rus, and ara, the last because the
        // pack pre-shapes Arabic into presentation forms that segoeui carries in full. The four remaining locales
        // each take their own regional font, so kanji and hanzi keep the shapes their readers expect.
        //
        // Every primary candidate below belongs to the Windows recommended font set, which ships on all desktop
        // editions regardless of installed language: msyh, msjh, malgun, YuGothR and msgothic are all present on an
        // en-US install with no CJK language pack. The Features-on-Demand supplemental fonts are not: meiryo,
        // mingliu, gulim and batang are all absent there. Never make a supplemental font a primary candidate.
        //
        // Only the four CJK locales need a face the base font lacks. The other four are listed for their PROBE,
        // which reports whether the locale can render at all, and their candidates repeat the base list so the
        // merge loop skips them whenever the base already resolved to the same file.
        constexpr std::array<LocaleFont, 8> SCRIPT_FONTS{
            LocaleFont{"zho-cn", {"msyh.ttc", "simsun.ttc"}, U'\u7b80'},
            LocaleFont{"zho-tw", {"msjh.ttc", "simsun.ttc"}, U'\u7e41'},
            LocaleFont{"jpn", {"YuGothR.ttc", "msgothic.ttc"}, U'\u3042'},
            LocaleFont{"kor", {"malgun.ttf"}, U'\uD55C'},
            // Probe an Arabic PRESENTATION form, not a nominal letter: the pack stores Arabic pre-shaped, so those
            // are the codepoints that actually have to render.
            LocaleFont{"ara", {"segoeui.ttf", "tahoma.ttf"}, U'\ufeb3'},
            LocaleFont{"rus", {"segoeui.ttf", "arial.ttf"}, U'\u0420'},
            LocaleFont{"pol", {"segoeui.ttf", "arial.ttf"}, U'\u0142'},
            LocaleFont{"tur", {"segoeui.ttf", "arial.ttf"}, U'\u011f'},
        };

        // Base font candidates, best first. Each covers Latin Extended-A, Cyrillic and Arabic presentation forms,
        // which ImGui's built-in bitmap font does not: it stops at U+00FF and draws everything past it as '?'.
        constexpr std::array<std::string_view, 4> BASE_FONTS{
            "segoeui.ttf",
            "arial.ttf",
            "tahoma.ttf",
            "micross.ttf",
        };

        std::mutex s_mutex;
        std::string s_font_path_override;
        std::atomic<unsigned> s_config_generation{0};
        std::string s_base_font_name{"built-in"};

        /// Absolute path of @p file_name inside the Windows font directory.
        [[nodiscard]] std::filesystem::path system_font(std::string_view file_name)
        {
            wchar_t buffer[MAX_PATH]{};
            const UINT written = GetWindowsDirectoryW(buffer, MAX_PATH);
            if (written == 0 || written >= MAX_PATH)
                return {};
            return std::filesystem::path{buffer} / L"Fonts" / std::filesystem::path{file_name};
        }

        /// First candidate that exists on disk, or an empty path.
        [[nodiscard]] std::filesystem::path first_present(const auto &candidates)
        {
            for (const std::string_view name : candidates)
            {
                if (name.empty())
                    continue;
                std::error_code ec;
                if (auto path = system_font(name); std::filesystem::exists(path, ec))
                    return path;
            }
            return {};
        }

        /**
         * @brief Resolves the INI font override, if one is set and present on disk.
         * @param rejected Receives the configured value when it names a file that does not resolve.
         * @return The override path, or an empty path when none applies.
         */
        [[nodiscard]] std::filesystem::path configured_override(std::string &rejected)
        {
            std::string configured;
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                configured = s_font_path_override;
            }
            if (configured.empty())
                return {};

            std::error_code ec;
            if (std::filesystem::path candidate{configured}; std::filesystem::exists(candidate, ec))
                return candidate;
            rejected = std::move(configured);
            return {};
        }

        [[nodiscard]] const LocaleFont *script_for(std::string_view locale_tag) noexcept
        {
            for (const auto &entry : SCRIPT_FONTS)
            {
                if (entry.tag == locale_tag)
                    return &entry;
            }
            return nullptr;
        }
    } // namespace

    void set_font_path_override(std::string path)
    {
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (s_font_path_override == path)
                return;
            s_font_path_override = std::move(path);
        }
        // Publish after the value lands, so a reader that sees the new generation also sees the new path.
        s_config_generation.fetch_add(1, std::memory_order_release);
    }

    unsigned config_generation() noexcept
    {
        return s_config_generation.load(std::memory_order_acquire);
    }

    std::string base_font_name()
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_base_font_name;
    }

    void install_base_font(float size_pixels)
    {
        auto &logger = DMK::log();

        std::string rejected_override;
        std::filesystem::path chosen = configured_override(rejected_override);
        if (!rejected_override.empty())
            logger.warning("[font] FontPath '{}' not found; falling back", rejected_override);
        if (chosen.empty())
            chosen = first_present(BASE_FONTS);

        ImFontConfig cfg;
        cfg.SizePixels = size_pixels;
        if (!chosen.empty())
        {
            const std::string utf8 = to_utf8(chosen);
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8.c_str(), size_pixels, &cfg) != nullptr)
            {
                const std::string name = to_utf8(chosen.filename());
                {
                    std::lock_guard<std::mutex> lk(s_mutex);
                    s_base_font_name = name;
                }
                logger.info("[font] base font '{}' at {:.1f}px", name, size_pixels);
                return;
            }
            logger.warning("[font] '{}' failed to load; falling back to the built-in font", utf8);
        }

        // The built-in bitmap font stops at U+00FF, so every non-Latin locale draws as '?'. Say so once here rather
        // than leave the user to discover it in the picker.
        ImGui::GetIO().Fonts->AddFontDefault(&cfg);
        logger.warning("[font] no system font resolved; non-Latin item names will not render");
    }

    bool host_font_is_live(const ForeignAtlas &target, void *font) noexcept
    {
        if (target.atlas == nullptr || font == nullptr)
            return false;
        const auto *const atlas = static_cast<const ImFontAtlas *>(target.atlas);
        for (const ImFont *const candidate : atlas->Fonts)
        {
            if (candidate == font)
                return true;
        }
        return false;
    }

    void scale_style_sizes(void *style, float factor) noexcept
    {
        if (style == nullptr || factor <= 0.0f)
            return;
        static_cast<ImGuiStyle *>(style)->ScaleAllSizes(factor);
    }

    void *
    add_host_font(const ForeignAtlas &target, std::string_view names_locale, std::string_view ui_locale, float size_px)
    {
        auto &logger = DMK::log();
        if (target.atlas == nullptr || size_px <= 0.0f)
            return nullptr;

        // One base font for EVERY locale, so this mod's widgets keep one set of metrics. A different file per
        // locale changes the apparent size of the whole tab on each switch, because two fonts at the same pixel
        // size do not occupy the same space.
        //
        // The INI override wins here exactly as it does for the standalone overlay, so one setting covers both.
        std::string rejected_override;
        std::filesystem::path base = configured_override(rejected_override);
        if (!rejected_override.empty())
            logger.warning("[font] FontPath '{}' not found; falling back", rejected_override);
        if (base.empty())
            base = first_present(BASE_FONTS);
        if (base.empty())
        {
            logger.warning("[font] no base font available for the host atlas");
            return nullptr;
        }

        // An atlas with no font at all is a host that has not finished building one. A live frame always has one,
        // so this only fires on a caller outside the draw path, where adding is premature.
        auto *const atlas = static_cast<ImFontAtlas *>(target.atlas);
        if (atlas->Fonts.Size == 0)
        {
            logger.warning("[font] the host atlas carries no font yet");
            return nullptr;
        }

        // Point this module's ImGui at the host allocator BEFORE the load. AddFont copies the font data with
        // IM_ALLOC here, and the host releases it with its own IM_FREE when it clears the atlas.
        if (target.alloc_fn != nullptr && target.free_fn != nullptr)
        {
            ImGui::SetAllocatorFunctions(
                reinterpret_cast<ImGuiMemAllocFunc>(target.alloc_fn),
                reinterpret_cast<ImGuiMemFreeFunc>(target.free_fn),
                target.alloc_user
            );
        }

        // A font of this mod's own, never a merge into the host's. ImGui clears a font's baked data, including its
        // cache of absent glyphs, only when MergeMode is false: see ImFontAtlasFontSourceAddToFont. A source merged
        // into the host's already-baked font is therefore never consulted for a codepoint it had recorded as
        // missing, which is every glyph this mod needs. Merging into a font created in the same call has no such
        // cache to defeat, because both sources land before the first bake.
        ImFontConfig cfg;
        cfg.PixelSnapH = true;
        cfg.SizePixels = size_px;
        const std::string base_utf8 = to_utf8(base);
        ImFont *const font = atlas->AddFontFromFileTTF(base_utf8.c_str(), size_px, &cfg);
        if (font == nullptr)
        {
            logger.warning("[font] base '{}' failed to load into the host atlas", base_utf8);
            return nullptr;
        }

        // A script face for EACH locale on screen. Item names and interface text are separate settings, so
        // an English interface over Chinese item names, or the reverse, both have to render. Merging is
        // additive, and the two usually resolve to the same file, which is skipped rather than merged twice.
        std::string merged{"none"};
        std::filesystem::path already_merged;
        for (const std::string_view locale_tag : {names_locale, ui_locale})
        {
            const LocaleFont *const entry = script_for(locale_tag);
            if (entry == nullptr)
                continue; // The base face already covers this locale.

            const std::filesystem::path script = first_present(entry->candidates);
            if (script.empty())
            {
                logger.warning("[font] no system font for locale '{}'", locale_tag);
                continue;
            }
            if (script == base || script == already_merged)
                continue;

            ImFontConfig merge_cfg;
            merge_cfg.PixelSnapH = true;
            merge_cfg.SizePixels = size_px;
            merge_cfg.MergeMode = true;
            merge_cfg.DstFont = font;
            const std::string script_utf8 = to_utf8(script);
            if (atlas->AddFontFromFileTTF(script_utf8.c_str(), size_px, &merge_cfg) == nullptr)
            {
                logger.warning("[font] script '{}' failed to merge", script_utf8);
                continue;
            }
            already_merged = script;
            const std::string name = to_utf8(script.filename());
            merged = (merged == "none") ? name : merged + " + " + name;
        }

        const std::string base_name = to_utf8(base.filename());
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_base_font_name = base_name;
        }
        logger.info(
            "[font] host font for names '{}' / ui '{}': base '{}' + script '{}' at {:.1f}px",
            names_locale,
            ui_locale,
            base_name,
            merged,
            size_px
        );
        return font;
    }

    char32_t script_probe_codepoint(std::string_view locale_tag) noexcept
    {
        const LocaleFont *entry = script_for(locale_tag);
        return entry != nullptr ? entry->probe : 0;
    }

} // namespace Transmog::overlay_font
