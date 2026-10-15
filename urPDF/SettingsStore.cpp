#include "pch.h"

// Corresponding
#include "SettingsStore.h"

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

namespace foundation = winrt::Windows::Foundation;
namespace storage    = winrt::Windows::Storage;

namespace urpdf
{
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

    std::unique_ptr<ISettingsStore> CreateSettingsStore()
    {
        return std::make_unique<PackagedSettingsStore>();
    }
}
