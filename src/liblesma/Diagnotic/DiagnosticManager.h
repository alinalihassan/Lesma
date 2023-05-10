#pragma once

#include <utility>

#include "liblesma/Diagnotic/Diagnostic.h"

namespace lesma {
    /// This class manages the construction and emission of diagnostics.
    class DiagnosticEngine {
    public:
        /// A function used to handle diagnostics emitted by the engine.
        using HandlerFn = std::function<void(Diagnostic &)>;

        /// Emit an error to the diagnostic engine.
        Diagnostic emitError(SMRange loc, const std::string &msg) {
            return {Diagnostic::Severity::Error, loc, msg};
        }

        Diagnostic emitWarning(SMRange loc, const std::string &msg) {
            return {Diagnostic::Severity::Warning, loc, msg};
        }

        /// Report the given diagnostic.
        void report(Diagnostic &&diagnostic) {
            diagnostics.push_back(std::move(diagnostic));
            if (handler) {
                handler(diagnostics.back());
            }
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