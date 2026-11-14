#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "ActivationArguments.h"
#include "AppSettings.h"
#include "CommandLine.h"
#include "MuPdfRuntime.h"

#include <cstdlib>
#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Dispatching.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
    namespace dispatching = winrt::Microsoft::UI::Dispatching;

    constexpr wchar_t MainInstanceKey[] = L"main";
    constexpr int StartupFailedExitCode = 1;
    constexpr int UnhandledErrorExitCode = 2;

    std::shared_ptr<urpdf::MuPdfRuntime> CreateMuPdfRuntime()
    {
#ifdef _DEBUG
        if (::GetEnvironmentVariableW(L"URPDF_SIMULATE_MUPDF_INIT_FAILURE", nullptr, 0) > 0)
        {
            throw std::runtime_error("Simulated MuPDF initialization failure.");
        }
#endif
        return std::make_shared<urpdf::MuPdfRuntime>();
    }

    void ReportStartupError() noexcept
    {
        std::wstring details;
        try { throw; }
        catch (std::exception const& error) { details = winrt::to_hstring(error.what()); }
        catch (...) {}

        std::wstring message = L"urPDF could not start because the PDF engine failed to initialize.";
        if (!details.empty()) { message += L"\n\n" + details; }
        OutputDebugStringW((L"urPDF: " + message + L"\n").c_str());
        ::MessageBoxW(nullptr, message.c_str(), L"urPDF", MB_OK | MB_ICONERROR);
    }
    
    class RedirectedActivations
    {
    public:
        using Handler = std::function<void(lifecycle::AppActivationArguments const&)>;

        // Any thread.
        void Post(lifecycle::AppActivationArguments const& args)
        {
            dispatching::DispatcherQueue dispatcher{ nullptr };
            {
                std::scoped_lock lock{ m_mutex };
                m_pending.push_back(args);
                dispatcher = m_dispatcher;
            }
            if (dispatcher) { dispatcher.TryEnqueue([this] { Drain(); }); }
        }

        // UI thread.
        void Attach(dispatching::DispatcherQueue const& dispatcher, Handler handler)
        {
            m_handler = std::move(handler);
            {
                std::scoped_lock lock{ m_mutex };
                m_dispatcher = dispatcher;
            }
            Drain();
        }

        // UI thread. Activations queued now or posted later stay pending until TakePending().
        void Detach() noexcept
        {
            {
                std::scoped_lock lock{ m_mutex };
                m_dispatcher = nullptr;
            }
            m_handler = nullptr;
        }

        // Any thread.
        std::deque<lifecycle::AppActivationArguments> TakePending()
        {
            std::scoped_lock lock{ m_mutex };
            return std::exchange(m_pending, {});
        }

    private:
        void Drain()
        {
            while (m_handler)
            {
                lifecycle::AppActivationArguments args{ nullptr };
                {
                    std::scoped_lock lock{ m_mutex };
                    if (m_pending.empty()) { return; }
                    args = std::move(m_pending.front());
                    m_pending.pop_front();
                }
                auto handler = m_handler;
                try { handler(args); }
                catch (...) { OutputDebugStringW(L"urPDF: failed to handle a redirected activation.\n"); }
            }
        }

        std::mutex m_mutex;
        std::deque<lifecycle::AppActivationArguments> m_pending;
        dispatching::DispatcherQueue m_dispatcher{ nullptr };
        Handler m_handler;
    };

    RedirectedActivations& Activations()
    {
        static RedirectedActivations activations;
        return activations;
    }

    std::optional<std::wstring> CurrentExecutablePath()
    {
        std::wstring executable(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) { return std::nullopt; }
        executable.resize(length);
        return executable;
    }

    void StartUrPdfProcess(std::wstring const& executable, std::wstring_view document)
    {
        std::wstring commandLine = L"\"" + executable + L"\"";
        if (!document.empty())
        {
            commandLine += L" \"";
            commandLine += document;
            commandLine += L"\"";
        }
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        winrt::check_bool(::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startupInfo, &processInfo));
        ::CloseHandle(processInfo.hThread);
        ::CloseHandle(processInfo.hProcess);
    }

    bool RedirectActivationToMainInstance(int argc, wchar_t const* const* argv)
    {
        auto mainInstance = lifecycle::AppInstance::FindOrRegisterForKey(MainInstanceKey);
        if (mainInstance.IsCurrent()) { return false; }
        if (urpdf::HasRelativeDocumentPathArgument(argc, argv))
        {
            auto path = urpdf::DocumentPathArgument(argc, argv);
            auto executable = CurrentExecutablePath();
            if (!path || !executable) { return false; }
            ::AllowSetForegroundWindow(mainInstance.ProcessId());
            StartUrPdfProcess(*executable, *path);
            return true;
        }

        auto activationArgs = lifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
        ::AllowSetForegroundWindow(mainInstance.ProcessId());

        winrt::handle redirected{ ::CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        winrt::check_bool(static_cast<bool>(redirected));
        bool succeeded = false;
        std::thread worker([&]
        {
            try
            {
                mainInstance.RedirectActivationToAsync(activationArgs).get();
                succeeded = true;
            }
            catch (...)
            {
            }
            ::SetEvent(redirected.get());
        });
        HANDLE handle = redirected.get();
        DWORD index = 0;
        ::CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, &handle, &index);
        worker.join();
        return succeeded;
    }

    void UnregisterMainInstance() noexcept
    {
        try { lifecycle::AppInstance::GetCurrent().UnregisterKey(); } catch (...) {}
        Activations().Detach();
    }

    void StopReceivingActivations(winrt::event_token& token) noexcept
    {
        UnregisterMainInstance();
        if (!token) { return; }
        try { lifecycle::AppInstance::GetCurrent().Activated(token); } catch (...) {}
        token = {};
    }

    std::vector<urpdf::WindowCreationOptions> DocumentsFromActivation(lifecycle::AppActivationArguments const& args, bool isInitialLaunch)
    {
        std::vector<urpdf::WindowCreationOptions> documents;
        if (args.Kind() == lifecycle::ExtendedActivationKind::File)
        {
            if (auto fileArgs = args.Data().try_as<winrt::Windows::ApplicationModel::Activation::IFileActivatedEventArgs>())
            {
                for (auto const& item : fileArgs.Files())
                {
                    if (auto file = item.try_as<winrt::Windows::Storage::StorageFile>())
                    {
                        documents.emplace_back().file = file;
                    }
                }
            }
            return documents;
        }

        std::optional<std::wstring> path;
        if (isInitialLaunch)
        {
            path = urpdf::DocumentPathArgument(__argc, __wargv);
        }
        else if (args.Kind() == lifecycle::ExtendedActivationKind::Launch)
        {
            if (auto launchArgs = args.Data().try_as<winrt::Windows::ApplicationModel::Activation::ILaunchActivatedEventArgs>())
            {
                path = urpdf::DocumentPathFromLaunchArguments(launchArgs.Arguments());
            }
        }
        if (path) { documents.emplace_back().filePath = winrt::hstring{ *path }; }
        return documents;
    }

    void RelaunchUnhandledActivations() noexcept
    {
        try
        {
            auto pending = Activations().TakePending();
            if (pending.empty()) { return; }

            auto executable = CurrentExecutablePath();
            if (!executable) { return; }

            for (auto const& args : pending)
            {
                try
                {
                    auto documents = DocumentsFromActivation(args, false);
                    if (documents.empty())
                    {
                        StartUrPdfProcess(*executable, {});
                        continue;
                    }
                    for (auto const& document : documents)
                    {
                        StartUrPdfProcess(*executable, document.file ? document.file.Path() : document.filePath);
                    }
                }
                catch (...)
                {
                    OutputDebugStringW(L"urPDF: failed to relaunch a redirected activation.\n");
                }
            }
        }
        catch (...)
        {
            OutputDebugStringW(L"urPDF: failed to relaunch redirected activations.\n");
        }
    }
}

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

        auto activationArgs = lifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
        OpenActivationDocuments(DocumentsFromActivation(activationArgs, true));

        Activations().Attach(Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread(),
            [weakThis = get_weak()](lifecycle::AppActivationArguments const& args)
            {
                if (auto self = weakThis.get())
                {
                    self->OpenActivationDocuments(DocumentsFromActivation(args, false));
                }
            });
    }

    void App::OpenActivationDocuments(std::vector<::urpdf::WindowCreationOptions> const& documents)
    {
        if (::urpdf::AppSettings::Instance().Get().openFilesInExistingWindow)
        {
            auto next = documents.begin();
            auto window = LastActiveWindow();
            if (window)
            {
                get_self<MainWindow>(window)->BringToFront();
            }
            else if (next != documents.end())
            {
                window = CreateWindow(*next++);
            }
            if (window)
            {
                auto target = get_self<MainWindow>(window);
                for (; next != documents.end(); ++next) { target->OpenDocument(*next); }
                return;
            }
        }

        if (documents.empty())
        {
            CreateWindow();
            return;
        }
        for (auto const& document : documents) { CreateWindow(document); }
    }

    void App::WindowActivated(winrt::urPDF::MainWindow const& window)
    {
        auto entry = std::find(m_windows.begin(), m_windows.end(), window);
        if (entry != m_windows.end()) { m_lastActiveWindow = winrt::make_weak(*entry); }
    }

    winrt::urPDF::MainWindow App::LastActiveWindow() const
    {
        auto isAvailable = [this](Window const& window)
        {
            return std::find(m_windows.begin(), m_windows.end(), window) != m_windows.end()
                && std::find(m_pendingWindowCloses.begin(), m_pendingWindowCloses.end(), window.AppWindow().Id()) == m_pendingWindowCloses.end();
        };

        if (auto window = m_lastActiveWindow.get(); window && isAvailable(window))
        {
            return window.as<winrt::urPDF::MainWindow>();
        }

        for (auto window = m_windows.rbegin(); window != m_windows.rend(); ++window)
        {
            if (isAvailable(*window)) { return window->as<winrt::urPDF::MainWindow>(); }
        }
        return nullptr;
    }

    winrt::urPDF::MainWindow App::CreateWindow(::urpdf::WindowCreationOptions const& options)
    {
        if (!m_muPdfRuntime) { throw hresult_error(RO_E_CLOSED); }
        auto window = make<MainWindow>(options);
        window.Closed([weakThis = get_weak(), id = window.AppWindow().Id()](IInspectable const& sender, WindowEventArgs const&)
        {
            if (auto self = weakThis.get())
            {
                auto closedWindow = sender.as<Window>();
                std::erase(self->m_windows, closedWindow);
                if (auto lastActive = self->m_lastActiveWindow.get(); !lastActive || lastActive == closedWindow)
                {
                    self->m_lastActiveWindow = nullptr;
                }
                self->m_windowsInMoveSize.erase(id.Value);
                std::erase(self->m_pendingWindowCloses, id);
                self->SchedulePendingWindowCloses();
                if (self->m_windows.empty() && self->m_muPdfRuntime)
                {
                    UnregisterMainInstance();
                    auto runtime = std::move(self->m_muPdfRuntime);
                    runtime->Work().Stop();
                }
            }
        });
        if (options.deferPresentation) { get_self<MainWindow>(window)->PrepareForDeferredPresentation(); }
        m_windows.push_back(window);
        if (!options.deferPresentation) { window.Activate(); }
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

    void App::RequestCloseEmptyWindow(Microsoft::UI::WindowId const& id)
    {
        if (std::find(m_pendingWindowCloses.begin(), m_pendingWindowCloses.end(), id) == m_pendingWindowCloses.end())
        {
            m_pendingWindowCloses.push_back(id);
        }
        SchedulePendingWindowCloses();
    }

    void App::RefreshTabDragRegions()
    {
        for (auto const& window : m_windows)
        {
            get_self<MainWindow>(window.as<winrt::urPDF::MainWindow>())->UpdateTitleBarPassthroughRegions();
        }
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

    std::shared_ptr<urpdf::MuPdfRuntime> runtime;
    try
    {
        runtime = CreateMuPdfRuntime();
    }
    catch (...)
    {
        ReportStartupError();
        return StartupFailedExitCode;
    }

    bool redirected = false;
    try { redirected = RedirectActivationToMainInstance(__argc, __wargv); }
    catch (...) { OutputDebugStringW(L"urPDF: failed to redirect activation to the main instance.\n"); }
    if (redirected) { return 0; }

    winrt::event_token activatedToken{};
    try
    {
        if (lifecycle::AppInstance::FindOrRegisterForKey(MainInstanceKey).IsCurrent())
        {
            activatedToken = lifecycle::AppInstance::GetCurrent().Activated(
                [](auto const&, lifecycle::AppActivationArguments const& args) { Activations().Post(args); });
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"urPDF: failed to register as the main instance.\n");
    }

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
        OutputDebugStringW(L"urPDF: the application stopped after an unhandled error.\n");
        StopReceivingActivations(activatedToken);
        runtime->Work().Stop();
        runtime->Work().Wait();
        return UnhandledErrorExitCode;
    }
    StopReceivingActivations(activatedToken);
    RelaunchUnhandledActivations();
    runtime->Work().Stop();
    runtime->Work().Wait();

    return 0;
}
