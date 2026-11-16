#pragma once

#include <cstdint>
#include <optional>

namespace urpdf
{
    // MuPDF page coordinates, exactly as returned by fz_bound_page/pdfBounds.
    // The origin is not necessarily (0, 0). Do not normalize these coordinates
    // or apply the PDF file's bottom-left coordinate convention to them.
    struct PdfPagePoint
    {
        double x{ 0.0 };
        double y{ 0.0 };
    };

    struct PdfPageBounds
    {
        double x0{ 0.0 };
        double y0{ 0.0 };
        double x1{ 0.0 };
        double y1{ 0.0 };
    };

    // Bitmap pixels have a local origin of (0, 0), unlike raster coordinates.
    struct BitmapPoint
    {
        double x{ 0.0 };
        double y{ 0.0 };
    };

    struct BitmapSize
    {
        std::int32_t width{ 0 };
        std::int32_t height{ 0 };
    };

    struct PixmapBounds
    {
        std::int32_t x0{ 0 };
        std::int32_t y0{ 0 };
        std::int32_t x1{ 0 };
        std::int32_t y1{ 0 };
    };

    // Same coefficient order as fz_matrix:
    // rasterX = a * pageX + c * pageY + e
    // rasterY = b * pageX + d * pageY + f
    // Subtract the integer pixmap origin afterwards to obtain bitmap pixels.
    struct PageRasterTransform
    {
        double a{ 1.0 };
        double b{ 0.0 };
        double c{ 0.0 };
        double d{ 1.0 };
        double e{ 0.0 };
        double f{ 0.0 };
    };

    // Copy the actual transform and final bbox from TryPageRasterBounds (after
    // fz_round_rect and minimum-size adjustment), and the produced bitmap size.
    // Keep this snapshot with its bitmap, including cache entries. Requested
    // zoom/rotation/DPI may already describe a different, pending render.
    struct PageRasterGeometry
    {
        PdfPageBounds pdfBounds;
        PageRasterTransform transform;
        PixmapBounds pixmapBounds;
        BitmapSize bitmapSize;
    };

    // Local, unzoomed DIPs shared by Image and overlay, inside the page Border.
    // ScrollViewer applies zoom to both; do not apply zoom or DPI again here.
    struct ElementPoint
    {
        double x{ 0.0 };
        double y{ 0.0 };
    };

    struct ElementSize
    {
        double width{ 0.0 };
        double height{ 0.0 };
    };

    struct ElementRect
    {
        double x{ 0.0 };
        double y{ 0.0 };
        double width{ 0.0 };
        double height{ 0.0 };
    };

    struct ElementInsets
    {
        double left{ 0.0 };
        double top{ 0.0 };
        double right{ 0.0 };
        double bottom{ 0.0 };
    };

    struct PageCoordinateInputs
    {
        PageRasterGeometry raster;
        ElementSize displaySize;
        ElementRect bitmapRect;
    };

    // All Try functions return nullopt for invalid/empty/nonfinite inputs or
    // unrepresentable results. No exceptions, clamping or fallback geometry.
    // Prefer the measured inner container size. When only Border.ActualWidth/
    // ActualHeight are available, subtract BorderThickness (and any Padding)
    // with this helper. The resulting content's local origin is still (0, 0).
    std::optional<ElementSize> TryPageContentSize(ElementSize borderSize,
        ElementInsets borderThickness, ElementInsets padding = {}) noexcept;

    std::optional<ElementRect> TryUniformBitmapRect(BitmapSize bitmapSize,
        ElementSize displaySize) noexcept;

    // Includes validation of a finite, representable inverse raster transform.
    std::optional<PageCoordinateInputs> TryCreatePageCoordinateInputs(
        PageRasterGeometry const& raster, ElementSize displaySize) noexcept;

    // nullopt = invalid input; false = outside bitmap (including Uniform's
    // empty margins); true = inside or on the bitmap edge. Never clamps points.
    // Rounded pixmap fringe can still be outside pdfBounds; page hit testing
    // must also test the inverse-mapped page point against the original bounds.
    std::optional<bool> IsElementPointInsideBitmap(ElementPoint point,
        ElementRect bitmapRect) noexcept;
}
