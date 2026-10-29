#pragma once

namespace urpdf
{
    struct DocumentViewState
    {
        int page{ 1 };
        double zoom{ 1.0 };
        int rotation{ 0 };
        bool bookView{ false };
        double anchorX{ 0.0 };
        double anchorY{ 0.0 };
        double viewportX{ 0.0 };
        double viewportY{ 0.0 };
    };
}
