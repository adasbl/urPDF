#include "PageText.h"

#include <algorithm>
#include <cmath>

namespace urpdf
{
    namespace
    {
        constexpr double LineHitTolerance = 1.0;

        double DistanceOutside(double value, double low, double high) noexcept
        {
            return value < low ? low - value : value > high ? value - high : 0.0;
        }
    }

    std::uint32_t HitTest(PageText const& text, PdfPagePoint point) noexcept
    {
        TextLine const* nearest = nullptr;
        double nearestDistance = 0.0;
        double nearestVertical = 0.0;
        for (auto const& line : text.lines)
        {
            if (line.firstChar >= line.endChar || line.endChar > text.chars.size()) { continue; }
            const double dx = DistanceOutside(point.x, line.bounds.x0, line.bounds.x1);
            const double dy = DistanceOutside(point.y, line.bounds.y0, line.bounds.y1);
            double distance = std::hypot(dx, dy);
            if (distance <= LineHitTolerance) { distance = 0.0; }
            if (!nearest || distance < nearestDistance || (distance == nearestDistance && dy < nearestVertical))
            {
                nearest = &line;
                nearestDistance = distance;
                nearestVertical = dy;
            }
        }
        if (!nearest) { return 0; }

        for (auto i = nearest->firstChar; i < nearest->endChar; ++i)
        {
            auto const& bounds = text.chars[i].bounds;
            if (point.x < (bounds.x0 + bounds.x1) / 2.0) { return i; }
        }
        return nearest->endChar;
    }
}
