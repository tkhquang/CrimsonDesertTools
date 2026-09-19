#include "lang.hpp"

#include "shared_state.hpp"

#include <DetourModKit/defines.hpp>
#include <DetourModKit/logger.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Transmog::lang
{
    namespace
    {
        using json = nlohmann::json;

        /// Hash that accepts any string-like key, so a lookup by `const char *` does not build a std::string.
        struct StringHash
        {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept
            {
                return std::hash<std::string_view>{}(key);
            }
        };

        /**
         * @brief One translated string, and whether it has been reconciled with its call site yet.
         * @details The ImGui id suffix and the printf conversions both live in the English text at the call site,
         *          not in the file, so neither can be checked at load time. Both are handled on the first lookup
         *          and the result is stored back here, which keeps every later frame to a single map lookup.
         */
        struct Entry
        {
            std::string text;
            bool prepared = false;
        };

        using Table = std::unordered_map<std::string, Entry, StringHash, std::equal_to<>>;

        std::mutex s_mutex;
        Table s_strings;
        /**
         * @brief Tables a reload replaced, kept alive rather than destroyed.
         * @details `t()` hands back a pointer INTO the table, which is what lets a per-frame ImGui call use it
         *          without a copy. Freeing a replaced table would dangle every pointer handed out from it, so a
         *          reload retires instead. The cost is one table per locale switch, a few KB, for the process life.
         */
        std::vector<Table> s_retired;

        /// Parse the file, or return a null json on any failure. Every caller treats that as "no translations".
        [[nodiscard]] json read_file(const std::filesystem::path &path)
        {
            std::ifstream file{path, std::ios::binary};
            if (!file.is_open())
                return {};
            // Non-throwing parse: a hand-edited file with a stray comma must leave the UI in English, not take the
            // process down from inside an overlay callback.
            return json::parse(file, nullptr, false);
        }

        /// Offset of the ImGui id suffix in @p text, or npos when it carries none.
        [[nodiscard]] std::size_t id_suffix_at(std::string_view text) noexcept
        {
            return text.find("##");
        }

        /**
         * @brief Appends the printf conversions of @p text, in order, to @p out.
         * @details Only the conversion shape matters, so flags, width and precision are skipped rather than
         *          recorded. `%%` is a literal percent and is deliberately included: a translation that turns one
         *          into a real conversion has changed the argument count.
         */
        void collect_conversions(std::string_view text, std::string &out)
        {
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != '%')
                    continue;
                ++i;
                while (i < text.size() && std::string_view{"-+ #0"}.find(text[i]) != std::string_view::npos)
                    ++i;
                while (i < text.size() && (text[i] == '*' || (text[i] >= '0' && text[i] <= '9')))
                    ++i;
                if (i < text.size() && text[i] == '.')
                {
                    ++i;
                    while (i < text.size() && (text[i] == '*' || (text[i] >= '0' && text[i] <= '9')))
                        ++i;
                }
                while (i < text.size() && std::string_view{"hljztL"}.find(text[i]) != std::string_view::npos)
                    ++i;
                if (i < text.size())
                    out.push_back(text[i]);
            }
        }

        /// True when @p translated carries the same printf conversions, in the same order, as @p english.
        [[nodiscard]] bool conversions_match(std::string_view english, std::string_view translated)
        {
            std::string a, b;
            collect_conversions(english, a);
            collect_conversions(translated, b);
            return a == b;
        }

        /**
         * @brief Publish the translation table for one locale.
         * @param json_path Translations file.
         * @param locale_tag Archive locale tag. An empty tag, or one the file does not carry, leaves the table empty
         *        so every lookup falls back to English.
         * @return The number of strings published.
         */
        std::size_t load(const std::filesystem::path &json_path, std::string_view locale_tag)
        {
            auto &logger = DMK::log();

            Table table;

            if (!locale_tag.empty())
            {
                const json root = read_file(json_path);
                if (root.is_discarded())
                {
                    logger.warning("[lang] '{}' is not valid JSON; the interface stays English", to_utf8(json_path));
                }
                else if (root.contains("locales") && root["locales"].is_object())
                {
                    const auto &locales = root["locales"];
                    if (const auto it = locales.find(std::string{locale_tag});
                        it != locales.end() && it->contains("strings") && (*it)["strings"].is_object())
                    {
                        for (const auto &[key, value] : (*it)["strings"].items())
                        {
                            if (key.empty())
                                continue;
                            // An entry is either the translated text on its own, or an object whose "text" holds it
                            // beside the "en" and "where" fields that are there for the translator to read. Anything
                            // else, including an empty "text", means "not translated" and falls back to English.
                            std::string text;
                            if (value.is_string())
                                text = value.get<std::string>();
                            else if (value.is_object() && value.contains("text") && value["text"].is_string())
                                text = value["text"].get<std::string>();
                            if (!text.empty())
                                table.emplace(key, Entry{std::move(text), false});
                        }
                    }
                }
            }

            const std::size_t count = table.size();
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                if (!s_strings.empty())
                    s_retired.push_back(std::move(s_strings));
                s_strings = std::move(table);
            }

            if (count > 0)
                logger.info("[lang] interface: {} string(s) for '{}'", count, locale_tag);
            else if (!locale_tag.empty())
                logger.info("[lang] interface: no translations for '{}'; staying English", locale_tag);
            return count;
        }
    } // namespace

    std::size_t
    load_for(const std::filesystem::path &json_path, std::string_view interface_tag, std::string_view item_names_tag)
    {
        return load(json_path, effective_locale(interface_tag, item_names_tag));
    }

    std::string_view effective_locale(std::string_view interface_tag, std::string_view item_names_tag) noexcept
    {
        const bool follow = interface_tag.empty() || interface_tag == FOLLOW_ITEM_NAMES;
        return follow ? item_names_tag : interface_tag;
    }

    const char *t(const char *key, const char *english) noexcept
    {
        if (english == nullptr)
            return "";
        if (key == nullptr)
            return english;

        // A replaced table is retired rather than freed, so a pointer handed out here stays valid for the life of
        // the process. A miss hands back the caller's own literal, which is static.
        std::unique_lock<std::mutex> lock(s_mutex);
        if (s_strings.empty())
            return english;
        const auto it = s_strings.find(std::string_view{key});
        if (it == s_strings.end())
            return english;

        Entry &entry = it->second;
        bool report_mismatch = false;
        if (!entry.prepared)
        {
            // Reconcile against the call site, once. `entry.text` is only ever rewritten here, before any pointer
            // to it has been handed out, so no caller can be holding the old buffer.
            entry.prepared = true;
            // The conversion scan and the id re-attach both allocate. A failure here must retire the entry, not
            // escape into the render callback this runs on.
            try
            {
                const std::string_view source{english};
                const std::size_t id_at = id_suffix_at(source);
                const std::string_view visible = source.substr(0, id_at);

                if (!conversions_match(visible, entry.text))
                {
                    entry.text.clear();
                    report_mismatch = true;
                }
                else if (id_at != std::string_view::npos && entry.text.find("##") == std::string::npos)
                {
                    // The file carries the visible text only. Re-attach the id the call site declared, unless a
                    // hand-edited pack already included one.
                    entry.text.append(source.substr(id_at));
                }
            }
            catch (...)
            {
                entry.text.clear();
            }
        }

        const char *const result = entry.text.empty() ? english : entry.text.c_str();
        lock.unlock();

        // Emit after the lock releases, per the deferred-log order. It fires once per entry, on its first lookup.
        if (report_mismatch)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "[lang] '{}' has different printf conversions from the English text; using English. "
                "A translation must keep every %s / %d / %u, in the same order.",
                key
            );
        }
        return result;
    }

    std::vector<LocaleInfo> available_locales(const std::filesystem::path &json_path)
    {
        std::vector<LocaleInfo> out;
        const json root = read_file(json_path);
        if (root.is_discarded() || !root.contains("locales") || !root["locales"].is_object())
            return out;
        for (const auto &[tag, block] : root["locales"].items())
        {
            if (tag.empty() || !block.is_object())
                continue;
            out.push_back(LocaleInfo{tag, block.value("endonym", tag)});
        }
        return out;
    }

} // namespace Transmog::lang
