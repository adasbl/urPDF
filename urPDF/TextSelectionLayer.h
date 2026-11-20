#pragma once

#include "TextSelectionLayer.g.h"

namespace winrt::urPDF::implementation
{
    struct TextSelectionLayer : TextSelectionLayerT<TextSelectionLayer>
    {
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct TextSelectionLayer : TextSelectionLayerT<TextSelectionLayer, implementation::TextSelectionLayer> {};
}
