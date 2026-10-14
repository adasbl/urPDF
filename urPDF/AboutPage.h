#pragma once

// Generated
#include "AboutPage.g.h"

// Standard C++
#include <cstdint>

// Windows SDK / WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::urPDF::implementation
{
    namespace xaml       = winrt::Microsoft::UI::Xaml;
    namespace foundation = winrt::Windows::Foundation;

    struct AboutPage : AboutPageT<AboutPage>
    {
        AboutPage() 
        {
            // Xaml objects should not call InitializeComponent during construction.
            // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent
        }

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void ClickHandler(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& /* args */);
        void BackButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct AboutPage : AboutPageT<AboutPage, implementation::AboutPage>
    {
    };
}
