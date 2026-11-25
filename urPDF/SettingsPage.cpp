#include "pch.h"

// Corresponding
#include "SettingsPage.h"

// Generated
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

// Project
#include "AppSettings.h"
#include "DefaultPdfViewer.h"

// Standard C++
#include <cstdint>

// Windows SDK / WinRT
#include <wil/resource.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace xaml       = winrt::Microsoft::UI::Xaml;
namespace controls   = xaml::Controls;
namespace foundation = winrt::Windows::Foundation;

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

    void SettingsPage::OpenFilesInExistingWindowToggle_Toggled(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& /* args */)
    {
        if (m_isInitializing == true) { return; }

        auto toggleSwitch = sender.as<controls::ToggleSwitch>();
        urpdf::AppSettings::Instance().SetOpenFilesInExistingWindow(toggleSwitch.IsOn());
    }

    void SettingsPage::UseWindowsAccentColorForTextSelectionToggle_Toggled(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& /* args */)
    {
        if (m_isInitializing) { return; }

        auto toggleSwitch = sender.as<controls::ToggleSwitch>();
        const bool enabled = toggleSwitch.IsOn();
        urpdf::AppSettings::Instance().SetUseWindowsAccentColorForTextSelection(enabled);
        m_textSelectionColorChangedEvent(*this, enabled);
    }

    void SettingsPage::AskToSetDefaultPdfViewerToggle_Toggled(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& /* args */)
    {
        if (m_isInitializing) { return; }

        auto toggleSwitch = sender.as<controls::ToggleSwitch>();
        urpdf::AppSettings::Instance().SetAskToSetAsDefaultPdfViewer(toggleSwitch.IsOn());
    }

    winrt::fire_and_forget SettingsPage::OpenDefaultAppsButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto strongThis = get_strong();
        auto button = OpenDefaultAppsButton();
        button.IsEnabled(false);
        auto restoreButton = wil::scope_exit([button]
        {
            try { button.IsEnabled(true); }
            catch (...) {}
        });

        const bool opened = co_await urpdf::OpenDefaultAppsSettingsAsync();
        if (!opened)
        {
            DefaultPdfViewerStatusText().Text(L"Couldn't open Windows Settings. Open Settings > Apps > Default apps to set urPDF as your default PDF viewer.");
        }
    }

    void SettingsPage::RefreshDefaultPdfViewerStatus()
    {
        switch (urpdf::QueryDefaultPdfViewerStatus())
        {
        case urpdf::DefaultPdfViewerStatus::Default:
            DefaultPdfViewerStatusText().Text(L"urPDF is currently set as your default PDF viewer.");
            break;
        case urpdf::DefaultPdfViewerStatus::NotDefault:
            DefaultPdfViewerStatusText().Text(L"urPDF is not currently set as your default PDF viewer.");
            break;
        case urpdf::DefaultPdfViewerStatus::Unknown:
        default:
            DefaultPdfViewerStatusText().Text(L"Manage which app opens .pdf files in Windows Settings.");
            break;
        }
    }

    void SettingsPage::ThemeSelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& e)
    {
        if (m_isInitializing == true) { return; }

        auto comboBox = sender.as<controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<controls::ComboBoxItem>())
        {
            winrt::hstring themeTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());
            auto theme = urpdf::ParseAppTheme(themeTag);
            if (!theme) { return; }

            urpdf::AppSettings::Instance().SetTheme(*theme);
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
            auto backdrop = urpdf::ParseBackdrop(transparencyTag);
            if (!backdrop) { return; }

            urpdf::AppSettings::Instance().SetBackdrop(*backdrop);
            m_backdropChangedEvent(*this, transparencyTag);
        }
    }

    void SettingsPage::WindowStartupModeSelection_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const&)
    {
        if (m_isInitializing) { return; }

        auto comboBox = sender.as<controls::ComboBox>();
        if (auto selectedItem = comboBox.SelectedItem().try_as<controls::ComboBoxItem>())
        {
            auto modeTag = winrt::unbox_value<winrt::hstring>(selectedItem.Tag());
            auto mode = urpdf::ParseWindowStartupMode(modeTag);
            if (!mode) { return; }

            urpdf::AppSettings::Instance().SetWindowStartupMode(*mode);
        }
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

    winrt::event_token SettingsPage::TextSelectionColorChanged(foundation::EventHandler<bool> const& handler)
    {
        return m_textSelectionColorChangedEvent.add(handler);
    }

    void SettingsPage::TextSelectionColorChanged(winrt::event_token const& token) noexcept
    {
        m_textSelectionColorChangedEvent.remove(token);
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
        const bool wasInitializing = m_isInitializing;
        m_isInitializing = true;
        auto restoreInitializing = wil::scope_exit([this, wasInitializing]
        {
            m_isInitializing = wasInitializing;
        });

        auto settings = urpdf::AppSettings::Instance().Get();

        ThemeComboBox().SelectedIndex(static_cast<int32_t>(settings.theme));
        TransparencyComboBox().SelectedIndex(static_cast<int32_t>(settings.backdrop));
        WindowStartupModeComboBox().SelectedIndex(static_cast<int32_t>(settings.windowStartupMode));
        OpenFilesInExistingWindowToggle().IsOn(settings.openFilesInExistingWindow);
        UseWindowsAccentColorForTextSelectionToggle().IsOn(settings.useWindowsAccentColorForTextSelection);
        AskToSetDefaultPdfViewerToggle().IsOn(settings.askToSetAsDefaultPdfViewer);
        RefreshDefaultPdfViewerStatus();
    }
}
