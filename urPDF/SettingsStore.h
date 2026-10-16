#pragma once

// Standard C++
#include <filesystem>
#include <functional>
#include <map>
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

    class FileSettingsStore final : public ISettingsStore
    {
    public:
        explicit FileSettingsStore(std::filesystem::path path);

        std::optional<std::wstring> Read(std::wstring_view key) const override;
        void Write(std::wstring_view key, std::wstring_view value) override;

    private:
        void Load();
        void Save() const;

        std::filesystem::path m_path;
        std::map<std::wstring, std::wstring, std::less<>> m_values;
    };

    std::optional<std::string> ReadTextFile(std::filesystem::path const& path);
    void WriteTextFileAtomically(std::filesystem::path const& path, std::string_view content);

    bool HasPackageIdentity() noexcept;
}
