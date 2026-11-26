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

        struct LayoutViewport
        {
            double left{ 0.0 };
            double top{ 0.0 };
            double width{ 0.0 };
            double height{ 0.0 };
        };

        std::optional<LayoutViewport> ToLayoutViewport(ViewportGeometry const& viewport) noexcept
        {
            if (!IsPositiveFinite(viewport.zoom) || !IsPositiveFinite(viewport.width) || !IsPositiveFinite(viewport.height) || !std::isfinite(viewport.offsetX) || !std::isfinite(viewport.offsetY) || !std::isfinite(viewport.contentX) || !std::isfinite(viewport.contentY))
            {
                return std::nullopt;
            }

            return LayoutViewport{
                viewport.offsetX / viewport.zoom - viewport.contentX,
                viewport.offsetY / viewport.zoom - viewport.contentY,
                viewport.width / viewport.zoom,
                viewport.height / viewport.zoom };
        }

        std::optional<PageRect> CalculatePageFitRowBounds(std::vector<PageRect> const& pages, int pageNumber, bool bookView) noexcept
        {
            if (pages.empty()) { return std::nullopt; }

            const std::size_t columns = bookView ? 2 : 1;
            const std::size_t pageIndex = (std::min)(static_cast<std::size_t>((std::max)(pageNumber, 1) - 1), pages.size() - 1);
            const std::size_t rowStart = (pageIndex / columns) * columns;
            const std::size_t rowEnd = (std::min)(pages.size(), rowStart + columns);

            bool found = false;
            double left = std::numeric_limits<double>::infinity();
            double top = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            double bottom = -std::numeric_limits<double>::infinity();

            for (std::size_t i = rowStart; i < rowEnd; ++i)
            {
                auto const& page = pages[i];
                if (!IsValidPage(page)) { continue; }
                found = true;
                left = (std::min)(left, page.x);
                top = (std::min)(top, page.y);
                right = (std::max)(right, page.x + page.width);
                bottom = (std::max)(bottom, page.y + page.height);
            }

            if (!found) { return std::nullopt; }

            const double width = right - left;
            const double height = bottom - top;
            if (!IsPositiveFinite(width) || !IsPositiveFinite(height)) { return std::nullopt; }

            return PageRect{ left, top, width, height };
        }
    }

    std::optional<DocumentViewState> CaptureViewAnchor(std::vector<PageRect> const& pages, ViewportGeometry const& viewport)
    {
        const auto view = ToLayoutViewport(viewport);
        if (!view) { return std::nullopt; }

        const double left = view->left;
        const double top = view->top;
        const double width = view->width;
        const double height = view->height;
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
        state.anchorPage = static_cast<int>(*bestIndex) + 1;
        state.zoom = viewport.zoom;
        state.anchorX = Fraction(bestX, page.x, page.width);
        state.anchorY = Fraction(bestY, page.y, page.height);
        state.viewportX = Fraction(bestX, left, width);
        state.viewportY = Fraction(bestY, top, height);
        return state;
    }

    std::optional<ScrollOffsets> RestoreViewAnchor(DocumentViewState const& state, std::vector<PageRect> const& pages, ViewportGeometry const& viewport)
    {
        if (pages.empty() || !ToLayoutViewport(viewport)
            || !std::isfinite(viewport.scrollableWidth) || viewport.scrollableWidth < 0.0
            || !std::isfinite(viewport.scrollableHeight) || viewport.scrollableHeight < 0.0)
        {
            return std::nullopt;
        }

        const auto isFraction = [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; };
        if (!isFraction(state.anchorX) || !isFraction(state.anchorY)
            || !isFraction(state.viewportX) || !isFraction(state.viewportY)) { return std::nullopt; }

        // Clamp only when a page exists, and before converting a one-based index to size_t.
        const auto pageIndex = (std::min)(static_cast<std::size_t>((std::max)(state.anchorPage, 1) - 1), pages.size() - 1);
        auto const& page = pages[pageIndex];
        if (!IsValidPage(page)) { return std::nullopt; }

        const double offsetX = (page.x + state.anchorX * page.width + viewport.contentX) * viewport.zoom - state.viewportX * viewport.width;
        const double offsetY = (page.y + state.anchorY * page.height + viewport.contentY) * viewport.zoom - state.viewportY * viewport.height;
        if (!std::isfinite(offsetX) || !std::isfinite(offsetY)) { return std::nullopt; }

        return ScrollOffsets{
            std::clamp(offsetX, 0.0, viewport.scrollableWidth),
            std::clamp(offsetY, 0.0, viewport.scrollableHeight) };
    }

    std::optional<int> CurrentPageNumber(std::vector<PageRect> const& pages, ViewportGeometry const& viewport)
    {
        const auto view = ToLayoutViewport(viewport);
        if (!view) { return std::nullopt; }

        const double right = view->left + view->width;
        const double bottom = view->top + view->height;
        const bool isScrolledToEnd = viewport.scrollableHeight > 0.0 && viewport.offsetY >= viewport.scrollableHeight - 1.0;

        std::optional<int> largestPage;
        std::optional<int> lastVisiblePage;
        double largestArea = 0.0;

        for (std::size_t i = 0; i < pages.size(); ++i)
        {
            auto const& page = pages[i];
            if (!IsValidPage(page)) { continue; }

            const double visibleWidth = (std::min)(right, page.x + page.width) - (std::max)(view->left, page.x);
            const double visibleHeight = (std::min)(bottom, page.y + page.height) - (std::max)(view->top, page.y);
            if (visibleWidth <= 0.0 || visibleHeight <= 0.0) { continue; }

            const int pageNumber = static_cast<int>(i) + 1;
            const double area = visibleWidth * visibleHeight;
            lastVisiblePage = pageNumber;
            if (area > largestArea)
            {
                largestArea = area;
                largestPage = pageNumber;
            }
        }

        return isScrolledToEnd && lastVisiblePage ? lastVisiblePage : largestPage;
    }

    std::optional<PageFitView> CalculatePageFitView(std::vector<PageRect> const& pages,
        int pageNumber, bool bookView, PageFitMode mode,
        double viewportWidth, double viewportHeight,
        double minZoom, double maxZoom)
    {
        if (!IsPositiveFinite(viewportWidth) || !IsPositiveFinite(viewportHeight)
            || !IsPositiveFinite(minZoom) || !IsPositiveFinite(maxZoom) || minZoom > maxZoom)
        {
            return std::nullopt;
        }

        const auto row = CalculatePageFitRowBounds(pages, pageNumber, bookView);
        if (!row) { return std::nullopt; }

        const double targetDimension = mode == PageFitMode::Vertical ? row->height : row->width;
        const double viewportDimension = mode == PageFitMode::Vertical ? viewportHeight : viewportWidth;
        const double rawZoom = viewportDimension / targetDimension;
        if (!IsPositiveFinite(rawZoom)) { return std::nullopt; }

        return PageFitView{ std::clamp(rawZoom, minZoom, maxZoom), *row };
    }

    std::optional<ScrollOffsets> CalculatePageFitScrollOffsets(std::vector<PageRect> const& pages,
        ViewportGeometry const& viewport, int pageNumber, bool bookView)
    {
        if (!ToLayoutViewport(viewport)
            || !std::isfinite(viewport.scrollableWidth) || viewport.scrollableWidth < 0.0
            || !std::isfinite(viewport.scrollableHeight) || viewport.scrollableHeight < 0.0)
        {
            return std::nullopt;
        }

        const auto row = CalculatePageFitRowBounds(pages, pageNumber, bookView);
        if (!row) { return std::nullopt; }

        const double rawX = (row->x + row->width * 0.5 + viewport.contentX) * viewport.zoom - viewport.width * 0.5;
        const double scaledHeight = row->height * viewport.zoom;
        const double rawY = scaledHeight <= viewport.height
            ? (row->y + row->height * 0.5 + viewport.contentY) * viewport.zoom - viewport.height * 0.5
            : (row->y + viewport.contentY) * viewport.zoom;

        if (!std::isfinite(rawX) || !std::isfinite(rawY)) { return std::nullopt; }

        return ScrollOffsets{
            std::clamp(rawX, 0.0, viewport.scrollableWidth),
            std::clamp(rawY, 0.0, viewport.scrollableHeight) };
    }
}
