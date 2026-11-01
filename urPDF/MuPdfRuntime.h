#pragma once

extern "C" {
#include <mupdf/fitz.h>
}

#include <array>
#include <memory>
#include <mutex>

namespace urpdf
{
    class MuPdfRuntime final
    {
    public:
        using Context = std::unique_ptr<fz_context, decltype(&fz_drop_context)>;

        MuPdfRuntime();
        ~MuPdfRuntime();

        MuPdfRuntime(MuPdfRuntime const&) = delete;
        MuPdfRuntime& operator=(MuPdfRuntime const&) = delete;
        MuPdfRuntime(MuPdfRuntime&&) = delete;
        MuPdfRuntime& operator=(MuPdfRuntime&&) = delete;

        Context CloneContext();

    private:
        std::array<std::mutex, FZ_LOCK_MAX> m_mutexes;
        fz_locks_context m_locks{};
        Context m_context{ nullptr, &fz_drop_context };
        std::mutex m_cloneMutex;
    };
}
