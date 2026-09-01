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
    #include <winrt/Windows.Graphics.Imaging.h>
    #include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
    #include <microsoft.ui.xaml.window.h>
    #include <shobjidl_core.h>
    #include <MemoryBuffer.h>
    #include <map>
    #include <winrt/Microsoft.UI.Dispatching.h>
    #include <string>
    #include <memory>
    #include <coroutine>
    #include <winrt/Windows.Graphics.Printing.h>
    #include <winrt/Microsoft.UI.Xaml.Printing.h>
    #include <printmanagerinterop.h>

    
    using namespace winrt;
    using namespace winrt::Windows::Storage::Pickers;
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Composition::SystemBackdrops;
    using namespace Microsoft::UI::Composition;
    

    namespace winrt::urPDF::implementation
    {
        // CLASSES
        MainWindow::MainWindow()
        {
            InitializeComponent();

            ExtendsContentIntoTitleBar(true);
            SetTitleBar(AppTitleBar());

            LoadSettings();

            m_ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
            fz_register_document_handlers(m_ctx);
        }

        MainWindow::~MainWindow()
        {
            if (m_ctx) {
                fz_drop_context(m_ctx);
                m_ctx = nullptr;
            }
        }

        PdfTabInfo::PdfTabInfo(fz_context* ctx, const std::string& filePath)
        {
            m_ctx = ctx;
            m_doc = fz_open_document(m_ctx, filePath.c_str());
        }

        PdfTabInfo::~PdfTabInfo()
        {
            fz_drop_document(m_ctx, m_doc);
        }

        void MainWindow::TabView_Loaded(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
            TabView_AddButtonClick(tabView, nullptr);
        }

        winrt::event_token MainWindow::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
        {
            return m_propertyChanged.add(handler);
        }

        void MainWindow::PropertyChanged(winrt::event_token const& token) noexcept
        {
            m_propertyChanged.remove(token);
        }

        // MULTITHREADING
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

        // UI UPDATE
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
        winrt::fire_and_forget MainWindow::SettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
            dialog.XamlRoot(this->Content().XamlRoot());
            dialog.CloseButtonText(L"Close");

            winrt::urPDF::SettingsPage settingsPage;

            settingsPage.ThemeChanged([this, dialog](auto const&, winrt::hstring const& themeName)
            {
                ApplyTheme(themeName);

                auto theme = winrt::Microsoft::UI::Xaml::ElementTheme::Default;

                if (themeName == L"Light")
                    theme = winrt::Microsoft::UI::Xaml::ElementTheme::Light;
                else if (themeName == L"Dark")
                    theme = winrt::Microsoft::UI::Xaml::ElementTheme::Dark;

                dialog.RequestedTheme(theme);
            });

            settingsPage.BackdropChanged([this, dialog](auto const&, winrt::hstring const& transparencyValue)
            {
                    ApplyBackdrop(transparencyValue);
            });

            dialog.Content(settingsPage);
            co_await dialog.ShowAsync();
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
        void winrt::urPDF::implementation::MainWindow::AboutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            RootFrame().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            RootFrame().Navigate(winrt::xaml_typename<winrt::urPDF::AboutPage>());
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
            targetTab.Header(winrt::box_value(fileName));

            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            targetTab.IconSource(tabIcon);

            m_openedTabs[targetTab] = PdfInfo;

            co_await RenderMultiPageAsync(PdfInfo);
        }

        // PAGE RENDERING
        winrt::Windows::Foundation::IAsyncAction MainWindow::RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
        {
            co_await winrt::resume_background();

            int pagecount = fz_count_pages(m_ctx, PdfTab->m_doc);
            PdfTab->m_pagecount = pagecount;

            fz_page* page = fz_load_page(m_ctx, PdfTab->m_doc, 0);

            fz_matrix transform = fz_scale(PdfTab->m_currentPageZoom, PdfTab->m_currentPageZoom);
            transform = fz_concat(transform, fz_rotate(PdfTab->m_currentRotation));

            fz_rect rect = fz_bound_page(m_ctx, page);
            rect = fz_transform_rect(rect, transform);
            fz_irect bbox = fz_round_rect(rect);

            fz_pixmap* pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_bgr(m_ctx), bbox, NULL, 1);
            fz_clear_pixmap_with_value(m_ctx, pix, 255);

            fz_device* dev = fz_new_draw_device(m_ctx, fz_identity, pix);
            fz_run_page(m_ctx, page, dev, transform, NULL);
            // fz_close_device(m_ctx, dev);

            int width = fz_pixmap_width(m_ctx, pix);
            int height = fz_pixmap_height(m_ctx, pix);

            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
            {
                BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                uint8_t* data = reference.data();

                memcpy(data, fz_pixmap_samples(m_ctx, pix), width * height * 4);
            }

            fz_drop_device(m_ctx, dev);
            fz_drop_pixmap(m_ctx, pix);
            fz_drop_page(m_ctx, page);

            co_await resume_foreground(DispatcherQueue());

            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
            bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());

            winrt::Microsoft::UI::Xaml::Controls::Image img;
            img.Source(current_writeable_bitmap);
            img.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 5 });

            PagesContainer().Children().Clear();
            PagesContainer().Children().Append(img);

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
            co_await winrt::resume_background();

            int pagecount = fz_count_pages(m_ctx, PdfTab->m_doc);
            PdfTab->m_pagecount = pagecount;

            fz_matrix transform = fz_scale(PdfTab->m_currentPageZoom, PdfTab->m_currentPageZoom);
            transform = fz_concat(transform, fz_rotate(PdfTab->m_currentRotation));

            for (int i = 0; i < pagecount; i++)
            {
                fz_page* page = fz_load_page(m_ctx, PdfTab->m_doc, i);

                fz_rect rect = fz_bound_page(m_ctx, page);
                rect = fz_transform_rect(rect, transform);
                fz_irect bbox = fz_round_rect(rect);

                fz_pixmap* pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_bgr(m_ctx), bbox, NULL, 1);
                fz_clear_pixmap_with_value(m_ctx, pix, 255);

                fz_device* dev = fz_new_draw_device(m_ctx, fz_identity, pix);
                fz_run_page(m_ctx, page, dev, transform, NULL);
                    
                int width = fz_pixmap_width(m_ctx, pix);
                int height = fz_pixmap_height(m_ctx, pix);

                SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
                {
                    BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                    winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                    uint8_t* data = reference.data();

                    memcpy(data, fz_pixmap_samples(m_ctx, pix), width * height * 4);
                }

                fz_drop_device(m_ctx, dev);
                fz_drop_pixmap(m_ctx, pix);
                fz_drop_page(m_ctx, page);
                
                co_await resume_foreground(DispatcherQueue());

                winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
                bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
                PdfTab->m_renderedPages.push_back(current_writeable_bitmap);

                winrt::Microsoft::UI::Xaml::Controls::Image img;
                img.Source(current_writeable_bitmap);
                img.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 5 });

                PagesContainer().Children().Append(img);

                if (i < pagecount - 1)
                {
                    co_await winrt::resume_background();
                }
            }
            co_await resume_foreground(DispatcherQueue());

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (currentTab)
            {
                currentTab.Header(winrt::box_value(PdfTab->m_fileName));
                DisplayPageCount(PdfTab->m_pagecount);
                DisplayPageNumber(PdfTab->m_currentPageIndex);
            }
        }

        // UI ELEMENTS
        void MainWindow::TabView_AddButtonClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Windows::Foundation::IInspectable const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            winrt::Microsoft::UI::Xaml::Controls::TabViewItem newTab;

            newTab.Header(winrt::box_value(L"New document"));
            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            newTab.IconSource(tabIcon);

            winrt::Microsoft::UI::Xaml::Controls::TextBlock newTabText;
            newTabText.Text(L"Here the document will be displayed");
            newTabText.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
            newTabText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            newTab.Content(newTabText);

            tabView.TabItems().Append(newTab);

            tabView.SelectedItem(newTab);
        }

        void MainWindow::TabView_TabCloseRequested(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            m_openedTabs.erase(args.Tab());

            uint32_t index;
            if (tabView.TabItems().IndexOf(args.Tab(), index))
            {
                tabView.TabItems().RemoveAt(index);
            }
        }

        void MainWindow::TabView_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
            auto currentTab = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab)
            {
                PagesContainer().Children().Clear();

                if (m_openedTabs.find(currentTab) != m_openedTabs.end())
                {
                    auto pdfInfo = m_openedTabs[currentTab];

                    DisplayPageNumber(pdfInfo->m_currentPageIndex);
                    DisplayPageCount(pdfInfo->m_pagecount);
                    for (auto const& savedSource : pdfInfo->m_renderedPages) 
                    {
                        winrt::Microsoft::UI::Xaml::Controls::Image img;
                        img.Source(savedSource);
                        img.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 5 });

                        PagesContainer().Children().Append(img);
                    }
                }
            }
            else
            {
                DisplayPageNumber(0);
                DisplayPageCount(0);
            }
        }

        winrt::fire_and_forget MainWindow::RotateButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto button = sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>();
            button.IsEnabled(false);

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];

                pdfInfo->m_currentRotation += 90;
                if (pdfInfo->m_currentRotation >= 360)
                {
                    pdfInfo->m_currentRotation = 0;
                }

                PagesContainer().Children().Clear();
                co_await RenderMultiPageAsync(pdfInfo);
            }
            button.IsEnabled(true);
        }

        void MainWindow::PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args)
        {
            auto scrollViewer = sender.as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();

            double scrollOffset = scrollViewer.VerticalOffset();
            double totalHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                auto pdfInfo = m_openedTabs[currentTab];
                int totalPages = pdfInfo->m_pagecount;

                if (totalPages > 0 && totalHeight > 0)
                {
                    double scrollPercentage = scrollOffset / scrollViewer.ScrollableHeight();

                    if (scrollViewer.ScrollableHeight() == 0) scrollPercentage = 0.0;
                    if (scrollPercentage > 1.0) scrollPercentage = 1.0;
                    if (scrollPercentage < 0.0) scrollPercentage = 0.0;

                    int32_t currentPage = static_cast<int32_t>(std::round(scrollPercentage * (totalPages - 1))) + 1;

                    pdfInfo->m_currentPageIndex = currentPage;

                    DisplayPageNumber(currentPage);
                }
            }
        }

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
                    double scrollPercentage = static_cast<double>(newPage - 1) / (totalPages - 1);
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

                if (totalPages > 1 && currentPage + 1 <= totalPages)
                {
                    int32_t newPage = currentPage + 1;

                    double scrollPercentage = static_cast<double>(newPage - 1) / (totalPages - 1);
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

                if (totalPages > 1 && currentPage + -1 >= 1)
                {
                    int32_t newPage = currentPage - 1;

                    double scrollPercentage = static_cast<double>(newPage - 1) / (totalPages - 1);
                    double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                    PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
                }
            }
            button.IsEnabled(true);
        }


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
                    auto senderDoc = sender.as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();

                    auto pageDesc = args.PrintTaskOptions().GetPageDescription(1);
                    printSize->Width = pageDesc.ImageableRect.Width;
                    printSize->Height = pageDesc.ImageableRect.Height;

                    senderDoc.SetPreviewPageCount(pdfInfo->m_pagecount, winrt::Microsoft::UI::Xaml::Printing::PreviewPageCountType::Final);
                });

            m_printDocument.GetPreviewPage([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::GetPreviewPageEventArgs const& args)
                {
                    auto senderDoc = sender.as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
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
                    auto senderDoc = sender.as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
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
