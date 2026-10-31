#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "CommandLine.h"
#include "MuPdfRuntime.h"

#include <cstdlib>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::urPDF::implementation
{
    App::App()
        : m_muPdfRuntime(std::make_shared<::urpdf::MuPdfRuntime>())
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
        auto window = make<MainWindow>(options);
        window.Closed([weakThis = get_weak()](IInspectable const& sender, WindowEventArgs const&)
        {
            if (auto self = weakThis.get())
            {
                std::erase(self->m_windows, sender.as<Window>());
            }
        });
        m_windows.push_back(window);
        window.Activate();
        return window;
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

    Application::Start(
        [](auto&&)
        {
            ::winrt::make<::winrt::urPDF::implementation::App>();
        });

    return 0;
}
