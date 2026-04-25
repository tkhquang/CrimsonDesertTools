#include <cdcore/prologue_check.hpp>

#include <Windows.h>

#include <cstdint>

namespace CDCore
{
    bool sanity_check_function_prologue(std::uintptr_t addr) noexcept
    {
        if (!addr)
            return false;

        std::uint8_t b0 = 0;
        __try
        {
            b0 = *reinterpret_cast<const volatile std::uint8_t *>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        // Blacklist: reject known poison, accept anything else.
        if (b0 == 0x00 || b0 == 0xCC || b0 == 0xC2 || b0 == 0xC3)
            return false;

        return true;
    }

} // namespace CDCore
