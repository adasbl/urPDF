#pragma once

// Standard C++
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace urpdf
{
    struct ISettingsStore
    {
        virtual ~ISettingsStore() = default;

        virtual std::optional<std::wstring> Read(std::wstring_view key) const = 0;
        virtual void Write(std::wstring_view key, std::wstring_view value) = 0;
    };

    class PackagedSettingsStore final : public ISettingsStore
    {
    public:
        std::optional<std::wstring> Read(std::wstring_view key) const override;
        void Write(std::wstring_view key, std::wstring_view value) override;
    };

    std::unique_ptr<ISettingsStore> CreateSettingsStore();
}
