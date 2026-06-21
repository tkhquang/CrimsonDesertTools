#include "color_matinst_owner.hpp"

#include <array>
#include <atomic>

namespace Transmog::ColorOverride::MatInstOwner
{
    namespace
    {
        struct Entry
        {
            std::atomic<std::uintptr_t> mi{0};
            std::atomic<std::uint32_t> expected_hash{0};
            std::atomic<int> slot{-1};
        };
        std::array<Entry, k_capacity> g_map{};

        std::size_t hash_mi(std::uintptr_t mi) noexcept
        {
            std::uint64_t x = static_cast<std::uint64_t>(mi);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            return static_cast<std::size_t>(x) & (k_capacity - 1);
        }
    } // namespace

    void set(std::uintptr_t mi, std::uint32_t expected_hash, int slot) noexcept
    {
        if (mi == 0 || slot < 0)
            return;
        std::size_t idx = hash_mi(mi);
        for (std::size_t step = 0; step < k_capacity; ++step)
        {
            auto &e = g_map[idx];
            auto cur = e.mi.load(std::memory_order_acquire);
            if (cur == 0)
            {
                std::uintptr_t expected = 0;
                if (e.mi.compare_exchange_strong(expected, mi, std::memory_order_acq_rel))
                {
                    e.expected_hash.store(expected_hash, std::memory_order_relaxed);
                    e.slot.store(slot, std::memory_order_release);
                    return;
                }
                cur = e.mi.load(std::memory_order_acquire);
            }
            if (cur == mi)
            {
                e.expected_hash.store(expected_hash, std::memory_order_relaxed);
                e.slot.store(slot, std::memory_order_release);
                return;
            }
            idx = (idx + 1) & (k_capacity - 1);
        }
    }

    int lookup_verified(std::uintptr_t mi, std::uint32_t live_hash) noexcept
    {
        if (mi == 0)
            return -1;
        std::size_t idx = hash_mi(mi);
        for (std::size_t step = 0; step < k_capacity; ++step)
        {
            auto &e = g_map[idx];
            const auto cur = e.mi.load(std::memory_order_acquire);
            if (cur == 0)
                return -1;
            if (cur == mi)
            {
                const auto eh = e.expected_hash.load(std::memory_order_relaxed);
                if (eh == 0 || eh != live_hash)
                    return -1;
                return e.slot.load(std::memory_order_acquire);
            }
            idx = (idx + 1) & (k_capacity - 1);
        }
        return -1;
    }

    void clear_for_slot(int slot) noexcept
    {
        if (slot < 0)
            return;
        for (auto &e : g_map)
        {
            if (e.slot.load(std::memory_order_acquire) == slot)
            {
                e.mi.store(0, std::memory_order_relaxed);
                e.expected_hash.store(0, std::memory_order_relaxed);
                e.slot.store(-1, std::memory_order_release);
            }
        }
    }

    void clear_all() noexcept
    {
        for (auto &e : g_map)
        {
            e.mi.store(0, std::memory_order_relaxed);
            e.expected_hash.store(0, std::memory_order_relaxed);
            e.slot.store(-1, std::memory_order_relaxed);
        }
    }
} // namespace Transmog::ColorOverride::MatInstOwner
