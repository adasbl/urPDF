#pragma once

#include "AppSettings.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <windows.h>

namespace urpdf
{
    struct WindowStartupPlacement
    {
        RECT normalBounds;
        bool maximized;
    };

    inline WindowStartupPlacement ResolveWindowStartupPlacement(
        SettingsData const& settings, RECT const& defaultBounds, RECT const& workArea) noexcept
    {
        const bool maximized = settings.windowStartupMode == WindowStartupMode::Maximized
            || (settings.windowStartupMode == WindowStartupMode::RememberLastState && settings.windowState.maximized);
        const std::int64_t availableWidth = static_cast<std::int64_t>(workArea.right) - workArea.left;
        const std::int64_t availableHeight = static_cast<std::int64_t>(workArea.bottom) - workArea.top;
        if (availableWidth <= 0 || availableHeight <= 0
            || availableWidth > (std::numeric_limits<std::int32_t>::max)()
            || availableHeight > (std::numeric_limits<std::int32_t>::max)())
        {
            return { defaultBounds, maximized };
        }

        std::int64_t width = static_cast<std::int64_t>(defaultBounds.right) - defaultBounds.left;
        std::int64_t height = static_cast<std::int64_t>(defaultBounds.bottom) - defaultBounds.top;
        if (settings.windowState.normalSize
            && settings.windowState.normalSize->width > 0 && settings.windowState.normalSize->height > 0)
        {
            width = settings.windowState.normalSize->width;
            height = settings.windowState.normalSize->height;
        }
        if (width <= 0 || height <= 0)
        {
            width = 1024;
            height = 768;
        }
        width = (std::min)(width, availableWidth);
        height = (std::min)(height, availableHeight);
        const auto left = std::clamp<std::int64_t>(defaultBounds.left, workArea.left, workArea.right - width);
        const auto top = std::clamp<std::int64_t>(defaultBounds.top, workArea.top, workArea.bottom - height);

        return { { static_cast<LONG>(left), static_cast<LONG>(top),
            static_cast<LONG>(left + width), static_cast<LONG>(top + height) }, maximized };
    }

    inline void UpdateWindowStateFromPlacement(WindowState& state, WINDOWPLACEMENT const& placement) noexcept
    {
        const bool minimized = placement.showCmd == SW_SHOWMINIMIZED;
        const bool maximized = placement.showCmd == SW_SHOWMAXIMIZED
            || (minimized && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);

        if (minimized || placement.showCmd == SW_SHOWMAXIMIZED)
        {
            state.maximized = maximized;
            if (state.normalSize) { return; }
        }
        else if (placement.showCmd != SW_SHOWNORMAL) { return; }

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
        state.maximized = maximized;
    }
}
