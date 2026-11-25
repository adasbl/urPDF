#pragma once

// Project
#include "SettingsStore.h"

// Standard C++
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace urpdf
{
    inline constexpr int SettingsSchemaVersion = 1;

    // Enumerator order matches the ComboBox items in SettingsPage.xaml.
    enum class AppTheme { Default, Light, Dark };
    enum class Backdrop { None, Mica, MicaAlt };
    enum class WindowStartupMode { RememberLastState, Maximized, Normal };

    struct WindowSize
    {
        std::int32_t width;
        std::int32_t height;

        bool operator==(WindowSize const&) const = default;
    };

    struct WindowPosition
    {
        // Offsets within the monitor's work area survive rearranging displays.
        std::int32_t x;
        std::int32_t y;
        std::wstring monitorDevice;

        bool operator==(WindowPosition const&) const = default;
    };

    struct WindowState
    {
        std::optional<WindowSize> normalSize;
        bool maximized{ false };
        std::optional<WindowPosition> normalPosition;

        bool operator==(WindowState const&) const = default;
    };

    struct SettingsData
    {
        AppTheme theme{ AppTheme::Default };
        Backdrop backdrop{ Backdrop::None };
        WindowState windowState;
        WindowStartupMode windowStartupMode{ WindowStartupMode::RememberLastState };
        bool openFilesInExistingWindow{ false };
        bool askToSetAsDefaultPdfViewer{ true };
    };

    std::wstring_view ToString(AppTheme theme) noexcept;
    std::wstring_view ToString(Backdrop backdrop) noexcept;
    std::wstring_view ToString(WindowStartupMode mode) noexcept;
    std::optional<AppTheme> ParseAppTheme(std::wstring_view text) noexcept;
    std::optional<Backdrop> ParseBackdrop(std::wstring_view text) noexcept;
    std::optional<WindowStartupMode> ParseWindowStartupMode(std::wstring_view text) noexcept;

    class AppSettings
    {
    public:
        explicit AppSettings(std::unique_ptr<ISettingsStore> store);

        AppSettings(AppSettings const&) = delete;
        AppSettings& operator=(AppSettings const&) = delete;

        static AppSettings& Instance();

        SettingsData Get() const;
        void SetTheme(AppTheme theme);
        void SetBackdrop(Backdrop backdrop);
        void SetWindowState(WindowState const& state);
        void SetWindowStartupMode(WindowStartupMode mode);
        void SetOpenFilesInExistingWindow(bool enabled);
        void SetAskToSetAsDefaultPdfViewer(bool enabled);

        std::wstring ExportJson() const;
        bool ImportJson(std::wstring_view text);

    private:
        void Store(SettingsData const& settings);

        std::unique_ptr<ISettingsStore> m_store;
        SettingsData m_cache;
    };
}
