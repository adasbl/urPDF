#pragma once

// Standard C++
#include <string>
#include <string_view>

// Windows SDK / WinRT
#include <winrt/Windows.Foundation.h>

namespace urpdf
{
    inline constexpr std::wstring_view DefaultPdfProgId = L"urPDF.Document";
    inline constexpr std::wstring_view DefaultPdfApplicationProgId = L"Applications\\urPDF.exe";
    inline constexpr std::wstring_view DefaultRegisteredAppName = L"urPDF";
    inline constexpr std::wstring_view FallbackDefaultAppsSettingsUri = L"ms-settings:defaultapps";

    enum class DefaultPdfViewerStatus
    {
        Default,
        NotDefault,
        Unknown
    };

    struct AppRegistrationInfo
    {
        bool isPackaged{ false };
        std::wstring appUserModelId;
        bool hasUserRegisteredApp{ false };
        bool hasMachineRegisteredApp{ false };
        std::wstring registeredAppName{ DefaultRegisteredAppName };
        std::wstring executablePath;
    };

    struct DefaultPdfAssociationSnapshot
    {
        std::wstring progId;
        std::wstring appUserModelId;
        std::wstring executablePath;
        bool querySucceeded{ false };
    };

    struct DefaultPdfViewerPromptContext
    {
        DefaultPdfViewerStatus status{ DefaultPdfViewerStatus::Unknown };
        bool promptEnabled{ true };
        bool alreadyPromptedThisSession{ false };
        bool isDeferredPresentation{ false };
    };

    DefaultPdfViewerStatus EvaluateDefaultPdfViewerStatus(
        DefaultPdfAssociationSnapshot const& association,
        AppRegistrationInfo const& app) noexcept;

    bool ShouldShowDefaultPdfViewerPrompt(DefaultPdfViewerPromptContext const& context) noexcept;

    std::wstring BuildDefaultAppsSettingsUri(AppRegistrationInfo const& app);

    AppRegistrationInfo QueryAppRegistrationInfo();
    DefaultPdfAssociationSnapshot QueryDefaultPdfAssociation();
    DefaultPdfViewerStatus QueryDefaultPdfViewerStatus();

    winrt::Windows::Foundation::IAsyncOperation<bool> OpenDefaultAppsSettingsAsync();
}
