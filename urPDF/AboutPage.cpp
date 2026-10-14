#include "pch.h"

// Corresponding
#include "AboutPage.h"

// Generated
#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif

// Standard C++
#include <cstdint>

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace xaml       = winrt::Microsoft::UI::Xaml;
namespace controls   = winrt::Microsoft::UI::Xaml::Controls;
namespace media      = winrt::Microsoft::UI::Xaml::Media;
namespace foundation = winrt::Windows::Foundation;

namespace winrt::urPDF::implementation
{
    int32_t AboutPage::MyProperty()
    {
        throw winrt::hresult_not_implemented();
    }

    void AboutPage::MyProperty(int32_t /* value */)
    {
        throw winrt::hresult_not_implemented();
    }

    void AboutPage::BackButton_Click(foundation::IInspectable const& /* sender */, xaml::RoutedEventArgs const& /* e */)
    {
        auto frame = this->Frame();
        if (!frame) return;

        frame.Content(nullptr);
        frame.Visibility(xaml::Visibility::Collapsed);

        auto parent = media::VisualTreeHelper::GetParent(frame);
        if (auto rootGrid = parent.try_as<controls::Grid>())
        {
            if (auto mainWindowUI = rootGrid.FindName(L"MainWindowUI").try_as<xaml::UIElement>())
            {
                mainWindowUI.Visibility(xaml::Visibility::Visible);
            }
        }
    }
}
