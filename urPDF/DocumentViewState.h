#pragma once

// Standard C++
#include <optional>
#include <vector>

namespace urpdf
{
    struct DocumentViewState
    {
        // One-based page containing the anchor, independent of the page counter.
        int anchorPage{ 1 };
        double zoom{ 1.0 };
        int rotation{ 0 };
        bool bookView{ false };

        double anchorX{ 0.0 };
        double anchorY{ 0.0 };

        double viewportX{ 0.0 };
        double viewportY{ 0.0 };
    };

    struct PageRect
    {
        double x{ 0.0 };
        double y{ 0.0 };
        double width{ 0.0 };
        double height{ 0.0 };
    };

    struct ViewportGeometry
    {
        double offsetX{ 0.0 };
        double offsetY{ 0.0 };
        double width{ 0.0 };
        double height{ 0.0 };
        double zoom{ 1.0 };

        double contentX{ 0.0 };
        double contentY{ 0.0 };

        double scrollableHeight{ 0.0 };
    };

    std::optional<DocumentViewState> CaptureViewAnchor(std::vector<PageRect> const& pages, ViewportGeometry const& viewport);
    std::optional<int> CurrentPageNumber(std::vector<PageRect> const& pages, ViewportGeometry const& viewport);
}
