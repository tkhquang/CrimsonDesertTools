#include "language_pack.hpp"

#include <DetourModKit/defines.hpp>
#include <DetourModKit/logger.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <fstream>
#include <utility>

#include <Windows.h>

namespace Transmog
{
    namespace
    {
        using DetourModKit::Error;
        using DetourModKit::ErrorCode;
        using DetourModKit::Result;

        constexpr std::array<char, 8> PACK_MAGIC{'C', 'D', 'L', 'T', 'L', 'A', 'N', 'G'};
        constexpr std::uint32_t PACK_FORMAT_VERSION = 1;

        constexpr std::uint32_t METHOD_RAW = 0;
        constexpr std::uint32_t METHOD_XPRESS_HUFF = 1;

        // ntdll format selector. It must match what gen_language_pack.py compresses with, engine bit included.
        constexpr USHORT NT_XPRESS_HUFF_MAX = 0x0004 | 0x0100;

        // Every bound below exists so a truncated or hostile pack fails closed instead of reserving an arbitrary
        // amount. The shipped pack uses 16 blocks, strings under 24 bytes, and a largest block near 500 KB.
        constexpr std::size_t MAX_BLOCKS = 64;
        constexpr std::uint32_t MAX_STRING_BYTES = 256;
        constexpr std::uint32_t MAX_UNPACKED_BYTES = 32u * 1024u * 1024u;
        constexpr std::size_t HEADER_SCAN_CAP = 128u * 1024u;

        using RtlGetCompressionWorkSpaceSizeFn = LONG(WINAPI *)(USHORT, PULONG, PULONG);
        using RtlDecompressBufferExFn = LONG(WINAPI *)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);

        /**
         * @brief CRC-32 table for the reflected 0xEDB88320 polynomial.
         * @details The generator writes `zlib.crc32`, so this reader must use the same model. A different model
         *          rejects every block, which reads as a corrupt pack rather than as a mismatch.
         */
        constexpr std::array<std::uint32_t, 256> build_crc_table() noexcept
        {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t value = i;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
                table[i] = value;
            }
            return table;
        }

        constexpr std::array<std::uint32_t, 256> CRC_TABLE = build_crc_table();

        /// Computes the CRC-32 of @p data under the same model the generator uses.
        [[nodiscard]] std::uint32_t crc32_of(std::string_view data) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const char c : data)
            {
                const auto index = static_cast<std::uint8_t>((crc ^ static_cast<std::uint8_t>(c)) & 0xFFu);
                crc = CRC_TABLE[index] ^ (crc >> 8);
            }
            return crc ^ 0xFFFFFFFFu;
        }

        /**
         * @brief Bounds-checked forward cursor over the header bytes.
         * @details Every read checks the remaining span first, so a truncated header fails on the read that runs past
         *          the end rather than through an out-of-range index.
         */
        class Cursor
        {
        public:
            explicit Cursor(std::string_view bytes) noexcept : m_bytes(bytes) {}

            [[nodiscard]] bool read_u32(std::uint32_t &out) noexcept
            {
                if (m_bytes.size() - m_offset < sizeof(std::uint32_t))
                    return false;
                std::memcpy(&out, m_bytes.data() + m_offset, sizeof(out));
                m_offset += sizeof(out);
                return true;
            }

            [[nodiscard]] bool read_u64(std::uint64_t &out) noexcept
            {
                if (m_bytes.size() - m_offset < sizeof(std::uint64_t))
                    return false;
                std::memcpy(&out, m_bytes.data() + m_offset, sizeof(out));
                m_offset += sizeof(out);
                return true;
            }

            /// Reads a u32 length prefix and its bytes, refusing a length past `MAX_STRING_BYTES`.
            [[nodiscard]] bool read_string(std::string &out)
            {
                std::uint32_t length = 0;
                if (!read_u32(length) || length > MAX_STRING_BYTES)
                    return false;
                if (m_bytes.size() - m_offset < length)
                    return false;
                out.assign(m_bytes.data() + m_offset, length);
                m_offset += length;
                return true;
            }

            [[nodiscard]] bool skip(std::size_t count) noexcept
            {
                if (m_bytes.size() - m_offset < count)
                    return false;
                m_offset += count;
                return true;
            }

        private:
            std::string_view m_bytes;
            std::size_t m_offset{0};
        };

        /**
         * @brief Expand one XPRESS_HUFF payload through ntdll.
         * @param workspace_size_fn_raw `RtlGetCompressionWorkSpaceSize`, or null when ntdll did not resolve.
         * @param decompress_fn_raw `RtlDecompressBufferEx`, or null when ntdll did not resolve.
         * @param packed The compressed bytes. ntdll declares the parameter writable but never writes it.
         * @param unpacked_size Size the index records, already bounded by `MAX_UNPACKED_BYTES`.
         * @param out Receives the expanded payload.
         * @return True when the expansion produced exactly @p unpacked_size bytes.
         */
        [[nodiscard]] bool decompress_block(
            void *workspace_size_fn_raw,
            void *decompress_fn_raw,
            std::string &packed,
            std::uint32_t unpacked_size,
            std::string &out
        )
        {
            if (workspace_size_fn_raw == nullptr || decompress_fn_raw == nullptr)
                return false;

            const auto workspace_size_fn = reinterpret_cast<RtlGetCompressionWorkSpaceSizeFn>(workspace_size_fn_raw);
            const auto decompress_fn = reinterpret_cast<RtlDecompressBufferExFn>(decompress_fn_raw);

            ULONG workspace_bytes = 0;
            ULONG fragment_bytes = 0;
            if (workspace_size_fn(NT_XPRESS_HUFF_MAX, &workspace_bytes, &fragment_bytes) != 0)
                return false;

            std::vector<std::uint8_t> workspace(workspace_bytes);
            out.assign(unpacked_size, '\0');
            ULONG produced = 0;
            const LONG status = decompress_fn(
                NT_XPRESS_HUFF_MAX,
                reinterpret_cast<PUCHAR>(out.data()),
                unpacked_size,
                reinterpret_cast<PUCHAR>(packed.data()),
                static_cast<ULONG>(packed.size()),
                &produced,
                workspace.data()
            );
            // A short expansion means the block disagrees with its index. Treat it as corrupt, not as a truncation.
            return status == 0 && produced == unpacked_size;
        }

        [[nodiscard]] Error make_error(ErrorCode code, std::uintptr_t detail = 0) noexcept
        {
            return Error{
                .code = code,
                .where = "language_pack",
                .detail = detail,
                .extra = 0,
            };
        }
    } // namespace

    LanguagePack &LanguagePack::instance()
    {
        static LanguagePack s_instance;
        return s_instance;
    }

    Result<void> LanguagePack::open(const std::filesystem::path &path)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file.is_open())
            return std::unexpected(make_error(ErrorCode::FileOpenFailed));

        file.seekg(0, std::ios::end);
        const std::streamoff file_size = file.tellg();
        if (file_size <= 0)
            return std::unexpected(make_error(ErrorCode::FileOpenFailed));
        file.seekg(0, std::ios::beg);

        // The index is bounded by MAX_BLOCKS and MAX_STRING_BYTES, so one capped read always covers it.
        const auto scan_bytes = static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(file_size),
                                                                    static_cast<std::uint64_t>(HEADER_SCAN_CAP)));
        std::string head(scan_bytes, '\0');
        file.read(head.data(), static_cast<std::streamsize>(scan_bytes));
        if (file.gcount() != static_cast<std::streamsize>(scan_bytes))
            return std::unexpected(make_error(ErrorCode::FileOpenFailed));

        Cursor cursor{head};
        if (scan_bytes < PACK_MAGIC.size() || std::memcmp(head.data(), PACK_MAGIC.data(), PACK_MAGIC.size()) != 0)
            return std::unexpected(make_error(ErrorCode::MissingHeader));
        if (!cursor.skip(PACK_MAGIC.size()))
            return std::unexpected(make_error(ErrorCode::MissingHeader));

        std::uint32_t format_version = 0;
        if (!cursor.read_u32(format_version))
            return std::unexpected(make_error(ErrorCode::MissingHeader));
        // Refuse a newer container outright. A partial read of an unknown layout produces plausible rows, and a
        // plausible wrong name is worse than an English fallback.
        if (format_version != PACK_FORMAT_VERSION)
            return std::unexpected(make_error(ErrorCode::MissingHeader, format_version));

        std::string game_version;
        if (!cursor.read_string(game_version))
            return std::unexpected(make_error(ErrorCode::MalformedLine));

        std::uint32_t block_count = 0;
        if (!cursor.read_u32(block_count) || block_count > MAX_BLOCKS)
            return std::unexpected(make_error(ErrorCode::MalformedLine, block_count));

        std::vector<BlockIndex> blocks;
        blocks.reserve(block_count);
        for (std::uint32_t i = 0; i < block_count; ++i)
        {
            BlockIndex block;
            if (!cursor.read_string(block.kind) || !cursor.read_string(block.tag) || !cursor.read_string(block.endonym))
            {
                return std::unexpected(make_error(ErrorCode::MalformedLine, i));
            }
            if (!cursor.read_u32(block.method) || !cursor.read_u64(block.offset) ||
                !cursor.read_u32(block.packed_size) || !cursor.read_u32(block.unpacked_size) ||
                !cursor.read_u32(block.crc32))
            {
                return std::unexpected(make_error(ErrorCode::MalformedLine, i));
            }
            // Check each payload span against the real file length here, so read_block never seeks past the end.
            const std::uint64_t end = block.offset + block.packed_size;
            if (block.unpacked_size > MAX_UNPACKED_BYTES || end > static_cast<std::uint64_t>(file_size) ||
                end < block.offset)
            {
                return std::unexpected(make_error(ErrorCode::MalformedLine, i));
            }
            blocks.push_back(std::move(block));
        }

        void *workspace_size_fn = nullptr;
        void *decompress_fn = nullptr;
        if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr)
        {
            workspace_size_fn = reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize"));
            decompress_fn = reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlDecompressBufferEx"));
        }

        auto &logger = DMK::log();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_path = path;
            m_blocks = std::move(blocks);
            m_workspace_size_fn = workspace_size_fn;
            m_decompress_fn = decompress_fn;
        }

        logger.info("[langpack] indexed {} blocks, game '{}'", block_count, game_version);
        if (decompress_fn == nullptr || workspace_size_fn == nullptr)
            logger.warning("[langpack] ntdll decompression unavailable; only raw blocks can load");
        return {};
    }

    bool LanguagePack::ready() const noexcept
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return !m_blocks.empty();
    }

    std::vector<LanguagePack::LocaleInfo> LanguagePack::locales() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<LocaleInfo> out;
        for (const auto &block : m_blocks)
        {
            if (block.kind == "names")
                out.push_back(LocaleInfo{block.tag, block.endonym});
        }
        return out;
    }

    std::optional<std::string> LanguagePack::read_block(std::string_view kind, std::string_view tag) const
    {
        auto &logger = DMK::log();

        BlockIndex found;
        std::filesystem::path path;
        void *workspace_size_fn = nullptr;
        void *decompress_fn = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            const auto it = std::find_if(
                m_blocks.begin(),
                m_blocks.end(),
                [&](const BlockIndex &block) { return block.kind == kind && block.tag == tag; }
            );
            if (it == m_blocks.end())
                return std::nullopt;
            found = *it;
            path = m_path;
            workspace_size_fn = m_workspace_size_fn;
            decompress_fn = m_decompress_fn;
        }

        std::ifstream file{path, std::ios::binary};
        if (!file.is_open())
        {
            logger.warning("[langpack] '{}/{}' unreadable: the pack moved after open()", kind, tag);
            return std::nullopt;
        }

        file.seekg(static_cast<std::streamoff>(found.offset), std::ios::beg);
        std::string packed(found.packed_size, '\0');
        file.read(packed.data(), static_cast<std::streamsize>(found.packed_size));
        if (file.gcount() != static_cast<std::streamsize>(found.packed_size))
        {
            logger.warning("[langpack] '{}/{}' truncated at offset {}", kind, tag, found.offset);
            return std::nullopt;
        }

        std::string payload;
        if (found.method == METHOD_RAW)
            payload = std::move(packed);
        else if (found.method == METHOD_XPRESS_HUFF)
        {
            if (!decompress_block(workspace_size_fn, decompress_fn, packed, found.unpacked_size, payload))
            {
                logger.warning("[langpack] '{}/{}' failed to decompress", kind, tag);
                return std::nullopt;
            }
        }
        else
        {
            logger.warning("[langpack] '{}/{}' uses unknown method {}", kind, tag, found.method);
            return std::nullopt;
        }

        if (payload.size() != found.unpacked_size)
        {
            logger.warning(
                "[langpack] '{}/{}' size {} disagrees with the index value {}",
                kind,
                tag,
                payload.size(),
                found.unpacked_size
            );
            return std::nullopt;
        }
        // The CRC is the one check that catches a payload that survives every structural test and still holds wrong
        // bytes. Corrupt rows would otherwise reach the picker as plausible names.
        if (const std::uint32_t actual = crc32_of(payload); actual != found.crc32)
        {
            logger.warning("[langpack] '{}/{}' CRC {:#010x} expected {:#010x}", kind, tag, actual, found.crc32);
            return std::nullopt;
        }

        return payload;
    }

} // namespace Transmog
