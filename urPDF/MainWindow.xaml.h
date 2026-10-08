#pragma once

#include "MainWindow.g.h"
#include "pch.h"
#include "PdfPageLayout.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include <atomic>
#include <map>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>

namespace winrt::urPDF::implementation
{
    enum class PageRenderState
    {
        ready,
        unloaded,
        loading,
        error
    };


    struct PageSlot
    {
        fz_rect pdfBounds{};
        PageRenderState state{ PageRenderState::unloaded };
        std::optional<winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> bitmap;
        std::uint64_t generation{ 0 };
        int zoomBucket{ 0 };

        bool requestInFlight{ false };
        std::uint64_t requestGeneration{ 0 };
        int requestZoomBucket{ 0 };
        int requestRotation{ 0 };

        bool IsRequestInFlight(std::uint64_t generation, int zoomBucketValue, int rotation) const noexcept
        {
            return requestInFlight && requestGeneration == generation && requestZoomBucket == zoomBucketValue && requestRotation == rotation;
        }
    };


    enum class PageRenderPriority
    {
        visible = 0,
        prefetch = 1
    };


    struct PageRenderRequest
    {
        int32_t pageIndex{ 0 };
        PageRenderPriority priority{ PageRenderPriority::visible };
        int32_t distance{ 0 };
    };


    struct PageViewRange
    {
        int32_t firstVisible{ 0 };
        int32_t lastVisible{ -1 };
        int32_t keepFirst{ 0 };
        int32_t keepLast{ -1 };
    };


    struct PageBitmapCacheViewContext
    {
        std::uint64_t documentId{ 0 };
        int32_t firstPage{ 0 };
        int32_t lastPage{ -1 };
        int rotation{ 0 };
        int zoomBucket{ 0 };
    };


    struct PageBitmapCacheKey
    {
        std::uint64_t documentId{ 0 };
        int pageIndex{ 0 };
        int rotation{ 0 };
        int zoomBucket{ 0 };

        bool operator== (PageBitmapCacheKey const& other) const noexcept
        {
            if (documentId == other.documentId && pageIndex == other.pageIndex && rotation == other.rotation && zoomBucket == other.zoomBucket)
            {
                return true;
            }
            return false;
        }
    };


    struct PageBitmapCacheKeyHash
    {
        std::size_t operator()(PageBitmapCacheKey const& key) const noexcept;
    };


    class PageBitmapCache
    {
        struct CacheEntry
        {
            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap{ nullptr };
            std::size_t memoryBytes{ 0 };
            std::list<PageBitmapCacheKey>::iterator lruPosition;
        };

        void MarkAsRecentlyUsed(std::unordered_map<PageBitmapCacheKey, CacheEntry, PageBitmapCacheKeyHash>::iterator cacheEntry);
        bool TryEvictOneEntry();
        void EvictEntry(PageBitmapCacheKey const& key);

        std::size_t m_budgetBytes{ 0 };
        std::size_t m_usedBytes{ 0 };
        bool m_hasViewContext{ false };
        PageBitmapCacheViewContext m_viewContext;
        std::list<PageBitmapCacheKey> m_lru;
        std::unordered_map<PageBitmapCacheKey, CacheEntry, PageBitmapCacheKeyHash> m_entries;

    public:
        explicit PageBitmapCache(std::size_t budgetBytes);

        std::optional<winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> TryGetBitmap(PageBitmapCacheKey const& key);
        void StoreBitmap(PageBitmapCacheKey const& key, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap const& bitmap);
        void ClearDocumentEntries(std::uint64_t documentId);

        // Describes the active document and its retained page window. Used to pick eviction victims.
        void SetViewContext(PageBitmapCacheViewContext const& context);
        // Evicts entries until the budget is satisfied (entries of the retained window are never evicted).
        void TrimToBudget();
    };


    class PdfTabInfo
    {
    public:
        PdfTabInfo(fz_context* m_ctx, const std::string& filePath);
        ~PdfTabInfo();

        winrt::hstring m_openedFilePath;
        winrt::hstring m_fileName = L"unknown";
        std::vector<PageSlot> m_renderedPages;
        int32_t m_currentPageIndex = { 1 };
        double m_currentPageZoom = { 1.0 };
        int m_currentRotation = { 0 };
        std::atomic<std::uint64_t> m_renderGeneration{ 0 };
        
        fz_context* m_ctx{ nullptr };
        fz_document* m_doc{ nullptr };
        int m_pagecount{ 0 };
        std::vector<fz_rect> m_pageBounds;
        std::uint64_t m_documentId{ 0 };

        // Last retained page window (visible + prefetch + margin), used to restore slots from cache on tab switch.
        int32_t m_retainedPageFirst{ 0 };
        int32_t m_retainedPageLast{ -1 };
    };


    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        int32_t DisplayPageNumber();
        void DisplayPageNumber(int32_t value);
        int32_t DisplayPageCount();
        void DisplayPageCount(int32_t value);

        void ApplyTheme(winrt::hstring const& themeName);
        void ApplyBackdrop(winrt::hstring const& transparency);
        void LoadSettings();

        winrt::fire_and_forget OpenPdfFileAsync(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget OpenPdfFileAsync(winrt::Windows::Storage::StorageFile const& file);
        void TabView_AddButtonClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const& args);
        void TabView_TabCloseRequested(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args);
        void TabView_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void TabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void EmptyTab_DragOver(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void EmptyTab_DragLeave(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::fire_and_forget EmptyTab_Drop(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::Windows::Foundation::IAsyncAction RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTabInfo);
        winrt::Windows::Foundation::IAsyncAction RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab);
        winrt::Windows::Foundation::IAsyncAction LoadDocumentLayoutAsync(std::shared_ptr<PdfTabInfo> PdfTab);
        winrt::fire_and_forget RenderVisiblePagesAsync();
        winrt::fire_and_forget PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args);
        void PagesRepeater_ElementPrepared(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementPreparedEventArgs const& args);
        void PagesRepeater_ElementClearing(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementClearingEventArgs const& args);
        void SettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget RotateButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void PageNumberBox_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const& sender, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);
        void NextPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void PreviousPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void BookViewButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void AboutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget PrintButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void CloseSettings_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ZoomInButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget ZoomOutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept;

    private:
        std::array<std::mutex, FZ_LOCK_MAX> m_mupdfMutexes;
        fz_locks_context m_mupdfLocks{};
        fz_context* m_ctx{ nullptr };
        std::map<winrt::Microsoft::UI::Xaml::Controls::TabViewItem, std::shared_ptr<PdfTabInfo>> m_openedTabs;
        int32_t m_pageNumber{ 0 };
        int32_t m_pageCounter{ 0 };
        winrt::Windows::Storage::StorageFile m_pendingFile{ nullptr };
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;

        winrt::Microsoft::UI::Xaml::Printing::PrintDocument m_printDocument{ nullptr };
        winrt::event_token m_printTaskRequestedToken{};
        bool m_isRendering{ false };
        bool m_isNormalizingZoom{ false };
        bool m_zoomWasEnabled{ false };
        bool m_bookViewMode{ false };
        bool m_isVisibleRenderRunning{ false };
        bool m_visibleRenderRequested{ false };
        int m_layoutRotation{ 0 };
        std::uint64_t m_nextDocumentId{ 1 };
        PageBitmapCache m_pageBitmapCache{ 256ull * 1024ull * 1024ull };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> m_pageRepeaterItems{ nullptr };

        bool TryAcquireRenderingLock();
        void ReleaseRenderingLock();
        void RebuildPagesContainer(std::vector<PageSlot> const& pages, double renderZoom);
        void ConfigurePageElement(winrt::Microsoft::UI::Xaml::UIElement const& element, std::size_t pageIndex);
        std::vector<PageRenderRequest> BuildRenderQueue(int32_t pageCount, PageViewRange& viewRange);
        void ReleaseSlotsOutsideRange(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t keepFirst, int32_t keepLast);
        void RestoreSlotsFromCache(std::shared_ptr<PdfTabInfo> const& PdfTab);
        void ApplyPageBitmap(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t pageIndex, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap const& bitmap, int zoomBucket);
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
