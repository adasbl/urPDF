#include "pch.h"
#include "AboutPage.h"
#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;


namespace winrt::urPDF::implementation
{
    int32_t AboutPage::MyProperty()
    {
        throw hresult_not_implemented();
    }

    void AboutPage::MyProperty(int32_t /* value */)
    {
        throw hresult_not_implemented();
    }

    void AboutPage::BackButton_Click(winrt::Windows::Foundation::IInspectable const& /* sender */, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /* e */)
    {
        auto frame = this->Frame();
        if (!frame) return;

        frame.Content(nullptr);
        frame.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(frame);
        if (auto rootGrid = parent.try_as<winrt::Microsoft::UI::Xaml::Controls::Grid>())
        {
            if (auto mainWindowUI = rootGrid.FindName(L"MainWindowUI").try_as<winrt::Microsoft::UI::Xaml::UIElement>())
            {
                mainWindowUI.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
        }
    }
}
