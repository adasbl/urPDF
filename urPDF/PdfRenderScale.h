#pragma once

#include <algorithm>
#include <cmath>

namespace urpdf
{
    constexpr double DipPerPdfPoint = 96.0 / 72.0;

    inline double PageRenderScale(double pdfWidth, double pdfHeight, int rotation, double layoutWidth, double layoutHeight, double zoom, double dpiScale, double borderThickness = 1.0) noexcept
    {
        if (!std::isfinite(pdfWidth) || !std::isfinite(pdfHeight) || !std::isfinite(layoutWidth) || !std::isfinite(layoutHeight) || !std::isfinite(zoom) || !std::isfinite(dpiScale) || !std::isfinite(borderThickness) || borderThickness < 0.0 || pdfWidth <= 0.0 || pdfHeight <= 0.0 || zoom <= 0.0 || dpiScale <= 0.0)
        {
            return 0.0;
        }

        rotation = (rotation % 360 + 360) % 360;
        if (rotation == 90 || rotation == 270)
        {
            std::swap(pdfWidth, pdfHeight);
        }

        const double imageWidth = layoutWidth - 2.0 * borderThickness;
        const double imageHeight = layoutHeight - 2.0 * borderThickness;
        if (imageWidth <= 0.0 || imageHeight <= 0.0)
        {
            return 0.0;
        }

        const double layoutScale = (std::min)(imageWidth / (pdfWidth * DipPerPdfPoint), imageHeight / (pdfHeight * DipPerPdfPoint));
        const double scale = DipPerPdfPoint * layoutScale * zoom * dpiScale;
        return std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
    }
}
