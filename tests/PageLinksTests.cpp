#include "CppUnitTest.h"
#include "../urPDF/MuPdfDocument.h"
#include "../urPDF/MuPdfRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        std::filesystem::path WriteLinkPdf(int rotation)
        {
            const std::vector<std::string> objects{
                "<< /Type /Catalog /Pages 2 0 R /Names << /Dests << /Names [(chapter) [4 0 R /Fit]] >> >> >>",
                "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Resources << >> /Rotate "
                    + std::to_string(rotation) + " /Annots [5 0 R 6 0 R 7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 13 0 R 14 0 R 15 0 R] >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Resources << >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 40 80 60] /A << /S /URI /URI (https://example.com) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 70 80 90] /A << /S /URI /URI (file:///C:/x) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 100 80 120] /A << /S /URI /URI (ms-settings:) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 130 80 150] /Dest [4 0 R /Fit] >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 160 80 180] /A << /S /GoTo /D (chapter) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 190 80 210] /Dest [99 0 R /Fit] >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 220 80 240] /A << /S /URI /URI (mailto:user@example.com) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 250 80 270] /A << /S /URI /URI (http://example.com) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 250 80 270] /A << /S /Launch /F (C:/Windows/notepad.exe) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [400 400 500 500] /A << /S /URI /URI (https://outside.example.com) >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [20 10 80 30] /A << /S /GoTo /D (missing-destination) >> >>"
            };
            std::ostringstream pdf;
            pdf << "%PDF-1.4\n";
            std::vector<std::streamoff> offsets{ 0 };
            for (std::size_t i = 0; i < objects.size(); ++i)
            {
                offsets.push_back(pdf.tellp());
                pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
            }
            const auto xref = pdf.tellp();
            pdf << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
            for (std::size_t i = 1; i < offsets.size(); ++i)
            {
                pdf << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
            }
            pdf << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
            static std::atomic<unsigned int> serial{ 0 };
            const auto name = "urpdf-links-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".pdf";
            auto path = std::filesystem::temp_directory_path() / name;
            std::ofstream file(path, std::ios::binary);
            file << pdf.str();
            return path;
        }

        struct LinkDocument
        {
            explicit LinkDocument(int rotation = 0)
                : runtime(std::make_shared<urpdf::MuPdfRuntime>()), context(runtime->CloneContext()), path(WriteLinkPdf(rotation))
            {
                const auto utf8 = path.u8string();
                document = urpdf::OpenPdfDocument(context.get(), reinterpret_cast<char const*>(utf8.c_str()));
            }
            ~LinkDocument()
            {
                fz_drop_document(context.get(), document);
                std::error_code error;
                std::filesystem::remove(path, error);
            }
            std::shared_ptr<urpdf::MuPdfRuntime> runtime;
            urpdf::MuPdfRuntime::Context context;
            std::filesystem::path path;
            fz_document* document{ nullptr };
        };

        urpdf::PageLink const& FindExternal(std::vector<urpdf::PageLink> const& links, std::string_view uri)
        {
            const auto found = std::find_if(links.begin(), links.end(), [&](auto const& link) { return link.uri == uri; });
            Assert::IsTrue(found != links.end());
            return *found;
        }
    }

    TEST_CLASS(PageLinksTests)
    {
    public:
        TEST_METHOD(CollectsAllowedExternalAndExplicitAndNamedInternalLinksOnClone)
        {
            LinkDocument pdf;
            auto workerContext = pdf.runtime->CloneContext();
            const auto links = urpdf::CollectPageLinks(workerContext.get(), pdf.document, 0);
            Assert::AreEqual(std::size_t(5), links.size());
            for (auto uri : { "https://example.com", "http://example.com", "mailto:user@example.com" })
            {
                Assert::IsTrue(FindExternal(links, uri).kind == urpdf::PageLinkKind::external);
            }
            int internalCount = 0;
            for (auto const& link : links)
            {
                if (link.kind == urpdf::PageLinkKind::internal)
                {
                    ++internalCount;
                    Assert::AreEqual(1, link.targetPageIndex);
                }
            }
            Assert::AreEqual(2, internalCount);
            const auto& rect = FindExternal(links, "https://example.com").rect;
            Assert::AreEqual(20.0, rect.x0);
            Assert::AreEqual(240.0, rect.y0);
            Assert::AreEqual(80.0, rect.x1);
            Assert::AreEqual(260.0, rect.y1);
            Assert::IsTrue(urpdf::CollectPageLinks(workerContext.get(), pdf.document, 1).empty());
        }

        TEST_METHOD(RotatedLinkBoundsUseMuPdfPageSpace)
        {
            LinkDocument pdf(90);
            const auto links = urpdf::CollectPageLinks(pdf.context.get(), pdf.document, 0);
            const auto& rect = FindExternal(links, "https://example.com").rect;
            Assert::AreEqual(40.0, rect.x0);
            Assert::AreEqual(20.0, rect.y0);
            Assert::AreEqual(60.0, rect.x1);
            Assert::AreEqual(80.0, rect.y1);
        }

        TEST_METHOD(PageLoadErrorsLeaveContextUsable)
        {
            LinkDocument pdf;
            Assert::ExpectException<std::runtime_error>([&] { urpdf::CollectPageLinks(pdf.context.get(), pdf.document, 99); });
            Assert::AreEqual(std::size_t(5), urpdf::CollectPageLinks(pdf.context.get(), pdf.document, 0).size());
        }
    };
}
