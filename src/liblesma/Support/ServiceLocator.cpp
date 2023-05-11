#include "ServiceLocator.h"

using namespace lesma;

std::unique_ptr<SourceManager> ServiceLocator::sourceManager = nullptr;
std::unique_ptr<DiagnosticManager> ServiceLocator::diagnosticManager = nullptr;

SourceManager &ServiceLocator::getSourceManager() {
    if (!sourceManager) {
        sourceManager = std::make_unique<SourceManager>(SourceManager());
    }
    return *sourceManager;
}

DiagnosticManager &ServiceLocator::getDiagnosticManager() {
    if (!diagnosticManager) {
        diagnosticManager = std::make_unique<DiagnosticManager>(DiagnosticManager());
    }
    return *diagnosticManager;
}