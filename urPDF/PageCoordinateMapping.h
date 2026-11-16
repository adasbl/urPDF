#pragma once

#include "PageCoordinateGeometry.h"

namespace urpdf
{
    class PageCoordinateMapping
    {
    public:
        static std::optional<PageCoordinateMapping> TryCreate(
            PageRasterGeometry const& raster, ElementSize displaySize) noexcept;

        // Finite points outside the page/image are mapped without clipping.
        // Invalid points or unrepresentable results return nullopt.
        std::optional<ElementPoint> PdfToElementPoint(PdfPagePoint point) const noexcept;
        std::optional<PdfPagePoint> ElementToPdfPoint(ElementPoint point) const noexcept;

        // Transform all four corners and return their axis-aligned bounds.
        // Empty/reversed/nonfinite rectangles return nullopt.
        std::optional<ElementRect> PdfToElementRect(PdfPageBounds rect) const noexcept;
        std::optional<PdfPageBounds> ElementToPdfRect(ElementRect rect) const noexcept;

        // Tests both Uniform margins and the actual page bounds (excluding
        // any fringe added by integer pixmap rounding). Edges are included with
        // a small relative tolerance for forward/inverse floating-point error.
        std::optional<bool> IsElementPointInsidePage(ElementPoint point) const noexcept;
        ElementRect BitmapRect() const noexcept { return m_bitmapRect; }

    private:
        PageCoordinateMapping(PdfPageBounds bounds, ElementRect bitmapRect,
            PageRasterTransform forward, PageRasterTransform inverse) noexcept;

        PdfPageBounds m_pdfBounds;
        ElementRect m_bitmapRect;
        PageRasterTransform m_forward;
        PageRasterTransform m_inverse;
    };
}
