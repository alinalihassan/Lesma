#pragma once

#include "fmt/core.h"

#include <algorithm>
#include <utility>

#include "liblesma/Support/Diagnostic.h"

namespace lesma {
    /// This class manages the construction and emission of diagnostics.
    class DiagnosticManager {
    public:
        /// A function used to handle diagnostics emitted by the engine.
        using HandlerFn = std::function<void(Diagnostic &)>;

        /// Emit an error to the diagnostic engine.
        void emitError(SMRange loc, const std::string &msg) {
            report({Diagnostic::Severity::Error, loc, msg});
        }

        template<typename... Args>
        void emitError(SMRange loc, const std::string &format, Args &&...args) {
            emitError(loc, fmt::format(format, std::forward<Args>(args)...));
        }

        void emitWarning(SMRange loc, const std::string &msg) {
            report({Diagnostic::Severity::Warning, loc, msg});
        }

        template<typename... Args>
        void emitWarning(SMRange loc, const std::string &format, Args &&...args) {
            emitWarning(loc, fmt::format(format, std::forward<Args>(args)...));
        }

        /// Report the given diagnostic.
        void report(Diagnostic &&diagnostic) {
            diagnostics.push_back(std::move(diagnostic));
            if (handler) {
                handler(diagnostics.back());
            }
        }

        [[nodiscard]] std::vector<Diagnostic> getDiagnostics() { return std::move(diagnostics); }

        [[nodiscard]] bool hasErrors() {
            return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto &diag) {
                return diag.getSeverity() == Diagnostic::Error;
            });
        }

        /// Get the current handler function of this diagnostic engine.
        [[nodiscard]] const HandlerFn &getHandlerFn() const { return handler; }

        /// Take the current handler function, resetting the current handler to null.
        HandlerFn takeHandlerFn() {
            HandlerFn oldHandler = std::move(handler);
            handler = {};
            return oldHandler;
        }

        /// Set the handler function for this diagnostic engine.
        void setHandlerFn(HandlerFn &&newHandler) { handler = std::move(newHandler); }

    private:
        /// The registered diagnostic handler function.
        HandlerFn handler;

        /// A vector of diagnostics
        std::vector<Diagnostic> diagnostics;
    };
}// namespace lesma