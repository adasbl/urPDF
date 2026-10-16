#include "pch.h"

// Corresponding
#include "SettingsStore.h"

// Standard C++
#include <fstream>
#include <iterator>
#include <utility>

// Windows SDK / WinRT
#include <appmodel.h>
#include <wil/resource.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace foundation = winrt::Windows::Foundation;
namespace json       = winrt::Windows::Data::Json;
namespace storage    = winrt::Windows::Storage;

namespace urpdf
{
    namespace
    {
        constexpr double SettingsSchemaVersion = 1;

        std::filesystem::path WithSuffix(std::filesystem::path path, wchar_t const* suffix)
        {
            path += suffix;
            return path;
        }

        std::filesystem::path DefaultSettingsFilePath()
        {
            std::filesystem::path localAppData{ std::wstring_view{ storage::UserDataPaths::GetDefault().LocalAppData() } };
            return localAppData / L"urPDF" / L"settings.json";
        }
    }

    std::optional<std::wstring> PackagedSettingsStore::Read(std::wstring_view key) const
    {
        auto values = storage::ApplicationData::Current().LocalSettings().Values();
        auto value = values.TryLookup(winrt::hstring{ key });
        if (auto text = value.try_as<foundation::IReference<winrt::hstring>>())
        {
            return std::wstring{ text.Value() };
        }
        return std::nullopt;
    }

    void PackagedSettingsStore::Write(std::wstring_view key, std::wstring_view value)
    {
        auto values = storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(winrt::hstring{ key }, winrt::box_value(winrt::hstring{ value }));
    }

    FileSettingsStore::FileSettingsStore(std::filesystem::path path)
        : m_path(std::move(path))
    {
        Load();
    }

    std::optional<std::wstring> FileSettingsStore::Read(std::wstring_view key) const
    {
        if (auto it = m_values.find(key); it != m_values.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void FileSettingsStore::Write(std::wstring_view key, std::wstring_view value)
    {
        m_values.insert_or_assign(std::wstring{ key }, std::wstring{ value });
        Save();
    }

    void FileSettingsStore::Load()
    {
        std::ifstream file{ m_path, std::ios::binary };
        if (!file) { return; }

        std::string content{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
        if (file.bad()) { return; }
        file.close();

        json::JsonObject root{ nullptr };
        if (!json::JsonObject::TryParse(winrt::to_hstring(content), root))
        {
            MoveFileExW(m_path.c_str(), WithSuffix(m_path, L".bak").c_str(), MOVEFILE_REPLACE_EXISTING);
            return;
        }

        for (auto const& entry : root)
        {
            if (entry.Value().ValueType() == json::JsonValueType::String)
            {
                m_values.insert_or_assign(std::wstring{ entry.Key() }, std::wstring{ entry.Value().GetString() });
            }
        }
    }

    void FileSettingsStore::Save() const
    {
        json::JsonObject root;
        root.SetNamedValue(L"schemaVersion", json::JsonValue::CreateNumberValue(SettingsSchemaVersion));
        for (auto const& [key, value] : m_values)
        {
            root.SetNamedValue(key, json::JsonValue::CreateStringValue(value));
        }
        std::string const content = winrt::to_string(root.Stringify());

        std::filesystem::create_directories(m_path.parent_path());
        auto const tempPath = WithSuffix(m_path, L".tmp");
        {
            wil::unique_hfile file{ CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (!file) { winrt::throw_last_error(); }

            DWORD written = 0;
            if (!WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
                || !FlushFileBuffers(file.get()))
            {
                winrt::throw_last_error();
            }
            if (written != content.size())
            {
                throw winrt::hresult_error{ HRESULT_FROM_WIN32(ERROR_WRITE_FAULT) };
            }
        }
        if (!MoveFileExW(tempPath.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            winrt::throw_last_error();
        }
    }

    bool HasPackageIdentity() noexcept
    {
        UINT32 length = 0;
        return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }

    std::unique_ptr<ISettingsStore> CreateSettingsStore()
    {
        if (HasPackageIdentity())
        {
            return std::make_unique<PackagedSettingsStore>();
        }
        return std::make_unique<FileSettingsStore>(DefaultSettingsFilePath());
    }
}
