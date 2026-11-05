#pragma once

#include "App.xaml.g.h"
#include "WindowCreationOptions.h"
#include <winrt/Microsoft.UI.h>

#include <cstdint>
#include <memory>
#include <map>
#include <vector>

namespace urpdf
{
    class MuPdfRuntime;
}

// Prevent the Win32 macro from replacing App::CreateWindow.
#ifdef CreateWindow
#undef CreateWindow
#endif

namespace winrt::urPDF
{
    struct MainWindow;
}

namespace winrt::Microsoft::UI::Xaml::Controls
{
    struct TabViewItem;
}

namespace winrt::urPDF::implementation
{
    struct App : AppT<App>
    {
        App();
        explicit App(std::shared_ptr<::urpdf::MuPdfRuntime> runtime);

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
        winrt::urPDF::MainWindow CreateWindow(::urpdf::WindowCreationOptions const& options = {});
        winrt::urPDF::MainWindow FindWindow(Microsoft::UI::WindowId const& id) const;
        winrt::urPDF::MainWindow FindWindowForTab(Microsoft::UI::Xaml::Controls::TabViewItem const& tab) const;
        void TrackTearOutWindow(Microsoft::UI::Xaml::Controls::TabViewItem const& tab, Microsoft::UI::WindowId const& id);
        winrt::urPDF::MainWindow FindTearOutWindow(Microsoft::UI::Xaml::Controls::TabViewItem const& tab) const;
        void ApplySettingsToWindows();
        std::shared_ptr<::urpdf::MuPdfRuntime> MuPdfRuntime() const noexcept;
        std::uint64_t AllocateDocumentId() noexcept;

    private:
        std::shared_ptr<::urpdf::MuPdfRuntime> m_muPdfRuntime;
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_windows;
        std::map<Microsoft::UI::Xaml::Controls::TabViewItem, Microsoft::UI::WindowId> m_tearOutWindows;
        std::uint64_t m_nextDocumentId{ 1 };
    };
}
