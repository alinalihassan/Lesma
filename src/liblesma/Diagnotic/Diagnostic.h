#pragma once

#include "liblesma/SourceManager/SMLoc.h"
#include <string>
#include <utility>

namespace lesma {
    class Diagnostic {
    public:
        enum Severity {
            Info,
            Warning,
            Error,
        };

        Diagnostic(Severity severity, SMRange loc, std::string msg)
            : severity(severity), message(std::move(msg)), location(loc) {}

        /// Return the severity of this diagnostic.
        [[nodiscard]] Severity getSeverity() const { return severity; }

        /// Return the message of this diagnostic.
        [[nodiscard]] std::string getMessage() const { return message; }

        /// Return the location of this diagnostic.
        [[nodiscard]] SMRange getLocation() const { return location; }

        /// Return the notes of this diagnostic.
        [[nodiscard]] auto getNotes() const { return &notes; }

        /// Attach a note to this diagnostic.
        Diagnostic &attachNote(const std::string &msg,
                               std::optional<SMRange> noteLoc) {
            assert(getSeverity() != Severity::Info &&
                   "cannot attach a Note to a Note");
            notes.emplace_back(
                    new Diagnostic(Severity::Info, noteLoc.value_or(location), msg));
            return *notes.back();
        }

    private:
        /// The severity of this diagnostic.
        Severity severity;
        /// The message held by this diagnostic.
        std::string message;
        /// The raw location of this diagnostic.
        SMRange location;
        /// Any additional note diagnostics attached to this diagnostic.
        std::vector<std::unique_ptr<Diagnostic>> notes;
    };

    //===----------------------------------------------------------------------===//
    // DiagnosticEngine
    //===----------------------------------------------------------------------===//

    /// This class manages the construction and emission of PDLL diagnostics.
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