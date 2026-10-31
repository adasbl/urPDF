#pragma once

#include "App.xaml.g.h"

#include <vector>

namespace winrt::urPDF::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_windows;
    };
}
