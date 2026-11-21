#include "MuPdfDocument.h"
#include "ExternalUriPolicy.h"

#include <mupdf/pdf.h>

#include <algorithm>
#include <cstring>
#include <memory>
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

    bool IsValidPdfDestination(fz_context* ctx, pdf_document* document, pdf_obj* dest, int pageCount, int depth = 0)
    {
        if (!dest || depth > 10) { return false; }
        if (pdf_is_name(ctx, dest) || pdf_is_string(ctx, dest))
        {
            return IsValidPdfDestination(ctx, document, pdf_lookup_dest(ctx, document, dest), pageCount, depth + 1);
        }
        if (pdf_is_dict(ctx, dest))
        {
            return IsValidPdfDestination(ctx, document, pdf_dict_get(ctx, dest, PDF_NAME(D)), pageCount, depth + 1);
        }
        if (!pdf_is_array(ctx, dest) || pdf_array_len(ctx, dest) < 2) { return false; }
        auto page = pdf_array_get(ctx, dest, 0);
        if (pdf_is_int(ctx, page))
        {
            const int index = pdf_to_int(ctx, page);
            return index >= 0 && index < pageCount;
        }
        if (!pdf_is_dict(ctx, page)) { return false; }
        const int index = pdf_lookup_page_number(ctx, document, page);
        return index >= 0 && index < pageCount;
    }

    bool HasInvalidPdfLinkDestination(fz_context* ctx, pdf_document* document, pdf_obj* annotation, int pageCount)
    {
        bool invalid = false;
        fz_try(ctx)
        {
            if (pdf_name_eq(ctx, pdf_dict_get(ctx, annotation, PDF_NAME(Subtype)), PDF_NAME(Link)))
            {
                auto dest = pdf_dict_get(ctx, annotation, PDF_NAME(Dest));
                if (!dest)
                {
                    auto action = pdf_dict_get(ctx, annotation, PDF_NAME(A));
                    if (!action) { action = pdf_dict_geta(ctx, pdf_dict_get(ctx, annotation, PDF_NAME(AA)), PDF_NAME(U), PDF_NAME(D)); }
                    if (pdf_name_eq(ctx, pdf_dict_get(ctx, action, PDF_NAME(S)), PDF_NAME(GoTo)))
                    {
                        dest = pdf_dict_get(ctx, action, PDF_NAME(D));
                        invalid = !dest;
                    }
                }
                if (dest) { invalid = !IsValidPdfDestination(ctx, document, dest, pageCount); }
            }
        }
        fz_catch(ctx)
        {
            fz_convert_error(ctx, nullptr);
            return true;
        }
        return invalid;
    }

    // Keep every longjmp frame free of C++ owners. The caller
    // adopts the returned linked list before constructing strings/vectors.
    fz_link* LoadPageLinks(fz_context* ctx, fz_document* document, int pageIndex, int pageCount,
        fz_rect* bounds, fz_link** invalidLinks)
    {
        fz_page* page = nullptr;
        fz_link* links = nullptr;
        pdf_obj* invalidAnnotations = nullptr;
        fz_var(page);
        fz_var(links);
        fz_var(invalidAnnotations);
        fz_try(ctx)
        {
            page = fz_load_page(ctx, document, pageIndex);
            *bounds = fz_bound_page(ctx, page);
            links = fz_load_links(ctx, page);
            auto pdf = pdf_specifics(ctx, document);
            if (pdf)
            {
                auto annotations = pdf_dict_get(ctx, pdf_lookup_page_obj(ctx, pdf, pageIndex), PDF_NAME(Annots));
                const int count = pdf_array_len(ctx, annotations);
                for (int i = 0; i < count; ++i)
                {
                    auto annotation = pdf_array_get(ctx, annotations, i);
                    if (HasInvalidPdfLinkDestination(ctx, pdf, annotation, pageCount))
                    {
                        if (!invalidAnnotations) { invalidAnnotations = pdf_new_array(ctx, pdf, 1); }
                        pdf_array_push(ctx, invalidAnnotations, annotation);
                    }
                }
                if (invalidAnnotations)
                {
                    // MuPDF can clamp a missing page reference to page 1 while
                    // creating its URI. Retain the original validation outcome:
                    // reproduce only those damaged annotations for matching.
                    fz_matrix transform;
                    auto pdfPage = pdf_page_from_fz_page(ctx, page);
                    pdf_page_transform(ctx, pdfPage, nullptr, &transform);
                    *invalidLinks = pdf_load_link_annots(ctx, pdf, pdfPage, invalidAnnotations, pageIndex, transform);
                }
            }
        }
        fz_always(ctx)
        {
            fz_drop_page(ctx, page);
            pdf_drop_obj(ctx, invalidAnnotations);
        }
        fz_catch(ctx)
        {
            fz_drop_link(ctx, links);
            fz_drop_link(ctx, *invalidLinks);
            *invalidLinks = nullptr;
            ThrowMuPdfError(ctx);
        }
        return links;
    }

    // Ligatures are not preserved: MuPDF then splits "fi" into separate letters with
    // split bounds, so copied text stays searchable.
    fz_stext_page* LoadStructuredText(fz_context* ctx, fz_document* document, int pageIndex)
    {
        fz_page* page = nullptr;
        fz_stext_page* text = nullptr;
        fz_stext_options options{};
        options.flags = FZ_STEXT_PRESERVE_WHITESPACE;
        fz_var(page);
        fz_try(ctx)
        {
            page = fz_load_page(ctx, document, pageIndex);
            text = fz_new_stext_page_from_page(ctx, page, &options);
        }
        fz_always(ctx)
        {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx)
        {
            ThrowMuPdfError(ctx);
        }
        return text;
    }

    int ResolvePageLink(fz_context* ctx, fz_document* document, char const* uri)
    {
        int page = -1;
        fz_try(ctx)
        {
            const auto dest = fz_resolve_link_dest(ctx, document, uri);
            if (dest.loc.chapter >= 0 && dest.loc.page >= 0)
            {
                page = fz_page_number_from_location(ctx, document, dest.loc);
            }
        }
        fz_catch(ctx)
        {
            // A damaged destination must not discard valid links on this page.
            fz_convert_error(ctx, nullptr);
            return -1;
        }
        return page;
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

    std::vector<PageLink> CollectPageLinks(fz_context* ctx, fz_document* document, int pageIndex)
    {
        const int pageCount = CountDocumentPages(ctx, document);
        fz_rect bounds{};
        const auto drop = [ctx](fz_link* links) { fz_drop_link(ctx, links); };
        fz_link* invalidList = nullptr;
        std::unique_ptr<fz_link, decltype(drop)> links(LoadPageLinks(ctx, document, pageIndex, pageCount, &bounds, &invalidList), drop);
        std::unique_ptr<fz_link, decltype(drop)> invalidOwner(invalidList, drop);
        std::vector<fz_link*> invalidLinks;
        for (auto link = invalidList; link; link = link->next) { invalidLinks.push_back(link); }
        const PdfPageBounds pageBounds{ bounds.x0, bounds.y0, bounds.x1, bounds.y1 };
        std::vector<PageLink> result;
        for (auto link = links.get(); link; link = link->next)
        {
            if (!link->uri || !*link->uri) { continue; }
            const auto rect = TryClipLinkRect({ link->rect.x0, link->rect.y0, link->rect.x1, link->rect.y1 }, pageBounds);
            if (!rect) { continue; }
            if (fz_is_external_link(ctx, link->uri))
            {
                auto uri = TryGetLaunchableExternalUri(link->uri);
                if (uri) { result.push_back({ *rect, PageLinkKind::external, std::move(*uri), -1 }); }
            }
            else
            {
                const auto invalid = std::find_if(invalidLinks.begin(), invalidLinks.end(), [link](fz_link const* candidate)
                    {
                        return candidate->uri && std::strcmp(candidate->uri, link->uri) == 0
                            && candidate->rect.x0 == link->rect.x0 && candidate->rect.y0 == link->rect.y0
                            && candidate->rect.x1 == link->rect.x1 && candidate->rect.y1 == link->rect.y1;
                    });
                if (invalid != invalidLinks.end()) { invalidLinks.erase(invalid); continue; }
                const int target = ResolvePageLink(ctx, document, link->uri);
                if (target >= 0 && target < pageCount)
                {
                    result.push_back({ *rect, PageLinkKind::internal, link->uri, target });
                }
            }
        }
        return result;
    }

    PageText LoadPageText(fz_context* ctx, fz_document* document, int pageIndex)
    {
        const auto drop = [ctx](fz_stext_page* text) { fz_drop_stext_page(ctx, text); };
        std::unique_ptr<fz_stext_page, decltype(drop)> text(LoadStructuredText(ctx, document, pageIndex), drop);
        PageText result;
        std::uint32_t blockIndex = 0;
        for (auto block = text->first_block; block; block = block->next)
        {
            if (block->type != FZ_STEXT_BLOCK_TEXT) { continue; }
            for (auto line = block->u.t.first_line; line; line = line->next)
            {
                const auto firstChar = static_cast<std::uint32_t>(result.chars.size());
                for (auto ch = line->first_char; ch; ch = ch->next)
                {
                    const auto bounds = fz_rect_from_quad(ch->quad);
                    result.chars.push_back({ static_cast<char32_t>(ch->c), { bounds.x0, bounds.y0, bounds.x1, bounds.y1 } });
                }
                const auto endChar = static_cast<std::uint32_t>(result.chars.size());
                if (firstChar == endChar) { continue; }
                result.lines.push_back({ firstChar, endChar, blockIndex,
                    { line->bbox.x0, line->bbox.y0, line->bbox.x1, line->bbox.y1 } });
            }
            ++blockIndex;
        }
        return result;
    }
}
