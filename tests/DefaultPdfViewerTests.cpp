#include "CppUnitTest.h"
#include "../urPDF/DefaultPdfViewer.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    TEST_CLASS(DefaultPdfViewerTests)
    {
    public:
        TEST_METHOD(FailedAssociationQueryReturnsUnknown)
        {
            urpdf::DefaultPdfAssociationSnapshot association;
            association.querySucceeded = false;
            association.progId = L"urPDF.Document";

            urpdf::AppRegistrationInfo app;
            app.executablePath = L"C:\\Users\\test\\AppData\\Local\\Programs\\urPDF\\urPDF.exe";

            Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(association, app) == urpdf::DefaultPdfViewerStatus::Unknown);
        }

        TEST_METHOD(UnpackagedMatchesUrPdfProgIdCaseInsensitively)
        {
            urpdf::AppRegistrationInfo app;
            app.isPackaged = false;
            app.executablePath = L"C:\\Other\\Path\\urPDF.exe";

            for (auto progId : { L"urPDF.Document", L"URPDF.DOCUMENT", L"Applications\\urPDF.exe", L"applications\\URPDF.EXE" })
            {
                urpdf::DefaultPdfAssociationSnapshot association;
                association.querySucceeded = true;
                association.progId = progId;

                Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(association, app) == urpdf::DefaultPdfViewerStatus::Default, progId);
            }
        }

        TEST_METHOD(UnpackagedMatchesNormalizedExecutablePath)
        {
            urpdf::AppRegistrationInfo app;
            app.isPackaged = false;
            app.executablePath = L"C:\\Users\\Test\\AppData\\Local\\Programs\\urPDF\\urPDF.exe";

            urpdf::DefaultPdfAssociationSnapshot association;
            association.querySucceeded = true;
            association.progId = L"SomeCustomProgId";
            association.executablePath = L"\"c:/users/test/appdata/local/programs/urpdf/urpdf.exe\"";

            Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(association, app) == urpdf::DefaultPdfViewerStatus::Default);
        }

        TEST_METHOD(UnpackagedReportsNotDefaultForOtherPdfViewers)
        {
            urpdf::AppRegistrationInfo app;
            app.isPackaged = false;
            app.executablePath = L"C:\\Users\\Test\\AppData\\Local\\Programs\\urPDF\\urPDF.exe";

            urpdf::DefaultPdfAssociationSnapshot association;
            association.querySucceeded = true;
            association.progId = L"MSEdgePDF";
            association.executablePath = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";

            Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(association, app) == urpdf::DefaultPdfViewerStatus::NotDefault);
        }

        TEST_METHOD(PackagedMatchesAppUserModelIdCaseInsensitively)
        {
            urpdf::AppRegistrationInfo app;
            app.isPackaged = true;
            app.appUserModelId = L"6eebccc8-e91a-4a2f-bfb0-a9fc89efee93_abc123!App";

            urpdf::DefaultPdfAssociationSnapshot matching;
            matching.querySucceeded = true;
            matching.progId = L"AppX1234567890abcdef";
            matching.appUserModelId = L"6EEBCCC8-E91A-4A2F-BFB0-A9FC89EFEE93_ABC123!app";

            Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(matching, app) == urpdf::DefaultPdfViewerStatus::Default);

            urpdf::DefaultPdfAssociationSnapshot other;
            other.querySucceeded = true;
            other.progId = L"urPDF.Document";
            other.appUserModelId = L"Microsoft.MicrosoftEdge_8wekyb3d8bbwe!MicrosoftEdge";

            Assert::IsTrue(urpdf::EvaluateDefaultPdfViewerStatus(other, app) == urpdf::DefaultPdfViewerStatus::NotDefault);
        }

        TEST_METHOD(PromptOnlyShowsWhenNotDefaultEnabledFirstTimeAndVisible)
        {
            urpdf::DefaultPdfViewerPromptContext base;
            base.status = urpdf::DefaultPdfViewerStatus::NotDefault;
            base.promptEnabled = true;
            base.alreadyPromptedThisSession = false;
            base.isDeferredPresentation = false;
            Assert::IsTrue(urpdf::ShouldShowDefaultPdfViewerPrompt(base));

            auto alreadyDefault = base;
            alreadyDefault.status = urpdf::DefaultPdfViewerStatus::Default;
            Assert::IsFalse(urpdf::ShouldShowDefaultPdfViewerPrompt(alreadyDefault));

            auto unknownStatus = base;
            unknownStatus.status = urpdf::DefaultPdfViewerStatus::Unknown;
            Assert::IsFalse(urpdf::ShouldShowDefaultPdfViewerPrompt(unknownStatus));

            auto disabled = base;
            disabled.promptEnabled = false;
            Assert::IsFalse(urpdf::ShouldShowDefaultPdfViewerPrompt(disabled));

            auto alreadyPrompted = base;
            alreadyPrompted.alreadyPromptedThisSession = true;
            Assert::IsFalse(urpdf::ShouldShowDefaultPdfViewerPrompt(alreadyPrompted));

            auto deferred = base;
            deferred.isDeferredPresentation = true;
            Assert::IsFalse(urpdf::ShouldShowDefaultPdfViewerPrompt(deferred));
        }

        TEST_METHOD(BuildDefaultAppsSettingsUriTargetsPackagedApp)
        {
            urpdf::AppRegistrationInfo app;
            app.isPackaged = true;
            app.appUserModelId = L"6eebccc8-e91a-4a2f-bfb0-a9fc89efee93_abc123!App";

            Assert::AreEqual(
                std::wstring{ L"ms-settings:defaultapps?registeredAUMID=6eebccc8-e91a-4a2f-bfb0-a9fc89efee93_abc123!App" },
                urpdf::BuildDefaultAppsSettingsUri(app));
        }

        TEST_METHOD(BuildDefaultAppsSettingsUriTargetsUserOrMachineRegisteredApp)
        {
            urpdf::AppRegistrationInfo userApp;
            userApp.isPackaged = false;
            userApp.hasUserRegisteredApp = true;
            userApp.hasMachineRegisteredApp = true;
            userApp.registeredAppName = L"urPDF";

            Assert::AreEqual(
                std::wstring{ L"ms-settings:defaultapps?registeredAppUser=urPDF" },
                urpdf::BuildDefaultAppsSettingsUri(userApp));

            urpdf::AppRegistrationInfo machineApp;
            machineApp.isPackaged = false;
            machineApp.hasUserRegisteredApp = false;
            machineApp.hasMachineRegisteredApp = true;
            machineApp.registeredAppName = L"urPDF Viewer";

            Assert::AreEqual(
                std::wstring{ L"ms-settings:defaultapps?registeredAppMachine=urPDF%20Viewer" },
                urpdf::BuildDefaultAppsSettingsUri(machineApp));
        }

        TEST_METHOD(BuildDefaultAppsSettingsUriFallsBackWhenUnregistered)
        {
            urpdf::AppRegistrationInfo unregistered;
            unregistered.isPackaged = false;
            unregistered.hasUserRegisteredApp = false;
            unregistered.hasMachineRegisteredApp = false;

            Assert::AreEqual(
                std::wstring{ urpdf::FallbackDefaultAppsSettingsUri },
                urpdf::BuildDefaultAppsSettingsUri(unregistered));
        }
    };
}
