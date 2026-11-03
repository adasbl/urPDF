#pragma once

// Generated
#include "MainWindow.g.h"

// Project
#include "DocumentViewState.h"
#include "MuPdfRuntime.h"
#include "PdfPageLayout.h"
#include "PdfRenderScale.h"
#include "WindowCreationOptions.h"

// External libraries
extern "C" {
#include <mupdf/fitz.h>
}

// Standard C++
#include <atomic>
#include <chrono>
#include <map>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Windows SDK / WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>

namespace winrt::urPDF::implementation
{
    namespace xaml        = winrt::Microsoft::UI::Xaml;
    namespace controls    = winrt::Microsoft::UI::Xaml::Controls;
    namespace imaging     = winrt::Microsoft::UI::Xaml::Media::Imaging;
    namespace dispatching = winrt::Microsoft::UI::Dispatching;
    namespace foundation  = winrt::Windows::Foundation;
    namespace storage     = winrt::Windows::Storage;

    enum class PageRenderState
    {
        ready,
        unloaded,
        loading,
        error
    };


    enum class PageRenderOutcome
    {
        applied,
        discarded,
        error
    };


    struct PageRenderMetrics
    {
        double elapsedMs{ 0.0 };
        std::uint64_t generation{ 0 };
        double zoom{ 1.0 };
        double renderScale{ 0.0 };
        int renderScaleBucket{ 0 };
        int rotation{ 0 };
        bool bitmapCreated{ false };
        int32_t pixelWidth{ 0 };
        int32_t pixelHeight{ 0 };
        std::uint64_t pixelCount{ 0 };
        std::uint64_t estimatedBufferBytes{ 0 };
        std::uint64_t documentId{ 0 };
        int32_t pageNumber{ 0 };
        PageRenderOutcome status{ PageRenderOutcome::discarded };
    };


    struct PageSlot
    {
        fz_rect pdfBounds{};
        PageRenderState state{ PageRenderState::unloaded };
        std::optional<imaging::WriteableBitmap> bitmap;
        std::uint64_t generation{ 0 };
        int renderScaleBucket{ 0 };

        std::optional<PageRenderMetrics> lastRenderMetrics;

        bool requestInFlight{ false };
        std::uint64_t requestGeneration{ 0 };
        int requestRenderScaleBucket{ 0 };
        int requestRotation{ 0 };

        bool IsRequestInFlight(std::uint64_t generationValue, int scaleBucket, int rotation) const noexcept
        {
            return requestInFlight && requestGeneration == generationValue && requestRenderScaleBucket == scaleBucket && requestRotation == rotation;
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
        std::vector<int> renderScaleBuckets;
    };


    using urpdf::PageBitmapCacheKey;
    using urpdf::PageBitmapCacheKeyHash;


    struct PageBitmapCacheMetrics
    {
        std::uint64_t hits{ 0 };
        std::uint64_t misses{ 0 };

        std::optional<double> HitRate() const noexcept
        {
            if (hits == 0 && misses == 0)
            {
                return std::nullopt;
            }

            return static_cast<double>(hits) / (static_cast<double>(hits) + static_cast<double>(misses));
        }
    };


    class PageBitmapCache
    {
        struct CacheEntry
        {
            imaging::WriteableBitmap bitmap{ nullptr };
            std::size_t memoryBytes{ 0 };
            std::list<PageBitmapCacheKey>::iterator lruPosition;
        };

        void MarkAsRecentlyUsed(std::unordered_map<PageBitmapCacheKey, CacheEntry, PageBitmapCacheKeyHash>::iterator cacheEntry);
        bool TryEvictOneEntry();
        void EvictEntry(PageBitmapCacheKey const& key);

        std::size_t m_budgetBytes{ 0 };
        std::size_t m_usedBytes{ 0 };
        PageBitmapCacheMetrics m_metrics;
        bool m_hasViewContext{ false };
        PageBitmapCacheViewContext m_viewContext;
        std::list<PageBitmapCacheKey> m_lru;
        std::unordered_map<PageBitmapCacheKey, CacheEntry, PageBitmapCacheKeyHash> m_entries;

    public:
        explicit PageBitmapCache(std::size_t budgetBytes);

        std::optional<imaging::WriteableBitmap> TryGetBitmap(PageBitmapCacheKey const& key);
        void StoreBitmap(PageBitmapCacheKey const& key, imaging::WriteableBitmap const& bitmap);
        void ClearDocumentEntries(std::uint64_t documentId);

        PageBitmapCacheMetrics GetMetrics() const noexcept { return m_metrics; }

        void SetViewContext(PageBitmapCacheViewContext const& context);
        void TrimToBudget();
    };


    struct DocumentRenderMetrics
    {
        std::optional<std::chrono::steady_clock::time_point> openStartedAt;
        std::optional<double> firstPageAttachedMs;
        std::optional<int32_t> firstVisiblePageIndex;
        std::optional<PageRenderMetrics> lastRenderAttempt;
        double totalRenderMs{ 0.0 };
        std::uint64_t appliedRenderCount{ 0 };
        std::uint64_t discardedRenderCount{ 0 };
        std::uint64_t failedRenderCount{ 0 };
    };


    class PdfTabInfo
    {
    public:
        PdfTabInfo(std::shared_ptr<urpdf::MuPdfRuntime> runtime, const std::string& filePath);
        ~PdfTabInfo();

        winrt::hstring m_openedFilePath;
        winrt::hstring m_fileName = L"unknown";
        std::vector<PageSlot> m_renderedPages;
        int32_t m_currentPageIndex = { 1 };
        urpdf::DocumentViewState m_viewState{};
        bool m_hasViewAnchor{ false };
        double m_renderZoom{ 1.0 };
        std::atomic<std::uint64_t> m_renderGeneration{ 0 };
        
        std::shared_ptr<urpdf::MuPdfRuntime> m_runtime;
        urpdf::MuPdfRuntime::Context m_ctx;
        std::mutex m_documentMutex;
        fz_document* m_doc{ nullptr };
        int m_pagecount{ 0 };
        bool m_layoutLoaded{ false };
        std::vector<fz_rect> m_pageBounds;
        std::uint64_t m_documentId{ 0 };
        std::uint64_t m_slotBitmapReuses{ 0 };
        DocumentRenderMetrics m_renderMetrics;

        int32_t m_retainedPageFirst{ 0 };
        int32_t m_retainedPageLast{ -1 };
    };


    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        explicit MainWindow(urpdf::WindowCreationOptions const& options);
        ~MainWindow();

        int32_t DisplayPageNumber();
        void DisplayPageNumber(int32_t value);
        int32_t DisplayPageCount();
        void DisplayPageCount(int32_t value);

        void ApplyTheme(winrt::hstring const& themeName);
        void ApplyBackdrop(winrt::hstring const& transparency);
        void LoadSettings();

        winrt::fire_and_forget OpenPdfFileAsync(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget OpenPdfInNewWindowAsync(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& args);
        void OpenPdfFileAsync(storage::StorageFile const& file);
        winrt::fire_and_forget OpenPdfPathAsync(winrt::hstring path);        
        void TabView_AddButtonClick(foundation::IInspectable const& sender, foundation::IInspectable const& args);
        void TabView_TabCloseRequested(foundation::IInspectable const& sender, controls::TabViewTabCloseRequestedEventArgs const& args);
        void TabView_Loaded(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void TabView_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& args);
        void EmptyTab_DragOver(foundation::IInspectable const& sender, xaml::DragEventArgs const& args);
        void EmptyTab_DragLeave(foundation::IInspectable const& sender, xaml::DragEventArgs const& args);
        winrt::fire_and_forget EmptyTab_Drop(foundation::IInspectable const& sender, xaml::DragEventArgs const& args);
        void RenderVisiblePagesAsync();
        winrt::fire_and_forget PdfScrollViewer_ViewChanged(foundation::IInspectable const& sender, controls::ScrollViewerViewChangedEventArgs const& args);
        void PagesRepeater_ElementPrepared(foundation::IInspectable const& sender, controls::ItemsRepeaterElementPreparedEventArgs const& args);
        void PagesRepeater_ElementClearing(foundation::IInspectable const& sender, controls::ItemsRepeaterElementClearingEventArgs const& args);
        void SettingsButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget RotateButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void PageNumberBox_ValueChanged(controls::NumberBox const& sender, controls::NumberBoxValueChangedEventArgs const& args);
        void NextPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void PreviousPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void BookViewButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void AboutButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget PrintButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        void CloseSettings_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ZoomInButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget ZoomOutButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e);

        winrt::event_token PropertyChanged(xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept;

    private:
        foundation::IAsyncOperation<storage::StorageFile> PickPdfFileAsync();
        winrt::fire_and_forget ShowOpenErrorAsync(winrt::hstring message);
        controls::ContentDialog m_openErrorDialog{ nullptr };
        std::list<winrt::hstring> m_openErrorMessages;

        std::shared_ptr<urpdf::MuPdfRuntime> m_runtime;
        urpdf::MuPdfRuntime::Context m_ctx;
        std::map<controls::TabViewItem, std::shared_ptr<PdfTabInfo>> m_openedTabs;
        std::shared_ptr<PdfTabInfo> m_loadingTab;
        std::shared_ptr<std::atomic<bool>> m_workCancelled{ std::make_shared<std::atomic<bool>>(false) };
        std::weak_ptr<PdfTabInfo> m_presentedTab;
        int32_t m_pageNumber{ 0 };
        int32_t m_pageCounter{ 0 };
        storage::StorageFile m_pendingFile{ nullptr };
        winrt::hstring m_pendingFilePath;
        winrt::event<xaml::Data::PropertyChangedEventHandler> m_propertyChanged;

        xaml::Printing::PrintDocument m_printDocument{ nullptr };
        winrt::Windows::Graphics::Printing::PrintManager m_printManager{ nullptr };
        winrt::event_token m_printTaskRequestedToken{};
        bool m_isRendering{ false };
        urpdf::ViewStateGuard m_viewStateGuard;
        bool m_isViewChanging{ false };
        std::weak_ptr<PdfTabInfo> m_pendingLayoutViewSaveTab;
        std::optional<double> m_pendingLayoutViewOffsetX;
        std::optional<double> m_pendingLayoutViewOffsetY;
        std::optional<double> m_pendingLayoutViewZoom;
        std::optional<std::array<double, 10>> m_pendingViewSnapshot;
        enum class ViewRestorePhase { None, Layout, Zoom, Offsets };
        ViewRestorePhase m_viewRestorePhase{ ViewRestorePhase::None };
        std::optional<urpdf::DocumentViewState> m_pendingRestoreState;
        bool m_pendingRestoreHasAnchor{ false };
        bool m_viewOperationReady{ false };
        bool m_layoutViewSaveQueued{ false };
        xaml::Media::CompositionTarget::Rendering_revoker m_viewStateRenderingRevoker;
        bool m_zoomWasEnabled{ false };
        bool m_isVisibleRenderRunning{ false };
        bool m_isClosed{ false };
        std::optional<std::vector<winrt::Windows::Graphics::RectInt32>> m_passthroughRects;
        xaml::XamlRoot m_observedXamlRoot{ nullptr };
        xaml::XamlRoot::Changed_revoker m_xamlRootChangedRevoker;
        dispatching::DispatcherQueueTimer m_dpiRenderTimer{ nullptr };
        double m_lastRasterizationScale{ 1.0 };
        bool m_dpiRefreshPending{ false };
        dispatching::DispatcherQueueTimer m_zoomRenderTimer{ nullptr };
        std::weak_ptr<PdfTabInfo> m_pendingZoomTab;
        bool m_zoomIsIntermediate{ false };
        bool m_zoomDebounceElapsed{ false };
        int m_layoutRotation{ 0 };
        std::uint64_t m_nextDocumentId{ 1 };
        PageBitmapCache m_pageBitmapCache{ 256ull * 1024ull * 1024ull };
        foundation::Collections::IObservableVector<foundation::IInspectable> m_pageRepeaterItems{ nullptr };

        void CloseTab(controls::TabViewItem const& tab);
        void ShutdownMuPdfWork() noexcept;
        void CompleteDocumentOpen(std::shared_ptr<PdfTabInfo> const& pdfTab, std::uint64_t generation,
            int pagecount, std::vector<fz_rect> pageBounds, std::exception_ptr error);
        void CompletePageRender(std::weak_ptr<PdfTabInfo> const& weakTab, PageRenderMetrics metrics,
            winrt::Windows::Graphics::Imaging::SoftwareBitmap const& bitmap, bool rendered,
            std::exception_ptr error, bool scheduleNext = true);
        void ReadViewGeometry(std::shared_ptr<PdfTabInfo> const& pdfTab, std::vector<urpdf::PageRect>& pages, urpdf::ViewportGeometry& viewport);
        std::optional<int32_t> VisiblePageNumber(std::shared_ptr<PdfTabInfo> const& pdfTab);
        std::optional<urpdf::DocumentViewState> CaptureViewState(std::shared_ptr<PdfTabInfo> const& pdfTab);
        void SaveViewState(std::shared_ptr<PdfTabInfo> const& pdfTab);
        bool HasViewGeometry(std::shared_ptr<PdfTabInfo> const& pdfTab);
        void UpdatePageCounter(std::shared_ptr<PdfTabInfo> const& pdfTab);
        void RestoreViewState(std::shared_ptr<PdfTabInfo> const& pdfTab);
        urpdf::ViewStateGuard::Operation DeferViewStateSave(std::shared_ptr<PdfTabInfo> const& pdfTab);
        bool IsCurrentViewOperation(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation);
        void FinishViewStateChange(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation);
        void CancelPendingViewStateSave();
        void QueuePendingViewStateSave();
        void AdvanceViewStateChange(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation);
        void ScrollToPage(int32_t page);
        bool TryAcquireRenderingLock();
        void ReleaseRenderingLock();
        void UpdateRenderZoom(std::shared_ptr<PdfTabInfo> const& pdfTab, double zoom, bool isIntermediate);
        void ApplyPendingRenderZoom();
        void CancelPendingZoomRender();
        void AttachDpiObserver();
        void UpdateTitleBarPassthroughRegions();
        void DetachDpiObserver();
        void RequestDpiRefresh();
        void ApplyPendingDpiRefresh();
        void RebuildPagesContainer(std::shared_ptr<PdfTabInfo> const& pdfTab, std::vector<PageSlot> const& pages, urpdf::ViewStateGuard::Operation operation);
        void ConfigurePageElement(xaml::UIElement const& element, std::size_t pageIndex);
        void TryRecordFirstVisiblePage(std::shared_ptr<PdfTabInfo> const& PdfTab);
        std::vector<PageRenderRequest> BuildRenderQueue(int32_t pageCount, PageViewRange& viewRange);
        void ReleaseSlotsOutsideRange(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t keepFirst, int32_t keepLast);
        void RestoreSlotsFromCache(std::shared_ptr<PdfTabInfo> const& PdfTab);
        double GetRasterizationScale();
        int GetPageRenderScaleBucket(std::shared_ptr<PdfTabInfo> const& pdfTab, int32_t pageIndex);
        bool ApplyPageBitmap(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t pageIndex, imaging::WriteableBitmap const& bitmap, int renderScaleBucket);
        void RecordPageRenderMetrics(std::shared_ptr<PdfTabInfo> const& PdfTab, PageRenderMetrics const& metrics) noexcept;
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
