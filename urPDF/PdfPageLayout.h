#pragma once

// Generated
#include "PdfPageLayout.g.h"

// Standard C++
#include <cstdint>
#include <vector>

// Windows SDK / WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace winrt::urPDF::implementation
{
    namespace controls   = winrt::Microsoft::UI::Xaml::Controls;
    namespace foundation = winrt::Windows::Foundation;

    struct PdfPageLayout : PdfPageLayoutT<PdfPageLayout>
    {
        PdfPageLayout() = default;

        int32_t ColumnCount();
        void ColumnCount(int32_t value);

        double Spacing();
        void Spacing(double value);

        foundation::Collections::IVectorView<foundation::Size> PageSizes();
        void PageSizes(foundation::Collections::IVectorView<foundation::Size> const& value);

        std::vector<foundation::Rect> const& PageRects() const noexcept { return m_pageRects; }

        foundation::Size MeasureOverride(controls::VirtualizingLayoutContext const& context, foundation::Size const& availableSize);
        foundation::Size ArrangeOverride(controls::VirtualizingLayoutContext const& context, foundation::Size const& finalSize);

    private:
        std::vector<foundation::Rect> CalculatePageRects(foundation::Size const& availableSize);

        static bool IntersectsRealizationRect(foundation::Rect const& pageRect, foundation::Rect const& realizationRect);

        int32_t m_columnCount{ 1 };
        double m_spacing{ 10.0 };
        double m_contentWidth{ 0.0 };
        foundation::Collections::IVectorView<foundation::Size> m_pageSizes{ nullptr };
        std::vector<foundation::Rect> m_pageRects;
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct PdfPageLayout : PdfPageLayoutT<PdfPageLayout, implementation::PdfPageLayout>
    {
    };
}
