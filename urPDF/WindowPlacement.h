#pragma once

#include "AppSettings.h"

#include <cstdint>
#include <limits>
#include <windows.h>

namespace urpdf
{
    inline void UpdateWindowStateFromPlacement(WindowState& state, WINDOWPLACEMENT const& placement) noexcept
    {
        if (placement.showCmd == SW_SHOWMAXIMIZED)
        {
            state.maximized = true;
            return;
        }
        if (placement.showCmd != SW_SHOWNORMAL) { return; }

        const auto& bounds = placement.rcNormalPosition;
        const std::int64_t width = static_cast<std::int64_t>(bounds.right) - bounds.left;
        const std::int64_t height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
        if (width <= 0 || height <= 0
            || width > (std::numeric_limits<std::int32_t>::max)()
            || height > (std::numeric_limits<std::int32_t>::max)())
        {
            return;
        }

        state.normalSize = WindowSize{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) };
        state.maximized = false;
    }
}
