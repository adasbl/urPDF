// Corresponding
#include "ActivationArguments.h"

// Standard C++
#include <filesystem>
#include <memory>
#include <vector>

// Windows SDK
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace urpdf
{
    namespace
    {
        struct LocalFreeDeleter
        {
            void operator()(LPWSTR* arguments) const noexcept { ::LocalFree(arguments); }
        };

        std::vector<std::wstring> SplitCommandLine(std::wstring_view commandLine)
        {
            std::vector<std::wstring> tokens;
            // CommandLineToArgvW returns the current executable path for an empty string.
            if (commandLine.find_first_not_of(L" \t") == std::wstring_view::npos) { return tokens; }

            const std::wstring buffer{ commandLine };
            int count = 0;
            std::unique_ptr<LPWSTR, LocalFreeDeleter> arguments{ ::CommandLineToArgvW(buffer.c_str(), &count) };
            if (!arguments) { return tokens; }

            tokens.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) { tokens.emplace_back(arguments.get()[i]); }
            return tokens;
        }

        bool IsExecutablePath(std::wstring const& token)
        {
            const auto extension = std::filesystem::path{ token }.extension().wstring();
            return ::CompareStringOrdinal(extension.c_str(), static_cast<int>(extension.size()), L".exe", 4, TRUE) == CSTR_EQUAL;
        }
    }

    std::optional<std::wstring> DocumentPathFromLaunchArguments(std::wstring_view arguments)
    {
        const auto tokens = SplitCommandLine(arguments);
        const std::size_t first = !tokens.empty() && IsExecutablePath(tokens.front()) ? 1 : 0;
        if (tokens.size() <= first || tokens[first].empty()) { return std::nullopt; }

        const std::filesystem::path path{ tokens[first] };
        if (!path.is_absolute()) { return std::nullopt; }
        return path.lexically_normal().wstring();
    }

    bool HasRelativeDocumentPathArgument(int argc, wchar_t const* const* argv)
    {
        if (argc < 2 || argv == nullptr || argv[1] == nullptr || argv[1][0] == L'\0') { return false; }
        return !std::filesystem::path{ argv[1] }.is_absolute();
    }
}
