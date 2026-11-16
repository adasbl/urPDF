#pragma once

#include "LinkHitArea.g.h"

namespace winrt::urPDF::implementation
{
    struct LinkHitArea : LinkHitAreaT<LinkHitArea>
    {
        LinkHitArea();
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct LinkHitArea : LinkHitAreaT<LinkHitArea, implementation::LinkHitArea> {};
}
