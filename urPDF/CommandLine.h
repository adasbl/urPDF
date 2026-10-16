#pragma once

// Standard C++
#include <optional>

namespace urpdf
{
    inline constexpr int CommandSucceeded = 0;
    inline constexpr int CommandFailed = 1;

    // Returns the process exit code, or std::nullopt when the arguments are not a command.
    std::optional<int> RunCommandLine(int argc, wchar_t const* const* argv);
}
