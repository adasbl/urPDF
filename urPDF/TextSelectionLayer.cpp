#include "pch.h"
#include "TextSelectionLayer.h"
#include "TextSelectionLayer.g.cpp"

#include <winrt/Microsoft.UI.Input.h>

namespace winrt::urPDF::implementation
{
    using Microsoft::UI::Input::InputSystemCursor;
    using Microsoft::UI::Input::InputSystemCursorShape;

    TextSelectionLayer::TextSelectionLayer()
    {
        ProtectedCursor(InputSystemCursor::Create(InputSystemCursorShape::Arrow));
    }

    void TextSelectionLayer::SetTextCursor(bool overText)
    {
        if (m_overText == overText) { return; }
        ProtectedCursor(InputSystemCursor::Create(overText ? InputSystemCursorShape::IBeam : InputSystemCursorShape::Arrow));
        m_overText = overText;
    }
}
