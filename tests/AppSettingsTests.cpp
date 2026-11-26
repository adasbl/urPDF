#include "CppUnitTest.h"
#include "../urPDF/AppSettings.h"
#include "../urPDF/WindowPlacement.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    using Values = std::map<std::wstring, std::wstring, std::less<>>;

    class InMemorySettingsStore final : public urpdf::ISettingsStore
    {
    public:
        explicit InMemorySettingsStore(std::shared_ptr<Values> values)
            : m_values(std::move(values))
        {
        }

        std::optional<std::wstring> Read(std::wstring_view key) const override
        {
            if (auto it = m_values->find(key); it != m_values->end()) { return it->second; }
            return std::nullopt;
        }

        void Write(std::wstring_view key, std::wstring_view value) override
        {
            m_values->insert_or_assign(std::wstring{ key }, std::wstring{ value });
        }

    private:
        std::shared_ptr<Values> m_values;
    };

    class FailingSettingsStore final : public urpdf::ISettingsStore
    {
    public:
        std::optional<std::wstring> Read(std::wstring_view) const override { return std::nullopt; }
        void Write(std::wstring_view, std::wstring_view) override { throw std::runtime_error{ "write failed" }; }
    };

    urpdf::AppSettings MakeSettings(std::shared_ptr<Values> values = std::make_shared<Values>())
    {
        return urpdf::AppSettings{ std::make_unique<InMemorySettingsStore>(std::move(values)) };
    }

    void AssertSettings(
        urpdf::AppSettings const& settings,
        urpdf::AppTheme theme,
        urpdf::Backdrop backdrop,
        bool openFilesInExistingWindow = false,
        bool useWindowsAccentColorForTextSelection = true,
        bool askToSetAsDefaultPdfViewer = true)
    {
        const auto data = settings.Get();
        Assert::IsTrue(data.theme == theme);
        Assert::IsTrue(data.backdrop == backdrop);
        Assert::AreEqual(openFilesInExistingWindow, data.openFilesInExistingWindow);
        Assert::AreEqual(useWindowsAccentColorForTextSelection, data.useWindowsAccentColorForTextSelection);
        Assert::AreEqual(askToSetAsDefaultPdfViewer, data.askToSetAsDefaultPdfViewer);
    }

    void AssertWindowSettings(urpdf::AppSettings const& settings, urpdf::WindowState const& state,
        urpdf::WindowStartupMode mode)
    {
        const auto data = settings.Get();
        Assert::IsTrue(data.windowState == state);
        Assert::IsTrue(data.windowStartupMode == mode);
    }

    TEST_CLASS(AppSettingsTests)
    {
    public:
        TEST_METHOD(WindowPositionAndMonitorPersistAndExport)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            const urpdf::WindowState state{ urpdf::WindowSize{ 1000, 740 }, true,
                urpdf::WindowPosition{ 120, -20, L"\\\\.\\DISPLAY2" } };
            settings.SetWindowState(state);
            auto reloaded = MakeSettings(values);
            AssertWindowSettings(reloaded, state, urpdf::WindowStartupMode::RememberLastState);

            auto imported = MakeSettings();
            Assert::IsTrue(imported.ImportJson(settings.ExportJson()));
            AssertWindowSettings(imported, state, urpdf::WindowStartupMode::RememberLastState);
        }

        TEST_METHOD(LegacyWindowStateStillLoadsWithoutPosition)
        {
            auto values = std::make_shared<Values>(Values{ { L"WindowState",
                LR"({"normalWidth":1200,"normalHeight":800,"maximized":true})" } });
            auto settings = MakeSettings(values);
            AssertWindowSettings(settings, { urpdf::WindowSize{ 1200, 800 }, true },
                urpdf::WindowStartupMode::RememberLastState);
        }

        TEST_METHOD(InvalidPositionImportsPreserveSavedSettings)
        {
            const wchar_t* invalidPositions[] = {
                LR"({"normalOffsetX":100,"monitorDevice":"DISPLAY2"})",
                LR"({"normalOffsetX":100,"normalOffsetY":200})",
                LR"({"normalOffsetX":100,"normalOffsetY":200,"monitorDevice":""})",
                LR"({"normalOffsetX":100,"normalOffsetY":200,"monitorDevice":false})",
                LR"({"normalOffsetX":"100","normalOffsetY":200,"monitorDevice":"DISPLAY2"})",
                LR"({"normalOffsetX":0.5,"normalOffsetY":200,"monitorDevice":"DISPLAY2"})",
                LR"({"normalOffsetX":2147483648,"normalOffsetY":200,"monitorDevice":"DISPLAY2"})",
                LR"({"normalOffsetX":100,"normalOffsetY":-2147483649,"monitorDevice":"DISPLAY2"})",
            };
            for (auto position : invalidPositions)
            {
                auto values = std::make_shared<Values>();
                auto settings = MakeSettings(values);
                const urpdf::WindowState state{ urpdf::WindowSize{ 1000, 740 }, false,
                    urpdf::WindowPosition{ 120, 80, L"DISPLAY1" } };
                settings.SetWindowState(state);
                const auto previousValues = *values;
                std::wstring json{ LR"({"schemaVersion":1,"WindowState":{"maximized":false,"normalWidth":1000,"normalHeight":740,)" };
                json.append(position + 1);
                json.append(L"}");
                Assert::IsFalse(settings.ImportJson(json), json.c_str());
                AssertWindowSettings(settings, state, urpdf::WindowStartupMode::RememberLastState);
                Assert::IsTrue(*values == previousValues);
            }
        }

        TEST_METHOD(StartupPositionFollowsMonitorWhenDisplayLayoutChanges)
        {
            urpdf::SettingsData settings;
            settings.windowState = { urpdf::WindowSize{ 900, 600 }, true,
                urpdf::WindowPosition{ 120, 80, L"DISPLAY2" } };
            const auto left = urpdf::ResolveWindowStartupPlacement(
                settings, { 100, 100, 1124, 868 }, { -1920, 40, 0, 1080 });
            const RECT expectedLeft{ -1800, 120, -900, 720 };
            Assert::IsTrue(EqualRect(&left.normalBounds, &expectedLeft) != FALSE);
            Assert::IsTrue(left.maximized);

            const auto right = urpdf::ResolveWindowStartupPlacement(
                settings, { 100, 100, 1124, 868 }, { 1920, 0, 3840, 1040 });
            const RECT expectedRight{ 2040, 80, 2940, 680 };
            Assert::IsTrue(EqualRect(&right.normalBounds, &expectedRight) != FALSE);
        }

        TEST_METHOD(MissingMonitorAndOffscreenPositionFallBackToVisibleBounds)
        {
            Assert::IsTrue(urpdf::FindWindowMonitor(L"urPDF nonexistent test display") == nullptr);
            urpdf::SettingsData settings;
            settings.windowState = { urpdf::WindowSize{ 1200, 800 }, false,
                urpdf::WindowPosition{ 2000, -100, L"Disconnected" } };
            const auto placement = urpdf::ResolveWindowStartupPlacement(
                settings, { 100, 100, 1124, 868 }, { 0, 40, 1024, 768 });
            const RECT expected{ 0, 40, 1024, 768 };
            Assert::IsTrue(EqualRect(&placement.normalBounds, &expected) != FALSE);
        }

        TEST_METHOD(MonitorLookupFindsAnAttachedDisplay)
        {
            const auto primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFOEXW monitor{};
            monitor.cbSize = sizeof(monitor);
            Assert::IsTrue(GetMonitorInfoW(primary, &monitor) != FALSE);
            Assert::IsTrue(urpdf::FindWindowMonitor(monitor.szDevice) == primary);
        }

        TEST_METHOD(PositionTrackingAccountsForTaskbarAndNegativeMonitorCoordinates)
        {
            MONITORINFOEXW monitor{};
            monitor.rcMonitor = { -1920, -1080, 0, 0 };
            monitor.rcWork = { -1880, -1040, 0, 0 };
            wcscpy_s(monitor.szDevice, L"DISPLAY2");
            WINDOWPLACEMENT placement{};
            placement.showCmd = SW_SHOWNORMAL;
            placement.rcNormalPosition = { -1800, -1000, -900, -400 };
            urpdf::WindowState state{ urpdf::WindowSize{ 900, 600 }, false };
            urpdf::UpdateWindowPositionFromPlacement(state, placement, monitor);
            Assert::IsTrue(state.normalPosition == urpdf::WindowPosition{ 120, 80, L"DISPLAY2" });

            // Tool-window placement is already in screen coordinates.
            OffsetRect(&placement.rcNormalPosition, 40, 40);
            urpdf::UpdateWindowPositionFromPlacement(state, placement, monitor, true);
            Assert::IsTrue(state.normalPosition == urpdf::WindowPosition{ 120, 80, L"DISPLAY2" });
        }

        TEST_METHOD(MaximizeAndMinimizePreservePositionWhenMovingMonitors)
        {
            urpdf::WindowState state{ urpdf::WindowSize{ 900, 600 }, false,
                urpdf::WindowPosition{ 120, 80, L"DISPLAY1" } };
            MONITORINFOEXW monitor{};
            wcscpy_s(monitor.szDevice, L"DISPLAY2");
            WINDOWPLACEMENT placement{};
            placement.showCmd = SW_SHOWMAXIMIZED;
            urpdf::UpdateWindowPositionFromPlacement(state, placement, monitor);
            Assert::IsTrue(state.normalPosition == urpdf::WindowPosition{ 120, 80, L"DISPLAY2" });

            placement.showCmd = SW_SHOWMINIMIZED;
            placement.rcNormalPosition = { -32000, -32000, -31840, -31972 };
            urpdf::UpdateWindowPositionFromPlacement(state, placement, monitor);
            Assert::IsTrue(state.normalPosition == urpdf::WindowPosition{ 120, 80, L"DISPLAY2" });
        }

        TEST_METHOD(StartupWithoutSavedSizeKeepsNativeDefaultBounds)
        {
            const RECT defaultBounds{ 100, 100, 1124, 868 };
            const auto placement = urpdf::ResolveWindowStartupPlacement({}, defaultBounds, { 0, 0, 1920, 1040 });
            Assert::IsTrue(EqualRect(&placement.normalBounds, &defaultBounds) != FALSE);
            Assert::IsFalse(placement.maximized);
        }

        TEST_METHOD(StartupPreferencesOverrideSavedMaximizedState)
        {
            for (bool maximized : { false, true })
            {
                for (auto mode : { urpdf::WindowStartupMode::RememberLastState,
                    urpdf::WindowStartupMode::Maximized, urpdf::WindowStartupMode::Normal })
                {
                    urpdf::SettingsData settings;
                    settings.windowState = { urpdf::WindowSize{ 900, 600 }, maximized };
                    settings.windowStartupMode = mode;
                    const auto placement = urpdf::ResolveWindowStartupPlacement(
                        settings, { 100, 100, 1124, 868 }, { 0, 0, 1920, 1040 });
                    const RECT expectedBounds{ 100, 100, 1000, 700 };
                    Assert::IsTrue(EqualRect(&placement.normalBounds, &expectedBounds) != FALSE);
                    Assert::IsTrue(placement.maximized == (mode == urpdf::WindowStartupMode::Maximized
                        || (mode == urpdf::WindowStartupMode::RememberLastState && maximized)));
                }
            }
        }

        TEST_METHOD(StartupShrinksOversizedWindowToMonitorWorkArea)
        {
            urpdf::SettingsData settings;
            settings.windowState = { urpdf::WindowSize{ 5000, 3000 }, true };
            const RECT workArea{ -1920, 40, 0, 1080 };
            const auto placement = urpdf::ResolveWindowStartupPlacement(settings, { -1800, 100, -800, 800 }, workArea);
            Assert::IsTrue(EqualRect(&placement.normalBounds, &workArea) != FALSE);
            Assert::IsTrue(placement.maximized);
        }

        TEST_METHOD(StartupKeepsResizedWindowInsideWorkArea)
        {
            urpdf::SettingsData settings;
            settings.windowState.normalSize = urpdf::WindowSize{ 900, 600 };
            const auto placement = urpdf::ResolveWindowStartupPlacement(
                settings, { 1800, 900, 2824, 1668 }, { 0, 40, 1920, 1040 });
            const RECT expectedBounds{ 1020, 440, 1920, 1040 };
            Assert::IsTrue(EqualRect(&placement.normalBounds, &expectedBounds) != FALSE);

            settings.windowState.normalSize = urpdf::WindowSize{ 5000, 600 };
            const auto wide = urpdf::ResolveWindowStartupPlacement(
                settings, { 100, 100, 1124, 868 }, { 0, 40, 1920, 1040 });
            const RECT expectedWideBounds{ 0, 100, 1920, 700 };
            Assert::IsTrue(EqualRect(&wide.normalBounds, &expectedWideBounds) != FALSE);
        }

        TEST_METHOD(StartupInvalidSizesUseDefaults)
        {
            const RECT defaultBounds{ 100, 100, 1124, 868 };
            for (auto size : { urpdf::WindowSize{ 0, 800 }, urpdf::WindowSize{ 1200, -1 } })
            {
                urpdf::SettingsData settings;
                settings.windowState.normalSize = size;
                const auto placement = urpdf::ResolveWindowStartupPlacement(settings, defaultBounds, { 0, 0, 1920, 1040 });
                Assert::IsTrue(EqualRect(&placement.normalBounds, &defaultBounds) != FALSE);
            }

            const auto fallback = urpdf::ResolveWindowStartupPlacement({}, {}, { 0, 0, 800, 600 });
            const RECT expectedFallback{ 0, 0, 800, 600 };
            Assert::IsTrue(EqualRect(&fallback.normalBounds, &expectedFallback) != FALSE);
        }

        TEST_METHOD(StartupWithoutMonitorDataKeepsNativeBoundsAndPreference)
        {
            urpdf::SettingsData settings;
            settings.windowStartupMode = urpdf::WindowStartupMode::Maximized;
            const RECT defaultBounds{ 100, 100, 1124, 868 };
            const auto placement = urpdf::ResolveWindowStartupPlacement(settings, defaultBounds, {});
            Assert::IsTrue(EqualRect(&placement.normalBounds, &defaultBounds) != FALSE);
            Assert::IsTrue(placement.maximized);
        }

        TEST_METHOD(WindowTrackingPreservesNormalSizeAcrossMaximizeAndMinimize)
        {
            urpdf::WindowState state;
            WINDOWPLACEMENT placement{};
            placement.showCmd = SW_SHOWNORMAL;
            placement.rcNormalPosition = { 100, 200, 1300, 1000 };
            urpdf::UpdateWindowStateFromPlacement(state, placement);

            placement.showCmd = SW_SHOWMAXIMIZED;
            placement.rcNormalPosition = { 0, 0, 1920, 1080 };
            urpdf::UpdateWindowStateFromPlacement(state, placement);
            Assert::IsTrue(state == urpdf::WindowState{ urpdf::WindowSize{ 1200, 800 }, true });

            placement.showCmd = SW_SHOWMINIMIZED;
            placement.flags = WPF_RESTORETOMAXIMIZED;
            placement.rcNormalPosition = { -32000, -32000, -31840, -31972 };
            urpdf::UpdateWindowStateFromPlacement(state, placement);
            Assert::IsTrue(state == urpdf::WindowState{ urpdf::WindowSize{ 1200, 800 }, true });

            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            settings.SetWindowStartupMode(urpdf::WindowStartupMode::Normal);
            settings.SetWindowState(state);
            auto reloaded = MakeSettings(values);
            AssertWindowSettings(reloaded, state, urpdf::WindowStartupMode::Normal);
        }

        TEST_METHOD(WindowTrackingUpdatesSizeAfterRestoreAndResize)
        {
            urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            WINDOWPLACEMENT placement{};
            placement.showCmd = SW_SHOWNORMAL;
            placement.rcNormalPosition = { -1200, -100, -300, 500 };
            urpdf::UpdateWindowStateFromPlacement(state, placement);
            const urpdf::WindowState expected{ urpdf::WindowSize{ 900, 600 }, false };
            Assert::IsTrue(state == expected);

            placement.showCmd = SW_SHOWMINIMIZED;
            placement.rcNormalPosition = {};
            urpdf::UpdateWindowStateFromPlacement(state, placement);
            Assert::IsTrue(state == expected);
        }

        TEST_METHOD(ClosingMinimizedWindowPersistsItsRestoreState)
        {
            for (bool wasMaximized : { false, true })
            {
                const DWORD style = WS_OVERLAPPEDWINDOW | WS_MINIMIZE | (wasMaximized ? WS_MAXIMIZE : 0);
                HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"urPDF window state test",
                    style, 100, 100, 900, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Assert::IsTrue(window != nullptr);

                WINDOWPLACEMENT placement{};
                placement.length = sizeof(placement);
                const bool captured = GetWindowPlacement(window, &placement) != FALSE;
                const bool visible = IsWindowVisible(window) != FALSE;
                DestroyWindow(window);
                Assert::IsTrue(captured);
                Assert::IsFalse(visible);
                Assert::IsTrue(placement.showCmd == SW_SHOWMINIMIZED);

                urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, !wasMaximized };
                urpdf::UpdateWindowStateFromPlacement(state, placement);
                Assert::IsTrue(state == urpdf::WindowState{ urpdf::WindowSize{ 1200, 800 }, wasMaximized });

                for (auto mode : { urpdf::WindowStartupMode::RememberLastState,
                    urpdf::WindowStartupMode::Maximized, urpdf::WindowStartupMode::Normal })
                {
                    auto values = std::make_shared<Values>();
                    auto settings = MakeSettings(values);
                    settings.SetWindowStartupMode(mode);
                    settings.SetWindowState(state);
                    auto reloaded = MakeSettings(values);
                    AssertWindowSettings(reloaded, state, mode);

                    const auto startup = urpdf::ResolveWindowStartupPlacement(
                        reloaded.Get(), { 100, 100, 1000, 700 }, { 0, 0, 1920, 1040 });
                    const RECT expectedBounds{ 100, 100, 1300, 900 };
                    Assert::IsTrue(EqualRect(&startup.normalBounds, &expectedBounds) != FALSE);
                    Assert::IsTrue(startup.maximized == (mode == urpdf::WindowStartupMode::Maximized
                        || (mode == urpdf::WindowStartupMode::RememberLastState && wasMaximized)));
                }

                urpdf::WindowState withoutSavedSize;
                urpdf::UpdateWindowStateFromPlacement(withoutSavedSize, placement);
                Assert::IsTrue(withoutSavedSize == urpdf::WindowState{ urpdf::WindowSize{ 900, 600 }, wasMaximized });
            }
        }

        TEST_METHOD(MinimizedWindowWithInvalidRestoreSizeKeepsSavedSize)
        {
            for (bool wasMaximized : { false, true })
            {
                urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, !wasMaximized };
                WINDOWPLACEMENT placement{};
                placement.showCmd = SW_SHOWMINIMIZED;
                placement.flags = WPF_ASYNCWINDOWPLACEMENT | (wasMaximized ? WPF_RESTORETOMAXIMIZED : 0);
                urpdf::UpdateWindowStateFromPlacement(state, placement);
                Assert::IsTrue(state == urpdf::WindowState{ urpdf::WindowSize{ 1200, 800 }, wasMaximized });

                urpdf::WindowState withoutSavedSize;
                urpdf::UpdateWindowStateFromPlacement(withoutSavedSize, placement);
                Assert::IsFalse(withoutSavedSize.normalSize.has_value());
                Assert::IsTrue(withoutSavedSize.maximized == wasMaximized);
            }
        }

        TEST_METHOD(WindowTrackingIgnoresInvalidNormalSizes)
        {
            const urpdf::WindowState expected{ urpdf::WindowSize{ 1200, 800 }, true };
            auto state = expected;
            WINDOWPLACEMENT placement{};
            placement.showCmd = SW_SHOWNORMAL;
            const RECT invalidBounds[] = {
                { 0, 0, 0, 800 }, { 0, 0, 1200, -1 },
                { (std::numeric_limits<LONG>::min)(), 0, (std::numeric_limits<LONG>::max)(), 800 }
            };
            for (auto bounds : invalidBounds)
            {
                placement.rcNormalPosition = bounds;
                urpdf::UpdateWindowStateFromPlacement(state, placement);
                Assert::IsTrue(state == expected);
            }
        }

        TEST_METHOD(WindowDefaultsRememberLastStateWithoutSavedSize)
        {
            auto settings = MakeSettings();
            AssertWindowSettings(settings, {}, urpdf::WindowStartupMode::RememberLastState);
        }

        TEST_METHOD(WindowSettingsPersistAcrossInstances)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            settings.SetWindowState(state);
            settings.SetTheme(urpdf::AppTheme::Dark);

            for (auto mode : { urpdf::WindowStartupMode::RememberLastState,
                urpdf::WindowStartupMode::Maximized, urpdf::WindowStartupMode::Normal })
            {
                settings.SetWindowStartupMode(mode);
                auto reloaded = MakeSettings(values);
                AssertWindowSettings(reloaded, state, mode);
                AssertSettings(reloaded, urpdf::AppTheme::Dark, urpdf::Backdrop::None);
            }

            settings.SetWindowState({ urpdf::WindowSize{ 900, 600 }, false });
            auto normal = MakeSettings(values);
            AssertWindowSettings(normal, { urpdf::WindowSize{ 900, 600 }, false }, urpdf::WindowStartupMode::Normal);
            settings.SetWindowState({});
            auto withoutSize = MakeSettings(values);
            AssertWindowSettings(withoutSize, {}, urpdf::WindowStartupMode::Normal);
        }

        TEST_METHOD(LegacySettingsKeepWindowDefaults)
        {
            auto values = std::make_shared<Values>(Values{ { L"AppTheme", L"Dark" }, { L"Transparency", L"Mica" } });
            auto settings = MakeSettings(values);
            AssertSettings(settings, urpdf::AppTheme::Dark, urpdf::Backdrop::Mica);
            AssertWindowSettings(settings, {}, urpdf::WindowStartupMode::RememberLastState);
        }

        TEST_METHOD(InvalidStoredWindowSettingsUseDefaults)
        {
            const wchar_t* invalidStates[] = {
                L"not json", L"[]", L"{}", L"null",
                LR"({"maximized":"true"})",
                LR"({"maximized":true,"normalWidth":1200})",
                LR"({"maximized":false,"normalHeight":800})",
                LR"({"maximized":true,"normalWidth":0,"normalHeight":800})",
                LR"({"maximized":true,"normalWidth":1200,"normalHeight":-1})",
                LR"({"maximized":true,"normalWidth":1200.5,"normalHeight":800})",
                LR"({"maximized":true,"normalWidth":2147483648,"normalHeight":800})",
                LR"({"maximized":true,"normalWidth":"1200","normalHeight":800})",
            };
            for (auto state : invalidStates)
            {
                auto values = std::make_shared<Values>(Values{
                    { L"AppTheme", L"Dark" }, { L"WindowState", state }, { L"WindowStartupMode", L"Minimized" } });
                auto settings = MakeSettings(values);
                AssertWindowSettings(settings, {}, urpdf::WindowStartupMode::RememberLastState);
                AssertSettings(settings, urpdf::AppTheme::Dark, urpdf::Backdrop::None);
            }
        }

        TEST_METHOD(InvalidWindowSizeDoesNotOverwriteSavedState)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            settings.SetWindowState(state);
            for (auto size : { urpdf::WindowSize{ 0, 800 }, urpdf::WindowSize{ 1200, -1 } })
            {
                settings.SetWindowState({ size, false });
                AssertWindowSettings(settings, state, urpdf::WindowStartupMode::RememberLastState);
                auto reloaded = MakeSettings(values);
                AssertWindowSettings(reloaded, state, urpdf::WindowStartupMode::RememberLastState);
            }
        }

        TEST_METHOD(WindowWriteFailureKeepsSettingsForCurrentSession)
        {
            urpdf::AppSettings settings{ std::make_unique<FailingSettingsStore>() };
            const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            settings.SetWindowState(state);
            settings.SetWindowStartupMode(urpdf::WindowStartupMode::Maximized);
            AssertWindowSettings(settings, state, urpdf::WindowStartupMode::Maximized);
        }

        TEST_METHOD(ExportThenImportRestoresSettings)
        {
            auto source = MakeSettings();
            source.SetTheme(urpdf::AppTheme::Dark);
            source.SetBackdrop(urpdf::Backdrop::MicaAlt);
            const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            source.SetWindowState(state);
            source.SetWindowStartupMode(urpdf::WindowStartupMode::Normal);
            source.SetOpenFilesInExistingWindow(true);
            source.SetUseWindowsAccentColorForTextSelection(false);
            source.SetAskToSetAsDefaultPdfViewer(false);

            auto values = std::make_shared<Values>();
            auto target = MakeSettings(values);
            Assert::IsTrue(target.ImportJson(source.ExportJson()));
            AssertSettings(target, urpdf::AppTheme::Dark, urpdf::Backdrop::MicaAlt, true, false, false);
            AssertWindowSettings(target, state, urpdf::WindowStartupMode::Normal);
            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Dark, urpdf::Backdrop::MicaAlt, true, false, false);
            AssertWindowSettings(reloaded, state, urpdf::WindowStartupMode::Normal);
        }

        TEST_METHOD(DefaultWindowSettingsRoundTripWithoutInventingSize)
        {
            auto source = MakeSettings();
            auto target = MakeSettings();
            target.SetWindowState({ urpdf::WindowSize{ 1200, 800 }, true });
            target.SetWindowStartupMode(urpdf::WindowStartupMode::Maximized);
            Assert::IsTrue(target.ImportJson(source.ExportJson()));
            AssertWindowSettings(target, {}, urpdf::WindowStartupMode::RememberLastState);
        }

        TEST_METHOD(LegacyImportPreservesCurrentWindowSettings)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, true };
            settings.SetWindowState(state);
            settings.SetWindowStartupMode(urpdf::WindowStartupMode::Maximized);
            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark"})"));
            AssertWindowSettings(settings, state, urpdf::WindowStartupMode::Maximized);
            auto reloaded = MakeSettings(values);
            AssertWindowSettings(reloaded, state, urpdf::WindowStartupMode::Maximized);
        }

        TEST_METHOD(InvalidWindowImportsDoNotChangeCacheOrStore)
        {
            const wchar_t* invalidImports[] = {
                LR"({"schemaVersion":1,"WindowStartupMode":"Minimized"})",
                LR"({"schemaVersion":1,"WindowStartupMode":true})",
                LR"({"schemaVersion":1,"WindowState":"invalid"})",
                LR"({"schemaVersion":1,"WindowState":{}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":1}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":1200}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalHeight":800}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":0,"normalHeight":800}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":1200,"normalHeight":-800}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":1200.5,"normalHeight":800}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":2147483648,"normalHeight":800}})",
                LR"({"schemaVersion":1,"WindowState":{"maximized":true,"normalWidth":"1200","normalHeight":800}})",
            };
            for (auto json : invalidImports)
            {
                auto values = std::make_shared<Values>();
                auto settings = MakeSettings(values);
                const urpdf::WindowState state{ urpdf::WindowSize{ 1200, 800 }, false };
                settings.SetWindowState(state);
                settings.SetWindowStartupMode(urpdf::WindowStartupMode::Normal);
                const auto previousValues = *values;
                Assert::IsFalse(settings.ImportJson(json), json);
                AssertWindowSettings(settings, state, urpdf::WindowStartupMode::Normal);
                Assert::IsTrue(*values == previousValues, json);
            }
        }

        TEST_METHOD(ImportPersistsToStore)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Light","Transparency":"Mica","OpenFilesInExistingWindow":true,"UseWindowsAccentColorForTextSelection":false})"));

            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Light, urpdf::Backdrop::Mica, true, false);
        }

        TEST_METHOD(OpenFilesInExistingWindowPersistsToStore)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            AssertSettings(settings, urpdf::AppTheme::Default, urpdf::Backdrop::None, false);

            settings.SetOpenFilesInExistingWindow(true);

            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Default, urpdf::Backdrop::None, true);
        }

        TEST_METHOD(AskToSetAsDefaultPdfViewerPersistsAndImports)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            AssertSettings(settings, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, true, true);

            settings.SetAskToSetAsDefaultPdfViewer(false);

            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, true, false);

            Assert::IsTrue(reloaded.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark"})"));
            AssertSettings(reloaded, urpdf::AppTheme::Dark, urpdf::Backdrop::None, false, true, false);

            Assert::IsTrue(reloaded.ImportJson(LR"({"schemaVersion":1,"AskToSetAsDefaultPdfViewer":true})"));
            AssertSettings(reloaded, urpdf::AppTheme::Dark, urpdf::Backdrop::None, false, true, true);
        }

        TEST_METHOD(UseWindowsAccentColorForTextSelectionDefaultsOnAndPersistsToStore)
        {
            auto values = std::make_shared<Values>();
            auto settings = MakeSettings(values);
            AssertSettings(settings, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, true);

            settings.SetUseWindowsAccentColorForTextSelection(false);

            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, false);

            reloaded.SetUseWindowsAccentColorForTextSelection(true);
            auto restored = MakeSettings(values);
            AssertSettings(restored, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, true);
        }

        TEST_METHOD(InvalidStoredTextSelectionColorUsesDefault)
        {
            auto values = std::make_shared<Values>(Values{ { L"UseWindowsAccentColorForTextSelection", L"blue" } });
            auto settings = MakeSettings(values);
            AssertSettings(settings, urpdf::AppTheme::Default, urpdf::Backdrop::None, false, true);
        }

        TEST_METHOD(MissingKeyKeepsCurrentValue)
        {
            auto settings = MakeSettings();
            settings.SetBackdrop(urpdf::Backdrop::Mica);
            settings.SetUseWindowsAccentColorForTextSelection(false);

            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark"})"));
            AssertSettings(settings, urpdf::AppTheme::Dark, urpdf::Backdrop::Mica, false, false);
        }

        TEST_METHOD(IgnoresUnknownKeys)
        {
            auto settings = MakeSettings();
            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark","Future":42})"));
            AssertSettings(settings, urpdf::AppTheme::Dark, urpdf::Backdrop::None);
        }

        TEST_METHOD(RejectsInvalidImportsWithoutChanges)
        {
            const wchar_t* invalidImports[] = {
                L"not json",
                L"[]",
                LR"({"AppTheme":"Dark"})",
                LR"({"schemaVersion":"1","AppTheme":"Dark"})",
                LR"({"schemaVersion":2,"AppTheme":"Dark"})",
                LR"({"schemaVersion":0,"AppTheme":"Dark"})",
                LR"({"schemaVersion":1.5,"AppTheme":"Dark"})",
                LR"({"schemaVersion":1,"AppTheme":"Purple","Transparency":"Mica"})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","Transparency":3})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","OpenFilesInExistingWindow":"true"})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","OpenFilesInExistingWindow":1})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","AskToSetAsDefaultPdfViewer":"false"})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","AskToSetAsDefaultPdfViewer":0})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","UseWindowsAccentColorForTextSelection":"false"})",
                LR"({"schemaVersion":1,"AppTheme":"Dark","UseWindowsAccentColorForTextSelection":0})",
            };

            for (auto json : invalidImports)
            {
                auto values = std::make_shared<Values>();
                auto settings = MakeSettings(values);
                settings.SetTheme(urpdf::AppTheme::Light);

                Assert::IsFalse(settings.ImportJson(json), json);
                AssertSettings(settings, urpdf::AppTheme::Light, urpdf::Backdrop::None);
                Assert::IsTrue(values->size() == 1, json);
                Assert::IsTrue(values->at(L"AppTheme") == L"Light", json);
            }
        }

        TEST_METHOD(WriteFailureRejectsImport)
        {
            urpdf::AppSettings settings{ std::make_unique<FailingSettingsStore>() };
            Assert::IsFalse(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark"})"));
            AssertSettings(settings, urpdf::AppTheme::Default, urpdf::Backdrop::None);
        }

        TEST_METHOD(ExportIncludesSchemaVersion)
        {
            auto settings = MakeSettings();
            const auto json = settings.ExportJson();
            Assert::IsTrue(json.find(L"\"schemaVersion\":1") != std::wstring::npos, json.c_str());
        }
    };
}
