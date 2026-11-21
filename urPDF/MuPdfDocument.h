#pragma once

#include "PageLinks.h"
#include "PageText.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include <vector>

namespace urpdf
{
    // Each helper keeps MuPDF's longjmp-based exceptions inside its own frame, away from
    // C++ owners, and reports errors as std::runtime_error.
    // The caller must hold exclusive access to the document.
    fz_document* OpenPdfDocument(fz_context* ctx, char const* utf8Path);
    int CountDocumentPages(fz_context* ctx, fz_document* document);
    std::vector<fz_rect> CollectPageBounds(fz_context* ctx, fz_document* document, int pageCount);
    std::vector<PageLink> CollectPageLinks(fz_context* ctx, fz_document* document, int pageIndex);
    PageText LoadPageText(fz_context* ctx, fz_document* document, int pageIndex);
}
