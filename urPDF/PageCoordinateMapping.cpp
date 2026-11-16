#include "PageCoordinateMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
    bool WithinClosedInterval(double value, double low, double high) noexcept
    {
        // Forward/inverse arithmetic can move an edge point by a few ULPs.
        // Keep this relative to the coordinates, including very small pages.
        const double tolerance = 16.0 * std::numeric_limits<double>::epsilon()
            * (std::max)({ std::abs(value), std::abs(low), std::abs(high) });
        return (value >= low || low - value <= tolerance)
            && (value <= high || value - high <= tolerance);
    }

    std::optional<urpdf::PdfPagePoint> TransformPoint(urpdf::PageRasterTransform const& m,
        double x, double y) noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y)) { return std::nullopt; }
        const urpdf::PdfPagePoint result{ m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f };
        if (!std::isfinite(result.x) || !std::isfinite(result.y)) { return std::nullopt; }
        return result;
    }

    std::optional<urpdf::PdfPageBounds> TransformBounds(urpdf::PageRasterTransform const& m,
        urpdf::PdfPageBounds rect) noexcept
    {
        if (!std::isfinite(rect.x0) || !std::isfinite(rect.y0)
            || !std::isfinite(rect.x1) || !std::isfinite(rect.y1)
            || rect.x1 <= rect.x0 || rect.y1 <= rect.y0) { return std::nullopt; }
        const std::array points{
            TransformPoint(m, rect.x0, rect.y0), TransformPoint(m, rect.x1, rect.y0),
            TransformPoint(m, rect.x0, rect.y1), TransformPoint(m, rect.x1, rect.y1) };
        urpdf::PdfPageBounds result{};
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            if (!points[i]) { return std::nullopt; }
            auto const& point = *points[i];
            if (i == 0) { result = { point.x, point.y, point.x, point.y }; }
            else
            {
                result.x0 = (std::min)(result.x0, point.x);
                result.y0 = (std::min)(result.y0, point.y);
                result.x1 = (std::max)(result.x1, point.x);
                result.y1 = (std::max)(result.y1, point.y);
            }
        }
        const double width = result.x1 - result.x0;
        const double height = result.y1 - result.y0;
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) { return std::nullopt; }
        return result;
    }
}

namespace urpdf
{
    PageCoordinateMapping::PageCoordinateMapping(PdfPageBounds bounds, ElementRect bitmapRect,
        PageRasterTransform forward, PageRasterTransform inverse) noexcept
        : m_pdfBounds(bounds), m_bitmapRect(bitmapRect), m_forward(forward), m_inverse(inverse) {}

    std::optional<PageCoordinateMapping> PageCoordinateMapping::TryCreate(
        PageRasterGeometry const& raster, ElementSize displaySize) noexcept
    {
        const auto inputs = TryCreatePageCoordinateInputs(raster, displaySize);
        if (!inputs) { return std::nullopt; }
        const double scale = (std::min)(displaySize.width / raster.bitmapSize.width,
            displaySize.height / raster.bitmapSize.height);
        auto const& m = raster.transform;
        auto const& rect = inputs->bitmapRect;
        const PageRasterTransform forward{
            m.a * scale, m.b * scale, m.c * scale, m.d * scale,
            (m.e - raster.pixmapBounds.x0) * scale + rect.x,
            (m.f - raster.pixmapBounds.y0) * scale + rect.y };
        const auto inverse = TryInvertPageRasterTransform(forward);
        if (!inverse || !TransformBounds(forward, raster.pdfBounds)) { return std::nullopt; }
        return PageCoordinateMapping{ raster.pdfBounds, rect, forward, *inverse };
    }

    std::optional<ElementPoint> PageCoordinateMapping::PdfToElementPoint(PdfPagePoint point) const noexcept
    {
        const auto result = TransformPoint(m_forward, point.x, point.y);
        if (!result) { return std::nullopt; }
        return ElementPoint{ result->x, result->y };
    }

    std::optional<PdfPagePoint> PageCoordinateMapping::ElementToPdfPoint(ElementPoint point) const noexcept
    {
        return TransformPoint(m_inverse, point.x, point.y);
    }

    std::optional<ElementRect> PageCoordinateMapping::PdfToElementRect(PdfPageBounds rect) const noexcept
    {
        const auto result = TransformBounds(m_forward, rect);
        if (!result) { return std::nullopt; }
        return ElementRect{ result->x0, result->y0, result->x1 - result->x0, result->y1 - result->y0 };
    }

    std::optional<PdfPageBounds> PageCoordinateMapping::ElementToPdfRect(ElementRect rect) const noexcept
    {
        if (!std::isfinite(rect.width) || !std::isfinite(rect.height)
            || rect.width <= 0.0 || rect.height <= 0.0) { return std::nullopt; }
        return TransformBounds(m_inverse, { rect.x, rect.y, rect.x + rect.width, rect.y + rect.height });
    }

    std::optional<bool> PageCoordinateMapping::IsElementPointInsidePage(ElementPoint point) const noexcept
    {
        const auto pagePoint = ElementToPdfPoint(point);
        if (!pagePoint) { return std::nullopt; }
        return WithinClosedInterval(point.x, m_bitmapRect.x, m_bitmapRect.x + m_bitmapRect.width)
            && WithinClosedInterval(point.y, m_bitmapRect.y, m_bitmapRect.y + m_bitmapRect.height)
            && WithinClosedInterval(pagePoint->x, m_pdfBounds.x0, m_pdfBounds.x1)
            && WithinClosedInterval(pagePoint->y, m_pdfBounds.y0, m_pdfBounds.y1);
    }
}
