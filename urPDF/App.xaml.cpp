#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "CommandLine.h"
#include "MuPdfRuntime.h"

#include <cstdlib>
#include <algorithm>
#include <utility>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Dispatching.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::urPDF::implementation
{
    App::App()
        : App(std::make_shared<::urpdf::MuPdfRuntime>())
    {
    }

    App::App(std::shared_ptr<::urpdf::MuPdfRuntime> runtime)
        : m_muPdfRuntime(std::move(runtime))
    {

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    std::shared_ptr<::urpdf::MuPdfRuntime> App::MuPdfRuntime() const noexcept
    {
        return m_muPdfRuntime;
    }

    std::uint64_t App::AllocateDocumentId() noexcept
    {
        return m_nextDocumentId++;
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        DispatcherShutdownMode(Microsoft::UI::Xaml::DispatcherShutdownMode::OnLastWindowClose);

        ::urpdf::WindowCreationOptions options;
        auto activationArgs = winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
        if (activationArgs.Kind() == winrt::Microsoft::Windows::AppLifecycle::ExtendedActivationKind::File)
        {
            auto fileArgs = activationArgs.Data().try_as<winrt::Windows::ApplicationModel::Activation::IFileActivatedEventArgs>();
            if (fileArgs && fileArgs.Files().Size() > 0)
            {
                options.file = fileArgs.Files().GetAt(0).try_as<winrt::Windows::Storage::StorageFile>();
            }
        }
        else if (auto path = ::urpdf::DocumentPathArgument(__argc, __wargv))
        {
            options.filePath = *path;
        }

        CreateWindow(options);
    }

    winrt::urPDF::MainWindow App::CreateWindow(::urpdf::WindowCreationOptions const& options)
    {
        if (!m_muPdfRuntime) { throw hresult_error(RO_E_CLOSED); }
        auto window = make<MainWindow>(options);
        window.Closed([weakThis = get_weak(), id = window.AppWindow().Id()](IInspectable const& sender, WindowEventArgs const&)
        {
            if (auto self = weakThis.get())
            {
                std::erase(self->m_windows, sender.as<Window>());
                std::erase_if(self->m_tearOutWindows, [&](auto const& entry) { return entry.second == id; });
                self->m_windowsInMoveSize.erase(id.Value);
                std::erase(self->m_pendingWindowCloses, id);
                self->SchedulePendingWindowCloses();
                if (self->m_windows.empty() && self->m_muPdfRuntime)
                {
                    auto runtime = std::move(self->m_muPdfRuntime);
                    runtime->Work().Stop();
                }
            }
        });
        m_windows.push_back(window);
        window.Activate();
        return window;
    }

    winrt::urPDF::MainWindow App::FindWindow(Microsoft::UI::WindowId const& id) const
    {
        for (auto const& window : m_windows)
        {
            if (window.AppWindow().Id() == id) { return window.as<winrt::urPDF::MainWindow>(); }
        }
        return nullptr;
    }

    winrt::urPDF::MainWindow App::FindWindowForTab(Microsoft::UI::Xaml::Controls::TabViewItem const& tab) const
    {
        if (!tab) { return nullptr; }
        for (auto const& window : m_windows)
        {
            auto mainWindow = window.as<winrt::urPDF::MainWindow>();
            uint32_t index;
            if (get_self<MainWindow>(mainWindow)->MainTabView().TabItems().IndexOf(tab, index)) { return mainWindow; }
        }
        return nullptr;
    }

    void App::TrackTearOutWindow(Microsoft::UI::Xaml::Controls::TabViewItem const& tab, Microsoft::UI::WindowId const& id)
    {
        if (tab && FindWindow(id)) { m_tearOutWindows.insert_or_assign(tab, id); }
    }

    winrt::urPDF::MainWindow App::FindTearOutWindow(Microsoft::UI::Xaml::Controls::TabViewItem const& tab) const
    {
        auto entry = m_tearOutWindows.find(tab);
        return entry != m_tearOutWindows.end() ? FindWindow(entry->second) : nullptr;
    }

    void App::RequestCloseEmptyWindow(Microsoft::UI::WindowId const& id)
    {
        if (std::find(m_pendingWindowCloses.begin(), m_pendingWindowCloses.end(), id) == m_pendingWindowCloses.end())
        {
            m_pendingWindowCloses.push_back(id);
        }
        SchedulePendingWindowCloses();
    }

    void App::WindowMoveSizeEntered(Microsoft::UI::WindowId const& id)
    {
        m_windowsInMoveSize.insert(id.Value);
    }

    void App::WindowMoveSizeExited(Microsoft::UI::WindowId const& id)
    {
        m_windowsInMoveSize.erase(id.Value);
        SchedulePendingWindowCloses();
    }

    void App::SchedulePendingWindowCloses()
    {
        if (m_windowCloseQueued || m_pendingWindowCloses.empty() || !m_windowsInMoveSize.empty()) { return; }
        auto dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_windowCloseQueued = dispatcher && dispatcher.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [weakThis = get_weak()]
            {
                auto self = weakThis.get();
                if (!self) { return; }
                self->m_windowCloseQueued = false;
                if (!self->m_windowsInMoveSize.empty()) { return; }
                auto pending = std::exchange(self->m_pendingWindowCloses, {});
                for (auto const& id : pending)
                {
                    if (auto window = self->FindWindow(id))
                    {
                        get_self<MainWindow>(window)->CloseIfEmpty();
                    }
                }
            });
    }

    void App::ApplySettingsToWindows()
    {
        auto windows = m_windows;
        for (auto const& window : windows)
        {
            get_self<MainWindow>(window.as<winrt::urPDF::MainWindow>())->LoadSettings();
        }
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    if (auto exitCode = urpdf::RunCommandLine(__argc, __wargv))
    {
        return *exitCode;
    }

    auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
    try
    {
        Application::Start(
            [&runtime](auto&&)
            {
                ::winrt::make<::winrt::urPDF::implementation::App>(runtime);
            });
    }
    catch (...)
    {
        runtime->Work().Stop();
        runtime->Work().Wait();
        throw;
    }
    runtime->Work().Stop();
    runtime->Work().Wait();

    return 0;
}
