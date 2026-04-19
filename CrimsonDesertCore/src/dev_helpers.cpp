#include <cdcore/dev_helpers.hpp>

#include <Windows.h>

#include <cstring>

namespace CDCore::Dev
{
    bool is_target_process(const char *expected) noexcept
    {
        if (!expected || !*expected)
            return false;

        char exePath[MAX_PATH];
        const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return false;

        const char *exeName = std::strrchr(exePath, '\\');
        exeName = exeName ? exeName + 1 : exePath;
        return _stricmp(exeName, expected) == 0;
    }

    bool acquire_instance_mutex(
        const wchar_t *prefixW, HANDLE &outHandle) noexcept
    {
        outHandle = nullptr;
        if (!prefixW || !*prefixW)
            return false;

        wchar_t mutexName[96];
        const int n = wsprintfW(
            mutexName, L"%s%lu", prefixW, GetCurrentProcessId());
        if (n <= 0)
            return false;

        HANDLE h = CreateMutexW(nullptr, FALSE, mutexName);
        if (!h)
            return false;

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(h);
            return false;
        }

        outHandle = h;
        return true;
    }

    void release_instance_mutex(HANDLE &mutexHandle) noexcept
    {
        if (mutexHandle)
        {
            CloseHandle(mutexHandle);
            mutexHandle = nullptr;
        }
    }

} // namespace CDCore::Dev
