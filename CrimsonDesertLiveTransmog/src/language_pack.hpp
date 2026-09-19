#ifndef TRANSMOG_LANGUAGE_PACK_HPP
#define TRANSMOG_LANGUAGE_PACK_HPP

#include <DetourModKit/error.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Transmog
{
    /**
     * @brief Reader for the generated `CrimsonDesertLiveTransmog_item_localization.pack` container.
     *
     * The authoritative generated source for localized item display names and the wearer-body classification.
     * `scripts/gen_language_pack.py` writes it from the packed game archives. Every block keys on the item INTERNAL
     * name, so a user edit to the loose display_names TSV overrides a row without shifting the join.
     *
     * A per-block index makes one locale cost one seek and one bounded read. `open()` holds only that index, and no
     * payload stays resident. An unknown kind is indexed and skipped, so a later generator can add a block without
     * a format version bump.
     *
     * Block kinds this reader understands:
     *   "body"   one payload of `internal_name<TAB>Male|Female` rows, locale independent.
     *   "names"  one payload per locale of `internal_name<TAB>display_name` rows.
     *   "search" the same rows in LOGICAL order, present only for a locale whose "names" block is pre-shaped.
     *
     * A locale is pre-shaped when its script needs contextual shaping and bidirectional reordering, which Dear ImGui
     * does neither of. Its "names" rows hold presentation forms in visual order, which no typed query can match, so
     * the "search" block carries the original text. The presence of a "search" block is the marker for that state.
     *
     * @note `[B-100]` Call `open()` and `read_block()` outside the loader lock. Both perform file I/O.
     * @warning Neither call is callback-safe. Both allocate and can wait on disk. Call them from setup or from a
     *          worker, never from a hook or a per-frame path.
     */
    class LanguagePack
    {
    public:
        /// Returns the process-wide pack instance.
        static LanguagePack &instance();

        /**
         * @brief One locale the pack carries, in the order the generator wrote it.
         */
        struct LocaleInfo
        {
            /// Archive locale tag, ASCII and lowercase, such as "zho-cn".
            std::string tag;
            /// Locale name in its own language, UTF-8, for direct display in the picker.
            std::string endonym;
        };

        /**
         * @brief Read and validate the container index at @p path.
         *
         * @param path Container path. Build it from the wide runtime directory so a non-ASCII install path resolves.
         * @return Empty on success. On failure the previous index stays published and the pack keeps its prior state.
         *
         * @details Failure codes, chosen from the existing DMK set rather than a new block:
         *          - `ErrorCode::FileOpenFailed` the file is absent, locked, or unreadable.
         *          - `ErrorCode::MissingHeader` the magic or the format version does not match.
         *          - `ErrorCode::MalformedLine` an index entry is truncated, or a declared size exceeds its bound.
         *
         *          Every size read from the file is bounded before it reaches an allocation, so a corrupt or hostile
         *          pack fails closed instead of reserving an arbitrary amount.
         */
        [[nodiscard]] DetourModKit::Result<void> open(const std::filesystem::path &path);

        /// Returns true once `open()` published a non-empty index.
        [[nodiscard]] bool ready() const noexcept;

        /**
         * @brief Locales the pack carries, for the picker combo.
         * @return A copy, because the caller reads it outside the lock that guards the index.
         */
        [[nodiscard]] std::vector<LocaleInfo> locales() const;

        /**
         * @brief Read one block payload and verify it.
         *
         * @param kind Block kind, "body", "names" or "search".
         * @param tag Locale tag for a "names" or "search" block. Pass an empty view for "body".
         * @return The decompressed payload, or `std::nullopt` on a miss or a failed verify.
         *
         * @details This is a best-effort query rather than a `Result`, because every caller degrades to the English
         *          baseline on a miss and needs no failure detail. The reason reaches the log instead. A size mismatch
         *          or a CRC mismatch returns `std::nullopt`, so corrupt bytes never reach the row parse.
         */
        [[nodiscard]] std::optional<std::string> read_block(std::string_view kind, std::string_view tag) const;

    private:
        LanguagePack() = default;

        /**
         * @brief One indexed block, without its payload.
         */
        struct BlockIndex
        {
            std::string kind;
            std::string tag;
            std::string endonym;
            std::uint32_t method{};
            std::uint64_t offset{};
            std::uint32_t packed_size{};
            std::uint32_t unpacked_size{};
            std::uint32_t crc32{};
        };

        std::filesystem::path m_path;
        std::vector<BlockIndex> m_blocks;

        /// ntdll entry points, re-resolved by every `open()`. Null when the pack holds only raw blocks.
        void *m_decompress_fn{nullptr};
        void *m_workspace_size_fn{nullptr};

        /// Guards every member above. It is the only lock this class takes, so there is no order to preserve.
        mutable std::mutex m_mutex;
    };

} // namespace Transmog

#endif // TRANSMOG_LANGUAGE_PACK_HPP
