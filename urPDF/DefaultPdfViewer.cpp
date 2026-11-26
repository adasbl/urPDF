// Corresponding
#include "DefaultPdfViewer.h"

// Standard C++
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Windows SDK / WinRT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace urpdf
{
    namespace
    {
        constexpr wchar_t PdfExtension[] = L".pdf";
        constexpr wchar_t RegisteredApplicationsKeyPath[] = L"Software\\RegisteredApplications";

        struct CoTaskMemDeleter
        {
            void operator()(void* pointer) const noexcept
            {
                if (pointer != nullptr)
                {
                    ::CoTaskMemFree(pointer);
                }
            }
        };

        struct UniqueRegKey
        {
            HKEY handle{ nullptr };

            ~UniqueRegKey()
            {
                if (handle != nullptr)
                {
                    ::RegCloseKey(handle);
                }
            }

            UniqueRegKey() = default;
            UniqueRegKey(UniqueRegKey const&) = delete;
            UniqueRegKey& operator=(UniqueRegKey const&) = delete;
        };

        bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) noexcept
        {
            if (left.size() != right.size()) { return false; }
            if (left.empty()) { return true; }
            return ::CompareStringOrdinal(
                left.data(), static_cast<int>(left.size()),
                right.data(), static_cast<int>(right.size()),
                TRUE) == CSTR_EQUAL;
        }

        std::wstring_view TrimWhitespace(std::wstring_view value) noexcept
        {
            while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' || value.front() == L'\r' || value.front() == L'\n'))
            {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r' || value.back() == L'\n'))
            {
                value.remove_suffix(1);
            }
            return value;
        }

        std::wstring NormalizePathForComparison(std::wstring_view rawPath)
        {
            auto trimmed = TrimWhitespace(rawPath);
            if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"')
            {
                trimmed.remove_prefix(1);
                trimmed.remove_suffix(1);
                trimmed = TrimWhitespace(trimmed);
            }

            std::wstring normalized{ trimmed };
            std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            while (normalized.size() > 3 && normalized.back() == L'\\')
            {
                normalized.pop_back();
            }
            return normalized;
        }

        bool PathsEqualIgnoreCase(std::wstring_view left, std::wstring_view right)
        {
            auto normalizedLeft = NormalizePathForComparison(left);
            auto normalizedRight = NormalizePathForComparison(right);
            if (normalizedLeft.empty() || normalizedRight.empty()) { return false; }
            return EqualsIgnoreCase(normalizedLeft, normalizedRight);
        }

        bool IsUnreservedQueryValueByte(unsigned char byte) noexcept
        {
            return (byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9')
                || byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '!';
        }

        std::wstring EncodeQueryValue(std::wstring_view text)
        {
            if (text.empty()) { return {}; }

            const int utf8Length = ::WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (utf8Length <= 0) { return {}; }

            std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
            ::WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), utf8Length, nullptr, nullptr);

            static constexpr wchar_t HexDigits[] = L"0123456789ABCDEF";
            std::wstring encoded;
            encoded.reserve(utf8.size());
            for (char character : utf8)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (IsUnreservedQueryValueByte(byte))
                {
                    encoded.push_back(static_cast<wchar_t>(byte));
                }
                else
                {
                    encoded.push_back(L'%');
                    encoded.push_back(HexDigits[(byte >> 4) & 0x0F]);
                    encoded.push_back(HexDigits[byte & 0x0F]);
                }
            }
            return encoded;
        }

        std::optional<std::wstring> QueryRegistryStringValue(HKEY root, wchar_t const* subKey, wchar_t const* valueName)
        {
            UniqueRegKey key;
            if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key.handle) != ERROR_SUCCESS)
            {
                return std::nullopt;
            }

            DWORD type = 0;
            DWORD bytes = 0;
            if (::RegQueryValueExW(key.handle, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS
                || (type != REG_SZ && type != REG_EXPAND_SZ)
                || bytes < sizeof(wchar_t))
            {
                return std::nullopt;
            }

            std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
            if (::RegQueryValueExW(key.handle, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS)
            {
                return std::nullopt;
            }

            while (!buffer.empty() && buffer.back() == L'\0')
            {
                buffer.pop_back();
            }
            if (buffer.empty()) { return std::nullopt; }
            return buffer;
        }

        bool HasRegisteredApplicationValue(HKEY root, std::wstring const& appName)
        {
            return QueryRegistryStringValue(root, RegisteredApplicationsKeyPath, appName.c_str()).has_value();
        }

        std::wstring QueryCurrentExecutablePath()
        {
            std::wstring path(MAX_PATH, L'\0');
            while (true)
            {
                const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length == 0) { return {}; }
                if (length < path.size())
                {
                    path.resize(length);
                    return path;
                }
                if (path.size() >= 32768) { return {}; }
                path.resize(path.size() * 2);
            }
        }

        bool IsCurrentProcessPackaged() noexcept
        {
            UINT32 length = 0;
            return ::GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
        }

        std::wstring QueryCurrentAppUserModelId()
        {
            UINT32 length = 0;
            if (::GetCurrentApplicationUserModelId(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0)
            {
                return {};
            }

            std::wstring aumid(length, L'\0');
            if (::GetCurrentApplicationUserModelId(&length, aumid.data()) != ERROR_SUCCESS)
            {
                return {};
            }

            while (!aumid.empty() && aumid.back() == L'\0')
            {
                aumid.pop_back();
            }
            return aumid;
        }

        std::optional<std::wstring> QueryAssocString(ASSOCSTR associationString)
        {
            DWORD length = 0;
            const HRESULT sizeResult = ::AssocQueryStringW(
                ASSOCF_NONE, associationString, PdfExtension, L"open", nullptr, &length);
            if ((sizeResult != S_FALSE && sizeResult != S_OK) || length == 0)
            {
                return std::nullopt;
            }

            std::wstring value(length, L'\0');
            const HRESULT queryResult = ::AssocQueryStringW(
                ASSOCF_NONE, associationString, PdfExtension, L"open", value.data(), &length);
            if (FAILED(queryResult))
            {
                return std::nullopt;
            }

            while (!value.empty() && value.back() == L'\0')
            {
                value.pop_back();
            }
            if (value.empty()) { return std::nullopt; }
            return value;
        }

        std::optional<std::wstring> QueryCurrentDefaultProgId()
        {
            winrt::com_ptr<IApplicationAssociationRegistration> registration;
            const HRESULT createResult = ::CoCreateInstance(
                CLSID_ApplicationAssociationRegistration,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(registration.put()));
            if (FAILED(createResult) || !registration)
            {
                return std::nullopt;
            }

            LPWSTR rawProgId = nullptr;
            const HRESULT queryResult = registration->QueryCurrentDefault(
                PdfExtension, AT_FILEEXTENSION, AL_EFFECTIVE, &rawProgId);
            std::unique_ptr<wchar_t, CoTaskMemDeleter> progIdGuard{ rawProgId };
            if (FAILED(queryResult) || rawProgId == nullptr || rawProgId[0] == L'\0')
            {
                return std::nullopt;
            }

            return std::wstring{ rawProgId };
        }
    }

    DefaultPdfViewerStatus EvaluateDefaultPdfViewerStatus(
        DefaultPdfAssociationSnapshot const& association,
        AppRegistrationInfo const& app) noexcept
    {
        if (!association.querySucceeded)
        {
            return DefaultPdfViewerStatus::Unknown;
        }

        if (app.isPackaged)
        {
            if (!app.appUserModelId.empty()
                && !association.appUserModelId.empty()
                && EqualsIgnoreCase(association.appUserModelId, app.appUserModelId))
            {
                return DefaultPdfViewerStatus::Default;
            }

            if (PathsEqualIgnoreCase(association.executablePath, app.executablePath))
            {
                return DefaultPdfViewerStatus::Default;
            }

            return DefaultPdfViewerStatus::NotDefault;
        }

        if (EqualsIgnoreCase(association.progId, DefaultPdfProgId)
            || EqualsIgnoreCase(association.progId, DefaultPdfApplicationProgId))
        {
            return DefaultPdfViewerStatus::Default;
        }

        if (PathsEqualIgnoreCase(association.executablePath, app.executablePath))
        {
            return DefaultPdfViewerStatus::Default;
        }

        return DefaultPdfViewerStatus::NotDefault;
    }

    bool ShouldShowDefaultPdfViewerPrompt(DefaultPdfViewerPromptContext const& context) noexcept
    {
        return context.status == DefaultPdfViewerStatus::NotDefault
            && context.promptEnabled
            && !context.alreadyPromptedThisSession
            && !context.isDeferredPresentation;
    }

    std::wstring BuildDefaultAppsSettingsUri(AppRegistrationInfo const& app)
    {
        std::wstring uri{ FallbackDefaultAppsSettingsUri };
        if (app.isPackaged && !app.appUserModelId.empty())
        {
            auto encoded = EncodeQueryValue(app.appUserModelId);
            if (!encoded.empty())
            {
                uri += L"?registeredAUMID=";
                uri += encoded;
                return uri;
            }
        }

        if (app.hasUserRegisteredApp && !app.registeredAppName.empty())
        {
            auto encoded = EncodeQueryValue(app.registeredAppName);
            if (!encoded.empty())
            {
                uri += L"?registeredAppUser=";
                uri += encoded;
                return uri;
            }
        }

        if (app.hasMachineRegisteredApp && !app.registeredAppName.empty())
        {
            auto encoded = EncodeQueryValue(app.registeredAppName);
            if (!encoded.empty())
            {
                uri += L"?registeredAppMachine=";
                uri += encoded;
                return uri;
            }
        }

        return uri;
    }

    AppRegistrationInfo QueryAppRegistrationInfo()
    {
        AppRegistrationInfo info;
        info.isPackaged = IsCurrentProcessPackaged();
        info.executablePath = QueryCurrentExecutablePath();

        if (info.isPackaged)
        {
            info.appUserModelId = QueryCurrentAppUserModelId();
        }
        else
        {
            info.hasUserRegisteredApp = HasRegisteredApplicationValue(HKEY_CURRENT_USER, info.registeredAppName);
            info.hasMachineRegisteredApp = HasRegisteredApplicationValue(HKEY_LOCAL_MACHINE, info.registeredAppName);
        }

        return info;
    }

    DefaultPdfAssociationSnapshot QueryDefaultPdfAssociation()
    {
        DefaultPdfAssociationSnapshot snapshot;

        if (auto progId = QueryCurrentDefaultProgId())
        {
            snapshot.progId = std::move(*progId);
            snapshot.querySucceeded = true;
        }

        if (auto aumid = QueryAssocString(ASSOCSTR_APPID))
        {
            snapshot.appUserModelId = std::move(*aumid);
            snapshot.querySucceeded = true;
        }
        else if (!snapshot.progId.empty())
        {
            std::wstring appKey = snapshot.progId + L"\\Application";
            if (auto progIdAumid = QueryRegistryStringValue(HKEY_CLASSES_ROOT, appKey.c_str(), L"AppUserModelID"))
            {
                snapshot.appUserModelId = std::move(*progIdAumid);
            }
        }

        if (auto executable = QueryAssocString(ASSOCSTR_EXECUTABLE))
        {
            snapshot.executablePath = std::move(*executable);
            snapshot.querySucceeded = true;
        }

        return snapshot;
    }

    DefaultPdfViewerStatus QueryDefaultPdfViewerStatus()
    {
        try
        {
            return EvaluateDefaultPdfViewerStatus(QueryDefaultPdfAssociation(), QueryAppRegistrationInfo());
        }
        catch (...)
        {
            return DefaultPdfViewerStatus::Unknown;
        }
    }

    winrt::Windows::Foundation::IAsyncOperation<bool> OpenDefaultAppsSettingsAsync()
    {
        const auto primaryUriText = BuildDefaultAppsSettingsUri(QueryAppRegistrationInfo());

        try
        {
            winrt::Windows::Foundation::Uri primaryUri{ primaryUriText };
            if (co_await winrt::Windows::System::Launcher::LaunchUriAsync(primaryUri))
            {
                co_return true;
            }
        }
        catch (...)
        {
        }

        if (primaryUriText == FallbackDefaultAppsSettingsUri)
        {
            co_return false;
        }

        try
        {
            winrt::Windows::Foundation::Uri fallbackUri{ FallbackDefaultAppsSettingsUri };
            co_return co_await winrt::Windows::System::Launcher::LaunchUriAsync(fallbackUri);
        }
        catch (...)
        {
            co_return false;
        }
    }
}
