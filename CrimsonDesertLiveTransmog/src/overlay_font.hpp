#ifndef TRANSMOG_OVERLAY_FONT_HPP
#define TRANSMOG_OVERLAY_FONT_HPP

#include <string>
#include <string_view>

namespace Transmog::overlay_font
{
    // Glyph coverage for the overlay, driven by the selected locale.
    //
    // ImGui 1.92 rasterizes on demand when the backend reports `ImGuiBackendFlags_RendererHasTextures`, which both
    // the bundled DirectX 11 backend and ReShade do, so coverage depends only on which font FILES the atlas carries.
    // `segoeui.ttf` alone covers 11 of the 15 locales, Arabic included because the pack pre-shapes it. Only Korean,
    // Japanese and the two Chinese locales need a second file.
    //
    // `add_host_font` serves both overlay modes: it builds ONE font carrying the base face plus, when the locale
    // needs one, a script face merged in AT CREATION. A later merge is ignored, because ImGui clears a font's
    // absent-glyph cache only when MergeMode is false.

    /**
     * @brief Install the base font into a freshly created atlas.
     * @param size_pixels Rasterization size the overlay computed from its DPI scale.
     * @details Falls back to ImGui's built-in bitmap font when no candidate resolves, and logs which file won.
     */
    void install_base_font(float size_pixels);

    /**
     * @brief Override path for the base font, from the INI.
     * @details An empty value restores the built-in candidate order. Set before `install_base_font`.
     */
    void set_font_path_override(std::string path);

    /**
     * @brief Counter that advances whenever the font configuration changes.
     * @details Lets a caller holding cached fonts notice an INI reload and rebuild them, so editing FontPath applies
     *          without a restart. Thread-safe.
     */
    [[nodiscard]] unsigned config_generation() noexcept;

    /**
     * @brief Representative codepoint that proves @p locale_tag is renderable.
     * @return The probe codepoint, or 0 when the base font already covers the locale.
     *
     * @details Deliberately returns DATA rather than performing the glyph test here. This translation unit compiles
     *          against the real ImGui, so its `ImGui::` calls bind to LT's own context, which exists only in the
     *          standalone overlay. Under ReShade that context is never created and any call here dereferences null.
     *          The caller lives in the overlay_ui target, which compiles against the ReShade SDK and routes through
     *          the addon function table, so it can run `ImFont::IsGlyphInFont` safely in BOTH modes.
     */
    [[nodiscard]] char32_t script_probe_codepoint(std::string_view locale_tag) noexcept;

    /**
     * @brief A font atlas owned by another module, with that module's ImGui allocator.
     * @details Passed as `void *` so this header stays free of ImGui. The overlay_ui target obtains the members
     *          through the ReShade addon function table and hands them here, where the real ImGui lives.
     */
    struct ForeignAtlas
    {
        /// The host's `ImFontAtlas *`.
        void *atlas = nullptr;
        /// The host's `ImGuiMemAllocFunc`.
        void *alloc_fn = nullptr;
        /// The host's `ImGuiMemFreeFunc`.
        void *free_fn = nullptr;
        /// User data the host's allocator pair expects.
        void *alloc_user = nullptr;
    };

    /**
     * @brief Add a font covering @p locale_tag to an atlas another module owns.
     * @return The new `ImFont *` as an opaque pointer, or nullptr. Push it around this mod's own widgets.
     *
     * @details This is how the ReShade overlay gains CJK coverage without a restart. ReShade exports no font API,
     *          but it does expose its atlas through `ImGui::GetIO()`, and this module links its own ImGui built from
     *          the tag the addon function table pins, so the struct layout matches. ReShade also sets
     *          `ImGuiBackendFlags_RendererHasTextures`, so ImGui never locks the atlas and rasterizes the new glyphs
     *          on demand.
     *
     *          The merged source inherits the destination font's reference size, so the overlay does not change size.
     *
     * @warning The host frees this font data with its own allocator when it clears the atlas, so the call first
     *          points this module's ImGui at the host allocator. A mismatched pair corrupts the heap.
     * @note The host may clear its atlas at any time, which drops the merged font. Callers must detect the loss and
     *       call again rather than assume one call is permanent.
     */
    [[nodiscard]] void *
    add_host_font(const ForeignAtlas &target, std::string_view names_locale, std::string_view ui_locale, float size_px);

    /**
     * @brief Check that a font this mod added is still present in the host atlas.
     * @return false once the host has cleared its atlas, which destroys every font in it.
     * @details The host rebuilds its atlas on its own schedule, and `ImFontAtlas::Clear` deletes the fonts. A cached
     *          pointer is dangling from that moment, so callers must re-check before every use rather than assume
     *          one successful add lasts the session.
     */
    [[nodiscard]] bool host_font_is_live(const ForeignAtlas &target, void *font) noexcept;

    /**
     * @brief Scale every size in an `ImGuiStyle` by @p factor.
     * @param style The host's `ImGuiStyle *`.
     *
     * @details Padding, spacing, rounding and border sizes are independent of font size, so scaling the font alone
     *          leaves a small font stranded in full-size rows and a large one cramped. `ImGuiStyle::ScaleAllSizes`
     *          is not in the ReShade addon function table, so the call lives here where the real ImGui is linked.
     *
     * @warning It mutates the host's live style. The caller must restore a by-value copy afterwards, because the
     *          scaling is cumulative and would compound every frame.
     */
    void scale_style_sizes(void *style, float factor) noexcept;

    /// File name of the base font in use, or "built-in" when no file resolved. Diagnostic only.
    [[nodiscard]] std::string base_font_name();

} // namespace Transmog::overlay_font

#endif // TRANSMOG_OVERLAY_FONT_HPP
