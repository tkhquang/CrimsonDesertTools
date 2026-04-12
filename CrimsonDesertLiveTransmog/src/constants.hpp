#pragma once

#include "version.hpp"

namespace Transmog
{
    // --- Mod identity ---
    inline constexpr const char *MOD_VERSION = VERSION_STRING;
    inline constexpr const char *MOD_NAME = "CrimsonDesertLiveTransmog";
    inline constexpr const char *MOD_AUTHOR = "tkhquang";
    inline constexpr const char *MOD_SOURCE = "https://github.com/tkhquang/CrimsonDesertTools";
    inline constexpr const char *LOG_FILE = "CrimsonDesertLiveTransmog.log";
    inline constexpr const char *INI_FILE = "CrimsonDesertLiveTransmog.ini";
    inline constexpr const char *PRESETS_FILE = "CrimsonDesertLiveTransmog_presets.json";
    inline constexpr const wchar_t *INSTANCE_MUTEX_PREFIX = L"CrimsonDesertLiveTransmog_";

    // --- Process gate ---
    inline constexpr const char *GAME_PROCESS_NAME = "CrimsonDesert.exe";

    // --- Game-version target ---
    // Stored in CrimsonDesertLiveTransmog_presets.json. Numeric itemIds are
} // namespace Transmog
