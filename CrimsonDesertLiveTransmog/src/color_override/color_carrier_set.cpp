#include "color_carrier_set.hpp"
#include "color_state.hpp"

#include <array>

namespace Transmog::ColorOverride::CarrierSet
{
    namespace
    {
        std::array<std::array<std::atomic<std::uintptr_t>, k_maxCarrierMatInst>, k_slotCount> g_matinst{};
        std::array<std::atomic<std::size_t>, k_slotCount> g_matinstCount{};

        std::array<std::array<std::atomic<std::uint32_t>, k_maxCarrierHashes>, k_slotCount> g_hash{};
        std::array<std::atomic<std::size_t>, k_slotCount> g_hashCount{};
    } // namespace

    bool add_matinst(int slot, std::uintptr_t mi) noexcept
    {
        if (mi == 0 || slot < 0 || static_cast<std::size_t>(slot) >= k_slotCount)
            return false;
        auto &matinst = g_matinst[static_cast<std::size_t>(slot)];
        auto &count = g_matinstCount[static_cast<std::size_t>(slot)];
        const auto cnt = count.load(std::memory_order_acquire);
        const auto upper = (cnt < k_maxCarrierMatInst) ? cnt : k_maxCarrierMatInst;
        for (std::size_t i = 0; i < upper; ++i)
        {
            if (matinst[i].load(std::memory_order_relaxed) == mi)
                return true;
        }
        const auto idx = count.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= k_maxCarrierMatInst)
        {
            count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        matinst[idx].store(mi, std::memory_order_release);
        return true;
    }

    bool add_hash(int slot, std::uint32_t hash) noexcept
    {
        if (hash == 0 || slot < 0 || static_cast<std::size_t>(slot) >= k_slotCount)
            return false;
        auto &hashes = g_hash[static_cast<std::size_t>(slot)];
        auto &count = g_hashCount[static_cast<std::size_t>(slot)];
        const auto cnt = count.load(std::memory_order_acquire);
        const auto upper = (cnt < k_maxCarrierHashes) ? cnt : k_maxCarrierHashes;
        for (std::size_t i = 0; i < upper; ++i)
            if (hashes[i].load(std::memory_order_relaxed) == hash)
                return true;

        const auto now = State::now_ms();
        const auto last = State::hash_set_last_add_ms(slot).load(std::memory_order_acquire);
        if (last != 0 && (now - last) > State::k_hashSetBurstLockMs)
            return false; // burst settled; refuse new growth

        const auto idx = count.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= k_maxCarrierHashes)
        {
            count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        hashes[idx].store(hash, std::memory_order_release);
        State::hash_set_last_add_ms(slot).store(now, std::memory_order_release);
        return true;
    }

    int find_slot_by_hash(std::uint32_t hash) noexcept
    {
        if (hash == 0)
            return -1;
        for (std::size_t s = 0; s < k_slotCount; ++s)
        {
            const auto cnt = g_hashCount[s].load(std::memory_order_acquire);
            const auto upper = (cnt < k_maxCarrierHashes) ? cnt : k_maxCarrierHashes;
            const auto &hashes = g_hash[s];
            for (std::size_t i = 0; i < upper; ++i)
            {
                if (hashes[i].load(std::memory_order_relaxed) == hash)
                    return static_cast<int>(s);
            }
        }
        return -1;
    }

    void clear_slot(int slot) noexcept
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= k_slotCount)
            return;
        const auto s = static_cast<std::size_t>(slot);
        for (auto &p : g_matinst[s])
            p.store(0, std::memory_order_relaxed);
        for (auto &h : g_hash[s])
            h.store(0, std::memory_order_relaxed);
        g_matinstCount[s].store(0, std::memory_order_release);
        g_hashCount[s].store(0, std::memory_order_release);
        State::hash_set_last_add_ms(slot).store(0, std::memory_order_release);
    }

    void clear_all() noexcept
    {
        for (std::size_t s = 0; s < k_slotCount; ++s)
            clear_slot(static_cast<int>(s));
    }
} // namespace Transmog::ColorOverride::CarrierSet
