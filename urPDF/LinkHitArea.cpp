#include "pch.h"
#include "LinkHitArea.h"
#include "LinkHitArea.g.cpp"

#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.UI.h>

namespace winrt::urPDF::implementation
{
    LinkHitArea::LinkHitArea()
    {
        ProtectedCursor(Microsoft::UI::Input::InputSystemCursor::Create(Microsoft::UI::Input::InputSystemCursorShape::Hand));
        Background(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::Colors::Transparent()));
    }
}
