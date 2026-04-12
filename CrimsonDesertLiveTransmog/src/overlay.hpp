#pragma once

#include <Windows.h>

namespace Transmog
{
    /// @brief Register as a ReShade addon and install the transmog overlay tab.
    /// @param hModule Handle of the current DLL module.
    /// @return true if ReShade was found and registration succeeded.
    bool init_overlay(HMODULE hModule);

    /// @brief Unregister overlay and addon from ReShade. Safe to call if init failed.
    void shutdown_overlay();

} // namespace Transmog
