#include "color_carrier_set.hpp"
#include "color_state.hpp"

#include <array>

namespace Transmog::color_override::carrier_set
{
    namespace
    {
        std::array<std::array<std::atomic<std::uintptr_t>, MAX_CARRIER_MAT_INST>, SLOT_COUNT> g_matinst{};
        std::array<std::atomic<std::size_t>, SLOT_COUNT> g_matinstCount{};

        std::array<std::array<std::atomic<std::uint32_t>, MAX_CARRIER_HASHES>, SLOT_COUNT> g_hash{};
        std::array<std::atomic<std::size_t>, SLOT_COUNT> g_hashCount{};
    } // namespace

    bool add_matinst(int slot, std::uintptr_t mi) noexcept
    {
        if (mi == 0 || slot < 0 || static_cast<std::size_t>(slot) >= SLOT_COUNT)
            return false;
        auto &matinst = g_matinst[static_cast<std::size_t>(slot)];
        auto &count = g_matinstCount[static_cast<std::size_t>(slot)];
        const auto cnt = count.load(std::memory_order_acquire);
        const auto upper = (cnt < MAX_CARRIER_MAT_INST) ? cnt : MAX_CARRIER_MAT_INST;
        for (std::size_t i = 0; i < upper; ++i)
        {
            if (matinst[i].load(std::memory_order_relaxed) == mi)
                return true;
        }
        const auto idx = count.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= MAX_CARRIER_MAT_INST)
        {
            count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        matinst[idx].store(mi, std::memory_order_release);
        return true;
    }

    bool add_hash(int slot, std::uint32_t hash) noexcept
    {
        if (hash == 0 || slot < 0 || static_cast<std::size_t>(slot) >= SLOT_COUNT)
            return false;
        auto &hashes = g_hash[static_cast<std::size_t>(slot)];
        auto &count = g_hashCount[static_cast<std::size_t>(slot)];
        const auto cnt = count.load(std::memory_order_acquire);
        const auto upper = (cnt < MAX_CARRIER_HASHES) ? cnt : MAX_CARRIER_HASHES;
        for (std::size_t i = 0; i < upper; ++i)
            if (hashes[i].load(std::memory_order_relaxed) == hash)
                return true;

        const auto now = state::now_ms();
        const auto last = state::hash_set_last_add_ms(slot).load(std::memory_order_acquire);
        if (last != 0 && (now - last) > state::HASH_SET_BURST_LOCK_MS)
            return false; // burst settled; refuse new growth

        const auto idx = count.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= MAX_CARRIER_HASHES)
        {
            count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        hashes[idx].store(hash, std::memory_order_release);
        state::hash_set_last_add_ms(slot).store(now, std::memory_order_release);
        return true;
    }

    int find_slot_by_hash(std::uint32_t hash) noexcept
    {
        if (hash == 0)
            return -1;
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
        {
            const auto cnt = g_hashCount[s].load(std::memory_order_acquire);
            const auto upper = (cnt < MAX_CARRIER_HASHES) ? cnt : MAX_CARRIER_HASHES;
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
        if (slot < 0 || static_cast<std::size_t>(slot) >= SLOT_COUNT)
            return;
        const auto s = static_cast<std::size_t>(slot);
        for (auto &p : g_matinst[s])
            p.store(0, std::memory_order_relaxed);
        for (auto &h : g_hash[s])
            h.store(0, std::memory_order_relaxed);
        g_matinstCount[s].store(0, std::memory_order_release);
        g_hashCount[s].store(0, std::memory_order_release);
        state::hash_set_last_add_ms(slot).store(0, std::memory_order_release);
    }

    void clear_all() noexcept
    {
        for (std::size_t s = 0; s < SLOT_COUNT; ++s)
            clear_slot(static_cast<int>(s));
    }
} // namespace Transmog::color_override::carrier_set
