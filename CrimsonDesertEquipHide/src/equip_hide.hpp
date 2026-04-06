#pragma once

namespace EquipHide
{
    /** @brief Initialise hooks, config, and input bindings. Called from the init thread. */
    bool init();

    /** @brief Tear down hooks and restore any patched bytes. */
    void shutdown();

} // namespace EquipHide
