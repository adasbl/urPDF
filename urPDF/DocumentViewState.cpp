// Corresponding
#include "DocumentViewState.h"

// Standard C++
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace urpdf
{
    namespace
    {
        bool IsPositiveFinite(double value) noexcept
        {
            return std::isfinite(value) && value > 0.0;
        }

        bool IsValidPage(PageRect const& page) noexcept
        {
            return std::isfinite(page.x) && std::isfinite(page.y) && IsPositiveFinite(page.width) && IsPositiveFinite(page.height);
        }

        double Fraction(double value, double origin, double length) noexcept
        {
            return std::clamp((value - origin) / length, 0.0, 1.0);
        }
    }

    std::optional<DocumentViewState> CaptureViewAnchor(std::vector<PageRect> const& pages, ViewportGeometry const& viewport)
    {
        if (!IsPositiveFinite(viewport.zoom) || !IsPositiveFinite(viewport.width) || !IsPositiveFinite(viewport.height) || !std::isfinite(viewport.offsetX) || !std::isfinite(viewport.offsetY) || !std::isfinite(viewport.contentX) || !std::isfinite(viewport.contentY))
        {
            return std::nullopt;
        }

        const double left = viewport.offsetX / viewport.zoom - viewport.contentX;
        const double top = viewport.offsetY / viewport.zoom - viewport.contentY;
        const double width = viewport.width / viewport.zoom;
        const double height = viewport.height / viewport.zoom;
        const double right = left + width;
        const double bottom = top + height;

        std::optional<std::size_t> bestIndex;
        bool bestIsVisible = false;
        double bestDistance = std::numeric_limits<double>::infinity();
        double bestX = 0.0;
        double bestY = 0.0;

        for (std::size_t i = 0; i < pages.size(); ++i)
        {
            auto const& page = pages[i];
            if (!IsValidPage(page)) { continue; }

            const bool isVisible = page.x < right && page.x + page.width > left && page.y < bottom && page.y + page.height > top;
            if (bestIsVisible && !isVisible) { continue; }

            const double pointX = std::clamp(left, page.x, page.x + page.width);
            const double pointY = std::clamp(top, page.y, page.y + page.height);
            const double distance = std::hypot(pointX - left, pointY - top);

            if (!bestIndex || (isVisible && !bestIsVisible) || distance < bestDistance)
            {
                bestIndex = i;
                bestIsVisible = isVisible;
                bestDistance = distance;
                bestX = pointX;
                bestY = pointY;
            }
        }

        if (!bestIndex) { return std::nullopt; }

        auto const& page = pages[*bestIndex];
        DocumentViewState state;
        state.page = static_cast<int>(*bestIndex) + 1;
        state.zoom = viewport.zoom;
        state.anchorX = Fraction(bestX, page.x, page.width);
        state.anchorY = Fraction(bestY, page.y, page.height);
        state.viewportX = Fraction(bestX, left, width);
        state.viewportY = Fraction(bestY, top, height);
        return state;
    }
}
