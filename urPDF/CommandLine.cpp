#include "pch.h"

// Corresponding
#include "CommandLine.h"

// Project
#include "AppSettings.h"
#include "SettingsStore.h"

// Standard C++
#include <filesystem>
#include <span>
#include <string_view>

// Windows SDK / WinRT
#include <winrt/base.h>

namespace urpdf
{
    namespace
    {
        using Arguments = std::span<wchar_t const* const>;

        struct Command
        {
            std::wstring_view name;
            int (*run)(Arguments arguments);
        };

        std::optional<std::filesystem::path> SinglePathArgument(Arguments arguments)
        {
            if (arguments.size() != 1 || arguments[0][0] == L'\0') { return std::nullopt; }
            return std::filesystem::path{ arguments[0] };
        }

        int ExportSettings(Arguments arguments)
        {
            auto path = SinglePathArgument(arguments);
            if (!path) { return CommandFailed; }

            WriteTextFileAtomically(*path, winrt::to_string(AppSettings::Instance().ExportJson()));
            return CommandSucceeded;
        }

        int ImportSettings(Arguments arguments)
        {
            auto path = SinglePathArgument(arguments);
            if (!path) { return CommandFailed; }

            auto content = ReadTextFile(*path);
            const bool imported = content && AppSettings::Instance().ImportJson(winrt::to_hstring(*content));
            return imported ? CommandSucceeded : CommandFailed;
        }

        constexpr Command Commands[] = {
            { L"--export-settings", ExportSettings },
            { L"--import-settings", ImportSettings },
        };
    }

    std::optional<int> RunCommandLine(int argc, wchar_t const* const* argv)
    {
        if (argc < 2 || argv == nullptr) { return std::nullopt; }

        const std::wstring_view name = argv[1];
        for (auto const& command : Commands)
        {
            if (command.name != name) { continue; }

            try
            {
                return command.run(Arguments{ argv + 2, static_cast<size_t>(argc - 2) });
            }
            catch (...)
            {
                return CommandFailed;
            }
        }
        return std::nullopt;
    }
}
