#pragma once

#include "AppSettings.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <windows.h>

namespace urpdf
{
    inline HMONITOR FindWindowMonitor(std::wstring_view deviceName) noexcept
    {
        if (deviceName.empty()) { return nullptr; }
        struct Search
        {
            std::wstring_view deviceName;
            HMONITOR result{};

            static BOOL CALLBACK Visit(HMONITOR monitor, HDC, LPRECT, LPARAM context)
            {
                auto& search = *reinterpret_cast<Search*>(context);
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info) && search.deviceName == info.szDevice)
                {
                    search.result = monitor;
                    return FALSE;
                }
                return TRUE;
            }
        } search{ deviceName };
        EnumDisplayMonitors(nullptr, nullptr, Search::Visit, reinterpret_cast<LPARAM>(&search));
        return search.result;
    }

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
        const auto& position = settings.windowState.normalPosition;
        const std::int64_t desiredLeft = position ? static_cast<std::int64_t>(workArea.left) + position->x : defaultBounds.left;
        const std::int64_t desiredTop = position ? static_cast<std::int64_t>(workArea.top) + position->y : defaultBounds.top;
        const auto left = std::clamp<std::int64_t>(desiredLeft, workArea.left, workArea.right - width);
        const auto top = std::clamp<std::int64_t>(desiredTop, workArea.top, workArea.bottom - height);

        return { { static_cast<LONG>(left), static_cast<LONG>(top),
            static_cast<LONG>(left + width), static_cast<LONG>(top + height) }, maximized };
    }

    inline std::optional<RECT> ResolveTabTearOutPlacement(RECT const& normalBounds,
        RECT const& workArea, POINT cursor, POINT dragOffset) noexcept
    {
        const std::int64_t availableWidth = static_cast<std::int64_t>(workArea.right) - workArea.left;
        const std::int64_t availableHeight = static_cast<std::int64_t>(workArea.bottom) - workArea.top;
        if (availableWidth <= 0 || availableHeight <= 0
            || availableWidth > (std::numeric_limits<std::int32_t>::max)()
            || availableHeight > (std::numeric_limits<std::int32_t>::max)())
        {
            return std::nullopt;
        }

        std::int64_t width = static_cast<std::int64_t>(normalBounds.right) - normalBounds.left;
        std::int64_t height = static_cast<std::int64_t>(normalBounds.bottom) - normalBounds.top;
        if (width <= 0 || height <= 0 || (width >= availableWidth && height >= availableHeight))
        {
            // An always-maximized window may have no useful restore size.
            width = (std::max)(std::int64_t{ 1 }, availableWidth * 3 / 4);
            height = (std::max)(std::int64_t{ 1 }, availableHeight * 3 / 4);
        }
        width = (std::min)(width, availableWidth);
        height = (std::min)(height, availableHeight);

        const auto offsetX = std::clamp<std::int64_t>(dragOffset.x, 0, width - 1);
        const auto offsetY = std::clamp<std::int64_t>(dragOffset.y, 0, height - 1);
        const auto left = std::clamp<std::int64_t>(static_cast<std::int64_t>(cursor.x) - offsetX,
            workArea.left, workArea.right - width);
        const auto top = std::clamp<std::int64_t>(static_cast<std::int64_t>(cursor.y) - offsetY,
            workArea.top, workArea.bottom - height);

        return RECT{ static_cast<LONG>(left), static_cast<LONG>(top),
            static_cast<LONG>(left + width), static_cast<LONG>(top + height) };
    }

    inline void UpdateWindowPositionFromPlacement(WindowState& state, WINDOWPLACEMENT const& placement,
        MONITORINFOEXW const& monitor, bool toolWindow = false)
    {
        if (!state.normalSize) { return; }
        if (placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_SHOWMAXIMIZED)
        {
            if (state.normalPosition)
            {
                // Keep the restore position, including when a maximized window
                // is moved to another monitor with Win+Shift+Arrow.
                state.normalPosition->monitorDevice = monitor.szDevice;
                return;
            }
        }
        else if (placement.showCmd != SW_SHOWNORMAL) { return; }

        const auto& bounds = placement.rcNormalPosition;
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) { return; }
        // Regular WINDOWPLACEMENT coordinates are in workspace coordinates;
        // tool windows use screen coordinates. Both become work-area offsets.
        const auto& origin = toolWindow ? monitor.rcWork : monitor.rcMonitor;
        const std::int64_t x = static_cast<std::int64_t>(bounds.left) - origin.left;
        const std::int64_t y = static_cast<std::int64_t>(bounds.top) - origin.top;
        if (x < (std::numeric_limits<std::int32_t>::min)() || x > (std::numeric_limits<std::int32_t>::max)()
            || y < (std::numeric_limits<std::int32_t>::min)() || y > (std::numeric_limits<std::int32_t>::max)()) { return; }
        state.normalPosition = WindowPosition{ static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), monitor.szDevice };
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

    struct FullscreenWindowTracker
    {
        bool active{ false };
        bool transitioning{ false };
        bool restoreMaximizedOnExit{ false };

        bool ShouldCaptureWindowState(bool presenterIsFullscreen) const noexcept
        {
            return !active && !transitioning && !presenterIsFullscreen;
        }

        void BeginEnter(bool overlappedWasMaximized) noexcept
        {
            if (!active)
            {
                restoreMaximizedOnExit = overlappedWasMaximized;
            }
            transitioning = true;
        }

        void CompleteEnter(bool presenterIsFullscreen) noexcept
        {
            transitioning = false;
            active = presenterIsFullscreen;
            if (!active)
            {
                restoreMaximizedOnExit = false;
            }
        }

        bool BeginExit() noexcept
        {
            transitioning = true;
            return restoreMaximizedOnExit;
        }

        void CompleteExit(bool presenterIsFullscreen) noexcept
        {
            transitioning = false;
            active = presenterIsFullscreen;
            if (!active)
            {
                restoreMaximizedOnExit = false;
            }
        }

        // Synchronizes external presenter changes when no initiated transition is active.
        // Returns true if fullscreen exited externally and the overlapped window should be maximized.
        bool SyncExternalPresenter(bool presenterIsFullscreen, bool& chromeChanged) noexcept
        {
            chromeChanged = false;
            if (transitioning || active == presenterIsFullscreen)
            {
                return false;
            }

            active = presenterIsFullscreen;
            chromeChanged = true;
            if (!active)
            {
                const bool shouldMaximize = restoreMaximizedOnExit;
                restoreMaximizedOnExit = false;
                return shouldMaximize;
            }
            return false;
        }
    };
}
