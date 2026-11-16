#include "ExternalUriPolicy.h"

namespace
{
    bool IsAlpha(unsigned char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    bool EqualsAsciiIgnoreCase(std::string_view value, std::string_view expected) noexcept
    {
        if (value.size() != expected.size()) { return false; }
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            auto c = static_cast<unsigned char>(value[i]);
            if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; }
            if (c != expected[i]) { return false; }
        }
        return true;
    }

    bool IsWhitespace(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }
}

namespace urpdf
{
    bool IsAllowedExternalScheme(std::string_view scheme) noexcept
    {
        return EqualsAsciiIgnoreCase(scheme, "http") || EqualsAsciiIgnoreCase(scheme, "https")
            || EqualsAsciiIgnoreCase(scheme, "mailto");
    }

    std::optional<std::string> TryGetLaunchableExternalUri(std::string_view uri)
    {
        while (!uri.empty() && IsWhitespace(uri.front())) { uri.remove_prefix(1); }
        while (!uri.empty() && IsWhitespace(uri.back())) { uri.remove_suffix(1); }
        if (uri.empty()) { return std::nullopt; }
        for (unsigned char c : uri)
        {
            if (c < 0x20 || c == 0x7f) { return std::nullopt; }
        }
        const auto colon = uri.find(':');
        if (colon == std::string_view::npos || colon == 0 || !IsAlpha(uri.front())) { return std::nullopt; }
        const auto scheme = uri.substr(0, colon);
        for (unsigned char c : scheme)
        {
            if (!IsAlpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.')
            {
                return std::nullopt;
            }
        }
        if (!IsAllowedExternalScheme(scheme)) { return std::nullopt; }
        auto rest = uri.substr(colon + 1);
        if (EqualsAsciiIgnoreCase(scheme, "mailto"))
        {
            if (rest.empty()) { return std::nullopt; }
        }
        else
        {
            if (!rest.starts_with("//") || rest.find('\\') != std::string_view::npos) { return std::nullopt; }
            rest.remove_prefix(2);
            auto host = rest.substr(0, rest.find_first_of("/?#"));
            if (host.find(' ') != std::string_view::npos) { return std::nullopt; }
            const auto at = host.rfind('@');
            if (at != std::string_view::npos) { host.remove_prefix(at + 1); }
            if (host.starts_with('['))
            {
                const auto end = host.find(']');
                if (end == std::string_view::npos || end <= 1) { return std::nullopt; }
            }
            else { host = host.substr(0, host.find(':')); }
            if (host.empty()) { return std::nullopt; }
        }
        return std::string(uri);
    }
}
