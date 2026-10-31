#pragma once

#include "App.xaml.g.h"
#include "WindowCreationOptions.h"

#include <vector>

// Prevent the Win32 macro from replacing App::CreateWindow.
#ifdef CreateWindow
#undef CreateWindow
#endif

namespace winrt::urPDF
{
    struct MainWindow;
}

namespace winrt::urPDF::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
        winrt::urPDF::MainWindow CreateWindow(::urpdf::WindowCreationOptions const& options = {});
        void ApplySettingsToWindows();

    private:
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_windows;
    };
}
