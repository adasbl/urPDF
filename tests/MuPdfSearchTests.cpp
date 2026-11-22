#include "CppUnitTest.h"
#include "../urPDF/MuPdfDocument.h"
#include "../urPDF/MuPdfRuntime.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        struct PdfPageSpec
        {
            std::string content;
            int rotation{ 0 };
            double width{ 300.0 };
            double height{ 400.0 };
        };

        // Builds a minimal multi-page PDF with Helvetica and a ToUnicode CMap mapping
        // bytes 0x80..0x88 to Polish letters (zazolc gesla jazn).
        std::filesystem::path WriteSearchPdf(std::vector<PdfPageSpec> const& pages)
        {
            static constexpr char kToUnicodeCMap[] =
                "/CIDInit /ProcSet findresource begin\n"
                "12 dict begin\n"
                "begincmap\n"
                "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                "/CMapName /Adobe-Identity-UCS def\n"
                "/CMapType 2 def\n"
                "1 begincodespacerange\n"
                "<00> <FF>\n"
                "endcodespacerange\n"
                "9 beginbfchar\n"
                "<80> <017C>\n" // U+017C (z with dot above)
                "<81> <00F3>\n" // U+00F3 (o with acute)
                "<82> <0142>\n" // U+0142 (l with stroke)
                "<83> <0107>\n" // U+0107 (c with acute)
                "<84> <0119>\n" // U+0119 (e with ogonek)
                "<85> <015B>\n" // U+015B (s with acute)
                "<86> <0105>\n" // U+0105 (a with ogonek)
                "<87> <017A>\n" // U+017A (z with acute)
                "<88> <0144>\n" // U+0144 (n with acute)
                "endbfchar\n"
                "endcmap\n"
                "CMapName currentdict /CMap defineresource pop\n"
                "end\n"
                "end\n";

            // Object 1: Catalog
            // Object 2: Pages
            // Object 3: Font (Type1 Helvetica + Polish Differences + ToUnicode)
            // Object 4: ToUnicode stream
            // Following pairs: Page object + Content stream (if non-empty)
            std::vector<std::string> objects;
            objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
            objects.push_back(""); // placeholder for Pages dict

            objects.push_back(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
                " /Encoding << /Type /Encoding /Differences [128 /zdotaccent /oacute /lslash /cacute /eogonek /sacute /aogonek /zacute /nacute] >>"
                " /ToUnicode 4 0 R >>");
            const std::string cmap(kToUnicodeCMap);
            objects.push_back("<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream");

            std::ostringstream kids;
            for (auto const& page : pages)
            {
                const int pageObjNum = static_cast<int>(objects.size()) + 1;
                kids << pageObjNum << " 0 R ";
                std::ostringstream pageDict;
                pageDict << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
                    << page.width << " " << page.height << "] /Rotate " << page.rotation;
                if (!page.content.empty())
                {
                    const int contentObjNum = pageObjNum + 1;
                    pageDict << " /Resources << /Font << /F1 3 0 R >> >> /Contents " << contentObjNum << " 0 R >>";
                    objects.push_back(pageDict.str());
                    objects.push_back(
                        "<< /Length " + std::to_string(page.content.size())
                        + " >>\nstream\n" + page.content + "\nendstream");
                }
                else
                {
                    pageDict << " /Resources << >> >>";
                    objects.push_back(pageDict.str());
                }
            }

            objects[1] = "<< /Type /Pages /Kids [" + kids.str() + "] /Count " + std::to_string(pages.size()) + " >>";

            std::ostringstream pdf(std::ios::binary);
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
            const auto name = "urpdf-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".pdf";
            auto path = std::filesystem::temp_directory_path() / name;
            std::ofstream file(path, std::ios::binary);
            const auto content = pdf.str();
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            return path;
        }

        struct SearchDocument
        {
            explicit SearchDocument(std::vector<PdfPageSpec> const& pages)
                : runtime(std::make_shared<urpdf::MuPdfRuntime>()),
                  context(runtime->CloneContext()),
                  path(WriteSearchPdf(pages))
            {
                const auto utf8 = path.u8string();
                document = urpdf::OpenPdfDocument(context.get(), reinterpret_cast<char const*>(utf8.c_str()));
            }
            ~SearchDocument()
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
    }

    TEST_CLASS(MuPdfSearchTests)
    {
    public:
        TEST_METHOD(FindsSingleMultipleAndMultiPageMatchesInPageOrder)
        {
            SearchDocument pdf({
                { "BT /F1 12 Tf 20 350 Td (alpha beta alpha) Tj 0 -20 Td (gamma alpha delta) Tj ET" },
                { "BT /F1 12 Tf 20 350 Td (second page alpha match) Tj ET" },
                { "" } // empty page without text
            });

            auto workerContext = pdf.runtime->CloneContext();

            const auto page0Alpha = urpdf::SearchPage(workerContext.get(), pdf.document, 0, "alpha");
            Assert::AreEqual(std::size_t(3), page0Alpha.size());
            for (auto const& match : page0Alpha)
            {
                Assert::AreEqual(0, match.pageIndex);
                Assert::AreEqual(std::size_t(1), match.rects.size());
                Assert::IsTrue(match.rects[0].x0 < match.rects[0].x1);
                Assert::IsTrue(match.rects[0].y0 < match.rects[0].y1);
            }
            // First two matches are on line 1 (left then right), third is on line 2 below.
            Assert::IsTrue(page0Alpha[0].rects[0].x1 < page0Alpha[1].rects[0].x0);
            Assert::IsTrue(page0Alpha[0].rects[0].y1 <= page0Alpha[2].rects[0].y0);

            const auto page0Beta = urpdf::SearchPage(workerContext.get(), pdf.document, 0, "beta");
            Assert::AreEqual(std::size_t(1), page0Beta.size());

            const auto page1Alpha = urpdf::SearchPage(workerContext.get(), pdf.document, 1, "alpha");
            Assert::AreEqual(std::size_t(1), page1Alpha.size());
            Assert::AreEqual(1, page1Alpha[0].pageIndex);

            const auto page2Alpha = urpdf::SearchPage(workerContext.get(), pdf.document, 2, "alpha");
            Assert::IsTrue(page2Alpha.empty());
        }

        TEST_METHOD(GroupsMultiLineMatchIntoSingleSearchMatchWithMultipleRects)
        {
            SearchDocument pdf({
                { "BT /F1 12 Tf 20 350 Td (The quick brown) Tj 0 -16 Td (fox jumps over) Tj ET" }
            });

            const auto matches = urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "brown fox");
            Assert::AreEqual(std::size_t(1), matches.size());
            Assert::AreEqual(0, matches[0].pageIndex);
            Assert::AreEqual(std::size_t(2), matches[0].rects.size());
            // First quad lies on line 1, second quad lies on line 2 below it.
            Assert::IsTrue(matches[0].rects[0].y0 < matches[0].rects[1].y0);
        }

        TEST_METHOD(MatchesCaseInsensitivelyAndSupportsPolishUnicodeText)
        {
            // "Za\x80\x81\x82\x83 g\x84\x85l\x86 ja\x87\x88" -> "Za\u017c\u00f3\u0142\u0107 g\u0119\u015bl\u0105 ja\u017a\u0144"
            const std::string line = "BT /F1 12 Tf 20 350 Td (Hello World) Tj 0 -20 Td (Za\\200\\201\\202\\203 g\\204\\205l\\206 ja\\207\\210) Tj ET";
            SearchDocument pdf({ { line } });

            const auto caseInsensitive = urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "hElLo wOrLd");
            Assert::AreEqual(std::size_t(1), caseInsensitive.size());

            const std::u8string polishLower = u8"za\u017c\u00f3\u0142\u0107";
            const auto polishMatches = urpdf::SearchPage(
                pdf.context.get(), pdf.document, 0,
                reinterpret_cast<char const*>(polishLower.c_str()));
            Assert::AreEqual(std::size_t(1), polishMatches.size());

            const std::u8string polishUpper = u8"G\u0118\u015aL\u0104";
            const auto polishUpperMatches = urpdf::SearchPage(
                pdf.context.get(), pdf.document, 0,
                reinterpret_cast<char const*>(polishUpper.c_str()));
            Assert::AreEqual(std::size_t(1), polishUpperMatches.size());
        }

        TEST_METHOD(RotatedPageMatchesStayInMuPdfPageSpace)
        {
            SearchDocument pdf({
                { "BT /F1 12 Tf 20 350 Td (Rotated target) Tj ET", 90, 200.0, 400.0 }
            });

            const auto matches = urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "target");
            Assert::AreEqual(std::size_t(1), matches.size());
            Assert::AreEqual(std::size_t(1), matches[0].rects.size());
            // 200x400 rotated 90 CW has MuPDF page bounds [0, 0, 400, 200].
            const auto& rect = matches[0].rects[0];
            Assert::IsTrue(rect.x0 >= 0.0 && rect.x1 <= 400.0 && rect.x0 < rect.x1);
            Assert::IsTrue(rect.y0 >= 0.0 && rect.y1 <= 200.0 && rect.y0 < rect.y1);
        }

        TEST_METHOD(BlankNullAndMissingQueriesReturnEmptyWithoutCallingMuPdf)
        {
            // Blank/null queries return immediately before touching MuPDF context/document.
            Assert::IsTrue(urpdf::SearchPage(nullptr, nullptr, 999, nullptr).empty());
            Assert::IsTrue(urpdf::SearchPage(nullptr, nullptr, 999, "").empty());
            Assert::IsTrue(urpdf::SearchPage(nullptr, nullptr, 999, "   \t\r\n  ").empty());
            Assert::IsTrue(urpdf::SearchPage(nullptr, nullptr, 999, "\xC2\xA0 \xE3\x80\x80").empty());

            SearchDocument pdf({
                { "BT /F1 12 Tf 20 350 Td (Only some words here) Tj ET" }
            });
            Assert::IsTrue(urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "nonexistent").empty());
        }

        TEST_METHOD(GrowsHitBufferBeyondInitialCapacity)
        {
            // 15 lines * 20 occurrences = 300 hits of "ok", exceeding the 256-quad initial buffer.
            std::ostringstream content;
            content << "BT /F1 8 Tf 10 380 Td ";
            for (int row = 0; row < 15; ++row)
            {
                if (row > 0) { content << "0 -14 Td "; }
                content << "(";
                for (int col = 0; col < 20; ++col)
                {
                    if (col > 0) { content << " "; }
                    content << "ok";
                }
                content << ") Tj ";
            }
            content << "ET";

            SearchDocument pdf({ { content.str(), 0, 400.0, 400.0 } });
            const auto matches = urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "ok");
            Assert::AreEqual(std::size_t(300), matches.size());
        }

        TEST_METHOD(InvalidPageIndexThrowsAndLeavesContextUsable)
        {
            SearchDocument pdf({
                { "BT /F1 12 Tf 20 350 Td (recoverable text) Tj ET" }
            });

            Assert::ExpectException<std::runtime_error>([&]
                {
                    urpdf::SearchPage(pdf.context.get(), pdf.document, 99, "recoverable");
                });

            const auto matches = urpdf::SearchPage(pdf.context.get(), pdf.document, 0, "recoverable");
            Assert::AreEqual(std::size_t(1), matches.size());
        }

        TEST_METHOD(CancellableBackgroundSearchCleansUpClonedContext)
        {
            std::vector<PdfPageSpec> pages;
            for (int i = 0; i < 6; ++i)
            {
                pages.push_back({ "BT /F1 12 Tf 20 350 Td (needle on page " + std::to_string(i) + ") Tj ET" });
            }
            SearchDocument pdf(pages);

            auto reference = pdf.runtime->CloneContext();
            const int initialContextCount = reference->master->context_count;

            std::mutex documentMutex;
            std::atomic<bool> searchCancelled{ false };
            std::promise<void> firstPageDone;
            std::promise<void> continueSignal;
            auto continueFuture = continueSignal.get_future().share();

            auto registration = pdf.runtime->Work().TryRegister();
            Assert::IsTrue(static_cast<bool>(registration));
            auto workerContext = pdf.runtime->CloneContext();

            auto worker = std::async(std::launch::async,
                [registration = std::move(registration),
                 workerContext = std::move(workerContext),
                 doc = pdf.document,
                 &documentMutex,
                 &searchCancelled,
                 &firstPageDone,
                 continueFuture]() mutable
                {
                    int searchedPages = 0;
                    for (int page = 0; page < 6; ++page)
                    {
                        if (searchCancelled.load(std::memory_order_acquire))
                        {
                            break;
                        }
                        {
                            std::lock_guard lock(documentMutex);
                            const auto hits = urpdf::SearchPage(workerContext.get(), doc, page, "needle");
                            Assert::AreEqual(std::size_t(1), hits.size());
                        }
                        ++searchedPages;
                        if (page == 0)
                        {
                            firstPageDone.set_value();
                            continueFuture.wait();
                        }
                    }
                    return searchedPages;
                });

            firstPageDone.get_future().wait();
            searchCancelled.store(true, std::memory_order_release);
            continueSignal.set_value();

            const int searchedPages = worker.get();
            Assert::AreEqual(1, searchedPages);
            Assert::AreEqual(initialContextCount, reference->master->context_count);
        }
    };
}
