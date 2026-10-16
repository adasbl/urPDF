// Corresponding
#include "AppSettings.h"

// Standard C++
#include <cmath>
#include <utility>

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace json = winrt::Windows::Data::Json;

namespace urpdf
{
    namespace
    {
        constexpr std::wstring_view SchemaVersionKey = L"schemaVersion";
        constexpr std::wstring_view ThemeKey = L"AppTheme";
        constexpr std::wstring_view BackdropKey = L"Transparency";

        template<typename T, typename Parse>
        bool TryReadImportedValue(json::JsonObject const& root, std::wstring_view key, Parse parse, T& value)
        {
            if (!root.HasKey(key)) { return true; }

            auto entry = root.GetNamedValue(key);
            if (entry.ValueType() != json::JsonValueType::String) { return false; }

            auto parsed = parse(entry.GetString());
            if (!parsed) { return false; }

            value = *parsed;
            return true;
        }
    }

    std::wstring_view ToString(AppTheme theme) noexcept
    {
        switch (theme)
        {
        case AppTheme::Light: return L"Light";
        case AppTheme::Dark: return L"Dark";
        default: return L"Default";
        }
    }

    std::wstring_view ToString(Backdrop backdrop) noexcept
    {
        switch (backdrop)
        {
        case Backdrop::Mica: return L"Mica";
        case Backdrop::MicaAlt: return L"MicaAlt";
        default: return L"None";
        }
    }

    std::optional<AppTheme> ParseAppTheme(std::wstring_view text) noexcept
    {
        if (text == L"Default") { return AppTheme::Default; }
        if (text == L"Light") { return AppTheme::Light; }
        if (text == L"Dark") { return AppTheme::Dark; }
        return std::nullopt;
    }

    std::optional<Backdrop> ParseBackdrop(std::wstring_view text) noexcept
    {
        if (text == L"None") { return Backdrop::None; }
        if (text == L"Mica") { return Backdrop::Mica; }
        if (text == L"MicaAlt") { return Backdrop::MicaAlt; }
        return std::nullopt;
    }

    AppSettings::AppSettings(std::unique_ptr<ISettingsStore> store)
        : m_store(std::move(store))
    {
        if (auto text = m_store->Read(ThemeKey))
        {
            m_cache.theme = ParseAppTheme(*text).value_or(m_cache.theme);
        }
        if (auto text = m_store->Read(BackdropKey))
        {
            m_cache.backdrop = ParseBackdrop(*text).value_or(m_cache.backdrop);
        }
    }

    SettingsData AppSettings::Get() const noexcept
    {
        return m_cache;
    }

    // A failed write keeps the new value for the current session only.
    void AppSettings::SetTheme(AppTheme theme)
    {
        m_cache.theme = theme;
        try { m_store->Write(ThemeKey, ToString(theme)); }
        catch (...) {}
    }

    void AppSettings::SetBackdrop(Backdrop backdrop)
    {
        m_cache.backdrop = backdrop;
        try { m_store->Write(BackdropKey, ToString(backdrop)); }
        catch (...) {}
    }

    std::wstring AppSettings::ExportJson() const
    {
        json::JsonObject root;
        root.SetNamedValue(SchemaVersionKey, json::JsonValue::CreateNumberValue(SettingsSchemaVersion));
        root.SetNamedValue(ThemeKey, json::JsonValue::CreateStringValue(ToString(m_cache.theme)));
        root.SetNamedValue(BackdropKey, json::JsonValue::CreateStringValue(ToString(m_cache.backdrop)));
        return std::wstring{ root.Stringify() };
    }

    bool AppSettings::ImportJson(std::wstring_view text)
    {
        json::JsonObject root{ nullptr };
        if (!json::JsonObject::TryParse(text, root)) { return false; }

        if (!root.HasKey(SchemaVersionKey)) { return false; }
        auto version = root.GetNamedValue(SchemaVersionKey);
        if (version.ValueType() != json::JsonValueType::Number) { return false; }
        const double versionNumber = version.GetNumber();
        if (versionNumber < 1 || versionNumber > SettingsSchemaVersion || versionNumber != std::floor(versionNumber))
        {
            return false;
        }

        SettingsData imported = m_cache;
        if (!TryReadImportedValue(root, ThemeKey, ParseAppTheme, imported.theme)
            || !TryReadImportedValue(root, BackdropKey, ParseBackdrop, imported.backdrop))
        {
            return false;
        }

        try
        {
            Store(imported);
        }
        catch (...)
        {
            try { Store(m_cache); }
            catch (...) {}
            return false;
        }

        m_cache = imported;
        return true;
    }

    void AppSettings::Store(SettingsData const& settings)
    {
        m_store->Write(ThemeKey, ToString(settings.theme));
        m_store->Write(BackdropKey, ToString(settings.backdrop));
    }
}
