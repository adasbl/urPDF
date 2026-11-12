#pragma once

// Standard C++
#include <optional>
#include <string>
#include <string_view>

namespace urpdf
{
    // Returns the document path from launch arguments forwarded by another instance.
    // Accepts a full command line (unpackaged apps report the executable first) or the
    // arguments alone (packaged apps). Only absolute paths are returned, because the
    // receiving instance cannot resolve a path against the sender's working directory.
    std::optional<std::wstring> DocumentPathFromLaunchArguments(std::wstring_view arguments);

    // True when argv names a document by a path relative to the current directory.
    // Such a launch must not be redirected as is; the path has to be made absolute first.
    bool HasRelativeDocumentPathArgument(int argc, wchar_t const* const* argv);
}
