#ifndef TRANSMOG_INPUT_HANDLER_HPP
#define TRANSMOG_INPUT_HANDLER_HPP

#include <DetourModKit/input.hpp>

namespace Transmog
{
    /**
     * @brief Binds every hotkey INI key and registers its press binding.
     * @details Each binding goes through DMK::config::press_combo, which fuses the INI key binding, the default-combo
     *          parse, and the input press registration into one call. The returned guards are added to @p scope, so
     *          the Session releases them in reverse insertion order before it tears the input engine down.
     *
     *          Must be invoked before the INI load (so the bound setters fire on the first load pass) and before
     *          input::Input::start() (so the poller picks the bindings up).
     * @param scope The Session's input scope, which owns every returned guard.
     * @note Setup/control-plane only: registration allocates and reshapes the binding set.
     */
    void register_hotkeys(DetourModKit::input::Scope &scope);

} // namespace Transmog

#endif // TRANSMOG_INPUT_HANDLER_HPP
