    #include "pch.h"
    #include <winrt/Windows.UI.Xaml.Interop.h>
    #include "MainWindow.xaml.h"
    #include "SettingsPage.h"
    #include "AboutPage.h"
    #if __has_include("MainWindow.g.cpp")
    #include "MainWindow.g.cpp"
    #endif

    extern "C" {
    #include <mupdf/fitz.h>
    }

    #include <winrt/Windows.Storage.h>
    #include <winrt/Windows.Storage.Pickers.h>
    #include <winrt/Windows.Storage.Streams.h>
    #include <winrt/Windows.ApplicationModel.DataTransfer.h>
    #include <winrt/Windows.Graphics.Imaging.h>
    #include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
    #include <microsoft.ui.xaml.window.h>
    #include <shobjidl_core.h>
    #include <MemoryBuffer.h>
    #include <map>
    #include <winrt/Microsoft.UI.Dispatching.h>
    #include <algorithm>
    #include <cwctype>
    #include <string>
    #include <memory>
    #include <coroutine>
    #include <winrt/Windows.Graphics.Printing.h>
    #include <winrt/Microsoft.UI.Xaml.Printing.h>
    #include <winrt/Microsoft.Windows.AppLifecycle.h>
    #include <printmanagerinterop.h>
    #include <winrt/Microsoft.UI.Windowing.h>

    
    using namespace winrt;
    using namespace winrt::Windows::Storage::Pickers;
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Composition::SystemBackdrops;
    using namespace Microsoft::UI::Composition;
    
    namespace
    {
        void LockMuPdf(void* user, int lock)
        {
            static_cast<std::mutex*>(user)[lock].lock();
        }

        void UnlockMuPdf(void* user, int lock)
        {
            static_cast<std::mutex*>(user)[lock].unlock();
        }

        constexpr double PdfRenderScale(double zoom) noexcept
        {
            constexpr double baseDpi = 96.0;
            constexpr double pdfPointsPerInch = 72.0;
            return (baseDpi / pdfPointsPerInch) * zoom;
        }
    }

    namespace winrt::urPDF::implementation
    {
        // MAINWINDOW
        MainWindow::~MainWindow()
        {
            if (m_ctx) {
                fz_drop_context(m_ctx);
                m_ctx = nullptr;
            }
        }

        MainWindow::MainWindow()
        {
            InitializeComponent();

            AppWindow().SetIcon(L"Assets\\logo.ico");

            ExtendsContentIntoTitleBar(true);
            SetTitleBar(AppTitleBar());

            LoadSettings();

            m_mupdfLocks.user = m_mupdfMutexes.data();
            m_mupdfLocks.lock = LockMuPdf;
            m_mupdfLocks.unlock = UnlockMuPdf;
            m_ctx = fz_new_context(nullptr, &m_mupdfLocks, FZ_STORE_UNLIMITED);
            fz_register_document_handlers(m_ctx);

            auto activationArgs = winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
            if (activationArgs.Kind() == winrt::Microsoft::Windows::AppLifecycle::ExtendedActivationKind::File)
            {
                auto fileArgs = activationArgs.Data().try_as<winrt::Windows::ApplicationModel::Activation::IFileActivatedEventArgs>();
                if (fileArgs && fileArgs.Files().Size() > 0)
                {
                    m_pendingFile = fileArgs.Files().GetAt(0).try_as<winrt::Windows::Storage::StorageFile>();
                }
            }
        }


        // HELPER FUNCTIONS
        auto resume_foreground(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher)
        {
            struct awaitable
            {
                winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher;
                bool await_ready() const noexcept { return false; }
                void await_suspend(std::coroutine_handle<> handle) const
                {
                    m_dispatcher.TryEnqueue([handle]() { handle(); });
                }
                void await_resume() const noexcept {}
            };
            return awaitable{ dispatcher };
        }


        // PDFTABINFO
        PdfTabInfo::PdfTabInfo(fz_context* ctx, const std::string& filePath)
        {
            m_ctx = ctx;
            m_doc = fz_open_document(m_ctx, filePath.c_str());
        }

        PdfTabInfo::~PdfTabInfo()
        {
            fz_drop_document(m_ctx, m_doc);
        }


        // XAML DATA
        winrt::event_token MainWindow::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
        {
            return m_propertyChanged.add(handler);
        }

        void MainWindow::PropertyChanged(winrt::event_token const& token) noexcept
        {
            m_propertyChanged.remove(token);
        }

        int32_t MainWindow::DisplayPageNumber()
        {
            return m_pageNumber;
        }

        void MainWindow::DisplayPageNumber(int32_t value)
        {
            if (m_pageNumber != value)
            {
                m_pageNumber = value;
                m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"DisplayPageNumber" });
            }
        }

        int32_t MainWindow::DisplayPageCount()
        {
            return m_pageCounter;
        }

        void MainWindow::DisplayPageCount(int32_t value)
        {
            if (m_pageCounter != value)
            {
                m_pageCounter = value;
                m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"DisplayPageCount" });
            }
        }


        // SETTINGS 
        void MainWindow::SettingsButton_Click(
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
            winrt::urPDF::SettingsPage settingsPage;

            settingsPage.BackRequested([this](auto const&, auto const&)
            {
                SettingsFrame().Content(nullptr);
                SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                MainWindowUI().IsHitTestVisible(true);
            });

            settingsPage.ThemeChanged([this](auto const&, winrt::hstring const& themeName)
            {
                ApplyTheme(themeName);
            });

            settingsPage.BackdropChanged([this](auto const&, winrt::hstring const& transparencyValue)
            {
                ApplyBackdrop(transparencyValue);
            });

            SettingsFrame().Content(settingsPage);
            SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            MainWindowUI().IsHitTestVisible(false);
        }

        void MainWindow::CloseSettings_Click(
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
            SettingsFrame().Content(nullptr);
            SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            MainWindowUI().IsHitTestVisible(true);
        }

        void MainWindow::LoadSettings()
        {
            auto localSettings = winrt::Windows::Storage::ApplicationData::Current().LocalSettings();

            if (localSettings.Values().HasKey(L"AppTheme"))
            {
                winrt::hstring theme = winrt::unbox_value<winrt::hstring>(localSettings.Values().Lookup(L"AppTheme"));
                ApplyTheme(theme);
            }
            else
            {
                ApplyTheme(L"Default");
            }

            if (localSettings.Values().HasKey(L"Transparency"))
            {
                winrt::hstring transparency = winrt::unbox_value<winrt::hstring>(localSettings.Values().Lookup(L"Transparency"));
                ApplyBackdrop(transparency);
            }
            else
            {
                ApplyBackdrop(L"None");
            }
        }

        void MainWindow::ApplyBackdrop(winrt::hstring const& transparencyValue)
        {
            if (transparencyValue == L"Mica")
            {
                auto mica = winrt::Microsoft::UI::Xaml::Media::MicaBackdrop();
                mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::Base);
                this->SystemBackdrop(mica);
            }
            else if (transparencyValue == L"MicaAlt")
            {
                winrt::Microsoft::UI::Xaml::Media::MicaBackdrop backdrop;
                backdrop.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
                SystemBackdrop(backdrop);
            }
            else if (transparencyValue == L"None")
            {
                this->SystemBackdrop(nullptr);
            }
            else
            {
                this->SystemBackdrop(nullptr);
            }
        }

        void MainWindow::ApplyTheme(winrt::hstring const& themeName)
        {
            ElementTheme theme = ElementTheme::Default;

            if (themeName == L"Light")
            {
                theme = ElementTheme::Light;
            }
            else if (themeName == L"Dark")
            {
                theme = ElementTheme::Dark;
            }
            else
            {
                theme = ElementTheme::Default;
            }

            if (auto rootElement = Content().try_as<FrameworkElement>())
            {
                rootElement.RequestedTheme(theme);
            }
        }


        // ABOUT
        void winrt::urPDF::implementation::MainWindow::AboutButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
            MainWindowUI().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

            RootFrame().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            RootFrame().Navigate(winrt::xaml_typename<winrt::urPDF::AboutPage>());
        }


        // TABVIEW
        void MainWindow::TabView_Loaded(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
            TabView_AddButtonClick(tabView, nullptr);

            if (m_pendingFile)
            {
                auto file = m_pendingFile;
                m_pendingFile = nullptr;
                OpenPdfFileAsync(file);
            }
        }

        void MainWindow::TabView_AddButtonClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const&)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            winrt::Microsoft::UI::Xaml::Controls::TabViewItem newTab;

            newTab.Header(winrt::box_value(L"New document"));
            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            newTab.IconSource(tabIcon);

            tabView.TabItems().Append(newTab);

            tabView.SelectedItem(newTab);
        }

        void MainWindow::TabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
            auto currentTab = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab)
            {
                PagesContainer().Children().Clear();

                if (m_openedTabs.find(currentTab) != m_openedTabs.end())
                {
                    EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

                    auto pdfInfo = m_openedTabs[currentTab];

                    DisplayPageNumber(pdfInfo->m_currentPageIndex);
                    DisplayPageCount(pdfInfo->m_pagecount);
                    RebuildPagesContainer(pdfInfo->m_renderedPages);
                }
                else
                {
                    EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                }
            }
            else
            {
                EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                DisplayPageNumber(0);
                DisplayPageCount(0);
            }
        }

        void MainWindow::TabView_TabCloseRequested(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            m_openedTabs.erase(args.Tab());

            uint32_t index;
            if (tabView.TabItems().IndexOf(args.Tab(), index))
            {
                tabView.TabItems().RemoveAt(index);
            }
        }

        void MainWindow::EmptyTab_DragOver(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
        {
            if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            {
                args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
                EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
            else
            {
                args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::None);
            }
        }

        void MainWindow::EmptyTab_DragLeave(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const&)
        {
            EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }

        winrt::fire_and_forget MainWindow::EmptyTab_Drop(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
        {
            auto deferral = args.GetDeferral();
            auto storageItems = co_await args.DataView().GetStorageItemsAsync();

            EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

            for (auto const& item : storageItems)
            {
                auto file = item.try_as<winrt::Windows::Storage::StorageFile>();
                if (file)
                {
                    auto fileType = file.FileType();
                    std::wstring normalizedFileType(fileType.c_str());
                    std::transform(normalizedFileType.begin(), normalizedFileType.end(), normalizedFileType.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });

                    if (normalizedFileType == L".pdf")
                    {
                        deferral.Complete();
                        OpenPdfFileAsync(file);
                        co_return;
                    }
                }
            }

            deferral.Complete();
        }


        // FILE OPENING 
        winrt::fire_and_forget MainWindow::OpenPdfFileAsync(IInspectable const&, RoutedEventArgs const&)
        {
            // TODO: add check if empty tab is opened
            // TODO: check if file is already opened (+ option to turn off in settings)
            FileOpenPicker filePicker;
            filePicker.FileTypeFilter().Append(L".pdf");

            HWND hwnd;
            this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
            filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

            auto file = co_await filePicker.PickSingleFileAsync();
            if (!file) co_return;

            OpenPdfFileAsync(file);
        }

        winrt::fire_and_forget MainWindow::OpenPdfFileAsync(winrt::Windows::Storage::StorageFile const& file)
        {
            if (!TryAcquireRenderingLock())
            {
                co_return;
            }

            std::string filePath = winrt::to_string(file.Path());
            winrt::hstring fileName = file.Name();

            auto PdfInfo = std::make_shared<PdfTabInfo>(m_ctx, filePath);
            PdfInfo->m_fileName = fileName;

            winrt::Microsoft::UI::Xaml::Controls::TabViewItem targetTab;

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            bool isCurrentTabEmpty = currentTab && !m_openedTabs.contains(currentTab);

            if (isCurrentTabEmpty)
            {
                targetTab = currentTab;
                targetTab.Content(nullptr);
            }
            else
            {
                targetTab = winrt::Microsoft::UI::Xaml::Controls::TabViewItem();

                MainTabView().TabItems().Append(targetTab);
                MainTabView().SelectedItem(targetTab);
            }
            PagesContainer().Children().Clear();
            EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            targetTab.Header(winrt::box_value(fileName));

            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            targetTab.IconSource(tabIcon);

            m_openedTabs[targetTab] = PdfInfo;

            co_await RenderMultiPageAsync(PdfInfo);
            ReleaseRenderingLock();
        }


        // PAGE RENDERING
        winrt::Windows::Foundation::IAsyncAction MainWindow::RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
        {
            fz_context* renderCtx = fz_clone_context(m_ctx);
            if (!renderCtx)
            {
                co_return;
            }

            co_await winrt::resume_background();

            int pagecount = fz_count_pages(renderCtx, PdfTab->m_doc);
            PdfTab->m_pagecount = pagecount;

            fz_page* page = fz_load_page(renderCtx, PdfTab->m_doc, 0);

            double renderScale = PdfRenderScale(PdfTab->m_currentPageZoom);
            fz_matrix transform = fz_scale(renderScale, renderScale);
            transform = fz_concat(transform, fz_rotate(PdfTab->m_currentRotation));

            fz_rect rect = fz_bound_page(renderCtx, page);
            rect = fz_transform_rect(rect, transform);
            fz_irect bbox = fz_round_rect(rect);

            fz_pixmap* pix = fz_new_pixmap_with_bbox(renderCtx, fz_device_bgr(renderCtx), bbox, NULL, 1);
            fz_clear_pixmap_with_value(renderCtx, pix, 255);

            fz_device* dev = fz_new_draw_device(renderCtx, fz_identity, pix);
            fz_run_page(renderCtx, page, dev, transform, NULL);
            fz_close_device(renderCtx, dev);

            int width = fz_pixmap_width(renderCtx, pix);
            int height = fz_pixmap_height(renderCtx, pix);

            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
            {
                BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                uint8_t* data = reference.data();

                memcpy(data, fz_pixmap_samples(renderCtx, pix), width * height * 4);
            }

            fz_drop_device(renderCtx, dev);
            fz_drop_pixmap(renderCtx, pix);
            fz_drop_page(renderCtx, page);
            fz_drop_context(renderCtx);

            co_await resume_foreground(DispatcherQueue());

            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
            bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());

            std::vector<WriteableBitmap> singlePage{
                current_writeable_bitmap
            };
            RebuildPagesContainer(singlePage);

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (currentTab)
            {
                currentTab.Tag(current_writeable_bitmap);
                currentTab.Header(winrt::box_value(PdfTab->m_fileName));
                DisplayPageCount(PdfTab->m_pagecount);
                DisplayPageNumber(PdfTab->m_currentPageIndex);
            }
        }

        winrt::Windows::Foundation::IAsyncAction MainWindow::RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
        {
            std::vector<WriteableBitmap> renderedPages;

            fz_context* renderCtx = fz_clone_context(m_ctx);
            if (!renderCtx)
            {
                co_return;
            }

            co_await winrt::resume_background();

            int pagecount = fz_count_pages(renderCtx, PdfTab->m_doc);
            PdfTab->m_pagecount = pagecount;
            renderedPages.reserve(pagecount);

            double renderScale = PdfRenderScale(PdfTab->m_currentPageZoom);
            fz_matrix transform = fz_scale(renderScale, renderScale);
            transform = fz_concat(transform, fz_rotate(PdfTab->m_currentRotation));

            for (int i = 0; i < pagecount; i++)
            {
                fz_page* page = fz_load_page(renderCtx, PdfTab->m_doc, i);

                fz_rect rect = fz_bound_page(renderCtx, page);
                rect = fz_transform_rect(rect, transform);
                fz_irect bbox = fz_round_rect(rect);

                fz_pixmap* pix = fz_new_pixmap_with_bbox(renderCtx, fz_device_bgr(renderCtx), bbox, NULL, 1);
                fz_clear_pixmap_with_value(renderCtx, pix, 255);

                fz_device* dev = fz_new_draw_device(renderCtx, fz_identity, pix);
                fz_run_page(renderCtx, page, dev, transform, NULL);
                fz_close_device(renderCtx, dev);
                    
                int width = fz_pixmap_width(renderCtx, pix);
                int height = fz_pixmap_height(renderCtx, pix);

                SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
                {
                    BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                    winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                    uint8_t* data = reference.data();

                    memcpy(data, fz_pixmap_samples(renderCtx, pix), width * height * 4);
                }

                fz_drop_device(renderCtx, dev);
                fz_drop_pixmap(renderCtx, pix);
                fz_drop_page(renderCtx, page);
                
                co_await resume_foreground(DispatcherQueue());

                winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
                bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
                renderedPages.push_back(current_writeable_bitmap);

                if (i < pagecount - 1)
                {
                    co_await winrt::resume_background();
                }
            }
            fz_drop_context(renderCtx);

            co_await resume_foreground(DispatcherQueue());

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            bool isCurrentTab = false;
            if (currentTab)
            {
                auto openedTab = m_openedTabs.find(currentTab);
                isCurrentTab = openedTab != m_openedTabs.end() && openedTab->second == PdfTab;
            }

            if (isCurrentTab)
            {
                RebuildPagesContainer(renderedPages);

                PdfTab->m_renderedPages = std::move(renderedPages);
                currentTab.Header(winrt::box_value(PdfTab->m_fileName));
                DisplayPageCount(PdfTab->m_pagecount);
                DisplayPageNumber(PdfTab->m_currentPageIndex);
            }
            else
            {
                PdfTab->m_renderedPages = std::move(renderedPages);
            }
        }

        bool MainWindow::TryAcquireRenderingLock()
        {
            if (m_isRendering)
            {
                return false;
            }

            m_isRendering = true;
            m_zoomWasEnabled = PdfScrollViewer().ZoomMode() == winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled;
            PdfScrollViewer().ZoomMode(winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Disabled);
            ZoomInButton().IsEnabled(false);
            ZoomOutButton().IsEnabled(false);
            RotateButton().IsEnabled(false);
            return true;
        }

        void MainWindow::ReleaseRenderingLock()
        {
            if (m_zoomWasEnabled)
            {
                PdfScrollViewer().ZoomMode(winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled);
            }
            m_zoomWasEnabled = false;
            m_isRendering = false;
            ZoomInButton().IsEnabled(true);
            ZoomOutButton().IsEnabled(true);
            RotateButton().IsEnabled(true);
        }

        void MainWindow::RebuildPagesContainer(std::vector<WriteableBitmap> const& pages)
        {
            auto children = PagesContainer().Children();
            children.Clear();

            const size_t pagesPerRow = m_bookViewMode ? 2 : 1;

            for (size_t i = 0; i < pages.size(); i += pagesPerRow)
            {
                winrt::Microsoft::UI::Xaml::Controls::StackPanel row;
                row.Orientation(
                    winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                row.HorizontalAlignment(
                    winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                row.Spacing(10);

                for (size_t j = i; j < (std::min)(i + pagesPerRow, pages.size()); ++j)
                {
                    winrt::Microsoft::UI::Xaml::Controls::Image image;
                    image.Source(pages[j]);
                    image.Stretch(
                        winrt::Microsoft::UI::Xaml::Media::Stretch::None);
                    image.VerticalAlignment(
                        winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
                    image.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 5 });

                    row.Children().Append(image);
                }

                children.Append(row);
            }
        }


        // NAVIGATION
        void MainWindow::PageNumberBox_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const& sender, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args)
        {
            if (std::isnan(args.NewValue())) return;

            int32_t newPage = static_cast<int32_t>(std::round(args.NewValue()));

            if (newPage == m_pageNumber) return;

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];
                int totalPages = pdfInfo->m_pagecount;

                if (totalPages > 1 && newPage >= 1 && newPage <= totalPages)
                {
                    int pagesPerRow = m_bookViewMode ? 2 : 1;
                    int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
                    int targetRow = (newPage - 1) / pagesPerRow;
                    double scrollPercentage = totalRows > 1
                        ? static_cast<double>(targetRow) / (totalRows - 1)
                        : 0.0;
                    double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                    PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
                }
            }
        }
        
        void MainWindow::NextPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto button = sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>();
            button.IsEnabled(false);

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];
                int currentPage = pdfInfo->m_currentPageIndex;
                int totalPages = pdfInfo->m_pagecount;
                int pagesPerRow = m_bookViewMode ? 2 : 1;
                int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
                int currentRow = (currentPage - 1) / pagesPerRow;

                if (totalRows > 1 && currentRow + 1 < totalRows)
                {
                    int targetRow = currentRow + 1;

                    double scrollPercentage = static_cast<double>(targetRow) / (totalRows - 1);
                    double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                    PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
                }
            }
            button.IsEnabled(true);
        }

        void MainWindow::PreviousPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto button = sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>();
            button.IsEnabled(false);

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];
                int currentPage = pdfInfo->m_currentPageIndex;
                int totalPages = pdfInfo->m_pagecount;
                int pagesPerRow = m_bookViewMode ? 2 : 1;
                int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
                int currentRow = (currentPage - 1) / pagesPerRow;

                if (totalRows > 1 && currentRow > 0)
                {
                    int targetRow = currentRow - 1;

                    double scrollPercentage = static_cast<double>(targetRow) / (totalRows - 1);
                    double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                    PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
                }
            }
            button.IsEnabled(true);
        }

        winrt::fire_and_forget MainWindow::PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args)
        {
            auto scrollViewer = sender.as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];
                int totalPages = pdfInfo->m_pagecount;

                if (!m_isNormalizingZoom && !args.IsIntermediate() && (scrollViewer.ZoomFactor() < 0.999f || scrollViewer.ZoomFactor() > 1.001f) && TryAcquireRenderingLock())
                {
                    constexpr double minZoom = 0.5;
                    constexpr double maxZoom = 3.0;
                    const double nativeZoom = scrollViewer.ZoomFactor();
                    const double oldContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
                    const double oldContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
                    const double horizontalCenterRatio = oldContentWidth > 0.0 ? (scrollViewer.HorizontalOffset() + scrollViewer.ViewportWidth() / 2.0) / oldContentWidth : 0.0;
                    const double verticalCenterRatio = oldContentHeight > 0.0 ? (scrollViewer.VerticalOffset() + scrollViewer.ViewportHeight() / 2.0) / oldContentHeight : 0.0;
                    const double resetContentWidth = nativeZoom > 0.0 ? oldContentWidth / nativeZoom : oldContentWidth;
                    const double resetContentHeight = nativeZoom > 0.0 ? oldContentHeight / nativeZoom : oldContentHeight;
                    const double resetHorizontalOffset = (std::clamp)(
                        horizontalCenterRatio * resetContentWidth - scrollViewer.ViewportWidth() / 2.0,
                        0.0,
                        (std::max)(0.0, resetContentWidth - scrollViewer.ViewportWidth()));
                    const double resetVerticalOffset = (std::clamp)(
                        verticalCenterRatio * resetContentHeight - scrollViewer.ViewportHeight() / 2.0,
                        0.0,
                        (std::max)(0.0, resetContentHeight - scrollViewer.ViewportHeight()));

                    pdfInfo->m_currentPageZoom = (std::clamp)(pdfInfo->m_currentPageZoom * nativeZoom, minZoom, maxZoom);

                    m_isNormalizingZoom = true;

                    scrollViewer.ChangeView(resetHorizontalOffset, resetVerticalOffset, 1.0f);
                    co_await resume_foreground(DispatcherQueue());
                    scrollViewer.UpdateLayout();

                    co_await RenderMultiPageAsync(pdfInfo);

                    co_await resume_foreground(DispatcherQueue());
                    scrollViewer.UpdateLayout();

                    const double newContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
                    const double newContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
                    const double horizontalOffset = (std::clamp)(horizontalCenterRatio * newContentWidth - scrollViewer.ViewportWidth() / 2.0, 0.0, scrollViewer.ScrollableWidth());
                    const double verticalOffset = (std::clamp)(verticalCenterRatio * newContentHeight - scrollViewer.ViewportHeight() / 2.0, 0.0, scrollViewer.ScrollableHeight());

                    scrollViewer.ChangeView(horizontalOffset, verticalOffset, nullptr);
                    co_await resume_foreground(DispatcherQueue());
                    m_isNormalizingZoom = false;
                    ReleaseRenderingLock();
                }

                double scrollOffset = scrollViewer.VerticalOffset();
                double totalHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();

                if (totalPages > 0 && totalHeight > 0)
                {
                    double scrollPercentage = scrollViewer.ScrollableHeight() > 0.0
                        ? scrollOffset / scrollViewer.ScrollableHeight()
                        : 0.0;
                    if (scrollPercentage > 1.0) scrollPercentage = 1.0;
                    if (scrollPercentage < 0.0) scrollPercentage = 0.0;

                    int pagesPerRow = m_bookViewMode ? 2 : 1;
                    int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
                    int currentRow = totalRows > 1
                        ? static_cast<int>(std::round(scrollPercentage * (totalRows - 1)))
                        : 0;

                    int32_t currentPage = (currentRow * pagesPerRow) + 1;
                    if (currentPage > totalPages)
                    {
                        currentPage = totalPages;
                    }

                    pdfInfo->m_currentPageIndex = currentPage;

                    DisplayPageNumber(currentPage);
                }
            }
        }


        // VIEW
        winrt::fire_and_forget MainWindow::RotateButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                if (!TryAcquireRenderingLock())
                {
                    co_return;
                }

                auto pdfInfo = m_openedTabs[currentTab];

                pdfInfo->m_currentRotation += 90;
                if (pdfInfo->m_currentRotation >= 360)
                {
                    pdfInfo->m_currentRotation = 0;
                }

                co_await RenderMultiPageAsync(pdfInfo);
                ReleaseRenderingLock();
            }
        }

        winrt::fire_and_forget MainWindow::ZoomInButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
            {
                co_return;
            }

            auto pdfInfo = m_openedTabs[currentTab];
            constexpr double maxZoom = 3.0;
            constexpr double zoomFactor = 1.1;

            if (pdfInfo->m_currentPageZoom >= maxZoom)
            {
                co_return;
            }

            auto scrollViewer = PdfScrollViewer();
            const double oldContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
            const double oldContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
            const double horizontalCenterRatio = oldContentWidth > 0.0 ? (scrollViewer.HorizontalOffset() + scrollViewer.ViewportWidth() / 2.0) / oldContentWidth : 0.0;
            const double verticalCenterRatio = oldContentHeight > 0.0 ? (scrollViewer.VerticalOffset() + scrollViewer.ViewportHeight() / 2.0) / oldContentHeight : 0.0;

            if (!TryAcquireRenderingLock())
            {
                co_return;
            }

            pdfInfo->m_currentPageZoom = (std::min)(maxZoom, pdfInfo->m_currentPageZoom * zoomFactor);
            co_await RenderMultiPageAsync(pdfInfo);

            co_await resume_foreground(DispatcherQueue());
            scrollViewer.UpdateLayout();
            const double newContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
            const double newContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
            const double horizontalOffset = (std::clamp)(horizontalCenterRatio * newContentWidth - scrollViewer.ViewportWidth() / 2.0, 0.0, scrollViewer.ScrollableWidth());
            const double verticalOffset = (std::clamp)(verticalCenterRatio * newContentHeight - scrollViewer.ViewportHeight() / 2.0, 0.0, scrollViewer.ScrollableHeight());
            scrollViewer.ChangeView(horizontalOffset, verticalOffset, nullptr);

            ReleaseRenderingLock();
        }

        winrt::fire_and_forget MainWindow::ZoomOutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
            {
                co_return;
            }

            auto pdfInfo = m_openedTabs[currentTab];
            constexpr double minZoom = 0.5;
            constexpr double zoomFactor = 1.1;

            if (pdfInfo->m_currentPageZoom <= minZoom)
            {
                co_return;
            }

            auto scrollViewer = PdfScrollViewer();
            const double oldContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
            const double oldContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
            const double horizontalCenterRatio = oldContentWidth > 0.0 ? (scrollViewer.HorizontalOffset() + scrollViewer.ViewportWidth() / 2.0) / oldContentWidth : 0.0;
            const double verticalCenterRatio = oldContentHeight > 0.0 ? (scrollViewer.VerticalOffset() + scrollViewer.ViewportHeight() / 2.0) / oldContentHeight : 0.0;

            if (!TryAcquireRenderingLock())
            {
                co_return;
            }

            pdfInfo->m_currentPageZoom = (std::max)(minZoom, pdfInfo->m_currentPageZoom / zoomFactor);
            co_await RenderMultiPageAsync(pdfInfo);

            co_await resume_foreground(DispatcherQueue());
            scrollViewer.UpdateLayout();
            const double newContentWidth = scrollViewer.ScrollableWidth() + scrollViewer.ViewportWidth();
            const double newContentHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
            const double horizontalOffset = (std::clamp)(horizontalCenterRatio * newContentWidth - scrollViewer.ViewportWidth() / 2.0, 0.0, scrollViewer.ScrollableWidth());
            const double verticalOffset = (std::clamp)(verticalCenterRatio * newContentHeight - scrollViewer.ViewportHeight() / 2.0, 0.0, scrollViewer.ScrollableHeight());
            scrollViewer.ChangeView(horizontalOffset, verticalOffset, nullptr);

            ReleaseRenderingLock();
        }

        void MainWindow::BookViewButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            std::shared_ptr<PdfTabInfo> pdfInfo;
            int currentPage = 1;

            if (currentTab)
            {
                auto openedTab = m_openedTabs.find(currentTab);
                if (openedTab != m_openedTabs.end())
                {
                    pdfInfo = openedTab->second;
                    currentPage = pdfInfo->m_currentPageIndex;
                }
            }

            m_bookViewMode = !m_bookViewMode;

            if (!pdfInfo)
            {
                return;
            }

            RebuildPagesContainer(pdfInfo->m_renderedPages);

            const int pagesPerRow = m_bookViewMode ? 2 : 1;
            const int totalRows = (pdfInfo->m_pagecount + pagesPerRow - 1) / pagesPerRow;
            const int targetRow = totalRows > 0 ? (std::min)((currentPage - 1) / pagesPerRow, totalRows - 1) : 0;
            const int targetPage = (targetRow * pagesPerRow) + 1;

            pdfInfo->m_currentPageIndex = targetPage;
            DisplayPageNumber(targetPage);

            PdfScrollViewer().UpdateLayout();

            const double scrollPercentage = totalRows > 1 ? static_cast<double>(targetRow) / (totalRows - 1) : 0.0;
            const double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

            PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
        }

        // PRINT
        winrt::fire_and_forget MainWindow::PrintButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
            {
                co_return;
            }
            auto pdfInfo = m_openedTabs[currentTab];

            HWND hwnd;
            this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);

            auto printInterop = winrt::get_activation_factory<winrt::Windows::Graphics::Printing::PrintManager, IPrintManagerInterop>();
            winrt::Windows::Graphics::Printing::PrintManager printManager{ nullptr };
            winrt::check_hresult(printInterop->GetForWindow(hwnd, winrt::guid_of<decltype(printManager)>(), winrt::put_abi(printManager)));

            m_printDocument = winrt::Microsoft::UI::Xaml::Printing::PrintDocument();
            auto printDocumentSource = m_printDocument.DocumentSource();

            auto printSize = std::make_shared<winrt::Windows::Foundation::Size>(0.0f, 0.0f);

            m_printDocument.Paginate([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::PaginateEventArgs const& args)
                {
                    auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();

                    auto pageDesc = args.PrintTaskOptions().GetPageDescription(1);
                    printSize->Width = pageDesc.ImageableRect.Width;
                    printSize->Height = pageDesc.ImageableRect.Height;

                    senderDoc.SetPreviewPageCount(pdfInfo->m_pagecount, winrt::Microsoft::UI::Xaml::Printing::PreviewPageCountType::Final);
                });

            m_printDocument.GetPreviewPage([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::GetPreviewPageEventArgs const& args)
                {
                    auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
                    if (args.PageNumber() > 0 && args.PageNumber() <= pdfInfo->m_pagecount)
                    {
                        winrt::Microsoft::UI::Xaml::Controls::Grid pageContainer;
                        pageContainer.Width(printSize->Width);
                        pageContainer.Height(printSize->Height);

                        winrt::Microsoft::UI::Xaml::Controls::Image pageImage;
                        pageImage.Source(pdfInfo->m_renderedPages[args.PageNumber() - 1]);
                        pageImage.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
                        pageImage.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                        pageImage.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

                        pageContainer.Children().Append(pageImage);

                        senderDoc.SetPreviewPage(args.PageNumber(), pageContainer);
                    }
                });

            m_printDocument.AddPages([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::AddPagesEventArgs const& args)
                {
                    auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
                    for (int i = 0; i < pdfInfo->m_pagecount; ++i)
                    {
                        winrt::Microsoft::UI::Xaml::Controls::Grid pageContainer;
                        pageContainer.Width(printSize->Width);
                        pageContainer.Height(printSize->Height);

                        winrt::Microsoft::UI::Xaml::Controls::Image pageImage;
                        pageImage.Source(pdfInfo->m_renderedPages[i]);
                        pageImage.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
                        pageImage.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                        pageImage.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

                        pageContainer.Children().Append(pageImage);

                        senderDoc.AddPage(pageContainer);
                    }
                    senderDoc.AddPagesComplete();
                });

            if (m_printTaskRequestedToken)
            {
                printManager.PrintTaskRequested(m_printTaskRequestedToken);
            }

            m_printTaskRequestedToken = printManager.PrintTaskRequested([printDocumentSource, fileName = pdfInfo->m_fileName](
                winrt::Windows::Graphics::Printing::PrintManager const&,
                winrt::Windows::Graphics::Printing::PrintTaskRequestedEventArgs const& args)
                {
                    auto deferral = args.Request().GetDeferral();
                    args.Request().CreatePrintTask(fileName, [printDocumentSource](winrt::Windows::Graphics::Printing::PrintTaskSourceRequestedArgs const& sourceArgs)
                        {
                            sourceArgs.SetSource(printDocumentSource);
                        });
                    deferral.Complete();
                });

            try
            {
                winrt::Windows::Foundation::IAsyncOperation<bool> showPrintUIOp{ nullptr };
                winrt::check_hresult(printInterop->ShowPrintUIForWindowAsync(hwnd, winrt::guid_of<decltype(showPrintUIOp)>(), winrt::put_abi(showPrintUIOp)));

                co_await showPrintUIOp;
            }
            catch (winrt::hresult_error const& ex)
            {
                // TODO: printing fails
            }
        }
    }
