#include "PageLayoutGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace urpdf
{
    PageLayoutGeometry CalculatePageLayoutGeometry(std::vector<PageSize> const& pageSizes,
        int columnCount, double spacing, double availableWidth)
    {
        PageLayoutGeometry result;
        if (pageSizes.empty()) { return result; }

        const auto columns = static_cast<std::size_t>(std::clamp(columnCount, 1, 2));
        if (!std::isfinite(spacing) || spacing < 0.0) { spacing = 0.0; }
        const auto pageCount = pageSizes.size();
        result.pageRects.resize(pageCount);

        double contentWidth = availableWidth;
        if (!std::isfinite(contentWidth) || contentWidth <= 0.0)
        {
            contentWidth = 0.0;
            for (auto const& size : pageSizes)
            {
                contentWidth = (std::max)(contentWidth, size.width);
            }

            if (columns == 2)
            {
                for (std::size_t i = 0; i < pageCount; i += 2)
                {
                    const double firstWidth = pageSizes[i].width;
                    const double secondWidth = i + 1 < pageCount ? pageSizes[i + 1].width : 0.0;
                    contentWidth = (std::max)(contentWidth, firstWidth + secondWidth + spacing);
                }
            }
        }
        result.contentWidth = contentWidth;

        double y = 0.0;
        for (std::size_t rowStart = 0; rowStart < pageCount; rowStart += columns)
        {
            const auto rowEnd = (std::min)(pageCount, rowStart + columns);
            double rowHeight = 0.0;
            std::array<double, 2> widths{};
            std::array<double, 2> heights{};

            for (std::size_t column = 0; rowStart + column < rowEnd; ++column)
            {
                auto const& size = pageSizes[rowStart + column];
                widths[column] = (std::max)(1.0, size.width);
                heights[column] = (std::max)(1.0, size.height);
            }

            const bool isPagePair = rowEnd - rowStart == 2;
            const double rawPagesWidth = widths[0] + (isPagePair ? widths[1] : 0.0);
            const double availablePageWidth = isPagePair ? (std::max)(0.0, contentWidth - spacing) : contentWidth;
            const double rowScale = rawPagesWidth > availablePageWidth && availablePageWidth > 0.0 ? availablePageWidth / rawPagesWidth : 1.0;

            for (std::size_t column = 0; rowStart + column < rowEnd; ++column)
            {
                widths[column] *= rowScale;
                heights[column] *= rowScale;
                rowHeight = (std::max)(rowHeight, heights[column]);
            }

            double x = isPagePair ? (contentWidth / 2.0) - (spacing / 2.0) - widths[0] : (contentWidth - widths[0]) / 2.0;
            for (std::size_t column = 0; rowStart + column < rowEnd; ++column)
            {
                result.pageRects[rowStart + column] = {
                    static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(widths[column]), static_cast<float>(heights[column]) };
                x += widths[column] + spacing;
            }

            y += rowHeight + spacing;
        }
        return result;
    }
}
