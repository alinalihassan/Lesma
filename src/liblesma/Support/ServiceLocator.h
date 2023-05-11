#pragma once

#include "liblesma/Support/DiagnosticManager.h"
#include "liblesma/Support/SourceManager.h"
#include <memory>

namespace lesma {
    class ServiceLocator {
    public:
        ServiceLocator() = delete;

        static SourceManager &getSourceManager();

        static DiagnosticManager &getDiagnosticManager();

    private:
        static std::unique_ptr<SourceManager> sourceManager;
        static std::unique_ptr<DiagnosticManager> diagnosticManager;
    };

}// namespace lesma
