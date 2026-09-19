/**
 * @file loader_log.hpp
 * @brief The loader log's line format, shared by the resident dev loader and every logic generation.
 *
 * The loader log outlives every generation, so it holds the records no Session can: the loader's own reload
 * progress and a generation's unload verdict, which is computed after ~Session. The loader and the generation
 * append to the same file, each opening it per line, and this header is the one place that spells the record: a
 * "[HH:MM:SS.mmm] " local-time stamp, the caller's line, a newline. A reader tells the writers apart by the bracket
 * tag the caller puts first: "[<mod> Loader]" for the loader, "[<short mod name>][DEV]" for a generation.
 *
 * Win32 only, on purpose. The loader links no DetourModKit archive, and a generation reaches for this after its
 * logger is gone.
 */
#ifndef CDCORE_DEV_LOADER_LOG_HPP
#define CDCORE_DEV_LOADER_LOG_HPP

#include <Windows.h>

#include <cstddef>
#include <cstdio>
#include <string>

namespace CDCore::dev
{
    /// Suffix the loader log's file name adds to the mod's deployed stem.
    inline constexpr const char *LOADER_LOG_SUFFIX = "_Loader.log";

    /**
     * @brief Builds the loader log path for @p mod_name inside @p directory.
     * @param directory The loader's directory, with or without a trailing separator.
     * @param mod_name The mod's deployed stem, ASCII, as the loader's CDCORE_LOADER_MOD_NAME spells it.
     * @return "<directory>\<mod_name>_Loader.log".
     */
    [[nodiscard]] inline std::wstring loader_log_path(std::wstring directory, const char *mod_name)
    {
        if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
        {
            directory.push_back(L'\\');
        }
        // Both inputs are ASCII, so a widening copy keeps one spelling of each in the project.
        for (const char *ch = mod_name; *ch != '\0'; ++ch)
        {
            directory.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*ch)));
        }
        for (const char *ch = LOADER_LOG_SUFFIX; *ch != '\0'; ++ch)
        {
            directory.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*ch)));
        }
        return directory;
    }

    /**
     * @brief Echoes @p line to the debugger, newline-terminated.
     * @details The debugger stamps records on its own, so the line goes out without the file stamp.
     */
    inline void echo_to_debugger(const char *line) noexcept
    {
        char body[800];
        if (std::snprintf(body, sizeof(body), "%s\n", line) > 0)
        {
            OutputDebugStringA(body);
        }
    }

    /**
     * @brief Writes @p line to @p file behind a "[HH:MM:SS.mmm] " local-time stamp, newline-terminated.
     * @param file A handle opened for append. The caller owns it.
     * @param line The record without its newline. A line longer than the buffer is cut at the buffer.
     */
    inline void write_stamped(HANDLE file, const char *line) noexcept
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamped[832];
        const int len = std::snprintf(
            stamped,
            sizeof(stamped),
            "[%02u:%02u:%02u.%03u] %s\n",
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds,
            line
        );
        if (len <= 0)
        {
            return;
        }
        // snprintf reports the length the record wanted, so a cut record must not read past the buffer.
        std::size_t to_write = static_cast<std::size_t>(len);
        if (to_write >= sizeof(stamped))
        {
            to_write = sizeof(stamped) - 1;
        }
        DWORD written = 0;
        (void)WriteFile(file, stamped, static_cast<DWORD>(to_write), &written, nullptr);
    }

    /**
     * @brief Appends @p line to the loader log at @p path and echoes it to the debugger.
     * @details The file is opened for append per line and shared for reading and writing, so the loader and a
     *          generation interleave on one file without a shared handle. A path that cannot be opened leaves the
     *          debugger echo as the only report.
     */
    inline void append_line(const wchar_t *path, const char *line) noexcept
    {
        echo_to_debugger(line);
        const HANDLE file = CreateFileW(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        write_stamped(file, line);
        CloseHandle(file);
    }

    /**
     * @brief Narrow-path overload of @ref append_line for the loader, which keeps its paths in the ANSI code page.
     */
    inline void append_line(const char *path, const char *line) noexcept
    {
        echo_to_debugger(line);
        const HANDLE file = CreateFileA(
            path,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        write_stamped(file, line);
        CloseHandle(file);
    }
} // namespace CDCore::dev

#endif // CDCORE_DEV_LOADER_LOG_HPP
