#include "color_pending_overrides.hpp"
#include "color_token_table.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace Transmog::ColorOverride::PendingOverrides
{
    namespace
    {
        struct Entry
        {
            std::string   submesh_name;
            std::string   token_name;
            std::uint16_t token_id_cached = 0;   // 0 means unresolved
            std::uint8_t  r = 0, g = 0, b = 0;
        };

        // Per-slot list. Linear scan on lookup -- entries per slot
        // typically <= 30 (number of dyeable submesh-token pairs in
        // an outfit), so a vector beats a hash map both for memory
        // locality and code simplicity.
        std::array<std::vector<Entry>, k_slotCount> g_entries;

        // Hot-path "does this slot have anything to check" gate so
        // the setter doesn't pay map-access cost when nothing was
        // persisted for the slot.
        std::array<std::atomic<bool>, k_slotCount> g_slotHas{};

        // Single mutex for all slots: insert/clear paths are cold
        // (load / preset switch / item swap). Lookup path is hot
        // but read-only under the lock for the brief vector scan.
        std::mutex g_mtx;

        std::atomic<std::uint64_t> g_hits{0};
        std::atomic<std::uint64_t> g_misses{0};

        bool valid_slot(int slot) noexcept
        {
            return slot >= 0
                && static_cast<std::size_t>(slot) < k_slotCount;
        }
    }

    void set(int slot,
             const std::string &submesh_name,
             const std::string &token_name,
             std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        if (!valid_slot(slot)) return;
        if (submesh_name.empty() || token_name.empty()) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        auto &vec = g_entries[static_cast<std::size_t>(slot)];
        // Update-or-insert by (submesh, token_name) so a save +
        // re-load of the same preset doesn't duplicate.
        for (auto &e : vec)
        {
            if (e.submesh_name == submesh_name
                && e.token_name == token_name)
            {
                e.r = r; e.g = g; e.b = b;
                return;
            }
        }
        Entry e;
        e.submesh_name = submesh_name;
        e.token_name   = token_name;
        e.token_id_cached =
            TokenTable::token_id_for_name(token_name.c_str());
        e.r = r; e.g = g; e.b = b;
        vec.push_back(std::move(e));
        g_slotHas[static_cast<std::size_t>(slot)].store(
            true, std::memory_order_release);
    }

    bool lookup(int slot,
                const char *submesh_name,
                std::uint16_t token_id,
                std::uint8_t &r, std::uint8_t &g, std::uint8_t &b) noexcept
    {
        if (!valid_slot(slot)
            || submesh_name == nullptr
            || submesh_name[0] == '\0'
            || token_id == 0)
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!g_slotHas[static_cast<std::size_t>(slot)].load(
                std::memory_order_acquire))
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        auto &vec = g_entries[static_cast<std::size_t>(slot)];
        // One-shot semantics: the entry is erased on a successful
        // match. The setter's substitute path calls
        // `set_override_active(true)` after a hit, so leaving the
        // entry in place would re-enable override on every
        // subsequent engine write -- making "Revert to default" and
        // per-row un-tick impossible (the user toggles override off,
        // the next engine write hits the same pending entry, and
        // override flips back on). The user's RGB lives in
        // SwatchOverride after a successful apply; pending is the
        // one-time bridge from JSON to live state, not a permanent
        // backing store.
        for (auto it = vec.begin(); it != vec.end(); ++it)
        {
            auto &e = *it;
            // Lazy late-resolve of token id when the snapshot
            // wasn't ready at insert time (early boot).
            if (e.token_id_cached == 0)
                e.token_id_cached =
                    TokenTable::token_id_for_name(e.token_name.c_str());
            if (e.token_id_cached != token_id) continue;
            if (e.submesh_name != submesh_name) continue;
            r = e.r; g = e.g; b = e.b;
            vec.erase(it);
            if (vec.empty())
                g_slotHas[static_cast<std::size_t>(slot)].store(
                    false, std::memory_order_release);
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool slot_has_pending(int slot) noexcept
    {
        if (!valid_slot(slot)) return false;
        return g_slotHas[static_cast<std::size_t>(slot)].load(
            std::memory_order_acquire);
    }

    bool has_any() noexcept
    {
        for (auto &h : g_slotHas)
            if (h.load(std::memory_order_acquire))
                return true;
        return false;
    }

    bool lookup_any_slot(const char *submesh_name,
                         std::uint16_t token_id,
                         std::uint8_t &r,
                         std::uint8_t &g,
                         std::uint8_t &b) noexcept
    {
        if (submesh_name == nullptr
            || submesh_name[0] == '\0'
            || token_id == 0)
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard<std::mutex> lk(g_mtx);
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            if (!g_slotHas[s].load(std::memory_order_acquire))
                continue;
            auto &vec = g_entries[s];
            for (auto &e : vec)
            {
                // Lazy late-resolve of token id when the snapshot
                // wasn't ready at insert time (early boot).
                if (e.token_id_cached == 0)
                    e.token_id_cached =
                        TokenTable::token_id_for_name(
                            e.token_name.c_str());
                if (e.token_id_cached != token_id) continue;
                if (e.submesh_name != submesh_name) continue;
                r = e.r;
                g = e.g;
                b = e.b;
                g_hits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        g_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    void set_by_token_id(int slot,
                         const std::string &submesh_name,
                         std::uint16_t token_id,
                         std::uint8_t r,
                         std::uint8_t g,
                         std::uint8_t b) noexcept
    {
        if (!valid_slot(slot)) return;
        if (submesh_name.empty() || token_id == 0) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        auto &vec = g_entries[static_cast<std::size_t>(slot)];
        // Update-or-insert by (submesh, token_id). Token name is left
        // empty -- lookup_any_slot matches by token_id_cached (already
        // populated here), and save-to-JSON writes the token name from
        // the picker's SwatchEntry, so the round-trip stays consistent
        // without us holding the name string here.
        for (auto &e : vec)
        {
            if (e.token_id_cached == token_id
                && e.submesh_name == submesh_name)
            {
                e.r = r; e.g = g; e.b = b;
                return;
            }
        }
        Entry e;
        e.submesh_name     = submesh_name;
        e.token_id_cached  = token_id;
        e.r = r; e.g = g; e.b = b;
        vec.push_back(std::move(e));
        g_slotHas[static_cast<std::size_t>(slot)].store(
            true, std::memory_order_release);
    }

    void erase_by_token_id(int slot,
                           const char *submesh_name,
                           std::uint16_t token_id) noexcept
    {
        if (!valid_slot(slot)) return;
        if (submesh_name == nullptr || submesh_name[0] == '\0'
            || token_id == 0)
            return;
        std::lock_guard<std::mutex> lk(g_mtx);
        auto &vec = g_entries[static_cast<std::size_t>(slot)];
        for (auto it = vec.begin(); it != vec.end(); ++it)
        {
            if (it->token_id_cached == token_id
                && it->submesh_name == submesh_name)
            {
                vec.erase(it);
                if (vec.empty())
                    g_slotHas[static_cast<std::size_t>(slot)].store(
                        false, std::memory_order_release);
                return;
            }
        }
    }

    void clear_slot(int slot) noexcept
    {
        if (!valid_slot(slot)) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        g_entries[static_cast<std::size_t>(slot)].clear();
        g_slotHas[static_cast<std::size_t>(slot)].store(
            false, std::memory_order_release);
    }

    void clear_all() noexcept
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            g_entries[s].clear();
            g_slotHas[s].store(false, std::memory_order_release);
        }
    }

    Stats snapshot_stats() noexcept
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        std::uint64_t total = 0;
        for (auto &v : g_entries) total += v.size();
        return Stats{
            total,
            g_hits.load(std::memory_order_relaxed),
            g_misses.load(std::memory_order_relaxed),
        };
    }

}
