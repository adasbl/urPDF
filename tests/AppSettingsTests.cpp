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

    void AssertSettings(urpdf::AppSettings const& settings, urpdf::AppTheme theme, urpdf::Backdrop backdrop)
    {
        const auto data = settings.Get();
        Assert::IsTrue(data.theme == theme);
        Assert::IsTrue(data.backdrop == backdrop);
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

            auto values = std::make_shared<Values>();
            auto target = MakeSettings(values);
            Assert::IsTrue(target.ImportJson(source.ExportJson()));
            AssertSettings(target, urpdf::AppTheme::Dark, urpdf::Backdrop::MicaAlt);
            AssertWindowSettings(target, state, urpdf::WindowStartupMode::Normal);
            auto reloaded = MakeSettings(values);
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
            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Light","Transparency":"Mica"})"));

            auto reloaded = MakeSettings(values);
            AssertSettings(reloaded, urpdf::AppTheme::Light, urpdf::Backdrop::Mica);
        }

        TEST_METHOD(MissingKeyKeepsCurrentValue)
        {
            auto settings = MakeSettings();
            settings.SetBackdrop(urpdf::Backdrop::Mica);

            Assert::IsTrue(settings.ImportJson(LR"({"schemaVersion":1,"AppTheme":"Dark"})"));
            AssertSettings(settings, urpdf::AppTheme::Dark, urpdf::Backdrop::Mica);
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
