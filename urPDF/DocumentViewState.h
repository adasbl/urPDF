#pragma once

// Standard C++
#include <cstdint>
#include <optional>
#include <vector>

namespace urpdf
{
    // UI-thread state only. Bitmap rendering does not begin or end a view operation.
    class ViewStateGuard
    {
    public:
        struct Operation
        {
            std::uint64_t documentId{ 0 };
            std::uint64_t generation{ 0 };
            bool operator==(Operation const&) const = default;
        };

        Operation Begin(std::uint64_t documentId) noexcept
        {
            m_operation = { documentId, m_operation.generation + 1 };
            m_pending = true;
            return m_operation;
        }

        void Cancel() noexcept
        {
            Begin(0);
            m_pending = false;
        }

        Operation CurrentOperation() const noexcept { return m_operation; }
        bool IsPending() const noexcept { return m_pending; }
        bool IsCurrent(Operation operation) const noexcept { return m_pending && operation == m_operation; }

        bool Complete(Operation operation) noexcept
        {
            if (!IsCurrent(operation)) { return false; }
            m_pending = false;
            return true;
        }

        bool CanSave(std::uint64_t documentId) const noexcept
        {
            return !m_pending && documentId != 0 && documentId == m_operation.documentId;
        }

    private:
        Operation m_operation;
        bool m_pending{ false };
    };

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

        double scrollableWidth{ 0.0 };
        double scrollableHeight{ 0.0 };
    };

    struct ScrollOffsets
    {
        double x{ 0.0 };
        double y{ 0.0 };
    };

    std::optional<DocumentViewState> CaptureViewAnchor(std::vector<PageRect> const& pages, ViewportGeometry const& viewport);
    // Uses the measured (already rotated/book-view) layout and the applied viewport zoom.
    // Missing/invalid anchors return nullopt; callers restore the document start instead.
    std::optional<ScrollOffsets> RestoreViewAnchor(DocumentViewState const& state, std::vector<PageRect> const& pages, ViewportGeometry const& viewport);
    std::optional<int> CurrentPageNumber(std::vector<PageRect> const& pages, ViewportGeometry const& viewport);
}
