#include "CppUnitTest.h"
#include "../urPDF/ExternalUriPolicy.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    TEST_CLASS(ExternalUriPolicyTests)
    {
    public:
        TEST_METHOD(AllowsOnlySupportedSchemesIgnoringAsciiCase)
        {
            for (auto scheme : { "http", "https", "mailto", "HTTP", "HtTpS", "MAILTO" })
            {
                Assert::IsTrue(urpdf::IsAllowedExternalScheme(scheme));
            }
            for (auto scheme : { "", "file", "ms-settings", "ms-msdt", "https2", "http:", " http" })
            {
                Assert::IsFalse(urpdf::IsAllowedExternalScheme(scheme));
            }
        }

        TEST_METHOD(PreservesAllowedUrisAndTrimsOuterWhitespace)
        {
            for (auto uri : { "http://example.com", "https://example.com/a?b=c#d", "HTTPS://EXAMPLE.COM",
                "HtTp://x", "mailto:user@example.com", "MAILTO:user@example.com?subject=x", "https://[::1]:443/a" })
            {
                auto checked = urpdf::TryGetLaunchableExternalUri(uri);
                Assert::IsTrue(checked.has_value());
                Assert::AreEqual(uri, checked->c_str());
            }
            auto checked = urpdf::TryGetLaunchableExternalUri(" \t https://example.com \r\n");
            Assert::IsTrue(checked.has_value());
            Assert::AreEqual("https://example.com", checked->c_str());
        }

        TEST_METHOD(RejectsOtherSchemesPathsAndMalformedUris)
        {
            for (auto uri : { "file:///C:/Windows/System32/calc.exe", "FILE://server/share", "ms-settings:",
                "ms-msdt:/id", "ms-word:ofe|u|https://x", "search-ms:query=x", "javascript:alert(1)",
                "vbscript:x", "data:text/html,x", "shell:startup", "C:\\Windows\\notepad.exe", "\\\\server\\share",
                "/abs/path", "relative.html", "#page=2", "", "   ", "http:", "http://", "https:/x", "mailto:",
                "1http://x", "ht tp://x", "http_s://x", "https://:80", "https://user@", "https://?x",
                "https://#x", "https://[]", "https://exam ple.com", "https://\\server", "ht\ttp://x", "https://x\ny" })
            {
                Assert::IsFalse(urpdf::TryGetLaunchableExternalUri(uri).has_value(), L"URI should be rejected");
            }
            Assert::IsFalse(urpdf::TryGetLaunchableExternalUri("http\xd1\x95://x").has_value());
        }

        TEST_METHOD(RejectsEmbeddedNullsAndDelWithoutCStringTruncation)
        {
            constexpr char nul[] = "https://example.com\0file:///x";
            constexpr char schemeNul[] = "java\0script:";
            Assert::IsFalse(urpdf::TryGetLaunchableExternalUri({ nul, sizeof(nul) - 1 }).has_value());
            Assert::IsFalse(urpdf::TryGetLaunchableExternalUri({ schemeNul, sizeof(schemeNul) - 1 }).has_value());
            Assert::IsFalse(urpdf::TryGetLaunchableExternalUri("https://x\x7f").has_value());
        }
    };
}
