#pragma once

#include "MainWindow.g.h"
#include "pch.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include <map>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

namespace winrt::urPDF::implementation
{
    class PdfTabInfo
    {
    public:
        PdfTabInfo(fz_context* m_ctx, const std::string& filePath);
        ~PdfTabInfo();

        winrt::hstring m_openedFilePath;
        winrt::hstring m_fileName = L"unknown";
        std::vector<winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource> m_renderedPages;
        int m_currentPageIndex = { 0 };
        double m_currentPageZoom = { 1.0 };
        int m_currentRotation = { 0 };
        
        fz_context* m_ctx{ nullptr };
        fz_document* m_doc{ nullptr };
        int m_pagecount;
    };


    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void ApplyTheme(winrt::hstring const& themeName);
        void ApplyBackdrop(winrt::hstring const& transparency);
        void LoadSettings();

        winrt::fire_and_forget OpenPdfFileAsync(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void TabView_AddButtonClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const& args);
        void TabView_TabCloseRequested(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args);
        void TabView_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void TabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        winrt::Windows::Foundation::IAsyncAction RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTabInfo);
        winrt::Windows::Foundation::IAsyncAction RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab);
        void PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args);
        void SettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        fz_context* m_ctx{ nullptr };
        std::map<winrt::Microsoft::UI::Xaml::Controls::TabViewItem, std::shared_ptr<PdfTabInfo>> m_openedTabs;
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
