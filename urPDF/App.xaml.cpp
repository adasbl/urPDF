#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "CommandLine.h"

#include <cstdlib>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::urPDF::implementation
{
    App::App()
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
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        DispatcherShutdownMode(Microsoft::UI::Xaml::DispatcherShutdownMode::OnLastWindowClose);

        auto window = make<MainWindow>();
        window.Closed([weakThis = get_weak()](IInspectable const& sender, WindowEventArgs const&)
        {
            if (auto self = weakThis.get())
            {
                std::erase(self->m_windows, sender.as<Window>());
            }
        });
        m_windows.push_back(window);
        window.Activate();
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
