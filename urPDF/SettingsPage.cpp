#include "pch.h"

// Corresponding
#include "SettingsPage.h"

// Generated
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

// Standard C++
#include <cstdint>

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace xaml       = winrt::Microsoft::UI::Xaml;
namespace controls   = xaml::Controls;
namespace foundation = winrt::Windows::Foundation;
namespace storage    = winrt::Windows::Storage;

namespace winrt::urPDF::implementation
{
    int32_t SettingsPage::MyProperty()
    {
        throw winrt::hresult_not_implemented();
    }

    void SettingsPage::MyProperty(int32_t /* value */)
    {
        throw winrt::hresult_not_implemented();
    }

    SettingsPage::SettingsPage()
    {
        InitializeComponent();
        LoadSavedSettings();
        m_isInitializing = false;
    }

    void SettingsPage::ClickHandler(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        Button().Content(winrt::box_value(L"Clicked"));
    }

    void SettingsPage::BackButton_Click(foundation::IInspectable const& /* sender */, xaml::RoutedEventArgs const& /* e */)
    {
        m_backRequestedEvent(*this, nullptr);
    }

    void SettingsPage::ToggleSwitch_Toggled(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& args)
    {
        auto toggleSwitch = sender.as<controls::ToggleSwitch>();

        if (toggleSwitch.IsOn())
        {
            // TODO
        }
        else
        {
            // TODO
        }
    }

    void SettingsPage::ThemeSelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& e)
    {
        if (m_isInitializing == true) { return; }

        auto comboBox = sender.as<controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<controls::ComboBoxItem>())
        {
            winrt::hstring themeTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());

            SaveSettings(themeTag);
            m_themeChangedEvent(*this, themeTag);
        }
    }

    void SettingsPage::TransparencySelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& e)
    {
        if (m_isInitializing == true) { return; }

        auto comboBox = sender.as<controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<controls::ComboBoxItem>())
        {
            winrt::hstring transparencyTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());

            SaveBackdropSettings(transparencyTag);
            m_backdropChangedEvent(*this, transparencyTag);
        }
    }

    void SettingsPage::SaveSettings(winrt::hstring const& themeName)
    {
        auto localSettings = storage::ApplicationData::Current().LocalSettings();
        localSettings.Values().Insert(L"AppTheme", winrt::box_value(themeName));
    }

    void SettingsPage::SaveBackdropSettings(winrt::hstring const& transparencyValue)
    {
        auto localSettings = storage::ApplicationData::Current().LocalSettings();
        localSettings.Values().Insert(L"Transparency", winrt::box_value(transparencyValue));
    }

    winrt::event_token SettingsPage::ThemeChanged(foundation::EventHandler<winrt::hstring> const& handler)
    {
        return m_themeChangedEvent.add(handler);
    }
    void SettingsPage::ThemeChanged(winrt::event_token const& token) noexcept
    {
        m_themeChangedEvent.remove(token);
    }

    winrt::event_token SettingsPage::BackdropChanged(foundation::EventHandler<winrt::hstring> const& handler)
    {
        return m_backdropChangedEvent.add(handler);
    }

    void SettingsPage::BackdropChanged(winrt::event_token const& token) noexcept
    {
        m_backdropChangedEvent.remove(token);
    }

    winrt::event_token SettingsPage::BackRequested(foundation::EventHandler<foundation::IInspectable> const& handler)
    {
        return m_backRequestedEvent.add(handler);
    }

    void SettingsPage::BackRequested(winrt::event_token const& token) noexcept
    {
        m_backRequestedEvent.remove(token);
    }

    void SettingsPage::LoadSavedSettings()
    {
        auto settings = storage::ApplicationData::Current().LocalSettings();

        winrt::hstring savedTheme = L"Default";
        if (settings.Values().HasKey(L"AppTheme"))
        {
            savedTheme = winrt::unbox_value<winrt::hstring>(settings.Values().Lookup(L"AppTheme"));
        }

        int index = 0;
        if (savedTheme == L"Light") { index = 1; }
        else if (savedTheme == L"Dark") { index = 2; }

        ThemeComboBox().SelectedIndex(index);

        winrt::hstring savedTransparency = L"None";
        if (settings.Values().HasKey(L"Transparency"))
        {
            savedTransparency = winrt::unbox_value<winrt::hstring>(settings.Values().Lookup(L"Transparency"));
        }
        index = 0;
        if (savedTransparency == L"Mica") { index = 1; }
        else if (savedTransparency == L"MicaAlt") { index = 2; }

        TransparencyComboBox().SelectedIndex(index);
    }
}
