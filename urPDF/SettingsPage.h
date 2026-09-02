#pragma once

#include "SettingsPage.g.h"

namespace winrt::urPDF::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();

        bool m_isInitializing{ true };

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void ClickHandler(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& /* args */);
        void ThemeSelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /* e */);
        void TransparencySelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /* e */);
        void SaveBackdropSettings(winrt::hstring const& transparencyValue);
        void SaveSettings(winrt::hstring const& themeName);
        void ToggleSwitch_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void LoadSavedSettings();

        winrt::event_token ThemeChanged(winrt::Windows::Foundation::EventHandler<winrt::hstring> const& handler);
        void ThemeChanged(winrt::event_token const& token) noexcept;

        winrt::event_token BackdropChanged(winrt::Windows::Foundation::EventHandler<winrt::hstring> const& handler);
        void BackdropChanged(winrt::event_token const& token) noexcept;

    private:
        winrt::event<winrt::Windows::Foundation::EventHandler<winrt::hstring>> m_themeChangedEvent;
        winrt::event<winrt::Windows::Foundation::EventHandler<winrt::hstring>> m_backdropChangedEvent;
    public:
        void BackButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}
