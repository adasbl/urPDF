#include "PageCoordinateGeometry.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool ValidSize(urpdf::ElementSize size) noexcept
    {
        return std::isfinite(size.width) && std::isfinite(size.height)
            && size.width > 0.0 && size.height > 0.0;
    }

    bool ValidInsets(urpdf::ElementInsets insets) noexcept
    {
        return std::isfinite(insets.left) && std::isfinite(insets.top)
            && std::isfinite(insets.right) && std::isfinite(insets.bottom)
            && insets.left >= 0.0 && insets.top >= 0.0
            && insets.right >= 0.0 && insets.bottom >= 0.0;
    }

    bool ValidRect(urpdf::ElementRect rect) noexcept
    {
        return std::isfinite(rect.x) && std::isfinite(rect.y)
            && ValidSize({ rect.width, rect.height })
            && std::isfinite(rect.x + rect.width) && std::isfinite(rect.y + rect.height)
            && rect.x + rect.width > rect.x && rect.y + rect.height > rect.y;
    }

    bool ValidRaster(urpdf::PageRasterGeometry const& raster) noexcept
    {
        auto const& bounds = raster.pdfBounds;
        if (!std::isfinite(bounds.x0) || !std::isfinite(bounds.y0)
            || !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1)
            || !ValidSize({ bounds.x1 - bounds.x0, bounds.y1 - bounds.y0 })) { return false; }

        auto const& pixmap = raster.pixmapBounds;
        const auto width = static_cast<std::int64_t>(pixmap.x1) - pixmap.x0;
        const auto height = static_cast<std::int64_t>(pixmap.y1) - pixmap.y0;
        if (raster.bitmapSize.width <= 0 || raster.bitmapSize.height <= 0
            || width != raster.bitmapSize.width || height != raster.bitmapSize.height) { return false; }

        auto const& m = raster.transform;
        for (double coefficient : { m.a, m.b, m.c, m.d, m.e, m.f })
        {
            if (!std::isfinite(coefficient)) { return false; }
        }

        // Normalize before taking the determinant to avoid overflow/underflow
        // for otherwise usable uniform scales. Do not use an absolute epsilon:
        // small rasterization scales can have perfectly valid inverses.
        const double scale = (std::max)({ std::abs(m.a), std::abs(m.b), std::abs(m.c), std::abs(m.d) });
        if (scale == 0.0) { return false; }
        const double a = m.a / scale;
        const double b = m.b / scale;
        const double c = m.c / scale;
        const double d = m.d / scale;
        const double determinant = a * d - b * c;
        if (!std::isfinite(determinant) || determinant == 0.0) { return false; }
        const double ia = (d / determinant) / scale;
        const double ib = (-b / determinant) / scale;
        const double ic = (-c / determinant) / scale;
        const double id = (a / determinant) / scale;
        const double ie = -(ia * m.e + ic * m.f);
        const double iff = -(ib * m.e + id * m.f);
        for (double coefficient : { ia, ib, ic, id, ie, iff })
        {
            if (!std::isfinite(coefficient)) { return false; }
        }
        return true;
    }
}

namespace urpdf
{
    std::optional<ElementSize> TryPageContentSize(ElementSize borderSize,
        ElementInsets borderThickness, ElementInsets padding) noexcept
    {
        if (!ValidSize(borderSize) || !ValidInsets(borderThickness) || !ValidInsets(padding)) { return std::nullopt; }
        const ElementSize content{
            borderSize.width - borderThickness.left - borderThickness.right - padding.left - padding.right,
            borderSize.height - borderThickness.top - borderThickness.bottom - padding.top - padding.bottom };
        if (!ValidSize(content)) { return std::nullopt; }
        return content;
    }

    std::optional<ElementRect> TryUniformBitmapRect(BitmapSize bitmapSize, ElementSize displaySize) noexcept
    {
        if (bitmapSize.width <= 0 || bitmapSize.height <= 0 || !ValidSize(displaySize)) { return std::nullopt; }
        const double scale = (std::min)(displaySize.width / bitmapSize.width, displaySize.height / bitmapSize.height);
        if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(1.0 / scale)) { return std::nullopt; }
        const double width = bitmapSize.width * scale;
        const double height = bitmapSize.height * scale;
        const ElementRect rect{ (displaySize.width - width) / 2.0, (displaySize.height - height) / 2.0, width, height };
        if (!ValidRect(rect)) { return std::nullopt; }
        return rect;
    }

    std::optional<PageCoordinateInputs> TryCreatePageCoordinateInputs(
        PageRasterGeometry const& raster, ElementSize displaySize) noexcept
    {
        if (!ValidRaster(raster)) { return std::nullopt; }
        const auto rect = TryUniformBitmapRect(raster.bitmapSize, displaySize);
        if (!rect) { return std::nullopt; }
        return PageCoordinateInputs{ raster, displaySize, *rect };
    }

    std::optional<bool> IsElementPointInsideBitmap(ElementPoint point, ElementRect bitmapRect) noexcept
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !ValidRect(bitmapRect)) { return std::nullopt; }
        return point.x >= bitmapRect.x && point.x <= bitmapRect.x + bitmapRect.width
            && point.y >= bitmapRect.y && point.y <= bitmapRect.y + bitmapRect.height;
    }
}
