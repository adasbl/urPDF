#pragma once

#include <winrt/Windows.Storage.h>

namespace urpdf
{
    struct WindowCreationOptions
    {
        winrt::Windows::Storage::StorageFile file{ nullptr };
        winrt::hstring filePath;
    };
}
