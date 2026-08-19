#include "pch.h"
#include "SettingsPage.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

#include <winrt/Windows.Storage.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::urPDF::implementation
{
    int32_t SettingsPage::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void SettingsPage::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }

    SettingsPage::SettingsPage()
    {
        InitializeComponent();
        this->m_isInitializing = false;
    }

    void SettingsPage::ClickHandler(IInspectable const&, RoutedEventArgs const&)
    {
        Button().Content(box_value(L"Clicked"));
    }

    void SettingsPage::BackButton_Click(winrt::Windows::Foundation::IInspectable const& /* sender */, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /* e */)
    {
        if (auto frame = this->Frame())
        {
            frame.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            frame.Content(nullptr);
        }
    }

    void SettingsPage::ToggleSwitch_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
    {
        auto toggleSwitch = sender.as<winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch>();

        if (toggleSwitch.IsOn())
        {
            // TODO
        }
        else
        {
            // TODO
        }
    }

    void SettingsPage::ThemeSelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e)
    {
        if (m_isInitializing == true) { return; }

        auto comboBox = sender.as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>())
        {
            winrt::hstring themeTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());

            SaveSettings(themeTag);
            m_themeChangedEvent(*this, themeTag);
        }
    }

    void SettingsPage::TransparencySelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e)
    {
        if (m_isInitializing == true) { return; }

        auto comboBox = sender.as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>())
        {
            winrt::hstring transparencyTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());

            SaveBackdropSettings(transparencyTag);
            m_backdropChangedEvent(*this, transparencyTag);
        }
    }

    void SettingsPage::SaveSettings(winrt::hstring const& themeName)
    {
        auto localSettings = winrt::Windows::Storage::ApplicationData::Current().LocalSettings();
        localSettings.Values().Insert(L"AppTheme", winrt::box_value(themeName));
    }

    void SettingsPage::SaveBackdropSettings(winrt::hstring const& transparencyValue)
    {
        auto localSettings = winrt::Windows::Storage::ApplicationData::Current().LocalSettings();
        localSettings.Values().Insert(L"Transparency", winrt::box_value(transparencyValue));
    }

    winrt::event_token SettingsPage::ThemeChanged(winrt::Windows::Foundation::EventHandler<winrt::hstring> const& handler)
    {
        return m_themeChangedEvent.add(handler);
    }
    void SettingsPage::ThemeChanged(winrt::event_token const& token) noexcept
    {
        m_themeChangedEvent.remove(token);
    }

    winrt::event_token SettingsPage::BackdropChanged(winrt::Windows::Foundation::EventHandler<winrt::hstring> const& handler)
    {
        return m_backdropChangedEvent.add(handler);
    }
    void SettingsPage::BackdropChanged(winrt::event_token const& token) noexcept
    {
        m_backdropChangedEvent.remove(token);
    }
}
