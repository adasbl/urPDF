#pragma once

#include "MainWindow.g.h"
#include "pch.h"
#include <map>

extern "C" {
#include <mupdf/fitz.h>
}

namespace winrt::urPDF::implementation
{
    class PdfTabInfo
    {
    public:
        PdfTabInfo(fz_context* m_ctx, const std::string& filePath);
        ~PdfTabInfo();

        int m_currentPageIndex = { 0 };
        double m_currentPageZoom = { 1.0 };
        winrt::hstring m_openedFilePath;
        winrt::hstring m_fileName = L"unknown";

        fz_context* m_ctx{ nullptr };
        fz_document* m_doc{ nullptr };
        int m_pageCount;
    };


    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        
        void ApplyBackdrop(winrt::hstring const& transparency);
        void SaveBackdropSettings(winrt::hstring const& themeName); // TODO: marge save functions
        void ApplyTheme(winrt::hstring const& themeName);
        void SaveSettings(winrt::hstring const& themeName);
        void LoadSettings();

        winrt::fire_and_forget OpenPdfFileAsync(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void TabView_AddButtonClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const& args);
        void TabView_TabCloseRequested(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args);
        void TabView_Loaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void TabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        winrt::Windows::Foundation::IAsyncAction RenderPageAsync(double zoomFactor, std::shared_ptr<PdfTabInfo> PdfTabInfo);
        void PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args);
        void ToggleSwitch_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void ThemeSelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void TransparencySelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

    private:
        fz_context* m_ctx{ nullptr };
        std::map<winrt::Microsoft::UI::Xaml::Controls::TabViewItem, std::shared_ptr<PdfTabInfo>> m_openedTabs;
        bool m_isInitializing{ true };
    };
}

namespace winrt::urPDF::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
