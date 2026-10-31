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
        window = make<MainWindow>();
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
