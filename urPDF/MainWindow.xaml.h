#pragma once

#include "MainWindow.g.h"
#include "pch.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include <map>
#include <vector>
#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>

namespace winrt::urPDF::implementation
{
    class PdfTabInfo
    {
    public:
        PdfTabInfo(fz_context* m_ctx, const std::string& filePath);
        ~PdfTabInfo();

        winrt::hstring m_openedFilePath;
        winrt::hstring m_fileName = L"unknown";
        std::vector<winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap> m_renderedPages;
        int32_t m_currentPageIndex = { 1 };
        double m_currentPageZoom = { 1.0 };
        int m_currentRotation = { 0 };
        
        fz_context* m_ctx{ nullptr };
        fz_document* m_doc{ nullptr };
        int m_pagecount{ 0 };
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
        winrt::Windows::Foundation::IAsyncAction RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTabInfo);
        winrt::Windows::Foundation::IAsyncAction RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab);
        void PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args);
        void SettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget RotateButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void PageNumberBox_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const& sender, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);
        void NextPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void PreviousPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void AboutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        winrt::fire_and_forget PrintButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void CloseSettings_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept;

    private:
        fz_context* m_ctx{ nullptr };
        std::map<winrt::Microsoft::UI::Xaml::Controls::TabViewItem, std::shared_ptr<PdfTabInfo>> m_openedTabs;
        int32_t m_pageNumber{ 0 };
        int32_t m_pageCounter{ 0 };
        winrt::Windows::Storage::StorageFile m_pendingFile{ nullptr };
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;

        winrt::Microsoft::UI::Xaml::Printing::PrintDocument m_printDocument{ nullptr };
        winrt::event_token m_printTaskRequestedToken{};
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
