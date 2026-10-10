#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace urpdf
{
    struct PageBitmapCacheKey
    {
        std::uint64_t documentId{ 0 };
        int pageIndex{ 0 };
        int rotation{ 0 };
        int renderScaleBucket{ 0 };

        bool operator==(PageBitmapCacheKey const& other) const noexcept
        {
            return documentId == other.documentId && pageIndex == other.pageIndex
                && rotation == other.rotation && renderScaleBucket == other.renderScaleBucket;
        }
    };

    struct PageBitmapCacheKeyHash
    {
        std::size_t operator()(PageBitmapCacheKey const& key) const noexcept
        {
            std::size_t hash = std::hash<std::uint64_t>{}(key.documentId);
            const auto combine = [&hash](std::size_t value)
            {
                hash ^= value + static_cast<std::size_t>(0x9e3779b9) + (hash << 6) + (hash >> 2);
            };
            combine(std::hash<int>{}(key.pageIndex));
            combine(std::hash<int>{}(key.rotation));
            combine(std::hash<int>{}(key.renderScaleBucket));
            return hash;
        }
    };

    constexpr double DipPerPdfPoint = 96.0 / 72.0;
    constexpr double RenderSupersampling = 1.25;

    // Sixteen evenly spaced levels per power-of-two interval. Binary steps give
    // exact representative scales and at most 6.25% extra raster resolution.
    constexpr int RenderScaleLevels = 16;
    constexpr int MinRenderScaleExponent = std::numeric_limits<double>::min_exponent
        - std::numeric_limits<double>::digits + 1;

    inline double RenderScaleForBucket(int bucket) noexcept
    {
        if (bucket <= 0) { return 0.0; }
        const int index = bucket - 1;
        const int exponent = index / RenderScaleLevels + MinRenderScaleExponent;
        const double fraction = static_cast<double>(RenderScaleLevels + index % RenderScaleLevels) / (2 * RenderScaleLevels);
        const double scale = std::ldexp(fraction, exponent);
        return std::isfinite(scale) ? scale : 0.0;
    }

    inline int RenderScaleBucket(double scale) noexcept
    {
        if (!std::isfinite(scale) || scale <= 0.0) { return 0; }
        int exponent = 0;
        const double fraction = std::frexp(scale, &exponent);
        int level = static_cast<int>(std::ceil(fraction * (2 * RenderScaleLevels)));
        if (level == 2 * RenderScaleLevels)
        {
            level = RenderScaleLevels;
            ++exponent;
        }
        const int bucket = (exponent - MinRenderScaleExponent) * RenderScaleLevels
            + level - RenderScaleLevels + 1;
        return RenderScaleForBucket(bucket) > 0.0 ? bucket : 0;
    }

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
        const double scale = DipPerPdfPoint * layoutScale * zoom * dpiScale * RenderSupersampling;
        return std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
    }
}
