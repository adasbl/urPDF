#include "pch.h"
#include "AboutPage.h"
#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif

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
        if (auto frame = this->Frame())
        {
            frame.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            frame.Content(nullptr);
        }
    }
}
