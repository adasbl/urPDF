#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace urpdf
{
    bool IsAllowedExternalScheme(std::string_view scheme) noexcept;
    // Returns a trimmed URI only for http, https and mailto. The launcher must
    // also check the scheme reported by its own URI parser before opening it.
    std::optional<std::string> TryGetLaunchableExternalUri(std::string_view uri);
}
