#ifndef TRANSMOG_LANG_HPP
#define TRANSMOG_LANG_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Transmog::lang
{
    // Interface-text lookup for the overlay. Every call site names a STABLE KEY and carries its English text
    // inline:
    //
    //     ImGui::Button(lang::t("footer.apply_all", "Apply All"));
    //
    // The key is what the translation file is keyed by, so rewording the English does not silently drop a
    // translation. The inline English is the fallback and stays the authority on what the string says: the
    // translations file ships in the optional language pack, so most installs never have one and the mod has to
    // read correctly with nothing loaded at all.
    //
    // Item NAMES are not translated here. Those come from the game's own localization tables through the language
    // pack, which needs no translator. This covers the mod's own interface text only.

    /// Tag that means "use whatever language the item names are in".
    inline constexpr std::string_view FOLLOW_ITEM_NAMES = "auto";

    /**
     * @brief Load the table for the effective interface locale.
     *
     * @param json_path Translations file. Build it from the wide runtime directory so a non-ASCII install path
     *        resolves.
     * @param interface_tag The stored interface preference. @ref FOLLOW_ITEM_NAMES, or empty, defers to
     *        @p item_names_tag.
     * @param item_names_tag Locale the item names are loaded in.
     * @return The number of strings published. Zero leaves every lookup on its inline English.
     *
     * @details The two are separate settings because they answer different questions: item names come from the
     *          game's own tables and exist in all 15 locales, while the interface exists only in the languages
     *          someone has translated. Following by default keeps the common case to one choice.
     *
     *          It reads and parses the file, so expect one hitch. It is safe on the render thread even so: a reload
     *          retires the table it replaces rather than freeing it, so a pointer @ref t already handed out this
     *          frame stays valid.
     */
    std::size_t
    load_for(const std::filesystem::path &json_path, std::string_view interface_tag, std::string_view item_names_tag);

    /**
     * @brief The locale the interface is actually drawn in.
     * @param interface_tag The stored preference. @ref FOLLOW_ITEM_NAMES, or empty, defers to @p item_names_tag.
     * @param item_names_tag Locale the item names are loaded in.
     * @details The font setup needs this as well as the loader does: the overlay draws item names and interface
     *          text side by side, so the font it builds has to cover both, and they are not always the same.
     *
     * @warning The result views one of the arguments. Keep both alive for as long as the result, or copy it.
     */
    [[nodiscard]] std::string_view
    effective_locale(std::string_view interface_tag, std::string_view item_names_tag) noexcept;

    /**
     * @brief Translate one interface string.
     *
     * @param key Stable lookup key, such as "footer.apply_all". Never shown to the user.
     * @param english The English text, which is also the fallback. Pass a string LITERAL.
     * @return The translation, or @p english unchanged when there is none.
     *
     * @details An ImGui id suffix in @p english (from the first `##`) is stripped before the lookup and
     *          re-appended to the result, so `"Dye##dye_btn"` is translated as `"Dye"` and returns
     *          `"<translated>##dye_btn"`. A translator never sees an id, and renaming one keeps its translation.
     *
     *          A translation whose printf conversions do not match @p english is REJECTED once, with a warning:
     *          these strings reach `snprintf`, where an added or dropped `%s` takes the process down.
     *
     *          The returned pointer stays valid for the life of the process, because a reload retires the old
     *          table rather than freeing it. Neither a hit nor a miss allocates, which is what makes this usable
     *          directly in a per-frame ImGui call. A failure while reconciling leaves the entry untranslated and
     *          returns @p english, so nothing propagates out of a render callback.
     *
     * @warning Pass a string LITERAL for @p english, never a temporary. A miss returns the pointer it was given.
     */
    [[nodiscard]] const char *t(const char *key, const char *english) noexcept;

    /**
     * @brief One locale the translations file offers.
     */
    struct LocaleInfo
    {
        /// Archive locale tag, such as "zho-cn".
        std::string tag;
        /// Locale name in its own language, for direct display.
        std::string endonym;
    };

    /**
     * @brief Locales the translations file carries, for the interface-language picker.
     * @return A copy. Empty when the file is absent or unreadable.
     * @note Call it off the render thread when possible. It reads and parses the file on every call.
     */
    [[nodiscard]] std::vector<LocaleInfo> available_locales(const std::filesystem::path &json_path);

} // namespace Transmog::lang

#endif // TRANSMOG_LANG_HPP
