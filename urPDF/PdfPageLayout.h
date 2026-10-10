#pragma once

#include "PdfPageLayout.g.h"
#include <vector>

namespace winrt::urPDF::implementation
{
    struct PdfPageLayout : PdfPageLayoutT<PdfPageLayout>
    {
        PdfPageLayout() = default;

        int32_t ColumnCount();
        void ColumnCount(int32_t value);

        double Spacing();
        void Spacing(double value);

        winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::Size> PageSizes();
        void PageSizes(winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::Size> const& value);

        std::vector<winrt::Windows::Foundation::Rect> const& PageRects() const noexcept { return m_pageRects; }

        winrt::Windows::Foundation::Size MeasureOverride(winrt::Microsoft::UI::Xaml::Controls::VirtualizingLayoutContext const& context, winrt::Windows::Foundation::Size const& availableSize);
        winrt::Windows::Foundation::Size ArrangeOverride(winrt::Microsoft::UI::Xaml::Controls::VirtualizingLayoutContext const& context, winrt::Windows::Foundation::Size const& finalSize);

    private:
        std::vector<winrt::Windows::Foundation::Rect> CalculatePageRects(winrt::Windows::Foundation::Size const& availableSize);

        static bool IntersectsRealizationRect(winrt::Windows::Foundation::Rect const& pageRect, winrt::Windows::Foundation::Rect const& realizationRect);

        int32_t m_columnCount{ 1 };
        double m_spacing{ 10.0 };
        double m_contentWidth{ 0.0 };
        winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::Size> m_pageSizes{ nullptr };
        std::vector<winrt::Windows::Foundation::Rect> m_pageRects;
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct PdfPageLayout : PdfPageLayoutT<PdfPageLayout, implementation::PdfPageLayout>
    {
    };
}
