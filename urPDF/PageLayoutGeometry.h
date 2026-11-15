#pragma once

#include "DocumentViewState.h"

#include <vector>

namespace urpdf
{
    struct PageSize
    {
        double width{ 0.0 };
        double height{ 0.0 };
    };

    struct PageLayoutGeometry
    {
        std::vector<PageRect> pageRects;
        double contentWidth{ 0.0 };
    };

    PageLayoutGeometry CalculatePageLayoutGeometry(std::vector<PageSize> const& pageSizes,
        int columnCount, double spacing, double availableWidth);
}
