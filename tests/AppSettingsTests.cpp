#include "CppUnitTest.h"
#include "../urPDF/AppSettings.h"

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

    TEST_CLASS(AppSettingsTests)
    {
    public:
        TEST_METHOD(ExportThenImportRestoresSettings)
        {
            auto source = MakeSettings();
            source.SetTheme(urpdf::AppTheme::Dark);
            source.SetBackdrop(urpdf::Backdrop::MicaAlt);

            auto target = MakeSettings();
            Assert::IsTrue(target.ImportJson(source.ExportJson()));
            AssertSettings(target, urpdf::AppTheme::Dark, urpdf::Backdrop::MicaAlt);
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
