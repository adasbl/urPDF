#pragma once

// Generated
#include "SettingsPage.g.h"

// Standard C++
#include <cstdint>

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace winrt::urPDF::implementation
{
    namespace xaml       = winrt::Microsoft::UI::Xaml;
    namespace controls   = xaml::Controls;
    namespace foundation = winrt::Windows::Foundation;

    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();

        bool m_isInitializing{ true };

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void ClickHandler(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& /* args */);
        void ThemeSelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& /* e */);
        void TransparencySelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& /* e */);
        void SaveBackdropSettings(winrt::hstring const& transparencyValue);
        void SaveSettings(winrt::hstring const& themeName);
        void ToggleSwitch_Toggled(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& args);
        void LoadSavedSettings();
        void BackButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);

        winrt::event_token ThemeChanged(foundation::EventHandler<winrt::hstring> const& handler);
        void ThemeChanged(winrt::event_token const& token) noexcept;

        winrt::event_token BackdropChanged(foundation::EventHandler<winrt::hstring> const& handler);
        void BackdropChanged(winrt::event_token const& token) noexcept;

        winrt::event_token BackRequested(foundation::EventHandler<foundation::IInspectable> const& handler);
        void BackRequested(winrt::event_token const& token) noexcept;

    private:
        winrt::event<foundation::EventHandler<winrt::hstring>> m_themeChangedEvent;
        winrt::event<foundation::EventHandler<winrt::hstring>> m_backdropChangedEvent;
        winrt::event<foundation::EventHandler<foundation::IInspectable>> m_backRequestedEvent;
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}
