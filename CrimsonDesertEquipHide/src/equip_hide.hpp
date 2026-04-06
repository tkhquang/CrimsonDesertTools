#pragma once

namespace EquipHide
{
    /** @brief Initialise hooks, config, and input bindings. Called from the init thread. */
    bool init();

    /** @brief Tear down hooks and restore any patched bytes. */
    void shutdown();

    /** @brief Arm the post-toggle cascade guard for body parts. */
    void arm_flush_guard() noexcept;

} // namespace EquipHide
