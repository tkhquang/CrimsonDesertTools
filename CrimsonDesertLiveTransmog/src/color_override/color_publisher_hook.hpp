#pragma once

// Body-shader publisher MidHook. Fires per-matInst during the engine's
// frame-render walk; captures matInsts (RCX + RDX) into the active
// slot's carrier set + hash set + matInst-owner map. The target is
// AOB-resolved via `k_colorPublisherCandidates` in aob_resolver.hpp.

#include <cstdint>

namespace Transmog::ColorOverride::PublisherHook
{
    /// Install the MidHook. Returns true on success, false if the AOB
    /// cascade fails to resolve the publisher entry point.
    bool init();

    struct Stats
    {
        std::uint64_t entries;
        std::uint64_t inserts;
        std::uint64_t batch_rejects;
        std::uint64_t window_rejects;
        std::uint64_t host_rejects;
        std::uint64_t arec_rejects;
    };
    Stats snapshot_stats() noexcept;
}
