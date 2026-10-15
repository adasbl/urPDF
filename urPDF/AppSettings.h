#pragma once

// Project
#include "SettingsStore.h"

// Standard C++
#include <memory>
#include <optional>
#include <string_view>

namespace urpdf
{
    // Enumerator order matches the ComboBox items in SettingsPage.xaml.
    enum class AppTheme { Default, Light, Dark };
    enum class Backdrop { None, Mica, MicaAlt };

    struct SettingsData
    {
        AppTheme theme{ AppTheme::Default };
        Backdrop backdrop{ Backdrop::None };
    };

    std::wstring_view ToString(AppTheme theme) noexcept;
    std::wstring_view ToString(Backdrop backdrop) noexcept;
    std::optional<AppTheme> ParseAppTheme(std::wstring_view text) noexcept;
    std::optional<Backdrop> ParseBackdrop(std::wstring_view text) noexcept;

    class AppSettings
    {
    public:
        explicit AppSettings(std::unique_ptr<ISettingsStore> store);

        AppSettings(AppSettings const&) = delete;
        AppSettings& operator=(AppSettings const&) = delete;

        static AppSettings& Instance();

        SettingsData Get() const noexcept;
        void SetTheme(AppTheme theme);
        void SetBackdrop(Backdrop backdrop);

    private:
        std::unique_ptr<ISettingsStore> m_store;
        SettingsData m_cache;
    };
}
