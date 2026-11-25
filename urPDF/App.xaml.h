#pragma once

#include "App.xaml.g.h"
#include "WindowCreationOptions.h"
#include <winrt/Microsoft.UI.h>

#include <cstdint>
#include <memory>
#include <set>
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
        void WindowActivated(winrt::urPDF::MainWindow const& window);
        winrt::urPDF::MainWindow LastActiveWindow() const;
        winrt::urPDF::MainWindow FindWindow(Microsoft::UI::WindowId const& id) const;
        winrt::urPDF::MainWindow FindWindowForTab(Microsoft::UI::Xaml::Controls::TabViewItem const& tab) const;
        void RequestCloseEmptyWindow(Microsoft::UI::WindowId const& id);
        void RefreshTabDragRegions();
        void WindowMoveSizeEntered(Microsoft::UI::WindowId const& id);
        void WindowMoveSizeExited(Microsoft::UI::WindowId const& id);
        void ApplySettingsToWindows();
        bool TryClaimDefaultPdfViewerPrompt() noexcept;
        void ReleaseDefaultPdfViewerPrompt() noexcept;
        std::shared_ptr<::urpdf::MuPdfRuntime> MuPdfRuntime() const noexcept;
        std::uint64_t AllocateDocumentId() noexcept;

    private:
        void SchedulePendingWindowCloses();
        void OpenActivationDocuments(std::vector<::urpdf::WindowCreationOptions> const& documents);
        std::shared_ptr<::urpdf::MuPdfRuntime> m_muPdfRuntime;
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_windows;
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Window> m_lastActiveWindow;
        std::set<std::uint64_t> m_windowsInMoveSize;
        std::vector<Microsoft::UI::WindowId> m_pendingWindowCloses;
        bool m_windowCloseQueued{ false };
        bool m_defaultPdfViewerPromptClaimed{ false };
        std::uint64_t m_nextDocumentId{ 1 };
    };
}
