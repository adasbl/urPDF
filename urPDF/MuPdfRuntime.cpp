#include "MuPdfRuntime.h"

#include <new>
#include <stdexcept>

namespace
{
    constexpr std::size_t PdfStoreBudgetBytes = 256ull * 1024ull * 1024ull;

    void LockMuPdf(void* user, int lock)
    {
        static_cast<std::mutex*>(user)[lock].lock();
    }

    void UnlockMuPdf(void* user, int lock)
    {
        static_cast<std::mutex*>(user)[lock].unlock();
    }

    void RegisterDocumentHandlers(fz_context* context)
    {
        fz_try(context)
        {
            fz_register_document_handlers(context);
        }
        fz_catch(context)
        {
            int code = 0;
            throw std::runtime_error(fz_convert_error(context, &code));
        }
    }
}

namespace urpdf
{
    MuPdfRuntime::MuPdfRuntime()
    {
        m_locks.user = m_mutexes.data();
        m_locks.lock = LockMuPdf;
        m_locks.unlock = UnlockMuPdf;
        m_context.reset(fz_new_context(nullptr, &m_locks, PdfStoreBudgetBytes));
        if (!m_context)
        {
            throw std::bad_alloc();
        }

        RegisterDocumentHandlers(m_context.get());
    }

    MuPdfRuntime::~MuPdfRuntime() = default;

    MuPdfRuntime::Context MuPdfRuntime::CloneContext()
    {
        std::lock_guard lock(m_cloneMutex);
        Context context{ fz_clone_context(m_context.get()), &fz_drop_context };
        if (!context)
        {
            throw std::bad_alloc();
        }
        return context;
    }
}
