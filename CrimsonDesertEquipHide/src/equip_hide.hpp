#pragma once

namespace EquipHide
{
    /// Initialise hooks, config, and input bindings.  Called from the init thread.
    bool init();

    /// Tear down hooks and restore any patched bytes.
    void shutdown();

} // namespace EquipHide
