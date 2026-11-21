// Corresponding
#include "AppSettings.h"

// Standard C++
#include <cmath>
#include <limits>
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
        constexpr std::wstring_view WindowStateKey = L"WindowState";
        constexpr std::wstring_view WindowStartupModeKey = L"WindowStartupMode";

        bool IsValid(WindowState const& state) noexcept
        {
            return (!state.normalSize || (state.normalSize->width > 0 && state.normalSize->height > 0))
                && (!state.normalPosition || (state.normalSize && !state.normalPosition->monitorDevice.empty()));
        }

        json::JsonObject WindowStateJson(WindowState const& state)
        {
            json::JsonObject root;
            root.SetNamedValue(L"maximized", json::JsonValue::CreateBooleanValue(state.maximized));
            if (state.normalSize)
            {
                root.SetNamedValue(L"normalWidth", json::JsonValue::CreateNumberValue(state.normalSize->width));
                root.SetNamedValue(L"normalHeight", json::JsonValue::CreateNumberValue(state.normalSize->height));
            }
            if (state.normalPosition)
            {
                root.SetNamedValue(L"normalOffsetX", json::JsonValue::CreateNumberValue(state.normalPosition->x));
                root.SetNamedValue(L"normalOffsetY", json::JsonValue::CreateNumberValue(state.normalPosition->y));
                root.SetNamedValue(L"monitorDevice", json::JsonValue::CreateStringValue(state.normalPosition->monitorDevice));
            }
            return root;
        }

        std::optional<std::int32_t> ParseDimension(json::IJsonValue const& value)
        {
            if (value.ValueType() != json::JsonValueType::Number) { return std::nullopt; }
            const double number = value.GetNumber();
            if (!std::isfinite(number) || number <= 0
                || number > (std::numeric_limits<std::int32_t>::max)() || number != std::floor(number))
            {
                return std::nullopt;
            }
            return static_cast<std::int32_t>(number);
        }

        std::optional<WindowState> ParseWindowState(json::IJsonValue const& value)
        {
            if (value.ValueType() != json::JsonValueType::Object) { return std::nullopt; }
            auto root = value.GetObject();
            if (!root.HasKey(L"maximized")) { return std::nullopt; }
            auto maximized = root.GetNamedValue(L"maximized");
            if (maximized.ValueType() != json::JsonValueType::Boolean) { return std::nullopt; }

            WindowState state;
            state.maximized = maximized.GetBoolean();
            if (root.HasKey(L"normalWidth") || root.HasKey(L"normalHeight"))
            {
                if (!root.HasKey(L"normalWidth") || !root.HasKey(L"normalHeight")) { return std::nullopt; }
                auto width = ParseDimension(root.GetNamedValue(L"normalWidth"));
                auto height = ParseDimension(root.GetNamedValue(L"normalHeight"));
                if (!width || !height) { return std::nullopt; }
                state.normalSize = WindowSize{ *width, *height };
            }
            if (root.HasKey(L"normalOffsetX") || root.HasKey(L"normalOffsetY") || root.HasKey(L"monitorDevice"))
            {
                if (!state.normalSize || !root.HasKey(L"normalOffsetX")
                    || !root.HasKey(L"normalOffsetY") || !root.HasKey(L"monitorDevice")) { return std::nullopt; }
                auto x = root.GetNamedValue(L"normalOffsetX");
                auto y = root.GetNamedValue(L"normalOffsetY");
                auto device = root.GetNamedValue(L"monitorDevice");
                if (x.ValueType() != json::JsonValueType::Number || y.ValueType() != json::JsonValueType::Number
                    || device.ValueType() != json::JsonValueType::String || device.GetString().empty()) { return std::nullopt; }
                const auto validCoordinate = [](double number)
                {
                    return std::isfinite(number) && number == std::floor(number)
                        && number >= (std::numeric_limits<std::int32_t>::min)()
                        && number <= (std::numeric_limits<std::int32_t>::max)();
                };
                if (!validCoordinate(x.GetNumber()) || !validCoordinate(y.GetNumber())) { return std::nullopt; }
                state.normalPosition = WindowPosition{ static_cast<std::int32_t>(x.GetNumber()),
                    static_cast<std::int32_t>(y.GetNumber()), std::wstring{ device.GetString() } };
            }
            return state;
        }

        constexpr std::wstring_view OpenFilesInExistingWindowKey = L"OpenFilesInExistingWindow";
        constexpr std::wstring_view UseWindowsAccentColorForTextSelectionKey = L"UseWindowsAccentColorForTextSelection";

        std::wstring_view ToString(bool value) noexcept
        {
            return value ? L"true" : L"false";
        }

        std::optional<bool> ParseBool(std::wstring_view text) noexcept
        {
            if (text == L"true") { return true; }
            if (text == L"false") { return false; }
            return std::nullopt;
        }

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

        bool TryReadImportedBool(json::JsonObject const& root, std::wstring_view key, bool& value)
        {
            if (!root.HasKey(key)) { return true; }

            auto entry = root.GetNamedValue(key);
            if (entry.ValueType() != json::JsonValueType::Boolean) { return false; }

            value = entry.GetBoolean();
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

    std::wstring_view ToString(WindowStartupMode mode) noexcept
    {
        switch (mode)
        {
        case WindowStartupMode::Maximized: return L"Maximized";
        case WindowStartupMode::Normal: return L"Normal";
        default: return L"RememberLastState";
        }
    }

    std::optional<WindowStartupMode> ParseWindowStartupMode(std::wstring_view text) noexcept
    {
        if (text == L"RememberLastState") { return WindowStartupMode::RememberLastState; }
        if (text == L"Maximized") { return WindowStartupMode::Maximized; }
        if (text == L"Normal") { return WindowStartupMode::Normal; }
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
        if (auto text = m_store->Read(WindowStateKey))
        {
            json::JsonObject root{ nullptr };
            if (json::JsonObject::TryParse(*text, root))
            {
                m_cache.windowState = ParseWindowState(root).value_or(m_cache.windowState);
            }
        }
        if (auto text = m_store->Read(WindowStartupModeKey))
        {
            m_cache.windowStartupMode = ParseWindowStartupMode(*text).value_or(m_cache.windowStartupMode);
        }
        if (auto text = m_store->Read(OpenFilesInExistingWindowKey))
        {
            m_cache.openFilesInExistingWindow = ParseBool(*text).value_or(m_cache.openFilesInExistingWindow);
        }
        if (auto text = m_store->Read(UseWindowsAccentColorForTextSelectionKey))
        {
            m_cache.useWindowsAccentColorForTextSelection = ParseBool(*text).value_or(m_cache.useWindowsAccentColorForTextSelection);
        }
    }

    SettingsData AppSettings::Get() const
    {
        return m_cache;
    }

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

    void AppSettings::SetOpenFilesInExistingWindow(bool enabled)
    {
        m_cache.openFilesInExistingWindow = enabled;
        try { m_store->Write(OpenFilesInExistingWindowKey, ToString(enabled)); }
        catch (...) {}
    }

    void AppSettings::SetUseWindowsAccentColorForTextSelection(bool enabled)
    {
        m_cache.useWindowsAccentColorForTextSelection = enabled;
        try { m_store->Write(UseWindowsAccentColorForTextSelectionKey, ToString(enabled)); }
        catch (...) {}
    }

    std::wstring AppSettings::ExportJson() const
    {
        json::JsonObject root;
        root.SetNamedValue(SchemaVersionKey, json::JsonValue::CreateNumberValue(SettingsSchemaVersion));
        root.SetNamedValue(ThemeKey, json::JsonValue::CreateStringValue(ToString(m_cache.theme)));
        root.SetNamedValue(BackdropKey, json::JsonValue::CreateStringValue(ToString(m_cache.backdrop)));
        root.SetNamedValue(WindowStateKey, WindowStateJson(m_cache.windowState));
        root.SetNamedValue(WindowStartupModeKey, json::JsonValue::CreateStringValue(ToString(m_cache.windowStartupMode)));
        root.SetNamedValue(OpenFilesInExistingWindowKey, json::JsonValue::CreateBooleanValue(m_cache.openFilesInExistingWindow));
        root.SetNamedValue(UseWindowsAccentColorForTextSelectionKey, json::JsonValue::CreateBooleanValue(m_cache.useWindowsAccentColorForTextSelection));
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
            || !TryReadImportedValue(root, BackdropKey, ParseBackdrop, imported.backdrop)
            || !TryReadImportedValue(root, WindowStartupModeKey, ParseWindowStartupMode, imported.windowStartupMode)
            || !TryReadImportedBool(root, OpenFilesInExistingWindowKey, imported.openFilesInExistingWindow)
            || !TryReadImportedBool(root, UseWindowsAccentColorForTextSelectionKey, imported.useWindowsAccentColorForTextSelection))
        {
            return false;
        }
        if (root.HasKey(WindowStateKey))
        {
            auto state = ParseWindowState(root.GetNamedValue(WindowStateKey));
            if (!state) { return false; }
            imported.windowState = *state;
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
        m_store->Write(OpenFilesInExistingWindowKey, ToString(settings.openFilesInExistingWindow));
        m_store->Write(UseWindowsAccentColorForTextSelectionKey, ToString(settings.useWindowsAccentColorForTextSelection));
        m_store->Write(WindowStateKey, WindowStateJson(settings.windowState).Stringify());
        m_store->Write(WindowStartupModeKey, ToString(settings.windowStartupMode));
    }

    void AppSettings::SetWindowState(WindowState const& state)
    {
        if (!IsValid(state)) { return; }
        m_cache.windowState = state;
        try { m_store->Write(WindowStateKey, WindowStateJson(state).Stringify()); }
        catch (...) {}
    }

    void AppSettings::SetWindowStartupMode(WindowStartupMode mode)
    {
        m_cache.windowStartupMode = mode;
        try { m_store->Write(WindowStartupModeKey, ToString(mode)); }
        catch (...) {}
    }
}
