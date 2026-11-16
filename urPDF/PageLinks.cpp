#include "PageLinks.h"

#include <algorithm>
#include <cmath>

namespace urpdf
{
    std::optional<PdfPageBounds> TryClipLinkRect(PdfPageBounds rect, PdfPageBounds page) noexcept
    {
        const auto valid = [](PdfPageBounds bounds)
            {
                return std::isfinite(bounds.x0) && std::isfinite(bounds.y0)
                    && std::isfinite(bounds.x1) && std::isfinite(bounds.y1)
                    && bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1;
            };
        if (!valid(rect) || !valid(page)) { return std::nullopt; }
        PdfPageBounds clipped{ (std::max)(rect.x0, page.x0), (std::max)(rect.y0, page.y0),
            (std::min)(rect.x1, page.x1), (std::min)(rect.y1, page.y1) };
        return valid(clipped) ? std::optional(clipped) : std::nullopt;
    }
}
