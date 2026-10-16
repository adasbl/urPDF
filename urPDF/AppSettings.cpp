#include "pch.h"

// Corresponding
#include "AppSettings.h"

// Standard C++
#include <utility>

namespace urpdf
{
    namespace
    {
        constexpr std::wstring_view ThemeKey = L"AppTheme";
        constexpr std::wstring_view BackdropKey = L"Transparency";
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

    AppSettings& AppSettings::Instance()
    {
        static AppSettings instance{ CreateSettingsStore() };
        return instance;
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
}
