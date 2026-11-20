#pragma once

#include "TextSelectionLayer.g.h"

namespace winrt::urPDF::implementation
{
    struct TextSelectionLayer : TextSelectionLayerT<TextSelectionLayer>
    {
        TextSelectionLayer();
        void SetTextCursor(bool overText);

    private:
        bool m_overText{ false };
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct TextSelectionLayer : TextSelectionLayerT<TextSelectionLayer, implementation::TextSelectionLayer> {};
}
