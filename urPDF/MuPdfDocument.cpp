#include "MuPdfDocument.h"

#include <stdexcept>

namespace
{
    [[noreturn]] void ThrowMuPdfError(fz_context* ctx)
    {
        int code = 0;
        throw std::runtime_error(fz_convert_error(ctx, &code));
    }

    fz_rect LoadPageBounds(fz_context* ctx, fz_document* document, int index)
    {
        fz_page* page = nullptr;
        fz_rect bounds{};
        fz_var(page);
        fz_try(ctx)
        {
            page = fz_load_page(ctx, document, index);
            bounds = fz_bound_page(ctx, page);
        }
        fz_always(ctx)
        {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx)
        {
            ThrowMuPdfError(ctx);
        }
        return bounds;
    }
}

namespace urpdf
{
    fz_document* OpenPdfDocument(fz_context* ctx, char const* utf8Path)
    {
        fz_document* document = nullptr;
        fz_try(ctx)
        {
            document = fz_open_document(ctx, utf8Path);
        }
        fz_catch(ctx)
        {
            ThrowMuPdfError(ctx);
        }
        return document;
    }

    int CountDocumentPages(fz_context* ctx, fz_document* document)
    {
        int count = 0;
        fz_try(ctx)
        {
            count = fz_count_pages(ctx, document);
        }
        fz_catch(ctx)
        {
            ThrowMuPdfError(ctx);
        }
        return count;
    }

    std::vector<fz_rect> CollectPageBounds(fz_context* ctx, fz_document* document, int pageCount)
    {
        std::vector<fz_rect> pageBounds;
        pageBounds.reserve(pageCount);
        for (int i = 0; i < pageCount; ++i)
        {
            pageBounds.push_back(LoadPageBounds(ctx, document, i));
        }
        return pageBounds;
    }
}
