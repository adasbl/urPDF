#pragma once

#include "App.xaml.g.h"
#include "WindowCreationOptions.h"

#include <cstdint>
#include <memory>
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

namespace winrt::urPDF::implementation
{
    struct App : AppT<App>
    {
        App();
        explicit App(std::shared_ptr<::urpdf::MuPdfRuntime> runtime);

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
        winrt::urPDF::MainWindow CreateWindow(::urpdf::WindowCreationOptions const& options = {});
        void ApplySettingsToWindows();
        std::shared_ptr<::urpdf::MuPdfRuntime> MuPdfRuntime() const noexcept;
        std::uint64_t AllocateDocumentId() noexcept;

    private:
        std::shared_ptr<::urpdf::MuPdfRuntime> m_muPdfRuntime;
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_windows;
        // Document IDs are allocated on the shared UI thread for all windows.
        std::uint64_t m_nextDocumentId{ 1 };
    };
}
